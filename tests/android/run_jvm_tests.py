#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run production Android Kotlin on the JVM, suite by suite, without an Android build or emulator."""
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
from typing import NamedTuple

PACKAGE = "io.github.arancormonk.dsdneo"
HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
APP_SOURCES = Path("android/package/src/io/github/arancormonk/dsdneo")

# The compiler and the tests run in short-lived JVMs. C1 alone and the serial collector do their work in about a third of
# the CPU time of the default C2 and G1 threads, and CPU is what a loaded host runs out of: two cold Kotlin compiles of
# the usb suite outlasted CTest's 90 s beside a full parallel build.
# No shared performance data either: HotSpot would make an hsperfdata_<user> folder in the system temp folder (on
# Windows the native temp folder, TMP) and leave it behind after every JVM.
JVM_OPTIONS = ("-XX:TieredStopAtLevel=1", "-XX:+UseSerialGC", "-XX:-UsePerfData")
# The serial collector is a default only: HotSpot exits ("Multiple garbage collectors selected") when two are selected,
# so a collector the caller selected, in the options the JVM is started with or in those every JVM (JAVA_TOOL_OPTIONS,
# and HotSpot's _JAVA_OPTIONS) or every java launch (JDK_JAVA_OPTIONS) reads, is left alone, as is one an argument file
# the launcher expands there, or a HotSpot options file named in any of them, may select (names_argument_file(),
# names_options_file()).
# Matched against whole arguments, as the JVM splits these options (jvm_option_words()).
COLLECTOR_SELECTION = re.compile(r"-XX:\+Use\w*GC")
JVM_WIDE_OPTIONS = ("JAVA_TOOL_OPTIONS", "JDK_JAVA_OPTIONS", "_JAVA_OPTIONS")
# kotlinc takes its JVM's options from JAVA_OPTS, in both launchers (bin/kotlinc and bin/kotlinc.bat), and puts each
# word on the java command line itself. Not as -J arguments: Windows runs kotlinc.bat, and cmd.exe splits a batch file's
# arguments at "=", so -J-XX:TieredStopAtLevel=1 reached the JVM as -XX:TieredStopAtLevel and a stray 1. A set JAVA_OPTS
# replaces the launchers' default heap options, so they go in with it (Kotlin 2.4's default).
KOTLINC_DEFAULT_JAVA_OPTS = "-Xmx512M -Xms128M"

# A double-quoted literal on one line, and the escapes the golden records use (C and Kotlin agree on these).
STRING_LITERAL = re.compile(r'"((?:[^"\\\n]|\\.)*)"')
ESCAPES = {"t": "\t", "n": "\n", "r": "\r", "\\": "\\", '"': '"'}


def decode_literal(body, where):
    out = []
    index = 0
    while index < len(body):
        char = body[index]
        if char == "$":
            raise ValueError(f"{where}: '$' (a Kotlin string template) does not belong in the golden record")
        if char != "\\":
            out.append(char)
            index += 1
            continue
        escape = body[index + 1] if index + 1 < len(body) else ""
        if escape not in ESCAPES:
            raise ValueError(f"{where}: unsupported escape \\{escape}")
        out.append(ESCAPES[escape])
        index += 2
    return "".join(out)


def literal_after(path, marker, continuations):
    """The string literals that follow `marker` in `path`, joined, read on while a line ends with a continuation."""
    where = f"{path.relative_to(REPO)}: {marker}"
    lines = path.read_text(encoding="utf-8").splitlines()
    start = next((number for number, line in enumerate(lines) if line.strip().startswith(marker)), None)
    if start is None:
        raise ValueError(f"{where}: not found")
    end = start
    while lines[end].rstrip().endswith(continuations) and end + 1 < len(lines):
        end += 1
    text = "\n".join([lines[start].split(marker, 1)[1], *lines[start + 1 : end + 1]])
    pieces = STRING_LITERAL.findall(text)
    if not pieces:
        raise ValueError(f"{where}: no string literal")
    return "".join(decode_literal(piece, where) for piece in pieces)


def golden_records_match():
    """The C encoder test and DecoderStatusTest.kt pin the same v4 record; a drift would let each side pass alone."""
    c_test = REPO / "tests/ui/test_app_control_notification_status.c"
    kotlin_test = HERE / "DecoderStatusTest.kt"
    try:
        encoded = literal_after(c_test, "#define NOTIFICATION_GOLDEN_V4_RECORD", ("\\",))
        parsed = literal_after(kotlin_test, "private const val GOLDEN_V4 =", ("=", "+"))
    except (OSError, ValueError) as error:
        return f"cannot read the golden v4 records: {error}"
    if encoded == parsed:
        return None
    left, right = encoded.split("\t"), parsed.split("\t")
    field = next((i for i, (a, b) in enumerate(zip(left, right)) if a != b), min(len(left), len(right)))
    first = left[field] if field < len(left) else "<none>"
    second = right[field] if field < len(right) else "<none>"
    return (
        f"NOTIFICATION_GOLDEN_V4_RECORD in {c_test.relative_to(REPO)} and GOLDEN_V4 in "
        f"{kotlin_test.relative_to(REPO)} must be byte-identical; they first differ at field {field} "
        f"({first!r} vs {second!r}; {len(left)} vs {len(right)} fields)"
    )


def kotlin_golden_matches(c_marker, kotlin_test, kotlin_marker):
    """Whether a golden diagnostics record in a Kotlin test and the C++ test that runs it through redaction agree."""
    c_test = REPO / "tests/ui/test_ui_qt_diagnostics_log.cpp"
    kotlin_path = HERE / kotlin_test
    try:
        native = literal_after(c_test, c_marker, ("\\",))
        kotlin = literal_after(kotlin_path, kotlin_marker, ("=", "+"))
    except (OSError, ValueError) as error:
        return f"cannot read the golden diagnostics records: {error}"
    if native == kotlin:
        return None
    return (
        f"{c_marker} in {c_test.relative_to(REPO)} and {kotlin_marker} in {kotlin_path.relative_to(REPO)} must be "
        "byte-identical, so the records Kotlin writes are the ones native redaction is shown to keep"
    )


def watchdog_golden_matches():
    return kotlin_golden_matches(
        "#define DIAGNOSTICS_GOLDEN_WATCHDOG", "LooperWatchdogTest.kt", "private const val GOLDEN_WATCHDOG ="
    )


def screen_golden_matches():
    return kotlin_golden_matches(
        "#define DIAGNOSTICS_GOLDEN_SCREEN", "ScreenTraceTest.kt", "private const val GOLDEN_SCREEN ="
    )


class Build(NamedTuple):
    sources: tuple  # Production files under APP_SOURCES.
    tests: tuple  # Test files beside this script; each one's main() runs, in this order.
    # Android platform stubs from stubs/ that compile with it. A build with none has no android.* classes at all, so an
    # Android import in its sources fails the compile.
    stubs: tuple = ()
    # Tests that also run with their temp folder as working directory: FileListingTest binds a Unix socket by a name
    # relative to it, since the absolute name under a build tree is longer than a socket address holds. The others run
    # in the caller's working directory, so a relative path the caller gave the JVM still resolves.
    in_folder: tuple = ()


class Suite(NamedTuple):
    builds: tuple  # Each compiles on its own and runs its tests, in this order.
    # Source checks run before anything compiles, and without the tools: each returns None or what is wrong.
    checks: tuple = ()


def document_copy_codes_match():
    """DecoderHost::DocumentCopy and DocumentTransfer's result codes cross JNI as plain ints, so they must agree."""
    header = REPO / "src/ui/qt/decoder_host.h"
    kotlin = REPO / APP_SOURCES / "DocumentTransfer.kt"
    try:
        enum = re.search(r"enum\s+DocumentCopy\s*\{([^}]*)\}", header.read_text(encoding="utf-8"))
        kotlin_text = kotlin.read_text(encoding="utf-8")
    except OSError as error:
        return f"cannot read the document copy codes: {error}"
    if not enum:
        return f"{header.relative_to(REPO)}: no enum DocumentCopy"
    native = {}
    for name, value in re.findall(r"Document(\w+)\s*=\s*(\d+)", enum.group(1)):
        native[re.sub(r"(?<!^)(?=[A-Z])", "_", name).upper()] = int(value)
    jvm = {name: int(value) for name, value in re.findall(r"const val ([A-Z_]+) = (\d+)", kotlin_text)}
    jvm.pop("CHUNK_BYTES", None)
    if native != jvm:
        return (
            f"DecoderHost::DocumentCopy in {header.relative_to(REPO)} ({native}) and the result codes in "
            f"{kotlin.relative_to(REPO)} ({jvm}) must name the same values"
        )
    return None


def backup_probe_constants_match():
    """DocumentCopies' probe reads on to count only after the backup magic and reads one backup header, so its copies of
    DSD_BACKUP_MAGIC (src/crypto/backup_seal.c) and DSD_BACKUP_HEADER_BYTES (backup_seal.h) must be the same."""
    seal = REPO / "src/crypto/backup_seal.c"
    header = REPO / "include/dsd-neo/crypto/backup_seal.h"
    kotlin = REPO / APP_SOURCES / "DocumentCopies.kt"
    try:
        seal_text = seal.read_text(encoding="utf-8")
        header_text = header.read_text(encoding="utf-8")
        kotlin_text = kotlin.read_text(encoding="utf-8")
    except OSError as error:
        return f"cannot read the backup probe constants: {error}"
    pairs = (
        ("the backup magic", re.search(r'#define\s+DSD_BACKUP_MAGIC\s+"([^"]*)"', seal_text), seal,
         re.search(r'const val BACKUP_MAGIC_TEXT = "([^"]*)"', kotlin_text)),
        ("the header size", re.search(r"#define\s+DSD_BACKUP_HEADER_BYTES\s+(\d+)U?\b", header_text), header,
         re.search(r"const val PROBE_HEADER_BYTES = (\d+)\b", kotlin_text)),
    )
    for what, native, native_path, jvm in pairs:
        if not native or not jvm:
            return (f"{what} must be found in {native_path.relative_to(REPO)} and {kotlin.relative_to(REPO)} "
                    "(BACKUP_MAGIC_TEXT, PROBE_HEADER_BYTES)")
        if native.group(1) != jvm.group(1):
            return (f"{what} in {kotlin.relative_to(REPO)} ({jvm.group(1)!r}) must match "
                    f"{native_path.relative_to(REPO)} ({native.group(1)!r})")
    return None


def kotlin_codes(path, names):
    """The `const val NAME = <int>` values of `names` in the Kotlin source at `path`."""
    text = path.read_text(encoding="utf-8")
    return {name: int(value) for name, value in re.findall(r"const val ([A-Z_]+) = (\d+)", text) if name in names}


def native_enum_matches(enum_name, prefix, source, names, jvm_prefix=""):
    """Whether DecoderHost's `enum_name` (members `prefix<Name>`) and the Kotlin `source`'s `names` agree; the Kotlin
    constants are `jvm_prefix<NAME>`."""
    header = REPO / "src/ui/qt/decoder_host.h"
    try:
        enum = re.search(rf"enum\s+{enum_name}\s*\{{([^}}]*)\}}", header.read_text(encoding="utf-8"))
        codes = kotlin_codes(REPO / APP_SOURCES / source, {jvm_prefix + name for name in names})
        jvm = {name[len(jvm_prefix) :]: value for name, value in codes.items()}
    except OSError as error:
        return f"cannot read the {enum_name} codes: {error}"
    if not enum:
        return f"{header.relative_to(REPO)}: no enum {enum_name}"
    native = {name.upper(): int(value) for name, value in re.findall(rf"{prefix}(\w+)\s*=\s*(\d+)", enum.group(1))}
    native = {name: value for name, value in native.items() if name in names}
    if len(jvm) != len(names) or native != jvm:
        return (
            f"DecoderHost::{enum_name} in {header.relative_to(REPO)} ({native}) and the codes in "
            f"{APP_SOURCES / source} ({jvm}) must name the same values"
        )
    return None


def recording_transfer_codes_match():
    """#708: a call recording transfer's result crosses JNI as a plain int."""
    names = ("DONE", "FAILED", "CANCELLED", "MISSING", "CHANGED")
    return native_enum_matches("RecordingTransferResult", "Transfer", "RecordingExport.kt", names)


def playback_states_match():
    """#708: the call recording player's state crosses JNI as a plain int."""
    names = ("STOPPED", "PREPARING", "PLAYING", "PAUSED", "FAILED", "STARTING")
    return native_enum_matches("PlaybackState", "Playback", "RecordingPlayer.kt", names)


def playback_modes_match():
    """#909: the play's mode (one recording, a call's parts, History's playlist) crosses JNI as a plain int."""
    names = ("SINGLE", "PARTS", "PLAYLIST")
    return native_enum_matches("PlaybackMode", "Playback", "RecordingPlayer.kt", names, jvm_prefix="MODE_")


def playback_codes_match():
    """#909: a status record whose reason is not one of its field's codes is ignored, so the codes the parser takes
    (playback_status_record.h) must be the ones RecordingPlayer.kt writes: its ERROR_*, PAUSED_* and ENDED_* values,
    less ENDED_REPLACED, which no record carries."""
    header = REPO / "android/playback_status_record.h"
    kotlin = REPO / APP_SOURCES / "RecordingPlayer.kt"
    try:
        header_text = header.read_text(encoding="utf-8")
        kotlin_text = kotlin.read_text(encoding="utf-8")
    except OSError as error:
        return f"cannot read the playback reason codes: {error}"
    jvm_codes = dict(re.findall(r'const val ((?:ERROR|PAUSED|ENDED)_[A-Z_]+) = "([a-z]*)"', kotlin_text))
    jvm_codes.pop("ENDED_REPLACED", None)
    for field, prefix in (("Error", "ERROR_"), ("Paused", "PAUSED_"), ("Ended", "ENDED_")):
        table = re.search(rf"kPlayback{field}Codes\[\]\s*=\s*\{{([^}}]*)\}}", header_text)
        if not table:
            return f"{header.relative_to(REPO)}: no kPlayback{field}Codes"
        native = sorted(re.findall(r'"([a-z]*)"', table.group(1)))
        jvm = sorted(value for name, value in jvm_codes.items() if name.startswith(prefix))
        if native != jvm:
            return (
                f"kPlayback{field}Codes in {header.relative_to(REPO)} ({native}) and the {prefix}* codes in "
                f"{kotlin.relative_to(REPO)} ({jvm}) must name the same values"
            )
    return None


def top_level_items(text):
    """The comma-separated items of @p text outside any parentheses, empty ones (a trailing comma's) left out."""
    items, depth, item = [], 0, []
    for char in text:
        if char == "," and depth == 0:
            items.append("".join(item).strip())
            item = []
            continue
        depth += {"(": 1, ")": -1}.get(char, 0)
        item.append(char)
    items.append("".join(item).strip())
    return [entry for entry in items if entry]


def playback_requests_checked_before_begin():
    """#909 R63/R70: RecordingPlayback refuses a request RecordingPlayer.validRequest() refuses before begin() accepts
    it, so a refused one changes neither the play running nor the service's ownership. RecordingPlayback has Android
    imports, so the JVM suites cannot run it: each play() entry is checked here to call validRequest( before begin(."""
    playback = REPO / APP_SOURCES / "RecordingPlayback.kt"
    try:
        source = playback.read_text(encoding="utf-8")
    except OSError as error:
        return f"cannot read {playback.relative_to(REPO)}: {error}"
    entries = (
        ("play(context, path)", r"fun play\(context: Context, path: String\): Int \{.*?\n    \}"),
        ("play(context, arrays)", r"fun play\(\s*context: Context,\s*paths: Array<String>,.*?\n    \}"),
    )
    for name, pattern in entries:
        body = re.search(pattern, source, re.S)
        if not body:
            return f"{playback.relative_to(REPO)}: {name} not found"
        text = body.group(0)
        checked = text.find("RecordingPlayer.validRequest(")
        begun = text.find("begin(")
        if checked < 0 or begun < 0 or checked > begun:
            return f"{playback.relative_to(REPO)}: {name} must call RecordingPlayer.validRequest( before begin("
    return None


