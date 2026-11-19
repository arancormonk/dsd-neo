#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""tools/check_release_notices.py on fabricated records, repositories and package trees."""

import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(os.environ.get("DSD_TEST_SOURCE_ROOT", Path(__file__).resolve().parents[2]))
CHECK = ROOT / "tools/check_release_notices.py"


class CheckReleaseNotices(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.work = Path(self.tmp.name)
        self.repo = self.work / "repo"
        self.write(self.repo / "LICENSES/a-NOTICES.txt", "notice of a\n")
        self.write(self.repo / "LICENSE", "project license\n")
        self.build = self.work / "build"
        self.write(self.build / "x-licenses/x-LICENSE.txt", "x license\n")
        self.share = self.work / "share"
        self.write(self.share / "port/copyright", "port copyright\n")
        self.records = {"components": [
            {"id": "a", "macos_release": {"notices": {
                "LICENSE": "repo:LICENSE",
                "licenses/a-NOTICES.txt": "repo:LICENSES/a-NOTICES.txt",
                "licenses/port-copyright.txt": "vcpkg:port",
                "licenses/x-LICENSE.txt": "build:x-licenses/x-LICENSE.txt",
                "licenses/dep/LICENSE": "legal-component"}}},
            # Another package's map is not this package's business.
            {"id": "b", "windows_release": {"binaries": ["b.dll"], "notices": {"licenses/b.txt": "repo:LICENSE"}}},
        ]}
        self.doc = self.work / "doc"
        self.write(self.doc / "LICENSE", "project license\n")
        self.write(self.doc / "licenses/a-NOTICES.txt", "notice of a\r\n")
        self.write(self.doc / "licenses/port-copyright.txt", "port copyright\n")
        self.write(self.doc / "licenses/x-LICENSE.txt", "x license\n")
        self.write(self.doc / "licenses/dep/LICENSE", "installed by the legal component\n")
        self.write(self.doc / "legal/status.md", "status\n")

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
        (self.doc / "licenses/dep/LICENSE").unlink()
        result = self.run_check()
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn("missing licenses/dep/LICENSE", result.stderr)

    def test_empty_notice_fails(self):
        self.write(self.doc / "licenses/dep/LICENSE", "\n")
        result = self.run_check()
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn("empty licenses/dep/LICENSE", result.stderr)

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

    def test_dmg_job_checks_the_staged_tree_and_the_dmg(self):
        # The DMG job runs the check on the staged tree and again on the mounted DMG, with the general files.
        workflow = (ROOT / ".github/workflows/macos-ci.yaml").read_text(encoding="utf-8")
        job = workflow[workflow.index("  package-portable-dmg:"):workflow.index("  publish-portable-dmg:")]
        self.assertEqual(job.count("tools/check_release_notices.py --platform macos"), 2)
        for name in ("LICENSE", "COPYRIGHT", "THIRD_PARTY.md", "legal/status.md", "legal/components.json"):
            self.assertIn(f" {name}", job[job.index("DMG_REQUIRED_DOCS"):])


if __name__ == "__main__":
    unittest.main()
