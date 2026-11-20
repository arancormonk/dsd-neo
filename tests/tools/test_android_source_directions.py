#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""tools/android_source_directions.py and the APK release steps that publish its text."""

import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path

ROOT = Path(os.environ.get("DSD_TEST_SOURCE_ROOT", Path(__file__).resolve().parents[2]))
TOOL = ROOT / "tools/android_source_directions.py"
WORKFLOW = ROOT / ".github/workflows/android-ci.yaml"
PSL_COMMIT = "e452c7058d6946bd76952b128c12f5ce87a5acb8"
PSL_VERSION = "2026-05-14_08-35-31_UTC"


def pinned_qt_version():
    for line in (ROOT / "tools/ci-dependency-pins.env").read_text(encoding="utf-8").splitlines():
        if line.startswith("ANDROID_QT_VERSION="):
            return line.split("=", 1)[1]
    raise AssertionError("tools/ci-dependency-pins.env pins no ANDROID_QT_VERSION")


def steps(job):
    return re.split(r"(?m)^      - ", job)[1:]


def job(source, name):
    return re.search(rf"(?ms)^  {name}:\n(.*?)(?=^  [\w-]+:|\Z)", source)[1]


def step(job_text, name):
    return next(s for s in steps(job_text) if s.startswith(f"name: {name}\n"))


def script(step_text):
    return textwrap.dedent(re.search(r"(?m)^        run: \|\n((?: {10}[^\n]*\n|\n)+)", step_text)[1])