def playback_scoped_commands_check_first():
    """#909 R73: RecordingPlayback's token- and generation-scoped commands (pauseFor, resumeFor, stopGeneration) go
    through PlayPauseTaps.scoped() (itemCommand() for the item ones), which checks the command before it cancels a
    waiting headset tap; never through explicit(), which cancels first. RecordingPlayback cannot run in the JVM suites,
    so its source is checked here."""
    playback = REPO / APP_SOURCES / "RecordingPlayback.kt"
    try:
        source = playback.read_text(encoding="utf-8")
    except OSError as error:
        return f"cannot read {playback.relative_to(REPO)}: {error}"
    for name in ("pauseFor", "resumeFor", "stopGeneration"):
        body = re.search(rf"fun {name}\(.*?(?=\n    @JvmStatic|\n    private fun |\n    /\*\*)", source, re.S)
        if not body:
            return f"{playback.relative_to(REPO)}: {name} not found"
        text = body.group(0)
        if "explicit" in text or ("itemCommand(" not in text and "PlayPauseTaps.scoped(" not in text):
            return f"{playback.relative_to(REPO)}: {name} must go through PlayPauseTaps.scoped(), not explicit()"
    item = re.search(r"private fun itemCommand\(.*?\n\n", source, re.S)
    if not item or "PlayPauseTaps.scoped(" not in item.group(0):
        return f"{playback.relative_to(REPO)}: itemCommand() must go through PlayPauseTaps.scoped()"
    return None


def playback_looper_reads_no_main_service():
    """#909: RecordingPlayback's `service` field is the main thread's. The noisy receiver, delivered on the
    playback looper, reads the instance's taps through the volatile `serviceTaps`, as the looper commands do; every
    other read of `service` sits inside a `main.post` block."""
    playback = REPO / APP_SOURCES / "RecordingPlayback.kt"
    try:
        source = playback.read_text(encoding="utf-8")
    except OSError as error:
        return f"cannot read {playback.relative_to(REPO)}: {error}"
    noisy = re.search(r"private object Noisy : BroadcastReceiver\(\) \{.*?\n    \}\n", source, re.S)
    if not noisy:
        return f"{playback.relative_to(REPO)}: the Noisy receiver not found"
    code = re.sub(r"//[^\n]*", "", noisy.group(0))
    if re.search(r"\bservice\b", code) or "serviceTaps" not in code:
        return f"{playback.relative_to(REPO)}: the Noisy receiver must read serviceTaps, not the main-thread service"
    return None


def playback_record_fields_match():
    """#909: the status record's fields are read by position, so Status.record() and the idle record in
    RecordingPlayback.kt must list as many as playback_status_record.h's kPlaybackStatusFields."""
    header = REPO / "android/playback_status_record.h"
    player = REPO / APP_SOURCES / "RecordingPlayer.kt"
    playback = REPO / APP_SOURCES / "RecordingPlayback.kt"
    try:
        count = re.search(r"constexpr int kPlaybackStatusFields = (\d+);", header.read_text(encoding="utf-8"))
        record = re.search(r"fun record\(nowMs: Long\): String = listOf\((.*?)\)\.joinToString",
                           player.read_text(encoding="utf-8"), re.S)
        idle = re.search(r"fun idleRecord\(\): String =\s*listOf\((.*?)\)\.joinToString",
                         playback.read_text(encoding="utf-8"), re.S)
    except OSError as error:
        return f"cannot read the playback status record: {error}"
    if not count or not record or not idle:
        return "kPlaybackStatusFields, Status.record() and RecordingPlayback's idleRecord() must all be found"
    for source, found in ((player, record), (playback, idle)):
        fields = len(top_level_items(found.group(1)))
        if fields != int(count.group(1)):
            return (f"{source.relative_to(REPO)} writes {fields} status record fields, and "
                    f"{header.relative_to(REPO)} reads {count.group(1)}")
    return None


def recordings_provider_contract():
    """#708: the recordings provider serves the two "Call recordings" folders and the share copies' cache folder that
    RecordingShare names, and no more."""
    package = REPO / "android/package"
    try:
        manifest = (package / "AndroidManifest.xml").read_text(encoding="utf-8")
        paths = (package / "res/xml/recordings_paths.xml").read_text(encoding="utf-8")
        share = (REPO / APP_SOURCES / "RecordingShare.kt").read_text(encoding="utf-8")
        host = (REPO / "src/ui/qt/decoder_host.cpp").read_text(encoding="utf-8")
    except OSError as error:
        return f"cannot read the recordings provider sources: {error}"
    providers = re.findall(r"<provider\b(.*?)</provider>", manifest, re.S)
    ours = [p for p in providers if 'android:authorities="${applicationId}.recordings"' in p]
    if len(ours) != 1:
        return "AndroidManifest.xml: one provider must have the authority ${applicationId}.recordings"
    for attribute in (
        'android:name="io.github.arancormonk.dsdneo.RecordingsProvider"',
        'android:exported="false"',
        'android:grantUriPermissions="true"',
        'android:resource="@xml/recordings_paths"',
    ):
        if attribute not in ours[0]:
            return f"AndroidManifest.xml: the recordings provider needs {attribute}"
    entries = re.findall(r"<([\w-]+)\s+name=\"(\w+)\"\s+path=\"([^\"]*)\"\s*/>", paths)
    served = [
        ("cache-path", "shares", "recording-shares/"),
        ("external-files-path", "external", "Call recordings/"),
        ("files-path", "internal", "Call recordings/"),
    ]
    if sorted(entries) != served:
        return (
            "res/xml/recordings_paths.xml: only the two Call recordings folders and the share copies' folder may be "
            f"served, not {entries}"
        )
    if (
        'const val FOLDER = "Call recordings"' not in share
        or 'const val AUTHORITY_SUFFIX = ".recordings"' not in share
        or 'const val SHARE_FOLDER = "recording-shares"' not in share
    ):
        return f"{APP_SOURCES / 'RecordingShare.kt'}: FOLDER, SHARE_FOLDER and AUTHORITY_SUFFIX must match the provider"
    if 'json_store_path(QStringLiteral("Call recordings"))' not in host:
        return "src/ui/qt/decoder_host.cpp: the base host's root must use the same folder name"
    if "FLAG_GRANT_WRITE_URI_PERMISSION" in share:
        return f"{APP_SOURCES / 'RecordingShare.kt'}: a share grants read access only"
    return None


def stop_callers_carry_their_cause():
    """Each production nativeStop caller names its own StopCause; only the user's Stop may abandon rdio uploads."""
    app = REPO / APP_SOURCES
    try:
        texts = {path.name: path.read_text(encoding="utf-8") for path in sorted(app.glob("*.kt"))}
    except OSError as error:
        return f"cannot read the Android sources: {error}"
    for name, text in texts.items():
        if name == "DsdNative.kt":
            continue
        for match in re.finditer(r"DsdNative(?:\.|::)nativeStop\b", text):
            line = text[: match.start()].count("\n") + 1
            head = text[max(0, match.start() - 160) : match.start()]
            if not re.search(r"EngineStop\.request\(\s*[\w.]+,\s*$", head):
                return f"{APP_SOURCES / name}:{line}: call nativeStop only through EngineStop.request(cause, ...)"
    # The expected cause at each caller, as (file, pattern, what it pins).
    expected = (
        ("UsbSourceManager.kt", r"EngineStop\.request\(StopCause\.DEVICE_DETACHED,", "a USB detach drains"),
        ("DsdNeoActivity.kt", r"EngineStop\.request\(StopCause\.LIFECYCLE,", "the Activity's teardown drains"),
        ("DecoderService.kt", r"stopDecoding\(StopCause\.LIFECYCLE\)", "the service's onDestroy drains"),
        (
            "DecoderService.kt",
            r"intent\?\.action == ACTION_STOP && requestEngineStop\(\s*StopCause\.fromWire\(intent\.getStringExtra\(EXTRA_STOP_CAUSE\)\)",
            "a stop intent's cause comes from its sender",
        ),
    )
    for name, pattern, what in expected:
        if not re.search(pattern, texts.get(name, "")):
            return f"{APP_SOURCES / name}: {what}; expected /{pattern}/"
    # Every stop intent the app sends names its cause: the notification's Stop action is the user's, and the Qt host's
    # one sender passes the cause of its entry point, Stop listening the user's and a roaming switch's its own (#786).
    service = texts.get("DecoderService.kt", "")
    senders = re.findall(r"\.setAction\(ACTION_STOP\)(?:\s*\.putExtra\(EXTRA_STOP_CAUSE, ([\w.]+)\))?", service)
    if sorted(senders) != ["StopCause.USER.wire", "cause.wire"]:
        return (
            f"{APP_SOURCES / 'DecoderService.kt'}: the ACTION_STOP senders must be the notification's "
            f"(StopCause.USER.wire) and requestStop()'s (cause.wire); found {senders}"
        )
    entries = (
        (r"fun stopDecoder\(context: Context\?\) = requestStop\(context, StopCause\.USER\)", "Stop listening is the user's"),
        (
            r"fun stopDecoderForSwitch\(context: Context\?\) = requestStop\(context, StopCause\.SWITCH\)",
            "a roaming switch drains",
        ),
    )
    for pattern, what in entries:
        if not re.search(pattern, service):
            return f"{APP_SOURCES / 'DecoderService.kt'}: {what}; expected /{pattern}/"
    return None


def start_status_contract():
    """#786 B2: startDecoder()'s status crosses JNI as a plain int, and 0 is what a failed JNI call answers.

    So handed over must be nonzero, every status distinct, android/start_status.h must name each one with the same
    value (START_NOT_ALLOWED is kStartNotAllowed), the JNI call must pass the status on as it is, and the host must
    treat anything but handed over as a start error.
    """
    kotlin_path = REPO / APP_SOURCES / "DecoderService.kt"
    header_path = REPO / "android/start_status.h"
    jni_path = REPO / "android/decoder_host_android.cpp"
    host_path = REPO / "android/decoder_host_android_lifecycle.cpp"
    try:
        kotlin = kotlin_path.read_text(encoding="utf-8")
        header = header_path.read_text(encoding="utf-8")
        jni = jni_path.read_text(encoding="utf-8")
        host = host_path.read_text(encoding="utf-8")
    except OSError as error:
        return f"cannot read the start status sources: {error}"
    statuses = {name: int(value) for name, value in re.findall(r"const val (START_[A-Z_]+) = (-?\d+)\b", kotlin)}
    handed = statuses.get("START_HANDED_OVER")
    if handed is None or len(statuses) < 2:
        return f"{APP_SOURCES / 'DecoderService.kt'}: no START_HANDED_OVER and refusal statuses"
    if 0 in statuses.values():
        return f"{APP_SOURCES / 'DecoderService.kt'}: no start status may be 0, a failed JNI call's answer: {statuses}"
    if len(set(statuses.values())) != len(statuses):
        return f"{APP_SOURCES / 'DecoderService.kt'}: the start statuses must be distinct: {statuses}"
    native = {name: int(value) for name, value in re.findall(r"constexpr int (kStart\w+) = (-?\d+);", header)}
    for name, value in statuses.items():
        twin = "kStart" + "".join(part.capitalize() for part in name[len("START_"):].split("_"))
        if native.get(twin) != value:
            return f"android/start_status.h: {twin} must equal {name} ({value})"
    if native.get("kStartCallFailed") != 0 or len(set(native.values())) != len(native):
        return f"android/start_status.h: kStartCallFailed must be 0 and every status distinct: {native}"
    call = re.search(r"DecoderHostAndroid::platformStartDecoder\(.*?\n\}", jni, re.S)
    if not call or "return static_cast<int>(status);" not in call.group(0):
        return "android/decoder_host_android.cpp: platformStartDecoder must return startDecoder()'s status as it is"
    if not re.search(r"if \(handover != kStartHandedOver\) \{\s*return refuseHandover\(attempt, handover,", host):
        return (
            "android/decoder_host_android_lifecycle.cpp: every status but kStartHandedOver must go to refuseHandover()"
        )
    refusal = re.search(r"DecoderHostAndroid::refuseHandover\(.*?\n\}", host, re.S)
    if not refusal or "return failStart(startRefusalText(status));" not in refusal.group(0):
        return "android/decoder_host_android_lifecycle.cpp: refuseHandover() must fail a start it does not supersede"
    return None


def counter_reader_contract():
    """#786: the roaming counters cross JNI as a plain long, and 0 is both a valid count and what a failed Qt call answers.

    QJniObject::callStaticMethod<jlong>() clears a failed lookup's exception and returns 0, so a reader built on it reads
    an unreadable counter as the counter's first value. Each reader must go through read_static_counter(), which checks
    the class, the method and the call one by one and answers -1 on any failure, and the Kotlin side must be the static
    ()J reader it names.
    """
    jni_path = REPO / "android/decoder_host_android.cpp"
    try:
        jni = jni_path.read_text(encoding="utf-8")
        kotlin = {name: (REPO / APP_SOURCES / name).read_text(encoding="utf-8")
                  for name in ("DecoderService.kt", "LocationStream.kt")}
    except OSError as error:
        return f"cannot read the counter reader sources: {error}"
    readers = (
        ("platformUserStopGeneration", "kServiceClass", "DecoderService.kt", "userStopGeneration"),
        ("platformForegroundEpoch", "kStreamClass", "LocationStream.kt", "foregroundEpoch"),
    )
    for host_fn, owner, source, method in readers:
        if not re.search(rf"@JvmStatic\s+fun {method}\(\): Long\b", kotlin[source]):
            return f"{APP_SOURCES / source}: expected a @JvmStatic fun {method}(): Long for the host to read"
        body = re.search(rf"DecoderHostAndroid::{host_fn}\(\) \{{(.*?)\n\}}", jni, re.S)
        if not body or not re.search(rf'return read_static_counter\({owner}, "{method}"\);', body.group(1)):
            return f"android/decoder_host_android.cpp: {host_fn} must answer read_static_counter({owner}, \"{method}\")"
    helper = re.search(r"\nread_static_counter\(const char\* owner, const char\* name\) \{(.*?)\n\}", jni, re.S)
    if not helper:
        return "android/decoder_host_android.cpp: no read_static_counter(owner, name)"
    steps = (
        r"=\s*env\.findClass\(owner\);\s*if \(found == nullptr\) \{\s*return -1;",
        r'=\s*env\.findStaticMethod\(found, name, "\(\)J"\);\s*if \(method == nullptr\) \{\s*return -1;',
        r"=\s*env->CallStaticLongMethod\(found, method\);\s*if \(env\.checkAndClearExceptions\(\) \|\| value < 0\) \{"
        r"\s*return -1;",
    )
    for step in steps:
        if not re.search(step, helper.group(1)):
            return f"android/decoder_host_android.cpp: read_static_counter must answer -1 on each failure; expected /{step}/"
    if "callStaticMethod" in helper.group(1):
        return "android/decoder_host_android.cpp: read_static_counter must not use QJniObject::callStaticMethod (0 on failure)"
    return None


