#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run the production location broker on the JVM with a deterministic platform fixture."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--require-tools", action="store_true")
    args = parser.parse_args()
    java = shutil.which("java")
    kotlinc = shutil.which("kotlinc")
    cached = cached_compiler() if not kotlinc else None
    if not java or (not kotlinc and not cached):
        print("Location JVM tests need Java and kotlinc, or the Kotlin compiler in the Gradle cache.")
        return 1 if args.require_tools else 77
    here = Path(__file__).resolve().parent
    source = here.parents[1] / "android/package/src/io/github/arancormonk/dsdneo/LocationSupport.kt"
    files = [source, here / "LocationGeocodeQueueTest.kt", here / "LocationSupportTest.kt", *sorted((here / "stubs").glob("*.kt"))]
    with tempfile.TemporaryDirectory(prefix="dsd-location-tests-") as directory:
        output = Path(directory) / "tests.jar"
        if kotlinc:
            compiler = [kotlinc, "-include-runtime"]
            runtime = str(output)
        else:
            classpath = os.pathsep.join(map(str, cached))
            compiler = [java, "-cp", classpath, "org.jetbrains.kotlin.cli.jvm.K2JVMCompiler", "-no-stdlib", "-no-reflect", "-classpath", classpath]
            runtime = os.pathsep.join((str(output), classpath))
        subprocess.run([*compiler, *map(str, files), "-d", str(output)], check=True)
        for test in ("LocationGeocodeQueueTestKt", "LocationSupportTestKt"):
            subprocess.run([java, "-cp", runtime, "io.github.arancormonk.dsdneo." + test], check=True, timeout=30)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
