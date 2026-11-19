#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""tools/check_release_notices.py on fabricated records, repositories and package trees."""

import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import textwrap
import unittest
import zipfile
from pathlib import Path

ROOT = Path(os.environ.get("DSD_TEST_SOURCE_ROOT", Path(__file__).resolve().parents[2]))
CHECK = ROOT / "tools/check_release_notices.py"
APK_DOC = "assets/doc/dsd-neo/"
# The general legal files the APK carries besides the notices its records name.
APK_GENERAL = ("LICENSE", "COPYRIGHT", "THIRD_PARTY.md", "legal/status.md", "legal/components.json",
               "licenses/mbelib-neo/legal/status.md", "licenses/mbelib-neo/legal/patents.md")


def workflow_step(workflow, name):
    """The run script of a workflow step, by its name."""
    source = (ROOT / ".github/workflows" / workflow).read_text(encoding="utf-8")
    step = next(s for s in re.split(r"(?m)^      - ", source)[1:] if s.startswith(f"name: {name}\n"))
    return textwrap.dedent(re.search(r"(?m)^        run: \|\n((?: {10}[^\n]*\n|\n)+)", step)[1])


class CheckReleaseNotices(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.work = Path(self.tmp.name)
        self.repo = self.work / "repo"
        self.write(self.repo / "LICENSES/a-NOTICES.txt", "notice of a\n")
        self.write(self.repo / "LICENSE", "project license\n")
        self.write(self.repo / "legal/status.md", "status\n")
        self.build = self.work / "build"
        self.write(self.build / "x-licenses/x-LICENSE.txt", "x license\n")
        # The mbelib-neo notice folder the build selected, as cmake/MbelibNotices.cmake records it.
        self.mbelib = self.work / "mbelib/share/doc/mbelib-neo"
        self.write(self.mbelib / "LICENSE", "mbelib license\n")
        self.write(self.mbelib / "legal/status.md", "mbelib status\n")
        self.write(self.build / "CMakeCache.txt",
                   f"# fixture\nDSD_MBELIB_SELECTED_NOTICE_DIR:INTERNAL={self.mbelib}\n")
        self.share = self.work / "share"
        self.write(self.share / "port/copyright", "port copyright\n")
        self.records = {"components": [
            {"id": "a", "macos_release": {"binaries": ["dsd-neo", "liba", "libusb-1.0"], "notices": {
                "LICENSE": "repo:LICENSE",
                "licenses/a-NOTICES.txt": "repo:LICENSES/a-NOTICES.txt",
                "licenses/port-copyright.txt": "vcpkg:port",
                "licenses/x-LICENSE.txt": "build:x-licenses/x-LICENSE.txt",
                "licenses/mbelib-neo/LICENSE": "legal-component"}}},
            # Another package's map is not this package's business.
            {"id": "b", "windows_release": {"binaries": ["b.dll"], "notices": {"licenses/b.txt": "repo:LICENSE"}}},
        ]}
        self.doc = self.work / "doc"
        self.write(self.doc / "LICENSE", "project license\n")
        self.write(self.doc / "licenses/a-NOTICES.txt", "notice of a\r\n")
        self.write(self.doc / "licenses/port-copyright.txt", "port copyright\n")
        self.write(self.doc / "licenses/x-LICENSE.txt", "x license\n")
        self.write(self.doc / "licenses/mbelib-neo/LICENSE", "mbelib license\n")
        self.write(self.doc / "licenses/mbelib-neo/legal/status.md", "mbelib status\n")
        self.write(self.doc / "legal/status.md", "status\n")
        # The package's binary folders: each file is a recorded name, a library also with its version.
        self.package = self.work / "package"
        for rel in ("bin/dsd-neo", "lib/liba.dylib", "lib/liba.2.dylib", "lib/libusb-1.0.0.dylib"):
            self.write(self.package / rel, "binary\n")

    def tearDown(self):
        self.tmp.cleanup()

    @staticmethod
    def write(path, text):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(text.encode("utf-8"))

    def run_check(self, *extra, sources=True):
        components = self.work / "components.json"
        components.write_text(json.dumps(self.records), encoding="utf-8")
        args = [sys.executable, str(CHECK), "--platform", "macos", "--doc-dir", str(self.doc),
                "--components", str(components), "--repo", str(self.repo)]
        if sources:
            args += ["--build-dir", str(self.build), "--vcpkg-share", str(self.share)]
        return subprocess.run(args + list(extra), capture_output=True, text=True, check=False)

    def test_complete_tree_passes(self):
        for sources in (True, False):
            with self.subTest(sources=sources):
                result = self.run_check("--require", "legal/status.md", sources=sources)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn("5 notices", result.stdout)

    def test_missing_notice_fails(self):
        (self.doc / "licenses/port-copyright.txt").unlink()
        result = self.run_check()
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn("missing licenses/port-copyright.txt (a, vcpkg:port)", result.stderr)

    def test_missing_legal_component_file_fails(self):
        (self.doc / "licenses/mbelib-neo/LICENSE").unlink()
        result = self.run_check()
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn("missing licenses/mbelib-neo/LICENSE", result.stderr)

    def test_empty_notice_fails(self):
        self.write(self.doc / "licenses/mbelib-neo/LICENSE", "\n")
        result = self.run_check()
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn("empty licenses/mbelib-neo/LICENSE", result.stderr)

    def test_altered_notice_fails(self):
        for rel, source in (("licenses/a-NOTICES.txt", "repo:LICENSES/a-NOTICES.txt"),
                            ("licenses/port-copyright.txt", "vcpkg:port"),
                            ("licenses/x-LICENSE.txt", "build:x-licenses/x-LICENSE.txt")):
            with self.subTest(notice=rel):
                original = (self.doc / rel).read_bytes()
                self.write(self.doc / rel, "something else\n")
                result = self.run_check()
                (self.doc / rel).write_bytes(original)
                self.assertEqual(result.returncode, 1, result.stdout)
                self.assertIn(f"{rel} differs from {source}", result.stderr)

    def test_legal_component_and_required_files_match_their_sources(self):
        # mbelib's notices against the folder the build selected, the general legal files against the repository.
        for rel, needle, extra in (
                ("licenses/mbelib-neo/LICENSE", "licenses/mbelib-neo/LICENSE differs from legal-component", ()),
                ("licenses/mbelib-neo/legal/status.md", "licenses/mbelib-neo/legal/status.md differs from",
                 ("--require", "licenses/mbelib-neo/legal/status.md")),
                ("legal/status.md", "legal/status.md differs from", ("--require", "legal/status.md"))):
            with self.subTest(file=rel):
                original = (self.doc / rel).read_bytes()
                self.write(self.doc / rel, "a placeholder\n")
                result = self.run_check(*extra)
                (self.doc / rel).write_bytes(original)
                self.assertEqual(result.returncode, 1, result.stdout)
                self.assertIn(needle, result.stderr)

    def test_build_without_a_selected_mbelib_folder_fails(self):
        self.write(self.build / "CMakeCache.txt", "# no mbelib-neo notice folder\n")
        result = self.run_check()
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn("DSD_MBELIB_SELECTED_NOTICE_DIR", result.stderr)

    def test_notice_index_must_list_the_folder(self):
        files = sorted(p.relative_to(self.doc).as_posix() for p in self.doc.rglob("*") if p.is_file())
        index = self.doc / "notice-index.txt"
        for label, lines, needle in (
                ("complete, with a repeat and a blank line", files + [files[0], ""], None),
                ("missing", None, "missing notice-index.txt"),
                ("an entry left out", files[1:], f"{files[0]} is in the folder but not in notice-index.txt"),
                ("a file the folder lacks", files + ["licenses/gone.txt"],
                 "notice-index.txt names licenses/gone.txt, which the folder lacks")):
            with self.subTest(case=label):
                if lines is None:
                    index.unlink(missing_ok=True)
                else:
                    index.write_text("\n".join(lines) + "\n", encoding="utf-8")
                result = self.run_check("--index", "notice-index.txt")
                if needle is None:
                    self.assertEqual(result.returncode, 0, result.stderr)
                else:
                    self.assertEqual(result.returncode, 1, result.stdout)
                    self.assertIn(needle, result.stderr)

    def test_missing_required_file_fails(self):
        result = self.run_check("--require", "legal/components.json")
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn("missing legal/components.json (required)", result.stderr)

    def test_platform_without_notices_fails(self):
        self.records["components"][0]["macos_release"]["notices"] = {}
        result = self.run_check()
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn("legal/components.json names no notice for macos", result.stderr)

    def test_notice_outside_the_tree_is_refused(self):
        self.records["components"][0]["macos_release"]["notices"]["../escape.txt"] = "repo:LICENSE"
        result = self.run_check()
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn("'../escape.txt' is not a path inside the package", result.stderr)

    def run_binary_check(self):
        return self.run_check("--binary-dir", str(self.package / "bin"), str(self.package / "lib"))

    def test_recorded_binaries_pass(self):
        result = self.run_binary_check()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("4 binaries", result.stdout)

    def test_unrecorded_binary_fails(self):
        # Another package's names do not count, and a recorded name covers only itself with a version.
        for rel in ("lib/libstray.3.dylib", "lib/b.dll", "lib/liba-extra.1.dylib", "lib/libab.1.dylib",
                    "lib/liba.1.dylib.bak", "bin/dsd-neo-helper"):
            with self.subTest(binary=rel):
                self.write(self.package / rel, "binary\n")
                result = self.run_binary_check()
                (self.package / rel).unlink()
                self.assertEqual(result.returncode, 1, result.stdout)
                self.assertIn(f"{Path(rel).name} in {self.package / Path(rel).parent} is in no record's macos_release "
                              "binaries", result.stderr)

    def test_missing_binary_folder_fails(self):
        result = self.run_check("--binary-dir", str(self.package / "Frameworks"))
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn(f"missing binary folder {self.package / 'Frameworks'}", result.stderr)

    def test_dmg_job_checks_the_staged_tree_and_the_dmg(self):
        # The DMG job runs the check on the staged tree and again on the mounted DMG, with the general files.
        workflow = (ROOT / ".github/workflows/macos-ci.yaml").read_text(encoding="utf-8")
        job = workflow[workflow.index("  package-portable-dmg:"):workflow.index("  publish-portable-dmg:")]
        self.assertEqual(job.count("tools/check_release_notices.py --platform macos"), 2)
        # The mounted DMG's bin/ and lib/ hold only files a record names.
        mounted = job[job.index('--doc-dir "$mnt/dsd-neo-macos/share/doc/dsd-neo"'):]
        self.assertIn('--binary-dir "$mnt/dsd-neo-macos/bin" "$mnt/dsd-neo-macos/lib"', mounted[:mounted.index("\n\n")])
        for line in [line for line in job.splitlines() if "python3 tools/check_release_notices.py" in line]:
            self.assertIn("--build-dir", job[job.index(line):job.index("\n\n", job.index(line))])
        for name in ("LICENSE", "COPYRIGHT", "THIRD_PARTY.md", "legal/status.md", "legal/components.json"):
            self.assertIn(f" {name}", job[job.index("DMG_REQUIRED_DOCS"):])


@unittest.skipUnless(os.name == "posix" and shutil.which("bash") and shutil.which("unzip"),
                     "the APK job's step runs in Bash with unzip")
class ApkNoticeStep(unittest.TestCase):
    """The android-ci step that checks the finished APK, run on an APK-shaped archive."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.work = Path(self.tmp.name)
        self.build = self.work / "build"
        self.share = self.work / "share"
        records = json.loads((ROOT / "legal/components.json").read_text(encoding="utf-8"))
        self.mbelib = self.work / "mbelib-notices"
        self.build.mkdir(parents=True)
        (self.build / "CMakeCache.txt").write_text(
            f"DSD_MBELIB_SELECTED_NOTICE_DIR:INTERNAL={self.mbelib}\n", encoding="utf-8")
        self.entries = {}
        for name in APK_GENERAL:
            if name.startswith("licenses/mbelib-neo/"):
                self.entries[name] = f"mbelib's {name}\n".encode()
                origin = self.mbelib / name.removeprefix("licenses/mbelib-neo/")
                origin.parent.mkdir(parents=True, exist_ok=True)
                origin.write_bytes(self.entries[name])
            else:
                self.entries[name] = (ROOT / name).read_bytes()
        self.sources = {}
        for component in records["components"]:
            for notice, source in component.get("android_release", {}).get("notices", {}).items():
                kind, _, rest = source.partition(":")
                if kind == "repo":
                    content = (ROOT / rest).read_bytes()
                else:
                    content = f"{component['id']} notice {notice}\n".encode()
                    origin = {"build": self.build / rest, "vcpkg": self.share / rest / "copyright",
                              "legal-component": self.mbelib / notice.removeprefix("licenses/mbelib-neo/")}.get(kind)
                    if origin is not None:
                        origin.parent.mkdir(parents=True, exist_ok=True)
                        origin.write_bytes(content)
                self.entries[notice] = content
                self.sources.setdefault(kind, notice)
        bin_dir = self.work / "bin"
        bin_dir.mkdir()
        (bin_dir / "python3").symlink_to(sys.executable)
        self.env = {**os.environ, "PATH": str(bin_dir) + os.pathsep + os.environ.get("PATH", ""),
                    "RUNNER_TEMP": str(self.work / "runner"), "APK_PATH": str(self.work / "app.apk"),
                    "APK_NOTICE_BUILD_DIR": str(self.build), "APK_NOTICE_VCPKG_SHARE": str(self.share)}
        (self.work / "runner").mkdir()

    def tearDown(self):
        self.tmp.cleanup()

    def run_step(self, entries, index=True):
        if index:
            entries = {**entries, "notice-index.txt": "".join(name + "\n" for name in entries).encode()}
        with zipfile.ZipFile(self.env["APK_PATH"], "w", zipfile.ZIP_DEFLATED) as apk:
            apk.writestr("AndroidManifest.xml", b"manifest fixture")
            apk.writestr("classes.dex", b"dex fixture")
            apk.writestr("lib/arm64-v8a/libdsd-neo-app_arm64-v8a.so", b"library fixture")
            for name, content in entries.items():
                apk.writestr(APK_DOC + name, content)
        script = workflow_step("android-ci.yaml", "Verify the APK carries the required license files")
        return subprocess.run(["bash", "-eu", "-o", "pipefail", "-c", script], cwd=ROOT, env=self.env,
                              capture_output=True, text=True, check=False)

    def test_complete_apk_passes(self):
        self.assertEqual(set(self.sources), {"repo", "build", "vcpkg", "legal-component"})
        result = self.run_step(self.entries)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_empty_notice_in_the_apk_fails(self):
        for kind, notice in sorted(self.sources.items()):
            with self.subTest(kind=kind, notice=notice):
                result = self.run_step({**self.entries, notice: b"\n"})
                self.assertNotEqual(result.returncode, 0, result.stdout)
                self.assertIn(f"empty {notice}", result.stderr)

    def test_changed_notice_in_the_apk_fails(self):
        for kind in ("repo", "build", "vcpkg", "legal-component"):
            notice = self.sources[kind]
            with self.subTest(kind=kind, notice=notice):
                result = self.run_step({**self.entries, notice: b"some other text\n"})
                self.assertNotEqual(result.returncode, 0, result.stdout)
                self.assertIn(f"{notice} differs from", result.stderr)

    def test_missing_notice_or_general_file_fails(self):
        for name in (self.sources["repo"], "legal/status.md"):
            with self.subTest(name=name):
                entries = dict(self.entries)
                del entries[name]
                result = self.run_step(entries)
                self.assertNotEqual(result.returncode, 0, result.stdout)
                self.assertIn(f"missing {name}", result.stderr)

    def test_changed_general_file_in_the_apk_fails(self):
        for name in ("legal/components.json", "licenses/mbelib-neo/legal/status.md"):
            with self.subTest(name=name):
                result = self.run_step({**self.entries, name: b"a placeholder\n"})
                self.assertNotEqual(result.returncode, 0, result.stdout)
                self.assertIn(f"{name} differs from", result.stderr)

    def test_notice_index_in_the_apk_is_checked(self):
        # The licences screen lists only what the index names: no index, or one missing a file, fails.
        result = self.run_step(self.entries, index=False)
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn("missing notice-index.txt", result.stderr)
        partial = dict(self.entries)
        partial["notice-index.txt"] = "".join(name + "\n" for name in sorted(self.entries)[1:]).encode()
        result = self.run_step(partial, index=False)
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn(f"{sorted(self.entries)[0]} is in the folder but not in notice-index.txt", result.stderr)


if __name__ == "__main__":
    unittest.main()