def native_lifecycle_contract():
    """DsdNative.kt never compiles here (it loads the app library), so its JNI pairing is read from the sources."""
    kotlin_path = REPO / APP_SOURCES / "DsdNative.kt"
    jni_paths = sorted((REPO / "android").glob("*.cpp"))
    try:
        kotlin = kotlin_path.read_text(encoding="utf-8")
        jni = "\n".join(path.read_text(encoding="utf-8") for path in jni_paths)
    except OSError as error:
        return f"cannot read the JNI sources: {error}"
    for name in re.findall(r"external fun (native\w+)\(", kotlin):
        if not re.search(rf"\bJava_io_github_arancormonk_dsdneo_DsdNative_{name}\(", jni):
            return f"{APP_SOURCES / 'DsdNative.kt'}: {name} has no JNI definition under android/"
    # #647: the first accepted cause of a session's ending is latched natively, so every stop passes its own.
    if not re.search(r"external fun nativeStop\(userRequested: Boolean, cause: String\): Int", kotlin):
        return f"{APP_SOURCES / 'DsdNative.kt'}: nativeStop must take (userRequested: Boolean, cause: String)"
    if not re.search(r"DsdNative_nativeStop\(JNIEnv\* env, jclass clazz, jboolean user_requested,\s*jstring cause\)", jni):
        return "android/dsdneo_jni.cpp: nativeStop must take the jstring cause after user_requested"
    # A stop the configure discards must not leave its cause latched (#647): the flag reset and the new record share one
    # lifecycle-lock block, and nativeStop latches its cause and raises its flag inside one.
    lock = r"std::lock_guard<std::mutex> status_guard\(g_lifecycle_lock\);"
    if not re.search(lock + r"[^}]*g_stop_requested\.store\(false\);[^}]*g_run_status\.begin\(", jni):
        return "android/dsdneo_jni.cpp: nativeConfigure must clear g_stop_requested before begin() under g_lifecycle_lock"
    if not re.search(lock + r"[^}]*note_stop_cause\([^}]*g_stop_requested\.store\(true\);", jni):
        return "android/dsdneo_jni.cpp: nativeStop must latch its cause and raise g_stop_requested under g_lifecycle_lock"
    # Android owns an unplug through the detach broadcast for the grace (#647); without it the reader's -1 races it.
    init = re.search(r"DsdNative_nativeInit\(.*?\n\}", jni, re.S)
    if not init or "dsd_android::install_receiver_policy();" not in init.group(0):
        return "android/dsdneo_jni.cpp: nativeInit must call dsd_android::install_receiver_policy()"
    # #782 gate r2 H3: only a run's dsd_engine_cleanup() closes the files the bootstrap opens (-c, -w, -6; -P opens its
    # per-call WAVs only at the run's start), so every way a configured session ends without nativeRun() closes them:
    # a discard, a failed or exiting configure, the next configure over one never run
    # (SessionObjects::prepare_for_configure() releases before its reset), and nativeDestroy()
    # (SessionObjects::destroy() releases before it frees the objects).
    release = "g_session.release_outputs();"
    prepare = "g_session.prepare_for_configure()"
    for name, call, why in (
        ("nativeDiscardConfigured", release, "for a session that never ran"),
        ("nativeConfigure", release, "after a failed or exiting bootstrap"),
        ("nativeConfigure", prepare, "so each configure starts from fresh objects (#786 B1)"),
        ("nativeDestroy", "g_session.destroy();", "for a session that never ran"),
    ):
        body = re.search(rf"DsdNative_{name}\(.*?\n\}}", jni, re.S)
        if not body or call not in body.group(0):
            return f"android/dsdneo_jni.cpp: {name} must call {call} {why}"
    # #786 B1: a start can reach the service instance an earlier one configured or ran on, so every configure resets
    # the objects (prepare_for_configure()) before its bootstrap, and releases what a failed one opened after it.
    configure = re.search(r"DsdNative_nativeConfigure\(.*?\n\}", jni, re.S).group(0)
    bootstrap = configure.find("dsd_runtime_bootstrap(")
    if bootstrap < 0 or not 0 <= configure.find(prepare) < bootstrap or configure.rfind(release) < bootstrap:
        return (
            "android/dsdneo_jni.cpp: nativeConfigure must call prepare_for_configure() before the bootstrap and "
            "release after a failed one"
        )
    return None


def receiver_option_tables_match():
    """UsbReceiverKind.sourceOfArgs() reads a start's input as the engine's parser does, from copies of its tables.

    The getopt string (k_short_options in src/runtime/cli/compact.c, which args.c's getopt() reads through
    dsd_cli_short_options()) and dsd_cli_compact_args()'s long-option tables in compact.c must equal the Kotlin ones, or
    an option's operand could be read as an input (#782)."""
    kotlin_path = REPO / APP_SOURCES / "UsbReceiverKind.kt"
    args_path = REPO / "src/runtime/cli/args.c"
    compact_path = REPO / "src/runtime/cli/compact.c"
    try:
        kotlin = kotlin_path.read_text(encoding="utf-8")
        args = args_path.read_text(encoding="utf-8")
        compact = compact_path.read_text(encoding="utf-8")
    except OSError as error:
        return f"cannot read the option tables: {error}"
    if not re.search(r"getopt\(argc, argv, dsd_cli_short_options\(\)\)", args):
        return f"the getopt() in {args_path.relative_to(REPO)} no longer reads dsd_cli_short_options(): mirror its string"
    native_short = re.search(r"\bk_short_options\[\] =\s*((?:\"[^\"]*\"\s*)+);", compact)
    kotlin_short = re.search(r"const val SHORT_OPTIONS =\s*((?:\"[^\"]*\"\s*\+?\s*)+)", kotlin)
    if not native_short or not kotlin_short:
        return f"cannot find k_short_options in {compact_path.relative_to(REPO)} or SHORT_OPTIONS in {APP_SOURCES}"
    joined = ["".join(re.findall(r'"([^"]*)"', match.group(1))) for match in (native_short, kotlin_short)]
    if joined[0] != joined[1]:
        return f"SHORT_OPTIONS in UsbReceiverKind.kt must equal k_short_options of compact.c: {joined[0]!r}"
    pairs = (
        ("k_skip_exact_no_arg", "LONG_NO_VALUE"),
        ("k_skip_exact_next_any", "LONG_NEXT_ANY"),
        ("k_skip_exact_next_nonnull", "LONG_NEXT_PRESENT"),
        ("k_free_value_options", "LONG_FREE_VALUE"),
        ("k_skip_exact_next_nonopt", "LONG_NEXT_NON_OPTION"),
        ("k_skip_prefix", "LONG_ATTACHED_PREFIXES"),
    )
    for native_name, kotlin_name in pairs:
        native = re.search(rf"\b{native_name}\[\] = \{{(.*?)\}};", compact, re.S)
        jvm = re.search(rf"val {kotlin_name}: Set<String> = setOf\((.*?)\n    \)", kotlin, re.S)
        if not native or not jvm:
            return f"cannot find {native_name} in compact.c or {kotlin_name} in UsbReceiverKind.kt"
        native_items = re.findall(r'"([^"]*)"', native.group(1))
        jvm_items = re.findall(r'"([^"]*)"', jvm.group(1))
        if sorted(native_items) != sorted(jvm_items):
            missing = sorted(set(native_items) - set(jvm_items))
            extra = sorted(set(jvm_items) - set(native_items))
            return f"{kotlin_name} in UsbReceiverKind.kt must equal {native_name} in compact.c: missing {missing}, extra {extra}"
    # k_skip_exact_next_nonopt_optional only marks which k_skip_exact_next_nonopt options may go without a value
    # (the Qt start check reads it); compaction treats them alike, so the Kotlin copy has nothing to mirror. It must
    # stay a subset of the table it qualifies.
    optional = re.search(r"\bk_skip_exact_next_nonopt_optional\[\] = \{(.*?)\};", compact, re.S)
    nonopt = re.search(r"\bk_skip_exact_next_nonopt\[\] = \{(.*?)\};", compact, re.S)
    if optional and nonopt:
        outside = sorted(set(re.findall(r'"([^"]+)"', optional.group(1))) - set(re.findall(r'"([^"]+)"', nonopt.group(1))))
        if outside:
            return f"k_skip_exact_next_nonopt_optional in compact.c names options k_skip_exact_next_nonopt lacks: {outside}"
    # Any other long-option table in compact.c would be one this copy does not know.
    tables = set(re.findall(r"static const char\* const (k_\w+)\[\]", compact)) | {"k_free_value_options"}
    unknown = sorted(tables - {native for native, _ in pairs} - {"k_skip_exact_next_nonopt_optional"})
    if unknown:
        return f"compact.c has long-option tables UsbReceiverKind.kt does not mirror: {unknown}"
    # -i lower-cases a known scheme given in another case, so "RTL:0" opens an RTL-SDR: the classifier reads it so too.
    if "dsd_input_spec_normalize_scheme(opts->audio_in_dev);" in args and not all(
        f'startsWith("{scheme}:", ignoreCase = true)' in kotlin for scheme in ("rtl", "airspy")
    ):
        return "UsbReceiverKind.kt must read an rtl: or airspy: scheme in any case, as -i normalizes it in args.c"
    return replay_prescan_matches(kotlin, args, compact)


# A write to the parse's input in args.c: a copy, format or concatenation into opts->audio_in_dev, or one of its bytes.
AUDIO_IN_DEV_WRITE = r"(?:DSD_\w+|memcpy|memmove|strn?cpy|strn?cat|v?snprintf)\(opts->audio_in_dev\b|opts->audio_in_dev\[[^\]]*\] = "


def iq_stream_respelling(args):
    """The match of cli_read_iq_stream_spec() in args.c, or why it is not only a respelling of -i's value (issue #743).

    It may write the input once: an -i value that is already an iqtcp: or iqudp: spec, rewritten as the canonical text
    of the spec parsed from it, which dsd_iq_net_spec_format() starts with that same kind. The input stays an I/Q
    stream, which UsbReceiverKind.sourceOfArgs() reads as no receiver either way, so the Kotlin side needs nothing. It
    runs only from getopt's -i, right after that value is copied in."""
    function = re.search(
        r"\nstatic int\ncli_read_iq_stream_spec\(dsd_opts\* opts, int\* out_exit_rc\) \{\n(.*?)\n\}\n", args, re.S
    )
    if not function:
        return "cannot find cli_read_iq_stream_spec() in args.c"
    body = function.group(1)
    where = "args.c cli_read_iq_stream_spec()"
    if not re.match(r"\s*if \(!dsd_opts_audio_in_dev_is_iq_stream_spec\(opts->audio_in_dev\)\) \{\s*return 0;\s*\}", body):
        return f"{where} must return before anything else unless -i is already an I/Q stream spec"
    respelling = 'DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", canonical);'
    if re.findall(AUDIO_IN_DEV_WRITE, body) != ["DSD_SNPRINTF(opts->audio_in_dev"] or body.count(respelling) != 1:
        return f"{where} may write the input only as {respelling}: model any other write in UsbReceiverKind.sourceOfArgs()"
    canonical_lines = {line.strip() for line in body.splitlines() if re.search(r"\bcanonical\b", line)}
    if canonical_lines != {
        "char canonical[DSD_IQ_NET_SPEC_TEXT_MAX];",
        "if (dsd_iq_net_spec_format(&spec, canonical, sizeof canonical) < 0) {",
        respelling,
    } or "if (dsd_iq_net_spec_parse(opts->audio_in_dev, &spec, why, sizeof why) != 0) {" not in body:
        return f"{where} must write only the canonical text of the spec it parsed from -i"
    calls = re.findall(r"\bcli_read_iq_stream_spec\(", args)
    # Between the copy and the read, the scheme of a known input given in another case is lower-cased
    # (dsd_input_spec_normalize_scheme(): "RTL:0" is "rtl:0"), which UsbReceiverKind.sourceOfArgs() reads alike.
    after_copy = (
        r"DSD_STRNCPY\(opts->audio_in_dev, optarg, 2047\);[^\n]*\n[^\n]*opts->audio_in_dev\[2047\] = '\\0';[^\n]*\n"
        r"[^\n]*dsd_input_spec_normalize_scheme\(opts->audio_in_dev\);[^\n]*\n"
        r"[^\n]*opts->input_cli_seen = 1;[^\n]*\n[^\n]*if \(cli_read_iq_stream_spec\(opts, out_exit_rc\) != 0\)"
    )
    if len(calls) != 2 or not re.search(after_copy, args):
        return (
            f"{where} must run once, from getopt's -i right after its value is copied in and its scheme"
            " normalized (dsd_input_spec_normalize_scheme())"
        )
    spec_path = REPO / "src/io/iq/iq_net_spec.c"
    try:
        spec_source = spec_path.read_text(encoding="utf-8")
    except OSError as error:
        return f"cannot read the I/Q stream spec formatter: {error}"
    prefix = re.search(
        r'iq_net_kind_prefix\(dsd_iq_net_kind kind\) \{\s*return kind == DSD_IQ_NET_KIND_TCP \? "iqtcp" : "iqudp";\s*\}',
        spec_source,
    )
    formatter = re.search(
        r"\ndsd_iq_net_spec_format\(const dsd_iq_net_spec\* spec, char\* out, size_t out_size\) \{\n(.*?)\n\}\n",
        spec_source,
        re.S,
    )
    # Every call that writes the text: each a DSD_SNPRINTF whose format and first argument put the kind first.
    body = formatter.group(1) if formatter else ""
    writers = re.findall(r"\b\w+\(out\b", body)
    texts = re.findall(r"DSD_SNPRINTF\(out, out_size, (\"[^\"]*\", [\w>-]+(?:\([^()]*\))?)", body)
    kind_first = {
        '"%s:%s:%d:%u:%s:%u", iq_net_kind_prefix(spec->kind)',
        '"%s:%s:%d:%u:%s", iq_net_kind_prefix(spec->kind)',
    }
    if not prefix or not texts or len(texts) != len(writers) or any(text not in kind_first for text in texts):
        return "dsd_iq_net_spec_format() must start its text with the spec's own iqtcp or iqudp kind"
    return function