class SourceDirections(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.work = Path(self.tmp.name)
        self.version = pinned_qt_version()
        self.kit = self.make_kit(self.version)

    def tearDown(self):
        self.tmp.cleanup()

    def make_kit(self, version, psl=True, modules=("qtbase", "qtdeclarative", "qtsvg"), qconfig=None):
        kit = self.work / f"kit-{version}-{psl}-{len(modules)}-{qconfig}"
        (kit / "mkspecs").mkdir(parents=True)
        (kit / "mkspecs/qconfig.pri").write_text(
            f"QT_EDITION = OpenSource\nQT_VERSION = {qconfig or version}\nQT_MAJOR_VERSION = 6\n", encoding="utf-8")
        (kit / "sbom").mkdir()
        for module in modules:
            packages = [{"name": f"{module}-unrelated", "versionInfo": version}]
            if module == "qtbase" and psl:
                packages.append({
                    "name": "Network_Attribution_psl-data", "versionInfo": PSL_VERSION,
                    "downloadLocation": "https://publicsuffix.org/list/public_suffix_list.dat",
                    "externalRefs": [
                        {"referenceCategory": "PACKAGE_MANAGER", "referenceType": "purl",
                         "referenceLocator": f"pkg:generic/psl-data@{PSL_COMMIT}?download_url="
                                             "https://publicsuffix.org/list/public_suffix_list.dat"},
                        {"referenceCategory": "PACKAGE_MANAGER", "referenceType": "purl",
                         "referenceLocator": "pkg:github/qt/qtbase@574695a7?library_name=Network_Attribution_psl-data"},
                    ]})
            (kit / f"sbom/{module}-{version}.spdx.json").write_text(
                json.dumps({"spdxVersion": "SPDX-2.3", "packages": packages}), encoding="utf-8")
        return kit

    def run_tool(self, *args):
        return subprocess.run([sys.executable, str(TOOL), *args], capture_output=True, text=True, check=False)

    def directions(self, kit=None, version=None):
        output = self.work / "directions.txt"
        output.unlink(missing_ok=True)
        result = self.run_tool("directions", "--qt-version", self.version if version is None else version,
                               "--qt-kit", str(kit or self.kit),
                               "--apk", "dsd-neo-android-arm64-app-v1.2.3.apk",
                               "--dsd-source", "https://github.com/arancormonk/dsd-neo/tree/v1.2.3",
                               "--output", str(output))
        return result, output

    def test_directions_name_the_shipped_qt_version_and_its_archives(self):
        result, output = self.directions()
        self.assertEqual(result.returncode, 0, result.stderr)
        text = output.read_text(encoding="utf-8")
        series = ".".join(self.version.split(".")[:2])
        for module in ("qtbase", "qtdeclarative", "qtsvg"):
            url = (f"https://download.qt.io/archive/qt/{series}/{self.version}/submodules/"
                   f"{module}-everywhere-src-{self.version}.tar.xz")
            self.assertIn(url, text)
        self.assertIn(f"Qt {self.version}", text)
        self.assertIn("dsd-neo-android-arm64-app-v1.2.3.apk", text)
        self.assertIn(".sha256", text)
        self.assertIn("no patches", text)
        self.assertIn(f"https://github.com/publicsuffix/list/blob/{PSL_COMMIT}/public_suffix_list.dat", text)
        self.assertIn(PSL_VERSION, text)
        self.assertIn("https://github.com/arancormonk/dsd-neo/tree/v1.2.3", text)
        # Every Qt version the text names is the one the build used.
        self.assertEqual(set(re.findall(r"\b6\.\d+\.\d+\b", text)), {self.version})

    def test_another_version_is_refused(self):
        # The kit the build used must be the version the text names, and the text names only that version.
        cases = {
            "kit reports another version": self.make_kit(self.version, qconfig="6.0.0"),
            "kit has no Public Suffix List record": self.make_kit(self.version, psl=False),
            "kit lacks a module's SBOM": self.make_kit(self.version, modules=("qtbase", "qtdeclarative")),
        }
        for label, kit in cases.items():
            with self.subTest(label):
                result, output = self.directions(kit=kit)
                self.assertNotEqual(result.returncode, 0, result.stdout)
                self.assertFalse(output.exists())
        for version in ("6.11", "6.11.3-beta1", "", "../6.11.3"):
            with self.subTest(version=version):
                result, output = self.directions(version=version)
                self.assertNotEqual(result.returncode, 0, result.stdout)
                self.assertFalse(output.exists())

    def test_release_notes_carry_one_current_copy(self):
        result, directions = self.directions()
        self.assertEqual(result.returncode, 0, result.stderr)
        body = self.work / "body.md"
        body.write_text("## What's Changed\n* a change\n", encoding="utf-8")
        merged = self.work / "merged.md"
        for _ in range(2):
            result = self.run_tool("notes", "--body", str(body), "--directions", str(directions),
                                   "--output", str(merged))
            self.assertEqual(result.returncode, 0, result.stderr)
            body.write_text(merged.read_text(encoding="utf-8"), encoding="utf-8")
        text = merged.read_text(encoding="utf-8")
        self.assertTrue(text.startswith("## What's Changed\n* a change\n"), text)
        self.assertEqual(text.count("qtbase-everywhere-src-"), 1)
        # An older copy is replaced, not kept beside the new one.
        body.write_text(text.replace(self.version, "6.0.0"), encoding="utf-8")
        result = self.run_tool("notes", "--body", str(body), "--directions", str(directions), "--output", str(merged))
        self.assertEqual(result.returncode, 0, result.stderr)
        text = merged.read_text(encoding="utf-8")
        self.assertNotIn("6.0.0", text)
        self.assertEqual(text.count("qtbase-everywhere-src-"), 1)

    def test_the_apk_jobs_publish_the_directions_beside_every_apk(self):
        source = WORKFLOW.read_text(encoding="utf-8")
        build = job(source, "build-apk")
        write = script(step(build, "Write the Qt source directions"))
        # Generated from the Qt version and kit the build used, never a typed-in version.
        self.assertIn('--qt-version "$ANDROID_QT_VERSION"', write)
        self.assertIn('--qt-kit "$QT_ANDROID_ROOT"', write)
        self.assertIn('"dist/${ART_NAME}.apk.qt-source.txt"', write)
        self.assertNotRegex(write, r"\b6\.\d+\.\d+\b")
        names = [s.split("\n", 1)[0] for s in steps(build)]
        self.assertLess(names.index("name: Write the Qt source directions"), names.index("name: Upload APK"))
        upload = step(build, "Upload APK")
        self.assertIn("dist/${{ steps.names.outputs.ART_NAME }}.apk.qt-source.txt", upload)
        self.assertIn("if-no-files-found: error", upload)
        publish = job(source, "publish-apk")
        self.assertIn('test -s "${APK_PATH}.qt-source.txt"', script(step(publish, "Verify downloaded release files")))
        tag = script(step(publish, "Upload APK release assets"))
        self.assertIn('"${APK_PATH}.qt-source.txt"', tag)
        notes = step(publish, "Add the Qt source directions to the release notes")
        self.assertIn("if: startsWith(github.ref, 'refs/tags/v')", notes)
        self.assertIn('tools/android_source_directions.py notes', script(notes))
        self.assertIn('--directions "${APK_PATH}.qt-source.txt"', script(notes))
        self.assertIn("gh release edit", script(notes))
        nightly = step(publish, "Upload APK nightly assets (overwrite)")
        self.assertIn("${{ needs.build-apk.outputs.apk_path }}.qt-source.txt", nightly)

    def test_the_apk_carries_the_same_directions(self):
        # Someone with only the APK finds the same directions, the Public Suffix List's pinned source among them: the
        # Android configure writes them with this tool from the build's kit into the build tree, and the qt record's
        # android_release map stages that file into the APK, where the licences screen lists it and points to it.
        cmake = (ROOT / "android/CMakeLists.txt").read_text(encoding="utf-8")
        call = cmake[cmake.index("tools/android_source_directions.py"):]
        call = " ".join(call[:call.index(")")].split())
        for words in ("directions", "--qt-version", "--qt-kit", "--apk", "--dsd-source",
                      '--output "${PROJECT_BINARY_DIR}/qt-source/qt-source.txt"'):
            self.assertIn(words, call)
        self.assertLess(cmake.index("tools/android_source_directions.py"),
                        cmake.index("cmake/stage_android_package.cmake"))
        # The APK names the repository and commit that built it, as the file published beside it does; android-ci
        # passes them in, and a local build that passes none says so instead of naming a revision.
        self.assertIn('--dsd-source "${dsd_neo_source}"', call)
        self.assertIn('set(dsd_neo_source "${DSD_ANDROID_SOURCE_URL}")', cmake)
        self.assertIn("revision it was built from is not recorded", cmake)
        url = '"https://github.com/${GITHUB_REPOSITORY}/tree/${GITHUB_SHA}"'
        source = WORKFLOW.read_text(encoding="utf-8")
        configure = script(step(job(source, "build-apk"), "Configure (android-app)"))
        self.assertIn(f"-DDSD_ANDROID_SOURCE_URL={url}", configure)
        self.assertIn(f"--dsd-source {url}", script(step(job(source, "build-apk"), "Write the Qt source directions")))
        # Editing the tool or the kit's files it reads configures again, so the APK never keeps older directions.
        depends = cmake[cmake.index("CMAKE_CONFIGURE_DEPENDS"):]
        depends = depends[:depends.index(")")]
        for path in ("${PROJECT_SOURCE_DIR}/tools/android_source_directions.py",
                     "${dsd_neo_qt_kit}/mkspecs/qconfig.pri",
                     *(f"${{dsd_neo_qt_kit}}/sbom/{module}-${{Qt6Core_VERSION}}.spdx.json"
                       for module in ("qtbase", "qtdeclarative", "qtsvg"))):
            self.assertIn(f'"{path}"', depends)
        records = json.loads((ROOT / "legal/components.json").read_text(encoding="utf-8"))
        qt = next(c for c in records["components"] if c["id"] == "qt")
        self.assertEqual(qt["android_release"]["notices"]["licenses/Qt-SOURCE.txt"], "build:qt-source/qt-source.txt")
        screen = (ROOT / "src/ui/qt/decoder_host.cpp").read_text(encoding="utf-8")
        self.assertIn("licenses/Qt-SOURCE.txt", screen)

    @unittest.skipUnless(os.name == "posix" and shutil.which("bash"), "the publisher runs in Bash")
    def test_release_notes_step_edits_the_tag_once(self):
        # Replay the publisher's step with a stand-in gh: it reads the notes, adds the directions and writes them back.
        result, directions = self.directions()
        self.assertEqual(result.returncode, 0, result.stderr)
        apk = self.work / "dist/dsd-neo-android-arm64-app-v1.2.3.apk"
        apk.parent.mkdir()
        shutil.copy(directions, str(apk) + ".qt-source.txt")
        bin_dir = self.work / "bin"
        bin_dir.mkdir()
        (bin_dir / "python3").symlink_to(sys.executable)
        gh = bin_dir / "gh"
        gh.write_text(textwrap.dedent("""\
            #!/usr/bin/env bash
            set -eu
            printf '%s\\n' "$*" >> "$DSD_TEST_GH_LOG/calls"
            case "$1 $2" in
              "release view") cat "$DSD_TEST_GH_LOG/body.md" ;;
              "release edit") while [ "$#" -gt 0 ]; do
                  if [ "$1" = --notes-file ]; then cp "$2" "$DSD_TEST_GH_LOG/body.md"; fi; shift; done ;;
            esac
            """), encoding="utf-8")
        gh.chmod(0o755)
        log = self.work / "gh"
        log.mkdir()
        (log / "body.md").write_text("## What's Changed\n* a change\n", encoding="utf-8")
        (self.work / "runner").mkdir()
        env = {**os.environ, "PATH": str(bin_dir) + os.pathsep + os.environ.get("PATH", ""),
               "DSD_TEST_GH_LOG": str(log), "RUNNER_TEMP": str(self.work / "runner"), "GH_TOKEN": "test-fixture",
               "RELEASE_REPOSITORY": "arancormonk/dsd-neo", "RELEASE_TAG": "v1.2.3", "APK_PATH": str(apk)}
        notes = script(step(job(WORKFLOW.read_text(encoding="utf-8"), "publish-apk"),
                            "Add the Qt source directions to the release notes"))
        for _ in range(2):
            result = subprocess.run(["bash", "-eu", "-o", "pipefail", "-c", notes], cwd=ROOT, env=env,
                                    capture_output=True, text=True, check=False)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        body = (log / "body.md").read_text(encoding="utf-8")
        self.assertTrue(body.startswith("## What's Changed\n* a change\n"), body)
        self.assertEqual(body.count(f"qtbase-everywhere-src-{self.version}.tar.xz"), 1)
        calls = (log / "calls").read_text(encoding="utf-8").splitlines()
        self.assertEqual([c.split()[:3] for c in calls],
                         [["release", "view", "v1.2.3"], ["release", "edit", "v1.2.3"]] * 2)


if __name__ == "__main__":
    unittest.main()
