#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run production Android Kotlin on the JVM, suite by suite, without an Android build or emulator."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
from typing import NamedTuple

PACKAGE = "io.github.arancormonk.dsdneo"
APP_SOURCES = Path("android/package/src/io/github/arancormonk/dsdneo")


class Suite(NamedTuple):
    sources: tuple  # Production files under APP_SOURCES.
    tests: tuple  # Test files beside this script; each one's main() runs, in this order.
    stubs: bool  # Whether the Android platform stubs in stubs/ compile with it.


# Each suite compiles its own file set on its own, so one suite's stubs never stand in for platform classes another
# suite's sources must not touch.
SUITES = {
    # The location broker and geocoder queue, against deterministic platform stubs.
    "location": Suite(
        sources=("LocationSupport.kt",),
        tests=("LocationGeocodeQueueTest.kt", "LocationSupportTest.kt"),
        stubs=True,
    ),
    # The notification status record's reader. Plain Kotlin with no Android imports, so no stubs.
    "decoder_status": Suite(
        sources=("DecoderStatus.kt",),
        tests=("DecoderStatusTest.kt",),
        stubs=False,
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


def run_suite(name, suite, java, kotlinc, cached):
    here = Path(__file__).resolve().parent
    app = here.parents[1] / APP_SOURCES
    files = [*(app / source for source in suite.sources), *(here / test for test in suite.tests)]
    if suite.stubs:
        files += sorted((here / "stubs").glob("*.kt"))
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
        for test in suite.tests:
            # Top-level functions in Foo.kt compile into the class FooKt.
            subprocess.run([java, "-cp", runtime, f"{PACKAGE}.{Path(test).stem}Kt"], check=True, timeout=30)


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
    # CI jobs that run the suite set DSD_NEO_REQUIRE_TEST_TOOLS=1, so a missing compiler fails there instead of skipping.
    require_tools = args.require_tools or os.environ.get("DSD_NEO_REQUIRE_TEST_TOOLS") == "1"
    java = shutil.which("java")
    kotlinc = shutil.which("kotlinc")
    cached = cached_compiler() if not kotlinc else None
    if not java or (not kotlinc and not cached):
        print("Android JVM tests need Java and kotlinc, or the Kotlin compiler in the Gradle cache.")
        return 1 if require_tools else 77
    for name in dict.fromkeys(args.suites or SUITES):
        run_suite(name, SUITES[name], java, kotlinc, cached)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