def replay_prescan_matches(kotlin, args, compact):
    """UsbReceiverKind.replayRequested() reads --iq-replay as dsd_parse_args()'s long-option pre-scan does (#782 r2 H1).

    The pre-scan takes a value for every long option of compact.c's value tables except the bootstrap's own
    (PRESCAN_BOOTSTRAP_ONLY), sets the replay from --iq-replay and --iq-replay=, and the replay is the only input the
    parse sets besides getopt's -i."""
    prescan = re.search(
        r"#define DSD_PARSE_ARGS_PRESCAN_BLOCK\(\)(.*?)\n#define DSD_PARSE_ARGS_", args, re.S
    )
    bootstrap_path = REPO / "src/runtime/bootstrap/bootstrap.c"
    receive_path = REPO / "src/runtime/receive_policy.c"
    jvm_bootstrap = re.search(r"val PRESCAN_BOOTSTRAP_ONLY: Set<String> = setOf\((.*?)\n    \)", kotlin, re.S)
    jvm_replay = re.search(r'const val IQ_REPLAY = "([^"]*)"', kotlin)
    try:
        bootstrap = bootstrap_path.read_text(encoding="utf-8")
        receive = receive_path.read_text(encoding="utf-8")
    except OSError as error:
        return f"cannot read the bootstrap or receive options: {error}"
    if not prescan or not jvm_bootstrap or not jvm_replay:
        return "cannot find DSD_PARSE_ARGS_PRESCAN_BLOCK in args.c, or PRESCAN_BOOTSTRAP_ONLY or IQ_REPLAY in UsbReceiverKind.kt"
    block = prescan.group(1)
    if jvm_replay.group(1) != "--iq-replay" or not re.search(
        r'strcmp\(argv\[i\], "--iq-replay"\) == 0\)(?:(?!\n        if \().)*?iq_replay_cli = DSD_PARSE_ARGS_NEXT_ARG\(\)',
        block,
        re.S,
    ) or not re.search(r'strncmp\(argv\[i\], "--iq-replay=", 12\) == 0\)\s*\{\s*\\?\s*iq_replay_cli = argv\[i\] \+ 12;', block):
        return "the pre-scan no longer reads --iq-replay <path> and --iq-replay=<path> as UsbReceiverKind.replayRequested() does"
    value_tables = ("k_skip_exact_next_any", "k_skip_exact_next_nonnull", "k_free_value_options", "k_skip_exact_next_nonopt")
    compact_values = set()
    for name in value_tables:
        table = re.search(rf"\b{name}\[\] = \{{(.*?)\}};", compact, re.S)
        if not table:
            return f"cannot find {name} in compact.c"
        compact_values |= set(re.findall(r'"([^"]*)"', table.group(1)))
    # What the pre-scan takes a value for: an exact name whose branch reads the next argument, every --airspy-* option
    # through cli_parse_airspy_option(), and the receive policy's options through dsd_receive_option_argument().
    taken = set()
    for branch in re.split(r"\n        if \(", block):
        head = branch.split("\n", 1)[0]
        if "DSD_PARSE_ARGS_NEXT_ARG()" in branch or "arg_advance = 2" in branch:
            taken |= set(re.findall(r'strcmp\(argv\[i\], "(--[^"]*)"\) == 0', head))
    if 'strncmp(argv[i], "--airspy-", 9) == 0' in block and "cli_parse_airspy_option" in block:
        taken |= {name for name in compact_values if name.startswith("--airspy-")}
    kinds = re.search(r"dsd_receive_kind_name\(int kind\) \{\s*static const char\* const names\[\] = \{(.*?)\};", receive, re.S)
    if "dsd_receive_option_argument(" in block and kinds:
        taken |= {f"--{kind}-{suffix}" for kind in re.findall(r'"([^"]*)"', kinds.group(1)) for suffix in ("allow", "block", "name")}
    bootstrap_only = set(re.findall(r'"([^"]*)"', jvm_bootstrap.group(1)))
    expected = compact_values - bootstrap_only
    if sorted(taken & compact_values) != sorted(expected):
        return (
            "PRESCAN_BOOTSTRAP_ONLY in UsbReceiverKind.kt must name exactly the value options the args.c pre-scan does "
            f"not take a value for: missing {sorted(expected - taken)}, extra {sorted((taken & compact_values) - expected)}"
        )
    bootstrap_options = set(re.findall(r'strcmp\(argv\[\*i\], "(--[^"]*)"\) != 0', bootstrap))
    if not bootstrap_only <= bootstrap_options:
        return f"PRESCAN_BOOTSTRAP_ONLY names options bootstrap.c does not read: {sorted(bootstrap_only - bootstrap_options)}"
    # -i's value respelled in canonical form (issue #743) is the same input, so it is set aside; any other write counts.
    canonical = iq_stream_respelling(args)
    if isinstance(canonical, str):
        return canonical
    args_without_respelling = args[: canonical.start()] + args[canonical.end() :]
    # The input the parse sets: getopt's -i (strncpy and terminator) and the replay (cli_set_iqreplay_audio_dev()).
    writes = re.findall(AUDIO_IN_DEV_WRITE, args_without_respelling)
    replay_sets = re.findall(r"opts->iq_replay_requested = 1;", args)
    if len(writes) != 4 or len(replay_sets) != 1:
        return (
            f"args.c sets the input in {len(writes)} places and the replay in {len(replay_sets)}, not 4 and 1: model the new "
            "one in UsbReceiverKind.sourceOfArgs()"
        )
    return None


def ready_branch_rewrites_status(source):
    """Whether every `if (isReady()) {` branch writes "Ready: …" as its first statement, past any comment lines.

    A line scan rather than one regex: a comment run in a regex lets `\\s*` and the comment body share `//`, which
    backtracks exponentially on a long run of slashes."""
    lines = source.splitlines()
    starts = [index for index, line in enumerate(lines) if line.strip() == "if (isReady()) {"]
    if not starts:
        return False
    for start in starts:
        body = (line.strip() for line in lines[start + 1 :])
        statement = next((line for line in body if line and not line.startswith("//")), "")
        if not statement.startswith('setStatus("Ready: '):
            return False
    return True


def usb_broker_contract():
    """UsbSourceManager talks to the platform, so it never compiles here; its use of UsbRequestState is read instead."""
    manager_path = REPO / APP_SOURCES / "UsbSourceManager.kt"
    record_path = REPO / "android/local_device_state.h"
    try:
        manager = manager_path.read_text(encoding="utf-8")
        record = record_path.read_text(encoding="utf-8")
    except OSError as error:
        return f"cannot read the USB broker sources: {error}"
    where = APP_SOURCES / "UsbSourceManager.kt"
    # #647 B9: each permission request has its own PendingIntent identity and says which request it answers.
    broadcasts = re.findall(r"PendingIntent\.getBroadcast\(([^)]*)\)", manager)
    if not broadcasts or any(not re.match(r"\s*\w+,\s*generation,", args) for args in broadcasts):
        return f"{where}: every permission PendingIntent must use the request's generation as its request code"
    if "FLAG_UPDATE_CURRENT" in manager:
        return f"{where}: FLAG_UPDATE_CURRENT would hand an old request's reply the new request's extras"
    if not re.search(r"\.putExtra\(EXTRA_GENERATION, generation\)", manager):
        return f"{where}: the permission intent must carry its generation"
    if not re.search(r"requests\.onReply\(\s*intent\.getIntExtra\(EXTRA_GENERATION,", manager):
        return f"{where}: a permission reply must be matched to its request by generation"
    # #647 B8: a detach stops the engine only for a session that uses the local receiver, and always releases.
    detach = re.search(r"UsbManager\.ACTION_USB_DEVICE_DETACHED ->(.*?)UsbManager\.ACTION_USB_DEVICE_ATTACHED ->", manager, re.S)
    owner = r"ownsSession = DsdNative\.nativeSessionUsesLocalReceiver\(\) \|\|\s*DsdNative\.nativeIsUsbFdInUse\(\)\n"
    stop = r"if \(ownsSession\) \{[^}]*EngineStop\.request\(StopCause\.DEVICE_DETACHED,[^}]*\}(\s*else \{[^}]*\})?\s*release\(\)"
    if not detach or not re.search(owner, detach.group(1)) or not re.search(stop, detach.group(1)):
        return (
            f"{where}: a detach must stop the engine only when nativeSessionUsesLocalReceiver() or nativeIsUsbFdInUse() "
            "says the session uses it, and release the connection either way"
        )
    # #647 WP4: the host polls deviceStatus(), which retires an unanswered request and publishes the request (the
    # record recordLocked() builds, which a session's reopen also holds, #782).
    status = re.search(r"fun deviceStatus\(\): String \{(.*?)\n    \}", manager, re.S)
    if not status or "requests.expireIfStale()" not in status.group(1):
        return f"{where}: deviceStatus() must retire an unanswered request"
    built = re.search(r"fun recordLocked\(\): String =(.*?)\.toString\(\)\n", manager, re.S)
    if "recordLocked()" not in status.group(1) or not built:
        return f"{where}: deviceStatus() must publish the record recordLocked() builds"
    for key in ("pending", "requestedSource", "requestedSerial"):
        if f'.put("{key}"' not in built.group(1):
            return f"{where}: deviceStatus() must publish {key}"
    # #647 B4: an access request that finds the receiver ready rewrites the record for it.
    if not ready_branch_rewrites_status(manager):
        return f"{where}: an access request while ready must rewrite \"Ready: <device>\" with no failure kind"
    # #647 B11: every failure kind the broker writes is one the host's record knows; "refused" is no device failure.
    kinds = set(re.findall(r'setStatus\([^\n]*,\s*"(\w+)"\)', manager))
    if "UsbRequestState.REFUSED)" not in manager:
        return f"{where}: the switch refusal must be written with kind UsbRequestState.REFUSED"
    for kind in sorted(kinds):
        if f'QStringLiteral("{kind}")' not in record:
            return f"android/local_device_state.h: UsbSourceManager writes kind \"{kind}\", which the record never maps"
    if not re.search(r'QStringLiteral\("refused"\)\s*\?\s*dsd_qt::DecoderHost::NoDeviceFailure', record):
        return "android/local_device_state.h: kind \"refused\" must map to NoDeviceFailure"
    return None


def method_body(text, signature):
    """The body of the Kotlin method whose declaration line starts with `signature`, up to its closing brace at the same
    indent; None when it is missing."""
    lines = text.splitlines()
    start = next((i for i, line in enumerate(lines) if line.strip().startswith(signature)), None)
    if start is None:
        return None
    indent = len(lines[start]) - len(lines[start].lstrip())
    end = next(
        (i for i in range(start + 1, len(lines)) if lines[i].strip() == "}" and len(lines[i]) - len(lines[i].lstrip()) == indent),
        None,
    )
    return None if end is None else "\n".join(lines[start + 1 : end])


def a11y_bridge_order():
    """#782: Qt reads QT_ANDROID_DISABLE_ACCESSIBILITY synchronously inside super.onCreate(), so the bridge's launch
    decision comes immediately before it and the freeze immediately after; the notice follows the activity's resume,
    pause, stop and destroy. LaunchGuard records a second instance first, before Qt can restart the process inside
    super.onCreate(), and after ProcessFacts.attach, which gives it the context to record with; it learns that Qt
    started right after super.onCreate() returns."""
    path = REPO / APP_SOURCES / "DsdNeoActivity.kt"
    where = APP_SOURCES / "DsdNeoActivity.kt"
    try:
        text = path.read_text(encoding="utf-8")
    except OSError as error:
        return f"cannot read the activity: {error}"
    body = method_body(text, "override fun onCreate(")
    if body is None:
        return f"{where}: no onCreate"
    statements = [line.strip() for line in body.splitlines() if line.strip() and not line.strip().startswith("//")]
    expected = [
        "LaunchGuard.onCreate(ProcessFacts::setStateSummary)",
        "AccessibilityBridge.effects = AccessibilityRestartNotice",
        "QmlGcPolicy.beforeQt(HostDiagnostics::submit)",
        "AccessibilityBridge.beforeQt(this)",
        "super.onCreate(savedInstanceState)",
        "AccessibilityBridge.afterQt(this)",
        "LaunchGuard.afterQt()",
    ]
    prefixes = ("LaunchGuard.", "AccessibilityBridge.", "QmlGcPolicy.", "super.onCreate(")
    found = [line for line in statements if line in expected or line.startswith(prefixes)]
    if found != expected:
        return f"{where}: onCreate must run exactly {expected}, in that order, with nothing of these elsewhere; found {found}"
    start = statements.index(expected[0])
    if statements[start : start + len(expected)] != expected:
        return f"{where}: in onCreate, {expected} must be consecutive statements: the variable is read inside super.onCreate()"
    attach = "ProcessFacts.attach(this)"
    if attach not in statements[:start]:
        return f"{where}: in onCreate, {attach} must come before {expected[0]}, which records through it"
    for callback in ("onResume", "onPause", "onStop", "onDestroy"):
        lifecycle = method_body(text, f"override fun {callback}(")
        if lifecycle is None or f"AccessibilityBridge.{callback}(this)" not in lifecycle:
            return f"{where}: {callback} must call AccessibilityBridge.{callback}(this)"
    destroy = method_body(text, "override fun onDestroy(")
    if destroy.find("AccessibilityBridge.onDestroy(this)") > destroy.find("super.onDestroy()"):
        return f"{where}: AccessibilityBridge.onDestroy(this) must come before super.onDestroy(), which may not return"
    return None


# Every android:configChanges flag that android-36's attrs_manifest.xml defines, except resourcesUnused, which overrides
# them all and is for activities that use no resources (#782). A flag missing here recreates the activity, and Qt 6.11.3
# then restarts the process without the app's teardown. Audited for AUDITED_COMPILE_SDK only.
PINNED_CONFIG_CHANGES = (
    "mcc", "mnc", "locale", "touchscreen", "keyboard", "keyboardHidden", "navigation", "orientation", "screenLayout",
    "uiMode", "screenSize", "smallestScreenSize", "density", "layoutDirection", "colorMode", "grammaticalGender",
    "fontScale", "fontWeightAdjustment", "assetsPaths",
)
# The platform PINNED_CONFIG_CHANGES was audited against; assetsPaths, the newest flag, exists from it on. Android adds a
# flag in most releases, so a compile SDK bump fails launch_policy until the list is audited for the new platform.
AUDITED_COMPILE_SDK = 36
# The heading in docs/code_map.md that lists the runs a Qt bump repeats.
QT_UPGRADE_CHECKS = 'the "Qt upgrade checks" in docs/code_map.md'
# An element's opening tag outside comments, and its android: attributes. The manifest is the app's own, so plain
# patterns do; no XML parser is needed for it.
XML_COMMENT = re.compile(r"<!--.*?-->", re.S)
ACTIVITY_TAG = re.compile(r"<activity\b([^>]*)>")
ANDROID_ATTRIBUTE = re.compile(r'\bandroid:(\w+)\s*=\s*"([^"]*)"')


def launch_policy():
    """#782: Qt 6.11.3 restarts the process inside onCreate, without the app's teardown, when a second activity instance
    is created in a process it started, so the manifest allows none: singleTask, and every configuration change handled
    in place, against the compile SDK the list was audited for, the one CI installs. LaunchGuard's summary names the Qt
    the restart path was audited on, so a Qt bump fails here until it is audited again; this proves only that the text
    was edited, not that the checks ran. The record's plumbing (ProcessFacts) is pinned by source, since recreation
    cannot be triggered on a fixed build."""
    manifest_path = REPO / "android/package/AndroidManifest.xml"
    cmake_path = REPO / "android/CMakeLists.txt"
    pins_path = REPO / "tools/ci-dependency-pins.env"
    try:
        manifest = XML_COMMENT.sub(" ", manifest_path.read_text(encoding="utf-8"))
        cmake = cmake_path.read_text(encoding="utf-8")
        pins = pins_path.read_text(encoding="utf-8")
    except OSError as error:
        return f"cannot read the launch policy: {error}"
    tags = (dict(ANDROID_ATTRIBUTE.findall(tag.group(1))) for tag in ACTIVITY_TAG.finditer(manifest))
    activities = [tag for tag in tags if tag.get("name") == f"{PACKAGE}.DsdNeoActivity"]
    if len(activities) != 1:
        return f"AndroidManifest.xml: expected one DsdNeoActivity, found {len(activities)}"
    activity = activities[0]
    mode = activity.get("launchMode")
    if mode != "singleTask":
        return (
            f"AndroidManifest.xml: DsdNeoActivity launchMode is {mode}; singleTask keeps a second instance out of the "
            "process"
        )
    flags = activity.get("configChanges", "").split("|")
    if len(flags) != len(set(flags)):
        return f"AndroidManifest.xml: configChanges repeats a flag: {flags}"
    missing = [flag for flag in PINNED_CONFIG_CHANGES if flag not in flags]
    extra = [flag for flag in flags if flag not in PINNED_CONFIG_CHANGES]
    if missing or extra:
        return (
            f"AndroidManifest.xml: configChanges must be exactly {'|'.join(PINNED_CONFIG_CHANGES)}; "
            f"missing {missing}, extra {extra}"
        )
    compile_sdk = re.findall(
        r"^\s*set_property\(TARGET dsd-neo-app PROPERTY QT_ANDROID_COMPILE_SDK_VERSION (\d+)\)", cmake, re.M
    )
    if len(compile_sdk) != 1:
        return "android/CMakeLists.txt: pin the compile SDK once with QT_ANDROID_COMPILE_SDK_VERSION"
    pinned = re.findall(r"^ANDROID_COMPILE_SDK=(\d+)$", pins, re.M)
    if len(pinned) != 1:
        return "tools/ci-dependency-pins.env: no single ANDROID_COMPILE_SDK"
    if int(compile_sdk[0]) != AUDITED_COMPILE_SDK:
        return (
            f"android/CMakeLists.txt: compile SDK {compile_sdk[0]}, but configChanges was audited for "
            f"android-{AUDITED_COMPILE_SDK}: compare the configChanges flags in platforms/android-{compile_sdk[0]}/data/"
            "res/values/attrs_manifest.xml with PINNED_CONFIG_CHANGES (all but resourcesUnused), update the manifest and "
            "the list, then AUDITED_COMPILE_SDK"
        )
    if compile_sdk[0] != pinned[0]:
        return (
            f"android/CMakeLists.txt compiles against android-{compile_sdk[0]}, but CI installs android-{pinned[0]} "
            "(ANDROID_COMPILE_SDK in tools/ci-dependency-pins.env)"
        )
    try:
        guard = (REPO / APP_SOURCES / "LaunchGuard.kt").read_text(encoding="utf-8")
        facts = (REPO / APP_SOURCES / "ProcessFacts.kt").read_text(encoding="utf-8")
    except OSError as error:
        return f"cannot read LaunchGuard or ProcessFacts: {error}"
    problem = launch_record_plumbing(facts)
    if problem:
        return f"{APP_SOURCES / 'ProcessFacts.kt'}: {problem}"
    qt = re.findall(r"^ANDROID_QT_VERSION=(\S+)$", pins, re.M)
    audited = re.findall(r'const val QT_RESTART = "Qt (\S+) restarts the process"', guard)
    if len(qt) != 1 or len(audited) != 1:
        return "cannot find ANDROID_QT_VERSION in tools/ci-dependency-pins.env or QT_RESTART in LaunchGuard.kt"
    if qt[0] != audited[0]:
        return (
            f"Qt is pinned at {qt[0]}, but LaunchGuard.QT_RESTART was audited on {audited[0]}: run {QT_UPGRADE_CHECKS} "
            "again, then update the text (or the record and the documented limit, if Qt no longer restarts the process)"
        )
    return None


