#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""tools/check_macos_portable_deps.sh on a fabricated portable tree.

A fake otool on PATH reads each "Mach-O" file as text: "dep <install name>",
"rpath <path>" and "minos <version>" lines stand for its load commands. Off
macOS the checker skips codesign, so the tree needs no real binaries.
"""

import os
import stat
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(os.environ.get("DSD_TEST_SOURCE_ROOT", Path(__file__).resolve().parents[2]))
CHECK = ROOT / "tools/check_macos_portable_deps.sh"

FAKE_OTOOL = r"""#!/usr/bin/env bash
set -euo pipefail
mode=$1
file=$2
case "$mode" in
  -L)
    echo "$file:"
    sed -n 's/^dep \(.*\)$/\t\1 (compatibility version 1.0.0, current version 1.0.0)/p' "$file"
    ;;
  -l)
    n=0
    while read -r kind value; do
      case "$kind" in
        rpath)
          printf 'Load command %d\n          cmd LC_RPATH\n      cmdsize 32\n         path %s (offset 12)\n' "$n" "$value"
          ;;
        minos)
          printf 'Load command %d\n      cmd LC_BUILD_VERSION\n  cmdsize 32\n platform 1\n    minos %s\n      sdk 26.5\n' \
            "$n" "$value"
          ;;
        legacy)
          printf 'Load command %d\n      cmd LC_VERSION_MIN_MACOSX\n  cmdsize 16\n  version %s\n      sdk 26.5\n' \
            "$n" "$value"
          ;;
      esac
      n=$((n + 1))
    done < "$file"
    ;;
  *) exit 2 ;;
esac
"""


class CheckMacosPortableDeps(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.work = Path(self.tmp.name)
        tools = self.work / "tools"
        tools.mkdir()
        otool = tools / "otool"
        otool.write_text(FAKE_OTOOL, encoding="utf-8")
        otool.chmod(otool.stat().st_mode | stat.S_IXUSR)
        self.env = dict(os.environ, PATH=f"{tools}{os.pathsep}{os.environ['PATH']}")
        self.stage = self.work / "dsd-neo-macos"
        (self.stage / "bin").mkdir(parents=True)
        (self.stage / "lib").mkdir()
        terminfo = self.stage / "share/terminfo/78"
        terminfo.mkdir(parents=True)
        (terminfo / "xterm-256color").write_text("entry\n", encoding="utf-8")
        (self.stage / "dsd-neo.sh").write_text(
            'export TERMINFO_DIRS="$DIR/share/terminfo:/usr/share/terminfo"\n', encoding="utf-8")
        self.write("bin/dsd-neo", "dep @rpath/libusb-1.0.0.dylib", "dep /usr/lib/libSystem.B.dylib",
                   "rpath @executable_path/../lib", "minos 26.0")
        self.write("lib/libusb-1.0.0.dylib", "dep @rpath/libusb-1.0.0.dylib", "rpath @loader_path", "minos 26.0")
        (self.stage / "dylibs-manifest.txt").write_text("libusb-1.0.0.dylib\n", encoding="utf-8")

    def tearDown(self):
        self.tmp.cleanup()

    def write(self, rel, *lines):
        (self.stage / rel).write_text("\n".join(lines) + "\n", encoding="utf-8")

    def run_check(self, *args):
        return subprocess.run(["bash", str(CHECK), str(self.stage), *args], env=self.env,
                              capture_output=True, text=True, check=False)

    def test_consistent_tree_passes(self):
        for args in ((), ("26.0",)):
            with self.subTest(args=args):
                result = self.run_check(*args)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn("check passed", result.stdout)

    def test_library_built_for_a_newer_macos_fails(self):
        self.write("lib/libusb-1.0.0.dylib", "dep @rpath/libusb-1.0.0.dylib", "rpath @loader_path", "minos 27.0")
        result = self.run_check("26.0")
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn("lib/libusb-1.0.0.dylib is built for macOS 27.0, not the deployment target 26.0",
                      result.stderr)

    def test_executable_built_for_an_older_macos_fails(self):
        self.write("bin/dsd-neo", "dep @rpath/libusb-1.0.0.dylib", "rpath @executable_path/../lib", "legacy 11.0")
        result = self.run_check("26.0")
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn("bin/dsd-neo is built for macOS 11.0, not the deployment target 26.0", result.stderr)

    def test_file_without_a_version_fails(self):
        self.write("lib/libusb-1.0.0.dylib", "dep @rpath/libusb-1.0.0.dylib", "rpath @loader_path")
        result = self.run_check("26.0")
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn("is built for macOS <unknown>", result.stderr)

    def test_missing_dependency_still_fails(self):
        self.write("bin/dsd-neo", "dep @rpath/libssl.3.dylib", "rpath @executable_path/../lib", "minos 26.0")
        result = self.run_check("26.0")
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn("loads @rpath/libssl.3.dylib but", result.stderr)


if __name__ == "__main__":
    unittest.main()
