#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The APK's Qt kit, NDK runtime and Gradle libraries checked against the legal records' lock file, with fake builds."""

import copy
import importlib.util
import io
import json
import os
import shutil
import subprocess
import tempfile
import unittest
import zipfile
from contextlib import redirect_stderr
from pathlib import Path

ROOT = Path(os.environ.get("DSD_TEST_SOURCE_ROOT", Path(__file__).resolve().parents[2]))
SPEC = importlib.util.spec_from_file_location("check_android_runtime_lock",
                                              ROOT / "tools/check_android_runtime_lock.py")
check = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(check)

LOCK = json.loads((ROOT / "legal/android-runtime-lock.json").read_text(encoding="utf-8"))

# The forms Gradle's dependency report uses: a requested version resolved to another, a module with no requested
# version, a repeated subtree, a constraint (not a dependency) and a strict version.
REPORT_FORMS = """
> Task :dependencies

------------------------------------------------------------
Root project 'android-build'
------------------------------------------------------------

releaseRuntimeClasspath - Runtime classpath of '/release'.
+--- org.jetbrains.kotlin:kotlin-stdlib:2.2.10
|    \\--- org.jetbrains:annotations:13.0 -> 23.0.0
\\--- androidx.core:core:1.17.0
     +--- androidx.core:core-viewtree:1.0.0
     |    +--- org.jetbrains.kotlin:kotlin-stdlib -> 2.2.10 (*)
     |    \\--- org.jetbrains.kotlin:kotlin-stdlib:1.8.22 -> 2.2.10 (c)
     +--- androidx.tracing:tracing:{strictly 1.2.0} -> 1.2.0
     \\--- com.google.guava:listenablefuture:1.0

(c) - A dependency constraint, not a dependency. The dependency affected by the constraint occurs elsewhere in the tree.
(*) - Indicates repeated occurrences of a transitive dependency subtree.
"""


def coordinates(lock=LOCK):
    return sorted(c for facts in lock["records"].values() for c in facts.get("release_runtime_classpath", []))


def libraries(lock=LOCK):
    return sorted(name for facts in lock["records"].values() for name in facts.get("libraries", []))


def report(coords):
    """A dependency report that resolves @p coords, the first at the root and the rest nested and repeated."""
    lines = ["", "releaseRuntimeClasspath - Runtime classpath of '/release'."]
    for i, coord in enumerate(coords):
        group, name, version = coord.split(":")
        if i % 3 == 1:
            lines.append(f"|    +--- {group}:{name}:0.1 -> {version}")
        elif i % 3 == 2:
            lines.append(f"|    |    \\--- {coord} (*)")
        else:
            lines.append(f"+--- {coord}")
    lines.append("+--- org.jetbrains.kotlin:kotlin-stdlib:2.0.21 -> 9.9.9 (c)")
    return "\n".join(lines + ["", "(*) - Indicates repeated occurrences of a transitive dependency subtree.", ""])


class FakeBuild:
    """A configured Android build: its CMake cache, the Qt kit and NDK it names, the Gradle report and the APK."""

    def __init__(self, root, lock=LOCK):
        self.root = Path(root)
        qt = lock["records"]["qt"]
        ndk = lock["records"]["ndk-runtime"]
        self.kit = self.root / "Qt" / qt["version"] / "android_arm64_v8a"
        self.ndk = self.root / "ndk" / ndk["revision"]
        self.build = self.root / "build"
        (self.kit / "lib/cmake/Qt6Core").mkdir(parents=True)
        (self.kit / "mkspecs").mkdir(parents=True)
        (self.kit / "sbom").mkdir(parents=True)
        self.ndk.mkdir(parents=True)
        self.build.mkdir()
        self.set_qt_version(qt["version"])
        for module, namespace in qt["sbom_namespaces"].items():
            self.set_namespace(module, namespace, qt["version"])
        self.set_ndk(ndk["revision"], ndk["release"])
        (self.build / "CMakeCache.txt").write_text(
            "# fake\n"
            f"Qt6Core_DIR:PATH={self.kit}/lib/cmake/Qt6Core\n"
            f"ANDROID_NDK_ROOT:STRING={self.ndk}\n", encoding="utf-8")
        self.gradle = self.root / "dependencies.txt"
        self.gradle.write_text(report(coordinates(lock)), encoding="utf-8")
        self.apk = self.root / "app.apk"
        self.set_apk_libraries(libraries(lock))

    def set_qt_version(self, version):
        (self.kit / "mkspecs/qconfig.pri").write_text(
            "QT_ARCH = arm64\n"
            f"QT_VERSION = {version}\n"
            "QT_MAJOR_VERSION = 6\n", encoding="utf-8")

    def set_namespace(self, module, namespace, version):
        (self.kit / "sbom" / f"{module}-{version}.spdx.json").write_text(
            json.dumps({"spdxVersion": "SPDX-2.3", "documentNamespace": namespace}), encoding="utf-8")

    def set_ndk(self, revision, release):
        (self.ndk / "source.properties").write_text(
            "Pkg.Desc = Android NDK\n"
            f"Pkg.Revision = {revision}\n"
            f"Pkg.ReleaseName = {release}\n", encoding="utf-8")

    def set_apk_libraries(self, names, abi="arm64-v8a"):
        with zipfile.ZipFile(self.apk, "w") as apk:
            apk.writestr("classes.dex", b"dex")
            apk.writestr("assets/doc/dsd-neo/LICENSE", b"license")
            for name in names:
                apk.writestr(f"lib/{abi}/{name}", b"\x7fELF")

    def run(self, lock_path=None, components=None):
        args = ["--build-dir", str(self.build), "--gradle-dependencies", str(self.gradle), "--apk", str(self.apk)]
        if lock_path is not None:
            args += ["--lock", str(lock_path)]
        if components is not None:
            args += ["--components", str(components)]
        err = io.StringIO()
        with redirect_stderr(err):
            code = check.main(args)
        return code, err.getvalue()


class AndroidRuntimeLockTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="dsd-runtime-lock-")
        self.fake = FakeBuild(self.temp.name)

    def tearDown(self):
        self.temp.cleanup()

    def assert_fails(self, needle):
        code, err = self.fake.run()
        self.assertNotEqual(code, 0, err)
        self.assertIn(needle, err)
        return err

    def test_build_matching_the_records_passes(self):
        code, err = self.fake.run()
        self.assertEqual(code, 0, err)

    def test_the_lock_names_reviewed_records(self):
        # Every record the lock describes is a reviewed record of an Android binary, named for the version it locks.
        inventory = json.loads((ROOT / "legal/components.json").read_text(encoding="utf-8"))
        records = {c["id"]: c for c in inventory["components"]}
        for ident in LOCK["records"]:
            self.assertIn("android", records[ident]["platforms"])
            self.assertIn("binary", records[ident]["distribution"])
        self.assertIn("Qt " + LOCK["records"]["qt"]["version"], records["qt"]["name"])
        self.assertIn(LOCK["records"]["ndk-runtime"]["release"], records["ndk-runtime"]["name"])
        # The reviewed set: androidx.core 1.17.0 and Kotlin 2.2.10's runtime, no debugger plugin.
        self.assertIn("androidx.core:core:1.17.0", coordinates())
        self.assertIn("org.jetbrains.kotlin:kotlin-stdlib:2.2.10", coordinates())
        self.assertFalse([name for name in libraries() if "qmltooling" in name])

    def test_parses_every_report_form(self):
        self.assertEqual(check.parse_dependency_report(REPORT_FORMS), {
            "org.jetbrains.kotlin:kotlin-stdlib:2.2.10", "org.jetbrains:annotations:23.0.0",
            "androidx.core:core:1.17.0", "androidx.core:core-viewtree:1.0.0", "androidx.tracing:tracing:1.2.0",
            "com.google.guava:listenablefuture:1.0"})
        for bad in ("+--- androidx.core:core:1.17.0 FAILED", "+--- androidx.core:core:1.17.0 (n)",
                    "+--- project :library"):
            with self.subTest(line=bad), self.assertRaises(ValueError):
                check.parse_dependency_report("releaseRuntimeClasspath - x\n" + bad + "\n")
        with self.assertRaises(ValueError):
            check.parse_dependency_report("debugRuntimeClasspath - x\n+--- a:b:1\n")
        with self.assertRaises(ValueError):
            check.parse_dependency_report("releaseRuntimeClasspath - x\nNo dependencies\n")

    def test_qt_kit_drift_fails(self):
        self.fake.set_qt_version("6.11.4")
        self.assert_fails("Qt 6.11.4")

    def test_qt_kit_rebuilt_at_the_same_version_fails(self):
        qt = LOCK["records"]["qt"]
        self.fake.set_namespace("qtbase", "https://qt.io/spdxdocs/qtbase-rebuilt", qt["version"])
        self.assert_fails("qtbase")
        (self.fake.kit / "sbom" / f"qtsvg-{qt['version']}.spdx.json").unlink()
        self.assert_fails("qtsvg")

    def test_ndk_drift_fails(self):
        self.fake.set_ndk("29.0.14206865", "r29")
        self.assert_fails("29.0.14206865")

    def test_gradle_drift_fails(self):
        for label, coords, needle in (
            ("a version bump", [c.replace("kotlin-stdlib:2.2.10", "kotlin-stdlib:2.3.0") for c in coordinates()],
             "org.jetbrains.kotlin:kotlin-stdlib:2.3.0"),
            ("a new library", coordinates() + ["com.example:extra:1.0"], "com.example:extra:1.0"),
            ("a library gone", [c for c in coordinates() if "jspecify" not in c], "org.jspecify:jspecify:1.0.0"),
        ):
            with self.subTest(case=label):
                self.fake.gradle.write_text(report(coords), encoding="utf-8")
                self.assert_fails(needle)

    def test_apk_library_drift_fails(self):
        # Qt's QML debugger plugins back in a release APK, a Qt library gone, or another ABI.
        debugger = "libplugins_qmltooling_qmldbg_debugger_arm64-v8a.so"
        self.fake.set_apk_libraries(libraries() + [debugger])
        self.assert_fails(debugger)
        self.fake.set_apk_libraries([n for n in libraries() if n != "libQt6Svg_arm64-v8a.so"])
        self.assert_fails("libQt6Svg_arm64-v8a.so")
        self.fake.set_apk_libraries(libraries(), abi="armeabi-v7a")
        self.assert_fails("armeabi-v7a")

    def test_lock_must_name_known_android_records(self):
        lock = copy.deepcopy(LOCK)
        lock["records"]["no-such-record"] = lock["records"].pop("androidx")
        path = Path(self.temp.name) / "lock.json"
        path.write_text(json.dumps(lock), encoding="utf-8")
        code, err = self.fake.run(lock_path=path)
        self.assertNotEqual(code, 0)
        self.assertIn("no-such-record", err)

    def test_record_must_name_the_locked_version(self):
        inventory = json.loads((ROOT / "legal/components.json").read_text(encoding="utf-8"))
        qt = next(c for c in inventory["components"] if c["id"] == "qt")
        qt["name"] = qt["name"].replace(LOCK["records"]["qt"]["version"], "6.10.0")
        path = Path(self.temp.name) / "components.json"
        path.write_text(json.dumps(inventory), encoding="utf-8")
        code, err = self.fake.run(components=path)
        self.assertNotEqual(code, 0)
        self.assertIn("qt", err)