def document_picker_wiring():
    """#829: DsdNeoActivity never compiles here, so its forwarding is read from the source: every result goes to
    DocumentPicker first, and only one it does not claim goes on to Qt. The GET_CONTENT fallback's handler queries see
    other apps only through the manifest's <queries> entry (package visibility, targetSdk 30+)."""
    where = APP_SOURCES / "DsdNeoActivity.kt"
    try:
        activity = (REPO / where).read_text(encoding="utf-8")
        manifest = XML_COMMENT.sub(" ", (REPO / "android/package/AndroidManifest.xml").read_text(encoding="utf-8"))
    except OSError as error:
        return f"cannot read the activity or the manifest: {error}"
    body = method_body(activity, "override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?)")
    statements = [line.strip() for line in (body or "").splitlines() if line.strip() and not line.strip().startswith("//")]
    expected = [
        "if (!DocumentPicker.onActivityResult(this, requestCode, resultCode, data)) {",
        "super.onActivityResult(requestCode, resultCode, data)",
        "}",
    ]
    if statements != expected:
        return f"{where}: onActivityResult must be exactly {expected}; found {statements}"
    queries = re.findall(r"<queries>(.*?)</queries>", manifest, re.S)
    intents = re.findall(r"<intent>(.*?)</intent>", "".join(queries), re.S)
    wanted = {
        ("action", "name", "android.intent.action.GET_CONTENT"),
        ("category", "name", "android.intent.category.OPENABLE"),
        ("data", "mimeType", "*/*"),
    }
    for intent in intents:
        if set(re.findall(r'<(\w+)\s+android:(\w+)="([^"]*)"', intent)) == wanted:
            return None
    return (
        "AndroidManifest.xml: <queries> needs an <intent> with exactly GET_CONTENT + OPENABLE + mimeType */*, which the "
        "GET_CONTENT fallback's handler queries see other apps through"
    )


def activity_statements(text, signature):
    """The statements of an activity method, one per line, without comments; None when the method is missing."""
    body = method_body(text, signature)
    if body is None:
        return None
    return [line.strip() for line in body.splitlines() if line.strip() and not line.strip().startswith("//")]


# The four filters DsdNeoActivity declares, each as its (element, android: attribute, value) set (#831). Open with is
# content only and lists text/plain (D1); Share has no scheme, since a filter that lists one does not match an intent with
# no data, which a SEND is, and leaves text/plain out so DSD-neo is not offered for every shared link or text (D1).
DOCUMENT_TYPES = {
    ("data", "mimeType", "text/csv"),
    ("data", "mimeType", "text/comma-separated-values"),
    ("data", "mimeType", "application/octet-stream"),
}
ACTIVITY_FILTERS = {
    "launcher": {
        ("action", "name", "android.intent.action.MAIN"),
        ("category", "name", "android.intent.category.LAUNCHER"),
    },
    "USB attachment": {("action", "name", "android.hardware.usb.action.USB_DEVICE_ATTACHED")},
    "Open with": {
        ("action", "name", "android.intent.action.VIEW"),
        ("category", "name", "android.intent.category.DEFAULT"),
        ("data", "scheme", "content"),
        ("data", "mimeType", "text/plain"),
        *DOCUMENT_TYPES,
    },
    "Share": {
        ("action", "name", "android.intent.action.SEND"),
        ("category", "name", "android.intent.category.DEFAULT"),
        *DOCUMENT_TYPES,
    },
}
FILTER_ELEMENT = re.compile(r"<(action|category|data)\b([^>]*)>")


def activity_filters(manifest):
    """Each intent filter of the one DsdNeoActivity element, as its (element, android: attribute, value) set; None when
    there is not exactly one such element."""
    bodies = [
        body
        for attributes, body in re.findall(r"<activity\b([^>]*)>(.*?)</activity>", manifest, re.S)
        if dict(ANDROID_ATTRIBUTE.findall(attributes)).get("name") == f"{PACKAGE}.DsdNeoActivity"
    ]
    if len(bodies) != 1:
        return None
    return [
        {
            (element.group(1), attribute, value)
            for element in FILTER_ELEMENT.finditer(block)
            for attribute, value in ANDROID_ATTRIBUTE.findall(element.group(2))
        }
        for block in re.findall(r"<intent-filter\b[^>]*>(.*?)</intent-filter>", bodies[0], re.S)
    ]


def broadened_filter(elements):
    """What in one filter would offer DSD-neo for more than one document of the listed types, or None."""
    values = {value for _, _, value in elements}
    if "*/*" in values or "android.intent.action.SEND_MULTIPLE" in values:
        return "*/* or SEND_MULTIPLE"
    sharing = ("action", "name", "android.intent.action.SEND") in elements
    if sharing and any(attribute == "scheme" or value == "text/plain" for _, attribute, value in elements):
        return "a scheme or text/plain on Share"
    return None


def incoming_document_filters():
    """#831: DsdNeoActivity is exported, and its filters are what other apps can send it. Exactly four: the launcher,
    the USB attachment, Open with and Share, each with exactly its elements, so no */*, no SEND_MULTIPLE, no scheme on
    Share and no text/plain there. What the filters cannot check (EXTRA_STREAM, a missing action) is the intake gate's."""
    try:
        manifest = XML_COMMENT.sub(" ", (REPO / "android/package/AndroidManifest.xml").read_text(encoding="utf-8"))
    except OSError as error:
        return f"cannot read the manifest: {error}"
    filters = activity_filters(manifest)
    if filters is None:
        return "AndroidManifest.xml: expected one DsdNeoActivity element"
    for elements in filters:
        problem = broadened_filter(elements)
        if problem:
            return f"AndroidManifest.xml: DsdNeoActivity must not list {problem}: {sorted(elements)}"
    expected = list(ACTIVITY_FILTERS.values())
    missing = [name for name, elements in ACTIVITY_FILTERS.items() if elements not in filters]
    extra = [sorted(elements) for elements in filters if elements not in expected]
    if missing or extra or len(filters) != len(expected):
        return (
            f"AndroidManifest.xml: DsdNeoActivity's intent filters must be exactly {list(ACTIVITY_FILTERS)}; "
            f"missing {missing}, unexpected {extra}"
        )
    return None


def incoming_document_reports():
    """#831 B1: DsdNeoActivity never compiles here, so its reports are read from the source. onCreate reports the launch
    intent, to the USB broker and to the document intake, only when it is a fresh delivery (a relaunch from Recents
    re-delivers the first one), and after the USB attachments are set up; onNewIntent reports every delivery. The
    intake's refusal record names the action's kind and the reason only."""
    where = APP_SOURCES / "DsdNeoActivity.kt"
    try:
        activity = (REPO / where).read_text(encoding="utf-8")
        intake = (REPO / APP_SOURCES / "IncomingDocumentIntake.kt").read_text(encoding="utf-8")
        documents = (REPO / APP_SOURCES / "IncomingDocuments.kt").read_text(encoding="utf-8")
    except OSError as error:
        return f"cannot read the activity or the intake: {error}"
    create = activity_statements(activity, "override fun onCreate(") or []
    reports = ("UsbSourceManager.reportAttachment(", "IncomingDocuments.")
    guarded = [
        "UsbSourceManager.initializeAttachments(this)",
        "val fromHistory = (intent.flags and Intent.FLAG_ACTIVITY_LAUNCHED_FROM_HISTORY) != 0",
        "val fresh = LaunchDelivery.fresh(savedInstanceState != null, fromHistory)",
        "if (fresh) {",
        "UsbSourceManager.reportAttachment(this, intent)",
        "IncomingDocuments.report(this, intent, HostDiagnostics::submit)",
        "}",
    ]
    if create[-len(guarded) :] != guarded:
        return f"{where}: onCreate must end with {guarded}; found {create[-len(guarded):]}"
    if sum(any(report in line for report in reports) for line in create) != 2:
        return f"{where}: onCreate reports the launch intent only behind its fresh check"
    expected = [
        "IntentExtras.sanitize(intent, HostDiagnostics::submit)",
        "super.onNewIntent(intent)",
        "UsbSourceManager.reportAttachment(this, intent)",
        "IncomingDocuments.report(this, intent, HostDiagnostics::submit)",
    ]
    found = activity_statements(activity, "override fun onNewIntent(intent: Intent)")
    if found != expected:
        return f"{where}: onNewIntent must be exactly {expected}; found {found}"
    line = 'val line: String get() = "incoming document refused: action=$action reason=$reason"'
    if intake.count(line) != 1:
        return f"IncomingDocumentIntake.kt: the refusal record must be exactly {line}"
    refusals = re.findall(r"IntakeResult\.Refuse\(([^,()]+), ([A-Z_]+)\)", intake)
    reasons = dict(re.findall(r'const val ([A-Z_]+) = "(\w+)"', intake))
    allowed = {"action", "noUri", "manyUris", "scheme", "ownProvider", "noGrant"}
    if not refusals or any(kind not in ('"other"', "action") or reasons.get(reason) not in allowed for kind, reason in refusals):
        return f"IncomingDocumentIntake.kt: every refusal names a fixed kind and one of {sorted(allowed)}; found {refusals}"
    if not re.search(r'val action = when \(request\.action\) \{\s*VIEW -> "VIEW"\s*SEND -> "SEND"', intake):
        return "IncomingDocumentIntake.kt: a refusal's action is VIEW, SEND or other, never the intent's own text"
    if re.findall(r"\blog\(([^)]*)\)", documents) != ["result.line"]:
        return "IncomingDocuments.kt: the only record is the refusal's line, which names no URI or file"
    return None


def intent_extras_order():
    """#831 B6: Qt 6.11.3 reads the extras inside super.onCreate() and super.onNewIntent(), so the guard runs before
    each, on the intent Qt then reads, and records only the exception's class."""
    where = APP_SOURCES / "DsdNeoActivity.kt"
    try:
        activity = (REPO / where).read_text(encoding="utf-8")
        extras = (REPO / APP_SOURCES / "IntentExtras.kt").read_text(encoding="utf-8")
    except OSError as error:
        return f"cannot read the activity or IntentExtras: {error}"
    guard = "IntentExtras.sanitize(intent, HostDiagnostics::submit)"
    create = activity_statements(activity, "override fun onCreate(") or []
    if create.count(guard) != 1 or "super.onCreate(savedInstanceState)" not in create:
        return f"{where}: onCreate must call {guard} once"
    if create.index(guard) > create.index("super.onCreate(savedInstanceState)"):
        return f"{where}: in onCreate, {guard} must come before super.onCreate(savedInstanceState)"
    found = (activity_statements(activity, "override fun onNewIntent(intent: Intent)") or [])[:2]
    if found != [guard, "super.onNewIntent(intent)"]:
        return f"{where}: onNewIntent must start with {guard}, then super.onNewIntent(intent); found {found}"
    if extras.count('log("launch extras dropped: " + e.javaClass.simpleName)') != 1:
        return "IntentExtras.kt: the record names the exception's class only"
    return None


def file_browser_wiring():
    """#830: DsdNeoActivity never compiles here, so its forwarding is read from the source. Every resume tells
    FileBrowser (All files access and the volumes may have changed while the app was away), and every permission result
    goes to FileBrowser first: it claims the codes of its Android 10 storage requests, a late one included, and only one
    it does not claim goes on."""
    where = APP_SOURCES / "DsdNeoActivity.kt"
    try:
        activity = (REPO / where).read_text(encoding="utf-8")
    except OSError as error:
        return f"cannot read the activity: {error}"

    def statements(signature):
        body = method_body(activity, signature)
        return [line.strip() for line in (body or "").splitlines() if line.strip() and not line.strip().startswith("//")]

    if "FileBrowser.onResume()" not in statements("override fun onResume("):
        return f"{where}: onResume must call FileBrowser.onResume()"
    expected = [
        "if (FileBrowser.onRequestPermissionsResult(this, requestCode, permissions, grantResults)) {",
        "return",
        "}",
    ]
    found = statements("override fun onRequestPermissionsResult(")[: len(expected)]
    if found != expected:
        return f"{where}: onRequestPermissionsResult must start with {expected}; found {found}"
    return None


def location_wiring():
    """#786: DsdNeoActivity never compiles here, so its forwarding to the location feed and the prompt is read from the
    source. Every start lets the feed start again; every stop, a recreation's included, advances the foreground epoch and
    stops the feed, which Qt cannot do once its thread freezes; destroy stops it too; and location permission answers
    reach the broker that owns the prompt, after the file browser's and the notification's codes."""
    where = APP_SOURCES / "DsdNeoActivity.kt"
    try:
        activity = (REPO / where).read_text(encoding="utf-8")
    except OSError as error:
        return f"cannot read the activity: {error}"

    def statements(signature):
        body = method_body(activity, signature)
        return [line.strip() for line in (body or "").splitlines() if line.strip() and not line.strip().startswith("//")]

    required = (
        ("override fun onStart(", "LocationStream.onActivityStart()"),
        ("override fun onStart(", "LocationSupport.onActivityStart()"),
        ("override fun onDestroy(", "LocationStream.onDestroy()"),
        (
            "override fun onRequestPermissionsResult(",
            "if (!LocationPermission.onRequestPermissionsResult(this, requestCode, grantResults)) {",
        ),
    )
    for signature, statement in required:
        if statement not in statements(signature):
            return f"{where}: {signature.split('(')[0].split()[-1]} must call {statement}"
    stop = statements("override fun onStop(")
    if "LocationStream.onActivityStop()" not in stop:
        return f"{where}: onStop must call LocationStream.onActivityStop()"
    guarded = stop.index("if (!isChangingConfigurations) {") if "if (!isChangingConfigurations) {" in stop else len(stop)
    if stop.index("LocationStream.onActivityStop()") > guarded:
        return f"{where}: onStop must call LocationStream.onActivityStop() on every stop, outside the recreation guard"
    if "LocationSupport.onActivityStop()" not in stop or stop.index("LocationSupport.onActivityStop()") > guarded:
        return f"{where}: onStop must call LocationSupport.onActivityStop() on every stop, outside the recreation guard"
    if "LocationSupport.onRequestPermissionsResult(" in activity:
        return f"{where}: location permission answers go to LocationPermission, which owns the prompt"
    return None


