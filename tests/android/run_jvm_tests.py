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
    """The C encoder test and DecoderStatusTest.kt pin the same v3 record; a drift would let each side pass alone."""
    c_test = REPO / "tests/ui/test_app_control_notification_status.c"
    kotlin_test = HERE / "DecoderStatusTest.kt"
    try:
        encoded = literal_after(c_test, "#define NOTIFICATION_GOLDEN_V3_RECORD", ("\\",))
        parsed = literal_after(kotlin_test, "private const val GOLDEN_V3 =", ("=", "+"))
    except (OSError, ValueError) as error:
        return f"cannot read the golden v3 records: {error}"
    if encoded == parsed:
        return None
    left, right = encoded.split("\t"), parsed.split("\t")
    field = next((i for i, (a, b) in enumerate(zip(left, right)) if a != b), min(len(left), len(right)))
    first = left[field] if field < len(left) else "<none>"
    second = right[field] if field < len(right) else "<none>"
    return (
        f"NOTIFICATION_GOLDEN_V3_RECORD in {c_test.relative_to(REPO)} and GOLDEN_V3 in "
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


def kotlin_codes(path, names):
    """The `const val NAME = <int>` values of `names` in the Kotlin source at `path`."""
    text = path.read_text(encoding="utf-8")
    return {name: int(value) for name, value in re.findall(r"const val ([A-Z_]+) = (\d+)", text) if name in names}


def native_enum_matches(enum_name, prefix, source, names):
    """Whether DecoderHost's `enum_name` (members `prefix<Name>`) and the Kotlin `source`'s `names` agree."""
    header = REPO / "src/ui/qt/decoder_host.h"
    try:
        enum = re.search(rf"enum\s+{enum_name}\s*\{{([^}}]*)\}}", header.read_text(encoding="utf-8"))
        jvm = kotlin_codes(REPO / APP_SOURCES / source, names)
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
    names = ("DONE", "FAILED", "CANCELLED", "MISSING")
    return native_enum_matches("RecordingTransferResult", "Transfer", "RecordingExport.kt", names)


def playback_states_match():
    """#708: the call recording player's state crosses JNI as a plain int."""
    names = ("STOPPED", "PREPARING", "PLAYING", "PAUSED", "FAILED")
    return native_enum_matches("PlaybackState", "Playback", "RecordingPlayer.kt", names)


def recordings_provider_contract():
    """#708: the recordings provider serves the two "Call recordings" folders RecordingShare names, and no more."""
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
    if sorted(entries) != [("external-files-path", "external", "Call recordings/"), ("files-path", "internal", "Call recordings/")]:
        return f"res/xml/recordings_paths.xml: only the two Call recordings folders may be served, not {entries}"
    if 'const val FOLDER = "Call recordings"' not in share or 'const val AUTHORITY_SUFFIX = ".recordings"' not in share:
        return f"{APP_SOURCES / 'RecordingShare.kt'}: FOLDER and AUTHORITY_SUFFIX must match the provider"
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
    # Every stop intent the app sends is the user's: the Qt host's Stop listening and the notification's Stop action.
    service = texts.get("DecoderService.kt", "")
    senders = re.findall(r"\.setAction\(ACTION_STOP\)(\s*\.putExtra\(EXTRA_STOP_CAUSE, StopCause\.USER\.wire\))?", service)
    if len(senders) != 2 or not all(senders):
        return f"{APP_SOURCES / 'DecoderService.kt'}: both ACTION_STOP senders must put EXTRA_STOP_CAUSE = StopCause.USER.wire"
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
    # #782 gate r2 H3: only a run's dsd_engine_cleanup() closes the files the bootstrap opens (-c, -w, -P, -6), so
    # every way a configured session ends without nativeRun() closes them: a discard, a failed or exiting configure (and
    # the next configure over one never run), and nativeDestroy() before it frees the options.
    release = "dsd_engine_release_outputs(g_opts, g_state);"
    for name, count in (("nativeDiscardConfigured", 1), ("nativeConfigure", 2), ("nativeDestroy", 1)):
        body = re.search(rf"DsdNative_{name}\(.*?\n\}}", jni, re.S)
        if not body or body.group(0).count(release) < count:
            return f"android/dsdneo_jni.cpp: {name} must call {release} ({count} time(s)) for a session that never ran"
    configure = re.search(r"DsdNative_nativeConfigure\(.*?\n\}", jni, re.S).group(0)
    tail = configure.split("g_configured = false;", 1)
    if len(tail) != 2 or release not in tail[1] or configure.find(release) > configure.find("dsd_runtime_bootstrap("):
        return "android/dsdneo_jni.cpp: nativeConfigure must release before the bootstrap and after a failed one"
    return None


def receiver_option_tables_match():
    """UsbReceiverKind.sourceOfArgs() reads a start's input as the engine's parser does, from copies of its tables.

    The getopt string in src/runtime/cli/args.c and dsd_cli_compact_args()'s long-option tables in
    src/runtime/cli/compact.c must equal the Kotlin ones, or an option's operand could be read as an input (#782)."""
    kotlin_path = REPO / APP_SOURCES / "UsbReceiverKind.kt"
    args_path = REPO / "src/runtime/cli/args.c"
    compact_path = REPO / "src/runtime/cli/compact.c"
    try:
        kotlin = kotlin_path.read_text(encoding="utf-8")
        args = args_path.read_text(encoding="utf-8")
        compact = compact_path.read_text(encoding="utf-8")
    except OSError as error:
        return f"cannot read the option tables: {error}"
    native_short = re.search(r"getopt\(argc, argv,\s*((?:\"[^\"]*\"\s*)+)\)", args)
    kotlin_short = re.search(r"const val SHORT_OPTIONS =\s*((?:\"[^\"]*\"\s*\+?\s*)+)", kotlin)
    if not native_short or not kotlin_short:
        return f"cannot find the getopt string in {args_path.relative_to(REPO)} or SHORT_OPTIONS in {APP_SOURCES}"
    joined = ["".join(re.findall(r'"([^"]*)"', match.group(1))) for match in (native_short, kotlin_short)]
    if joined[0] != joined[1]:
        return f"SHORT_OPTIONS in UsbReceiverKind.kt must equal the getopt string of args.c: {joined[0]!r}"
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
    # Any other long-option table in compact.c would be one this copy does not know.
    tables = set(re.findall(r"static const char\* const (k_\w+)\[\]", compact)) | {"k_free_value_options"}
    unknown = sorted(tables - {native for native, _ in pairs})
    if unknown:
        return f"compact.c has long-option tables UsbReceiverKind.kt does not mirror: {unknown}"
    return replay_prescan_matches(kotlin, args, compact)


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
    # The input the parse sets: getopt's -i (strncpy and terminator) and the replay (cli_set_iqreplay_audio_dev()).
    writes = re.findall(
        r"(?:DSD_\w+|memcpy|memmove|strn?cpy|strn?cat|v?snprintf)\(opts->audio_in_dev\b|opts->audio_in_dev\[[^\]]*\] = ", args
    )
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


# Each build compiles its own file set on its own, so one build's stubs never stand in for platform classes another
# build's sources must not touch.
SUITES = {
    # The location broker and geocoder queue, against deterministic platform stubs.
    "location": Suite(
        builds=(
            Build(
                sources=("LocationSupport.kt",),
                tests=("LocationGeocodeQueueTest.kt", "LocationSupportTest.kt"),
                stubs=(
                    "Activity.kt",
                    "Context.kt",
                    "JSONObject.kt",
                    "Location.kt",
                    "Manifest.kt",
                    "Os.kt",
                    "PackageManager.kt",
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
    # delete and cancellation) builds apart, against a resolver stub that plays the document providers.
    "document_transfer": Suite(
        checks=(document_copy_codes_match,),
        builds=(
            Build(sources=("DocumentTransfer.kt",), tests=("DocumentTransferTest.kt",)),
            Build(
                sources=("DocumentCopies.kt", "DocumentTransfer.kt"),
                tests=("DocumentCopiesTest.kt",),
                stubs=(
                    "AssetFileDescriptor.kt",
                    "ContentResolver.kt",
                    "Cursor.kt",
                    "Log.kt",
                    "Os.kt",
                    "ParcelFileDescriptor.kt",
                    "Provider.kt",
                    "SystemOs.kt",
                    "Uri.kt",
                ),
            ),
        ),
    ),
    # The call recording player's state machine (#708). Plain Kotlin with no Android imports, so no stubs;
    # RecordingPlayback runs it with a MediaPlayer and the audio focus.
    "recording_player": Suite(
        checks=(playback_states_match,),
        builds=(Build(sources=("RecordingPlayer.kt",), tests=("RecordingPlayerTest.kt",)),),
    ),
    # Call recording export and save (#708): the ZIP writer and the transfer into a document, through DocumentCopies'
    # managed write, against the same resolver stubs as the backup's copies.
    "recording_export": Suite(
        checks=(recording_transfer_codes_match,),
        builds=(
            Build(
                sources=("RecordingExport.kt", "DocumentCopies.kt", "DocumentTransfer.kt"),
                tests=("RecordingExportTest.kt",),
                stubs=(
                    "AssetFileDescriptor.kt",
                    "ContentResolver.kt",
                    "Cursor.kt",
                    "Log.kt",
                    "Os.kt",
                    "ParcelFileDescriptor.kt",
                    "Provider.kt",
                    "SystemOs.kt",
                    "Uri.kt",
                ),
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
    # stubs; ProcessFacts reads the records off ActivityManager.
    "exit_facts": Suite(builds=(Build(sources=("ExitFacts.kt",), tests=("ExitFactsTest.kt",)),)),
    # The reason mapping builds without Android; the actual service also runs against a deferred-intent transport.
    "engine_stop": Suite(
        builds=(
            Build(sources=("EngineStop.kt",), tests=("EngineStopTest.kt",)),
            Build(
                # UsbSourceManager is the stub in ServiceNative.kt: the service's take of its receiver is scripted.
                sources=("EngineStop.kt", "DecoderService.kt", "DecoderStatus.kt", "UsbReceiverKind.kt", "UsbSessionTake.kt"),
                tests=("DecoderServiceStopTest.kt", "DecoderServiceAttemptTest.kt", "DecoderServiceUsbTest.kt"),
                stubs=("Service.kt", "ServiceContext.kt", "ServicePackageManager.kt", "ServiceNative.kt",
                       "Os.kt", "PowerManager.kt", "Log.kt", "JSONObject.kt", "SuppressLint.kt"),
            ),
        ),
        checks=(stop_callers_carry_their_cause, native_lifecycle_contract),
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
        jars.append(candidates[-1])
    return jars


def run_build(name, build, java, kotlinc, cached):
    app = REPO / APP_SOURCES
    files = [
        *(app / source for source in build.sources),
        *(HERE / test for test in build.tests),
        *(HERE / "stubs" / stub for stub in build.stubs),
    ]
    with tempfile.TemporaryDirectory(prefix=f"dsd-{name}-tests-") as directory:
        output = Path(directory) / "tests.jar"
        if kotlinc:
            compiler = [kotlinc, "-include-runtime"]
            runtime = str(output)
        else:
            classpath = os.pathsep.join(map(str, cached))
            compiler = [java, "-cp", classpath, "org.jetbrains.kotlin.cli.jvm.K2JVMCompiler", "-no-stdlib", "-no-reflect", "-classpath", classpath]
            runtime = os.pathsep.join((str(output), classpath))
        subprocess.run([*compiler, *map(str, files), "-d", str(output)], check=True)
        for test in build.tests:
            # Top-level functions in Foo.kt compile into the class FooKt.
            subprocess.run([java, "-cp", runtime, f"{PACKAGE}.{Path(test).stem}Kt"], check=True, timeout=30)


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
    java = shutil.which("java")
    kotlinc = shutil.which("kotlinc")
    cached = cached_compiler() if not kotlinc else None
    if not java or (not kotlinc and not cached):
        print("Android JVM tests need Java and kotlinc, or the Kotlin compiler in the Gradle cache.")
        return 1 if require_tools else 77
    for name in selected:
        run_suite(name, SUITES[name], java, kotlinc, cached)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
