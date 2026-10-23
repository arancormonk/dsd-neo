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
    # How Android says recent app processes ended, as diagnostics lines. Plain Kotlin with no Android imports, so no
    # stubs; ProcessFacts reads the records off ActivityManager.
    "exit_facts": Suite(builds=(Build(sources=("ExitFacts.kt",), tests=("ExitFactsTest.kt",)),)),
    # The reason mapping builds without Android; the actual service also runs against a deferred-intent transport.
    "engine_stop": Suite(
        builds=(
            Build(sources=("EngineStop.kt",), tests=("EngineStopTest.kt",)),
            Build(
                sources=("EngineStop.kt", "DecoderService.kt", "DecoderStatus.kt"),
                tests=("DecoderServiceStopTest.kt",),
                stubs=("Service.kt", "ServiceContext.kt", "ServicePackageManager.kt", "ServiceNative.kt",
                       "Os.kt", "PowerManager.kt", "Log.kt", "JSONObject.kt"),
            ),
        ),
        checks=(stop_callers_carry_their_cause,),
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