USES_PERMISSION_TAG = re.compile(r"<uses-permission\b([^>]*)>")
PREFIXED_ATTRIBUTE = re.compile(r'\b(android|tools):(\w+)\s*=\s*"([^"]*)"')
STORAGE_PERMISSION = "android.permission.{}_EXTERNAL_STORAGE"


def declared_permissions(manifest):
    """Each <uses-permission> outside comments, by name: its prefixed attributes ("android:maxSdkVersion", ...)."""
    declared = {}
    for tag in USES_PERMISSION_TAG.finditer(manifest):
        attributes = {f"{prefix}:{name}": value for prefix, name, value in PREFIXED_ATTRIBUTE.findall(tag.group(1))}
        declared[attributes.get("android:name")] = attributes
    return declared


def storage_permissions_policy():
    """#830: every permission is declared in the app's own manifests, so Qt inserts none (Qt6Core's rules would add
    WRITE_EXTERNAL_STORAGE, which the Android 10 legacy view would grant with READ). Every build reads device storage on
    Android 10 only; All files access comes from the sideload overlay, which build.gradle merges into every build but a
    bundle, so the Play bundle never asks for it. CI checks the packaged artifacts the same way."""
    checks = (
        ("android/package/AndroidManifest.xml", main_manifest_permissions),
        ("android/package/sideload/AndroidManifest.xml", sideload_manifest_permissions),
        ("android/package/build.gradle", sideload_overlay_wiring),
    )
    for where, check in checks:
        try:
            text = (REPO / where).read_text(encoding="utf-8")
        except OSError as error:
            return f"cannot read {where}: {error}"
        problem = check(text)
        if problem:
            return f"{where}: {problem}"
    return None


def main_manifest_permissions(text):
    """What is wrong with the main manifest's permissions, or None."""
    if "%%INSERT_PERMISSIONS" in text:
        return "drop the %%INSERT_PERMISSIONS marker; the manifest declares every permission itself"
    manifest = XML_COMMENT.sub(" ", text)
    permissions = declared_permissions(manifest)
    for name in ("android.permission.INTERNET", "android.permission.ACCESS_NETWORK_STATE"):
        if name not in permissions:
            return f"declare {name} (Qt no longer inserts it)"
    read = permissions.get(STORAGE_PERMISSION.format("READ"), {})
    if read.get("android:maxSdkVersion") != "29":
        return f"declare {STORAGE_PERMISSION.format('READ')} with maxSdkVersion=\"29\"; found {read}"
    for name in ("MANAGE", "WRITE"):
        if STORAGE_PERMISSION.format(name) in manifest:
            return f"must not name {STORAGE_PERMISSION.format(name)}"
    applications = re.findall(r"<application\b([^>]*)>", manifest)
    if len(applications) != 1 or 'android:requestLegacyExternalStorage="true"' not in applications[0]:
        return '<application> must set android:requestLegacyExternalStorage="true"'
    return None


def sideload_manifest_permissions(text):
    """What is wrong with the sideload overlay's permissions, or None: All files access and nothing else."""
    overlay = declared_permissions(XML_COMMENT.sub(" ", text))
    wanted = {STORAGE_PERMISSION.format("MANAGE")}
    # Android 11-12 offer "Allow management of all files" in App info only to an app that asks for READ there.
    wanted.add(STORAGE_PERMISSION.format("READ"))
    read = overlay.get(STORAGE_PERMISSION.format("READ"), {})
    if read.get("android:maxSdkVersion") != "32" or read.get("tools:replace") != "android:maxSdkVersion":
        return (
            f"declare {STORAGE_PERMISSION.format('READ')} with maxSdkVersion=\"32\" and "
            f'tools:replace="android:maxSdkVersion"; found {read}'
        )
    if set(overlay) != wanted:
        return f"must declare exactly {sorted(wanted)}; found {sorted(overlay)}"
    return None


def sideload_overlay_wiring(gradle):
    """What is wrong with how build.gradle picks the sideload overlay, or None: one file-scope bundleBuild decides both
    the native library packaging and whether the two build types merge the overlay."""
    if len(re.findall(r"\bdef bundleBuild\b", gradle)) != 1 or not re.search(r"^def bundleBuild = ", gradle, re.M):
        return "define bundleBuild once, at file scope"
    if gradle.find("def bundleBuild") > gradle.find("\nandroid {"):
        return "define bundleBuild above android {"
    if not re.search(r"useLegacyPackaging = bundleBuild \?", gradle):
        return "useLegacyPackaging must follow bundleBuild"
    branch = re.search(r"if \(!bundleBuild\) \{([^{}]*)\}", gradle)
    overlays = re.findall(
        r"^\s*(\w+)\.manifest\.srcFile 'sideload/AndroidManifest\.xml'$", branch.group(1) if branch else "", re.M
    )
    if sorted(overlays) != ["debug", "release"] or gradle.count("sideload/AndroidManifest.xml") != 2:
        return (
            "under if (!bundleBuild), debug and release must each set manifest.srcFile 'sideload/AndroidManifest.xml', "
            f"and nothing else may name it; found {overlays}"
        )
    return None


def launch_record_plumbing(facts):
    """What is wrong with how ProcessFacts.kt carries LaunchGuard's record, or None: the summary is set on API 30+ and
    read back into each exit record. Nothing else exercises this path."""
    setter = method_body(facts, "fun setStateSummary(")
    if setter is None:
        return "no setStateSummary"
    gate = re.search(r"if \(Build\.VERSION\.SDK_INT < Build\.VERSION_CODES\.R\) \{\s*return\s*\}", setter)
    call = setter.find("setProcessStateSummary(summary)")
    if call < 0 or gate is None or gate.start() > call:
        return "setStateSummary must call setProcessStateSummary(summary) after returning below API 30 (R)"
    reader = method_body(facts, "fun exitLines(")
    if reader is None or not re.search(r"ExitFacts\.Record\([^)]*it\.processStateSummary", reader, re.S):
        return "exitLines must pass it.processStateSummary into each ExitFacts.Record"
    return None


# One left-to-right pass over C++ text: a string or character literal, a line comment or a block comment, whichever
# starts first. Separate passes would let a "//" or "/*" inside a literal hide the code after it, or a quote inside a
# comment hide a call.
CPP_LITERAL_OR_COMMENT = re.compile(
    r'"(?:[^"\\\n]|\\.)*"' + r"|'(?:[^'\\\n]|\\.)*'" + r"|//[^\n]*" + r"|/\*.*?\*/",
    re.S,
)
# Any environment writer, the project's wrappers (dsd_setenv, dsd_unsetenv) and Qt's (qputenv, qunsetenv) included,
# and the QML collector policy that writes one.
CPP_ENVIRONMENT_WRITER = re.compile(r"\b(\w*(?:setenv|unsetenv|putenv)|applyQmlGcPolicy)\s*\(")


def cpp_environment_writer(text):
    """The first environment-writing call in C++ `text`, outside literals and comments, or None."""
    code = CPP_LITERAL_OR_COMMENT.sub(lambda match: '""' if match.group(0)[0] in "\"'" else " ", text)
    writer = CPP_ENVIRONMENT_WRITER.search(code)
    return writer.group(1) if writer else None


def cpp_environment_writer_probes():
    """The scanner behind android_main_writes_no_environment, on the cases that once got past it (#782 T4 review)."""
    caught = (
        'dsd_setenv("X", "1", 1);',
        'dsd_unsetenv("X");',
        'qputenv("X", "1");',
        'qunsetenv("X");',
        '::setenv("X", "1", 1);',
        'putenv(buffer);',
        "dsd_qt::applyQmlGcPolicy();",
        'const char* u = "http://x"; qputenv("X", "1");',
        'const char* g = "*.cpp/*"; qputenv("X", "1"); /* end */',
        "char q = '\"'; qputenv(\"X\", \"1\");",
        "/* a comment */ setenv(\"X\", \"1\", 1);",
    )
    ignored = (
        '// qputenv("X", "1");',
        '/* dsd_setenv("X", "1", 1); */',
        'const char* s = "qputenv(";',
        'log("setenv(X) failed");',
        "char c = '('; int setenvironment_count = 0;",
        "int resetenv_count = 1; (void)resetenv_count;",
    )
    for text in caught:
        if not cpp_environment_writer(text):
            return f"the main.cpp environment scan misses a writer in: {text}"
    for text in ignored:
        found = cpp_environment_writer(text)
        if found:
            return f"the main.cpp environment scan reports {found}() in code that writes nothing: {text}"
    return None


def android_main_writes_no_environment():
    """#782: once super.onCreate() starts Qt's main(), nothing may add, remove or reallocate an environment entry, so
    android/main.cpp writes none, and QmlGcPolicy.kt sets the QML collector's variable, the one applyQmlGcPolicy()
    writes, before super.onCreate()."""
    main_path = REPO / "android/main.cpp"
    header_path = REPO / "src/ui/qt/qml_gc_policy.h"
    kotlin_path = REPO / APP_SOURCES / "QmlGcPolicy.kt"
    try:
        main = main_path.read_text(encoding="utf-8")
        header = header_path.read_text(encoding="utf-8")
        kotlin = kotlin_path.read_text(encoding="utf-8")
    except OSError as error:
        return f"cannot read the environment writers: {error}"
    # Code only: comments and string literals may name the calls.
    writer = cpp_environment_writer(main)
    if writer:
        return f"android/main.cpp: {writer}() writes the environment while the Android main thread reads it"
    native = re.search(r'qputenv\("([^"]+)",\s*QByteArrayLiteral\("([^"]*)"\)\)', header)
    jvm_name = re.search(r'const val VARIABLE = "([^"]+)"', kotlin)
    jvm_value = re.search(r'Os\.setenv\(VARIABLE, "([^"]*)", true\)', kotlin)
    if not native or not jvm_name or not jvm_value:
        return "cannot find the collector's variable in qml_gc_policy.h or QmlGcPolicy.kt"
    if (native.group(1), native.group(2)) != (jvm_name.group(1), jvm_value.group(1)):
        return (
            f"QmlGcPolicy.kt sets {jvm_name.group(1)}={jvm_value.group(1)}, but applyQmlGcPolicy() in "
            f"src/ui/qt/qml_gc_policy.h sets {native.group(1)}={native.group(2)}"
        )
    return None


# The vendored snapshots under android/third_party keep upstream code; the link check
# (cmake/check_no_environment_writers.cmake) covers what they import.
ANDROID_NATIVE_SUFFIXES = (".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp")
# The two Kotlin files that set Qt's own inputs, before super.onCreate() starts Qt (a11y_bridge_order pins the order).
KOTLIN_ENVIRONMENT_WRITERS = ("QmlGcPolicy.kt", "AccessibilityBridge.kt")
KOTLIN_ENVIRONMENT_WRITE = re.compile(r"\bOs\.(?:set|unset)env\s*\(")


def android_native_writes_no_environment():
    """#782: the app's native code runs on the Android main thread and the engine's threads while Qt's thread reads
    the environment, so no C or C++ source under android/ writes it: nativeInit sets host overrides in the engine's
    store (<dsd-neo/platform/env.h>) instead. Kotlin writes it only in the two files that set Qt's own inputs."""
    for probe, writes in (
        ('Os.setenv("X", "1", true)', True),
        ("android.system.Os.unsetenv(name)", True),
        ('val v = Os.getenv("X")', False),
        ("resetenvCount()", False),
    ):
        if bool(KOTLIN_ENVIRONMENT_WRITE.search(probe)) != writes:
            return f"the Kotlin environment scan gets {probe!r} wrong"
    root = REPO / "android"
    for path in sorted(root.rglob("*")):
        relative = path.relative_to(REPO)
        if "third_party" in relative.parts or not path.is_file():
            continue
        try:
            if path.suffix in ANDROID_NATIVE_SUFFIXES:
                writer = cpp_environment_writer(path.read_text(encoding="utf-8"))
                if writer:
                    return f"{relative}: {writer}() writes the process environment; set a dsd_env_set() override"
            elif path.suffix == ".kt" and path.name not in KOTLIN_ENVIRONMENT_WRITERS:
                code = CPP_LITERAL_OR_COMMENT.sub(
                    lambda match: '""' if match.group(0)[0] in "\"'" else " ", path.read_text(encoding="utf-8")
                )
                if KOTLIN_ENVIRONMENT_WRITE.search(code):
                    return f"{relative}: writes the process environment; only {KOTLIN_ENVIRONMENT_WRITERS} may"
        except OSError as error:
            return f"cannot read {relative}: {error}"
    return None


# The platform the document writes run on (DocumentCopies, RecordingExport) with the Download folder save they settle
# through (DownloadsSave): a resolver that plays the document providers and MediaStore.
DOWNLOADS_SAVE_STUBS = (
    "AssetFileDescriptor.kt",
    "ContentResolver.kt",
    "ContentValues.kt",
    "Cursor.kt",
    "GrantIntent.kt",
    "Log.kt",
    "MediaStore.kt",
    "Os.kt",
    "ParcelFileDescriptor.kt",
    "Provider.kt",
    "ShareEnvironment.kt",
    "SystemOs.kt",
    "RecordingNative.kt",
    "Uri.kt",
)

# The delivered intent, its extras and the lookups the incoming-document intake makes (#831).
INCOMING_INTENT_STUBS = (
    "IncomingIntent.kt",
    "IncomingIntentCompat.kt",
    "IncomingOs.kt",
    "IncomingPackageManager.kt",
    "IncomingUri.kt",
)

