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
    # #647 WP4: the host polls deviceStatus(), which retires an unanswered request and publishes the request.
    status = re.search(r"fun deviceStatus\(\): String \{(.*?)\n    \}", manager, re.S)
    if not status or "requests.expireIfStale()" not in status.group(1):
        return f"{where}: deviceStatus() must retire an unanswered request"
    for key in ("pending", "requestedSource", "requestedSerial"):
        if f'.put("{key}"' not in status.group(1):
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
    # Which receiver kind is attached, from the USB ids on the bus. Plain Kotlin with no Android imports, so no stubs.
    "usb_receiver": Suite(builds=(Build(sources=("UsbReceiverKind.kt",), tests=("UsbReceiverKindTest.kt",)),)),
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
                       "Os.kt", "PowerManager.kt", "Log.kt", "JSONObject.kt"),
            ),
        ),
        checks=(stop_callers_carry_their_cause, native_lifecycle_contract),
    ),
    # The screen policy and the pure adapters around it compile with no stubs at all, so an android.* import in any of
    # them fails the build. ScreenLocks, the one wrapper over the platform's wake locks, builds apart, with the pure
    # ScreenWake it answers with, against a PowerManager stub that models reference counting.
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
                ),
                tests=(
                    "ScreenPolicyTest.kt",
                    "ScreenControllerTest.kt",
                    "ScreenTouchTest.kt",
                    "StatusFeedTest.kt",
                    "ActivitySlotTest.kt",
                ),
            ),
            Build(
                sources=("ScreenLocks.kt", "ScreenWake.kt"),
                tests=("ScreenLocksTest.kt",),
                stubs=("PowerManager.kt",),
            ),
        ),
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