@unittest.skipUnless(shutil.which("cmake") and shutil.which("ninja"), "cmake and ninja are required")
class AndroidPackagingGenerator(unittest.TestCase):
    """Release APKs leave out the QML debugger plugins by CMAKE_BUILD_TYPE, which a multi-config generator leaves
    empty: android/CMakeLists.txt refuses such a generator before anything else, so no APK is built with them."""

    REFUSAL = "The Android app is packaged with a single-configuration generator"

    def configure(self, *args):
        with tempfile.TemporaryDirectory(prefix="dsd-android-generator-") as work:
            probe = Path(work) / "probe"
            probe.mkdir()
            (probe / "CMakeLists.txt").write_text(
                "cmake_minimum_required(VERSION 3.20)\n"
                "project(probe NONE)\n"
                f"add_subdirectory(\"{(ROOT / 'android').as_posix()}\" android)\n", encoding="utf-8")
            # No Qt is looked for, so the single-config run stops at Qt and never builds anything.
            return subprocess.run(["cmake", "-S", str(probe), "-B", str(Path(work) / "build"),
                                   "-DCMAKE_DISABLE_FIND_PACKAGE_Qt6=ON", *args],
                                  capture_output=True, text=True, check=False)

    def test_multi_config_generator_is_refused(self):
        result = self.configure("-G", "Ninja Multi-Config")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(self.REFUSAL, result.stderr)

    def test_single_config_generator_passes_the_check(self):
        for build_type in ("Release", "Debug"):
            with self.subTest(build_type=build_type):
                result = self.configure("-G", "Ninja", f"-DCMAKE_BUILD_TYPE={build_type}")
                self.assertNotIn(self.REFUSAL, result.stderr)
                self.assertIn("Qt6", result.stderr)


if __name__ == "__main__":
    unittest.main()