# Each build compiles its own file set on its own, so one build's stubs never stand in for platform classes another
# build's sources must not touch.
SUITES = {
    # The one-shot location requests and geocoder queue, the permission prompt they share with GPS roaming, and
    # roaming's location feed (#786), against deterministic platform stubs. The Activity's forwarding is read from source.
    "location": Suite(
        checks=(location_wiring,),
        builds=(
            Build(
                sources=(
                    "LocationSupport.kt",
                    "LocationPermission.kt",
                    "LocationStream.kt",
                    "ForegroundEpoch.kt",
                    "SettingsScreens.kt",
                ),
                tests=(
                    "LocationGeocodeQueueTest.kt",
                    "LocationSupportTest.kt",
                    "LocationPermissionTest.kt",
                    "LocationStreamTest.kt",
                ),
                stubs=(
                    "Activity.kt",
                    "Context.kt",
                    "JSONObject.kt",
                    "Location.kt",
                    "Manifest.kt",
                    "Os.kt",
                    "PackageManager.kt",
                    "Settings.kt",
                    "Uri.kt",
                ),
            ),
        ),
    ),
    # The notification status record's reader. Plain Kotlin with no Android imports, so no stubs.
    "decoder_status": Suite(
        builds=(Build(sources=("DecoderStatus.kt",), tests=("DecoderStatusTest.kt",)),),
        checks=(golden_records_match,),
    ),
    # The backup's document transfers. The bounded stream copy and its teardown abort are plain Kotlin and build with no
    # stubs, so an android.* import there fails the compile. The Android glue around it (size lookup, opens, trim, sync,
    # delete and cancellation, and the import probes, #831) builds apart, against a resolver stub that plays the
    # document providers. The probe's copies of the backup magic and header size are checked against backup_seal.
    "document_transfer": Suite(
        checks=(document_copy_codes_match, backup_probe_constants_match),
        builds=(
            Build(sources=("DocumentTransfer.kt",), tests=("DocumentTransferTest.kt",)),
            Build(
                sources=("DocumentCopies.kt", "DocumentTransfer.kt", "DownloadsSave.kt"),
                tests=("DocumentCopiesTest.kt",),
                stubs=DOWNLOADS_SAVE_STUBS,
            ),
        ),
    ),
    # The app's document picker (#829): the request core (launch order, request codes, results, deadlines, supersede
    # and cancel) against a fake activity, and the Intents, handler queries and name lookup it makes, against stubs of
    # the activity, intents, package manager and resolver. The activity's forwarding and the manifest's <queries> are
    # read from the sources.
    "document_picker": Suite(
        checks=(document_picker_wiring,),
        builds=(
            Build(
                sources=("DocumentPicker.kt", "DownloadsSave.kt", "DocumentTransfer.kt"),
                tests=("DocumentPickerTest.kt",),
                stubs=(
                    "AssetFileDescriptor.kt",
                    "ContentResolver.kt",
                    "ContentValues.kt",
                    "Cursor.kt",
                    "JSONObject.kt",
                    "Log.kt",
                    "MediaStore.kt",
                    "Os.kt",
                    "ParcelFileDescriptor.kt",
                    "PickerActivity.kt",
                    "PickerContext.kt",
                    "PickerPackageManager.kt",
                    "Provider.kt",
                    "ShareEnvironment.kt",
                    "Uri.kt",
                ),
            ),
        ),
    ),
    # Documents other apps open with or share to DSD-neo (#831): the intake gate and the launch delivery rule in plain
    # Kotlin with no stubs, then IncomingDocuments, which reads the delivered intent, makes the two lookups and keeps the
    # mailbox, against stubs of the intent, its extras, the package manager, the grant check and IntentCompat. The
    # manifest's two filters and the activity's guarded reports are read from the sources.
    "incoming_document": Suite(
        checks=(incoming_document_filters, incoming_document_reports),
        builds=(
            Build(sources=("IncomingDocumentIntake.kt", "LaunchDelivery.kt"), tests=("IncomingDocumentIntakeTest.kt",)),
            Build(
                sources=("IncomingDocuments.kt", "IncomingDocumentIntake.kt"),
                tests=("IncomingDocumentsTest.kt",),
                stubs=INCOMING_INTENT_STUBS,
            ),
        ),
    ),
    # The guard that drops unreadable launch extras before Qt reads them (#831 B6), against the same intent stubs, whose
    # extras unparcel as API 29-32's do. The activity's call order is read from the source.
    "intent_extras": Suite(
        checks=(intent_extras_order,),
        builds=(Build(sources=("IntentExtras.kt",), tests=("IntentExtrasTest.kt",), stubs=INCOMING_INTENT_STUBS),),
    ),
    # The Download folder save (#829): pending rows, their publication or delete at every exit of the backup's save and
    # the recording transfers, the discards, and the release of leftover persisted grants, against a MediaStore stub.
    "downloads_save": Suite(
        builds=(
            Build(
                sources=(
                    "DownloadsSave.kt",
                    "DocumentCopies.kt",
                    "DocumentTransfer.kt",
                    "RecordingExport.kt",
                    "RecordingDescriptor.kt",
                ),
                tests=("DownloadsSaveTest.kt",),
                stubs=DOWNLOADS_SAVE_STUBS,
            ),
        ),
    ),
    # The call recording player's state machine (#708, #909) and the playlist service's order (#909): which play the
    # service serves, what each delivery does, when it retires and the teardown by phase. Plain Kotlin with no Android
    # imports, so no stubs; RecordingPlayback runs the machine with a MediaPlayer and the audio focus, and
    # RecordingPlaybackService carries out the phases.
    "recording_player": Suite(
        checks=(
            playback_states_match,
            playback_modes_match,
            playback_codes_match,
            playback_record_fields_match,
            playback_requests_checked_before_begin,
            playback_scoped_commands_check_first,
            playback_looper_reads_no_main_service,
        ),
        builds=(
            Build(sources=("RecordingPlayer.kt",), tests=("RecordingPlayerTest.kt",)),
            Build(
                sources=("PlaybackServicePhases.kt", "RecordingPlayer.kt", "MediaKeys.kt"),
                tests=("PlaybackServicePhasesTest.kt",),
            ),
            # The media keys the Activity hands past Qt to the playlist's media session (#909).
            Build(sources=("MediaKeys.kt",), tests=("MediaKeysTest.kt",)),
            # A player's owner thread: one platform seek in flight (R59), and silence only once the audio stopped (R62).
            Build(sources=("PlayerOwnerState.kt",), tests=("PlayerOwnerStateTest.kt",)),
        ),
    ),
    # Call recording export and save (#708): the ZIP writer and the transfer into a document, through DocumentCopies'
    # managed write, against the same resolver stubs as the backup's copies.
    "recording_export": Suite(
        checks=(recording_transfer_codes_match,),
        builds=(
            Build(
                sources=(
                    "RecordingExport.kt",
                    "RecordingDescriptor.kt",
                    "DocumentCopies.kt",
                    "DocumentTransfer.kt",
                    "DownloadsSave.kt",
                ),
                tests=("RecordingExportTest.kt",),
                stubs=DOWNLOADS_SAVE_STUBS,
            ),
        ),
    ),
    # Where call recordings live and sharing one (#708): the roots, the path checks and the share's intent, against
    # stubs of the context, intents and FileProvider; the provider's declaration is read from the sources.
    "recording_share": Suite(
        checks=(recordings_provider_contract,),
        builds=(
            Build(
                sources=("RecordingShare.kt",),
                tests=("RecordingShareTest.kt",),
                stubs=("ShareContext.kt", "ShareEnvironment.kt", "ShareFileProvider.kt", "Uri.kt", "Log.kt"),
            ),
            # The diagnostics share's copies (issue #800, D10): cleaned at app start and at each share.
            Build(
                sources=("DiagnosticsShare.kt",),
                tests=("DiagnosticsShareTest.kt",),
                stubs=("ShareContext.kt", "ShareFileProvider.kt", "Uri.kt", "DiagnosticsNative.kt"),
            ),
        ),
    ),
    # Which receiver kind is attached, from the USB ids on the bus. Plain Kotlin with no Android imports, so no stubs.
    # Which receiver a start's arguments name, read with copies of the engine's option tables, which the check compares.
    "usb_receiver": Suite(
        builds=(Build(sources=("UsbReceiverKind.kt",), tests=("UsbReceiverKindTest.kt",)),),
        checks=(receiver_option_tables_match,),
    ),
    # The USB broker's permission requests, detach and attach bookkeeping, the attachment's kind and the USB resets
    # attachments owe: the plain-Kotlin state with no stubs, then UsbSourceManager itself against stubs of the USB
    # service, prompts and engine.
    "usb": Suite(
        builds=(
            Build(
                sources=("UsbRequestState.kt", "UsbAttachmentTracker.kt", "UsbReceiverKind.kt", "UsbResetObligations.kt"),
                tests=("UsbRequestStateTest.kt", "UsbResetObligationsTest.kt"),
            ),
            # The production manager itself, against a scripted bus, prompt and engine.
            Build(
                sources=(
                    "UsbSourceManager.kt",
                    "UsbRequestState.kt",
                    "UsbAttachmentTracker.kt",
                    "UsbReceiverKind.kt",
                    "UsbResetObligations.kt",
                    "UsbSessionTake.kt",
                    "EngineStop.kt",
                ),
                tests=("UsbSourceManagerTest.kt",),
                stubs=(
                    "UsbContext.kt",
                    "UsbHardware.kt",
                    "UsbPendingIntent.kt",
                    "UsbOs.kt",
                    "UsbNative.kt",
                    "Log.kt",
                    "JSONObject.kt",
                ),
            ),
        ),
        checks=(usb_broker_contract,),
    ),
    # How Android says recent app processes ended, as diagnostics lines. Plain Kotlin with no Android imports, so no
    # stubs; ProcessFacts reads the records off ActivityManager. LaunchGuard names the state summary it knows.
    "exit_facts": Suite(builds=(Build(sources=("ExitFacts.kt", "LaunchGuard.kt"), tests=("ExitFactsTest.kt",)),)),
    # The reason mapping builds without Android; the actual service also runs against a deferred-intent transport.
    "engine_stop": Suite(
        builds=(
            Build(sources=("EngineStop.kt",), tests=("EngineStopTest.kt",)),
            Build(
                # UsbSourceManager is the stub in ServiceNative.kt: the service's take of its receiver is scripted.
                sources=(
                    "EngineStop.kt",
                    "DecoderService.kt",
                    "DecoderStatus.kt",
                    "UsbReceiverKind.kt",
                    "UsbSessionTake.kt",
                    "ForegroundEpoch.kt",
                ),
                tests=("DecoderServiceStopTest.kt", "DecoderServiceAttemptTest.kt", "DecoderServiceUsbTest.kt"),
                stubs=("Service.kt", "ServiceContext.kt", "ServicePackageManager.kt", "ServiceNative.kt",
                       "Os.kt", "PowerManager.kt", "Log.kt", "JSONObject.kt", "SuppressLint.kt"),
            ),
        ),
        checks=(
            stop_callers_carry_their_cause,
            native_lifecycle_contract,
            start_status_contract,
            counter_reader_contract,
        ),
    ),
    # The screen policy and the pure adapters around it compile with no stubs at all, so an android.* import in any of
    # them fails the build. ScreenLocks, the one wrapper over the platform's wake locks, builds apart, with the pure
    # ScreenWake it answers with, against a PowerManager stub that models reference counting. The screen trace (#782)
    # builds with the policy, whose inputs it records.
    "screen": Suite(
        builds=(
            Build(
                sources=(
                    "ScreenPolicy.kt",
                    "ScreenController.kt",
                    "ScreenWake.kt",
                    "StatusFeed.kt",
                    "ActivitySlot.kt",
                    "DecoderStatus.kt",
                    "ScreenTrace.kt",
                    "HostRecordText.kt",
                    "ActivityTrace.kt",
                ),
                tests=(
                    "ScreenPolicyTest.kt",
                    "ScreenControllerTest.kt",
                    "ScreenTouchTest.kt",
                    "StatusFeedTest.kt",
                    "ActivitySlotTest.kt",
                    "ScreenTraceTest.kt",
                ),
            ),
            Build(
                sources=("ScreenLocks.kt", "ScreenWake.kt"),
                tests=("ScreenLocksTest.kt",),
                stubs=("PowerManager.kt",),
            ),
        ),
        checks=(screen_golden_matches,),
    ),
    # Qt's accessibility bridge (#782): the gate's decisions in plain Kotlin with no stubs, then AccessibilityBridge and
    # QmlGcPolicy against stubs of the accessibility manager (listeners fired in AOSP's order) and of the process
    # environment, with Qt's own listener modelled in the test. Both are process state, so each test file runs in its
    # own JVM. The checks pin the environment writes before super.onCreate(), none from android/main.cpp, and none from
    # any other native source or Kotlin file of the app. The launch policy goes with it: the manifest that keeps a second
    # activity instance out of the process (launch_policy), and LaunchGuard's record of one, with the exit line that
    # prints it, in the first build.
    "a11y_bridge": Suite(
        builds=(
            Build(
                sources=("AccessibilityGate.kt", "ActivitySlot.kt", "LaunchGuard.kt", "ExitFacts.kt"),
                tests=("AccessibilityGateTest.kt", "LaunchGuardTest.kt", "ExitFactsTest.kt"),
            ),
            Build(
                sources=("AccessibilityGate.kt", "ActivitySlot.kt", "AccessibilityBridge.kt", "QmlGcPolicy.kt"),
                tests=(
                    "AccessibilityBridgeTest.kt",
                    "AccessibilityBridgeLaunchTest.kt",
                    "AccessibilityBridgeErrnoTest.kt",
                    "AccessibilityBridgeErrnoOffTest.kt",
                    "AccessibilityBridgeLeftoverTest.kt",
                    "QmlGcPolicyTest.kt",
                ),
                stubs=("AccessibilityManager.kt", "AccessibilityServiceInfo.kt", "SystemOsEnv.kt", "A11yContext.kt", "A11yActivity.kt"),
            ),
        ),
        checks=(
            a11y_bridge_order,
            launch_policy,
            cpp_environment_writer_probes,
            android_main_writes_no_environment,
            android_native_writes_no_environment,
        ),
    ),
    # The in-app file browser (#830). Its listing core first: folder reads behind a filesystem seam, containment,
    # filters, order, pages, snapshots, the worker cap and watchdog, in plain Kotlin with no Android imports, so no stubs;
    # real fixtures (links, FIFOs, sockets, mode 000) run where the host has them. Then FileBrowser on it: roots and their
    # states, Saved by DSD-neo through MediaStore, the All files access chain and the Android 10 permission, against a
    # fake platform and stubs of the activity, storage volumes, resolver and Settings. The activity's forwarding is read
    # from the source.
    "file_browser": Suite(
        checks=(file_browser_wiring, storage_permissions_policy),
        builds=(
            Build(sources=("FileListing.kt",), tests=("FileListingTest.kt",), in_folder=("FileListingTest.kt",)),
            Build(
                sources=("FileListing.kt", "FileBrowser.kt", "SettingsScreens.kt"),
                tests=("FileBrowserTest.kt",),
                stubs=(
                    "AssetFileDescriptor.kt",
                    "ContentResolver.kt",
                    "ContentUris.kt",
                    "ContentValues.kt",
                    "Cursor.kt",
                    "FileBrowserStorage.kt",
                    "JSONObject.kt",
                    "Manifest.kt",
                    "MediaStore.kt",
                    "Os.kt",
                    "ParcelFileDescriptor.kt",
                    "PickerActivity.kt",
                    "PickerContext.kt",
                    "PickerPackageManager.kt",
                    "Settings.kt",
                    "ShareEnvironment.kt",
                    "Uri.kt",
                ),
            ),
        ),
    ),
    # The RadioReference sign-in form as the Autofill Framework sees it: tokens, the field snapshot, the framework
    # session and the effects SignInAutofill carries out. Plain Kotlin with no Android imports, so no stubs;
    # SignInAutofill runs it on the main looper against AutofillManager.
    "sign_in_autofill": Suite(
        builds=(Build(sources=("SignInAutofillState.kt",), tests=("SignInAutofillStateTest.kt",)),),
    ),
    # The main-looper watchdog (#782), the activity trace it reads and the records waiting for the native library:
    # plain Kotlin with no stubs, against a fake clock, watchdog thread and main looper. MainLooperWatchdog and
    # HostDiagnostics, the Android glue around them, are not built here.
    "watchdog": Suite(
        builds=(
            Build(
                sources=("LooperWatchdog.kt", "ActivityTrace.kt", "PendingHostRecords.kt", "HostRecordText.kt"),
                tests=("LooperWatchdogTest.kt", "ActivityTraceTest.kt", "PendingHostRecordsTest.kt"),
            ),
        ),
        checks=(watchdog_golden_matches,),
    ),
}


def cached_compiler():
    cache = Path(os.environ.get("GRADLE_USER_HOME", Path.home() / ".gradle")) / "caches/modules-2/files-2.1"
    packages = (
        "org.jetbrains.kotlin/kotlin-compiler-embeddable",
        "org.jetbrains.kotlin/kotlin-stdlib",
        "org.jetbrains.kotlin/kotlin-script-runtime",
        "org.jetbrains.kotlin/kotlin-reflect",
        "org.jetbrains.kotlinx/kotlinx-coroutines-core-jvm",
        "org.jetbrains/annotations",
    )
    jars = []
    for package in packages:
        candidates = sorted((cache / package).glob("*/*/*.jar"))
        if not candidates:
            return None
        # Absolute: the tests run in a working directory of their own, where a relative GRADLE_USER_HOME means nothing.
        jars.append(candidates[-1].resolve())
    return jars


def jvm_option_words(text, quotes="\"'"):
    """@p text split into arguments as HotSpot splits JAVA_TOOL_OPTIONS (and the launcher JDK_JAVA_OPTIONS): at
    whitespace outside quotes, a run in one of @p quotes taken as it is and its quotes dropped, anywhere in a word
    (-XX:+"UseG1GC" is -XX:+UseG1GC; -Dname="-XX:+UseG1GC" is one property)."""
    words, word, quote, quoted = [], [], None, False
    for char in text:
        if quote:
            if char == quote:
                quote = None
            else:
                word.append(char)
        elif char in quotes:
            quote, quoted = char, True
        elif char.isspace():
            if word or quoted:
                words.append("".join(word))
            word, quoted = [], False
        else:
            word.append(char)
    if word or quoted:
        words.append("".join(word))
    return words


def selects_collector(words):
    """Whether the JVM arguments @p words select a garbage collector: one whole argument -XX:+Use...GC."""
    return any(COLLECTOR_SELECTION.fullmatch(word) for word in words)


def java_opts_words(text, kotlinc):
    """@p text, JAVA_OPTS, split as the @p kotlinc launcher puts it on the java command line: bin/kotlinc expands it
    unquoted, so the shell splits it at whitespace and its quotes are literal; kotlinc.bat hands it to java.exe, whose
    command line takes double quotes only."""
    if str(kotlinc).lower().endswith(".bat"):
        return jvm_option_words(text, quotes='"')
    return text.split()


def names_argument_file(words):
    """Whether the java launcher arguments @p words name an argument file (@path), which the launcher expands in their
    place on its command line and in JDK_JAVA_OPTIONS ("@@" is a literal "@"). The JVM-wide JAVA_TOOL_OPTIONS and
    _JAVA_OPTIONS expand none."""
    return any(word.startswith("@") and not word.startswith("@@") for word in words)


def names_options_file(words):
    """Whether the JVM arguments @p words name a file of HotSpot options: -XX:Flags= (its lines +Name, -Name or
    Name=value) or -XX:VMOptionsFile= (options as on the command line). HotSpot reads either wherever it takes
    options, JAVA_TOOL_OPTIONS and _JAVA_OPTIONS included."""
    return any(word.startswith(("-XX:Flags=", "-XX:VMOptionsFile=")) for word in words)


def jvm_options(environ, given=()):
    """JVM_OPTIONS for a JVM started with the arguments @p given under @p environ, without the serial collector when
    @p given or the JVM-wide options in @p environ select a collector already, or may: an argument file the launcher
    expands there (in @p given, or in JDK_JAVA_OPTIONS), or a HotSpot options file named in any of them, can select
    one, and is not read here."""
    wide = {name: jvm_option_words(environ.get(name, "")) for name in JVM_WIDE_OPTIONS}
    sources = [list(given), *wide.values()]
    selected = any(selects_collector(words) or names_options_file(words) for words in sources)
    if selected or names_argument_file(given) or names_argument_file(wide["JDK_JAVA_OPTIONS"]):
        return tuple(option for option in JVM_OPTIONS if not selects_collector([option]))
    return JVM_OPTIONS


def kotlinc_environment(environ, kotlinc):
    """@p environ with jvm_options() in JAVA_OPTS, for the @p kotlinc launcher: after the options already set there, or
    after the launchers' default when none are."""
    given = environ.get("JAVA_OPTS", "").strip()
    words = java_opts_words(given, kotlinc)
    return {**environ, "JAVA_OPTS": " ".join((given or KOTLINC_DEFAULT_JAVA_OPTS, *jvm_options(environ, words)))}


def compile_command(java, kotlinc, cached, files, output, environ, tmpdir=None):
    """The compile of @p files into the jar @p output: its argv, its environment and the classpath the tests run on. With
    kotlinc, the JVM options go through JAVA_OPTS and no argument carries one; with the cached compiler, java takes them
    as arguments itself. With @p tmpdir, the compiler JVM's java.io.tmpdir is there (it unpacks native libraries into
    it): an argument to the cached compiler, and for kotlinc, whose POSIX launcher splits JAVA_OPTS at every space,
    a quoted word appended to JAVA_TOOL_OPTIONS."""
    if kotlinc:
        argv = [kotlinc, "-include-runtime", *map(str, files), "-d", str(output)]
        env = kotlinc_environment(environ, kotlinc)
        if tmpdir is not None:
            temp = quote_jvm_word(f"-Djava.io.tmpdir={tmpdir}")
            if temp is None:
                raise ValueError(f"the compiler's temp folder {tmpdir} holds both quote characters")
            # With the no-perf flag too: the POSIX launcher first starts `java -version` without JAVA_OPTS.
            given = environ.get("JAVA_TOOL_OPTIONS", "").strip()
            env["JAVA_TOOL_OPTIONS"] = " ".join(word for word in (given, "-XX:-UsePerfData", temp) if word)
        return argv, env, str(output)
    classpath = os.pathsep.join(map(str, cached))
    temp = [f"-Djava.io.tmpdir={tmpdir}"] if tmpdir is not None else []
    argv = [
        java, *jvm_options(environ), *temp, "-cp", classpath, "org.jetbrains.kotlin.cli.jvm.K2JVMCompiler",
        "-no-stdlib", "-no-reflect", "-classpath", classpath, *map(str, files), "-d", str(output),
    ]
    return argv, dict(environ), os.pathsep.join((str(output), classpath))


def test_command(java, runtime, test, environ, tmpdir=None):
    """The run of the compiled @p test (a .kt file name) on the classpath @p runtime, under @p environ, with its JVM's
    temp folder (java.io.tmpdir) at @p tmpdir when given. The JVM reads no TMPDIR: on Linux it uses /tmp, whatever
    the environment says."""
    temp = [f"-Djava.io.tmpdir={tmpdir}"] if tmpdir is not None else []
    # Top-level functions in Foo.kt compile into the class FooKt.
    return [java, *jvm_options(environ), *temp, "-cp", runtime, f"{PACKAGE}.{Path(test).stem}Kt"]


def temp_environment(environ, tmpdir):
    """@p environ with the native temp folder (TMPDIR, TEMP, TMP) at @p tmpdir, for what a JVM writes outside
    java.io.tmpdir."""
    return {**environ, "TMPDIR": str(tmpdir), "TEMP": str(tmpdir), "TMP": str(tmpdir)}


def quote_jvm_word(word):
    """@p word as one argument in a JVM-wide option variable, which HotSpot splits at whitespace outside quotes
    (jvm_option_words()); None when it holds both kinds of quote and so cannot be written there."""
    if not any(char.isspace() or char in "\"'" for char in word):
        return word
    if '"' not in word:
        return f'"{word}"'
    if "'" not in word:
        return f"'{word}'"
    return None


def jvm_isolation(java, options, environ, cwd):
    """What a JVM started as @p java @p options under @p environ in @p cwd ends up with: its java.io.tmpdir and
    whether it writes shared performance data. _JAVA_OPTIONS comes after the command line, and an options file named
    there can say anything, so only the JVM can tell. Raises RuntimeError, with what the JVM printed, when it does not
    start or does not say."""
    # -XX:+PerfDisableSharedMem last: the probe itself must not publish performance data, even when the caller turned
    # UsePerfData on, which it still reports.
    probe = subprocess.run([java, *options, "-XshowSettings:properties", "-XX:+PrintFlagsFinal", "-XX:+PerfDisableSharedMem",
                            "-version"], env=environ, cwd=cwd, capture_output=True, text=True, timeout=60)
    settings = isolation_settings(probe.stdout + probe.stderr) if probe.returncode == 0 else None
    if settings is None:
        # Not the options or the JVM's output, which can hold the caller's secrets: only what says why it stopped.
        reasons = [launch_error(line) for line in probe.stderr.splitlines() if LAUNCH_ERROR.match(line.strip())]
        raise RuntimeError(f"{java} did not start or did not report its settings (exit {probe.returncode})"
                           + "".join(f"\n  {reason}" for reason in reasons[:5]))
    return settings


# The lines a JVM that does not start prints about why.
LAUNCH_ERROR = re.compile(r"(Error|Unrecognized|Improperly specified|Could not|Invalid|Missing)\b")


def launch_error(line):
    """@p line, a JVM's launch error, without any value it quotes after an '=' (an option's value can be secret)."""
    return re.sub(r"=\S*", "=...", line.strip())


def isolation_settings(output):
    """(java.io.tmpdir, UsePerfData) from what -XX:+PrintFlagsFinal and -XshowSettings:properties print, on either
    stream (-XX:+DisplayVMOutputToStderr moves the flags); None when either is missing."""
    tmpdir = tmpdir_setting(output)
    perf = re.search(r"\bbool\s+UsePerfData\s*=\s*(true|false)\b", output)
    if tmpdir is None or perf is None:
        return None
    return tmpdir, perf.group(1) == "true"


def require_isolation(who, java, options, environ, cwd, tmpdir):
    """Refuse to run @p who unless its JVM's java.io.tmpdir is @p tmpdir and it writes no shared performance data,
    whatever the caller's JVM options say: a file it wrote elsewhere would escape every check."""
    seen, perf = jvm_isolation(java, options, environ, cwd)
    if seen != str(tmpdir):
        raise RuntimeError(f"{who}: the JVM's java.io.tmpdir would be {seen!r}, not {str(tmpdir)!r}; unset what sets it "
                           "(_JAVA_OPTIONS, or an options file it names)")
    if perf:
        raise RuntimeError(f"{who}: the JVM would write shared performance data (hsperfdata) to the system temp "
                           "folder; unset what turns UsePerfData back on")


def tmpdir_setting(settings):
    """java.io.tmpdir in @p settings, what -XshowSettings:properties prints; None when it is not there."""
    for line in settings.splitlines():
        key, sep, value = line.strip().partition(" = ")
        if sep and key == "java.io.tmpdir":
            return value
    return None


def leftovers(folder):
    """What is left in @p folder, by name, sorted."""
    return sorted(entry.name for entry in Path(folder).iterdir())


def run_test(java, runtime, test, directory, in_folder=False):
    """Run the compiled @p test with a temp folder of its own inside @p directory as its java.io.tmpdir and native temp
    folder (and, with @p in_folder, its working directory). Each test must remove what it creates there: what is left
    fails the run, named, whether the test passed or not, and is removed."""
    jvm_tmp = Path(tempfile.mkdtemp(prefix="t", dir=directory))
    env = temp_environment(os.environ, jvm_tmp)
    cwd = test_directory(in_folder, env, jvm_tmp)
    if in_folder and cwd is None:
        print(f"{test}: runs in the caller's working directory, since the JVM options name a relative file",
              file=sys.stderr)
    try:
        require_isolation(test, java, [*jvm_options(env), f"-Djava.io.tmpdir={jvm_tmp}"], env, cwd, jvm_tmp)
        failure = None
        try:
            subprocess.run(test_command(java, runtime, test, env, jvm_tmp), env=env, cwd=cwd, check=True, timeout=30)
        except (subprocess.CalledProcessError, subprocess.TimeoutExpired) as error:
            failure = error
        left = leftovers(jvm_tmp)
    finally:
        shutil.rmtree(jvm_tmp, ignore_errors=True)
    if left:
        print(f"{test} left {len(left)} entries in its temp folder: {', '.join(left[:20])}", file=sys.stderr)
    if failure is not None:
        raise failure
    if left:
        raise RuntimeError(f"{test} left files in its temp folder")


# The arguments in a JVM-wide option variable that name a file: the launcher's @argfiles and HotSpot's options files.
JVM_FILE_ARGUMENTS = ("-XX:VMOptionsFile=", "-XX:Flags=")


def names_relative_file(environ):
    """Whether a JVM-wide option variable in @p environ names a file by a relative path, which a JVM started in another
    working directory would not find."""
    for name in JVM_WIDE_OPTIONS:
        for word in jvm_option_words(environ.get(name, "")):
            prefix = next((p for p in JVM_FILE_ARGUMENTS if word.startswith(p)), None)
            if prefix is None and word.startswith("@") and not word.startswith("@@"):
                prefix = "@"
            if prefix is not None and not Path(word[len(prefix):]).is_absolute():
                return True
    return False


def test_directory(in_folder, environ, jvm_tmp):
    """The working directory for a test: its temp folder @p jvm_tmp for one that binds a socket by a relative name
    (@p in_folder), unless the caller's JVM options name a relative file, which would not be found there; then, as for
    every other test, the caller's own (None)."""
    if in_folder and not names_relative_file(environ):
        return jvm_tmp
    return None


def kotlinc_java(environ, java):
    """The java the kotlinc launchers start: JAVACMD, else JAVA_HOME's, else @p java (the one on PATH)."""
    if environ.get("JAVACMD"):
        return environ["JAVACMD"]
    home = environ.get("JAVA_HOME")
    if home:
        return str(Path(home) / "bin" / ("java.exe" if os.name == "nt" else "java"))
    return java


def compiler_isolation(java, kotlinc, argv, env, directory):
    """Refuse to compile unless the compiler JVM's java.io.tmpdir is @p directory and it writes no shared performance
    data, as the JVM itself reports them: for kotlinc, the java it starts with JAVA_OPTS on its command line after
    JAVA_TOOL_OPTIONS."""
    if kotlinc:
        words = java_opts_words(env.get("JAVA_OPTS", ""), kotlinc)
        require_isolation("the Kotlin compiler", kotlinc_java(env, java), words, env, None, directory)
    else:
        require_isolation("the Kotlin compiler", java, argv[1:argv.index("-cp")], env, None, directory)


def run_build(name, build, java, kotlinc, cached):
    app = REPO / APP_SOURCES
    files = [
        *(app / source for source in build.sources),
        *(HERE / test for test in build.tests),
        *(HERE / "stubs" / stub for stub in build.stubs),
    ]
    with tempfile.TemporaryDirectory(prefix=f"dsd-{name}-tests-") as directory:
        output = Path(directory) / "tests.jar"
        argv, env, runtime = compile_command(java, kotlinc, cached, files, output,
                                             temp_environment(os.environ, directory), directory)
        compiler_isolation(java, kotlinc, argv, env, directory)
        subprocess.run(argv, env=env, check=True)
        for test in build.tests:
            run_test(java, runtime, test, directory, test in build.in_folder)


def run_suite(name, suite, java, kotlinc, cached):
    for build in suite.builds:
        run_build(name, build, java, kotlinc, cached)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--require-tools", action="store_true")
    parser.add_argument(
        "--suite",
        action="append",
        choices=list(SUITES),
        dest="suites",
        help="run this suite; repeat for several (default: all)",
    )
    args = parser.parse_args()
    selected = list(dict.fromkeys(args.suites or SUITES))
    # Before the tool lookup, so a failed source check fails the run even where the compiler is missing.
    for name in selected:
        for check in SUITES[name].checks:
            problem = check()
            if problem:
                print(f"{name}: {problem}", file=sys.stderr)
                return 1
    # CI jobs that run the suite set DSD_NEO_REQUIRE_TEST_TOOLS=1, so a missing compiler fails there instead of skipping.
    require_tools = args.require_tools or os.environ.get("DSD_NEO_REQUIRE_TEST_TOOLS") == "1"
    # Absolute: FileListingTest runs in a working directory of its own, where a relative PATH entry means nothing.
    java = shutil.which("java")
    java = os.path.abspath(java) if java else None
    kotlinc = shutil.which("kotlinc")
    kotlinc = os.path.abspath(kotlinc) if kotlinc else None
    cached = cached_compiler() if not kotlinc else None
    if not java or (not kotlinc and not cached):
        print("Android JVM tests need Java and kotlinc, or the Kotlin compiler in the Gradle cache.")
        return 1 if require_tools else 77
    for name in selected:
        run_suite(name, SUITES[name], java, kotlinc, cached)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
