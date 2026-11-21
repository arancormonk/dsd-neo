#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""tools/check_macos_portable_deps.sh on a fabricated portable tree.

A fake otool on PATH reads each "Mach-O" file as text: "dep <install name>",
"rpath <path>" and "minos <version>" lines stand for its load commands. A fake
uname reports Darwin, so every host runs the checker's macOS path, and a fake
codesign accepts every file but one with a "bad-signature" line, so the tree
needs no real binaries. A fake infocmp, which DSD_MACOS_INFOCMP names, reads
the terminal database's entries. The fakes are written with LF line ends,
which their shebang lines need.
"""

import os
import stat
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(os.environ.get("DSD_TEST_SOURCE_ROOT", Path(__file__).resolve().parents[2]))
CHECK = ROOT / "tools/check_macos_portable_deps.sh"
# The bash ctest found (macOS's /bin/bash is 3.2); plain bash from PATH otherwise.
BASH = os.environ.get("DSD_TEST_BASH", "bash")

FAKE_OTOOL = r"""#!/usr/bin/env bash
set -euo pipefail
mode=$1
file=$2
case "$mode" in
  -L)
    echo "$file:"
    while read -r kind value; do
      if [[ "$kind" == dep ]]; then
        printf '\t%s (compatibility version 1.0.0, current version 1.0.0)\n' "$value"
      fi
    done < "$file"
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
        later)
          # Far more load commands than a pipe and a reader's read-ahead hold,
          # so a reader that leaves early ends this with SIGPIPE every time.
          printf 'Load command %d\n' "$n"
          awk 'BEGIN { for (i = 0; i < 50000; i++) print "      cmd LC_SOURCE_VERSION" }'
          ;;
      esac
      n=$((n + 1))
    done < "$file"
    ;;
  *) exit 2 ;;
esac
"""

# codesign --verify --strict --verbose=2 <file>, as the checker calls it.
FAKE_CODESIGN = r"""#!/usr/bin/env bash
set -euo pipefail
[[ $# -eq 4 && "$1" == --verify && "$2" == --strict && "$3" == --verbose=2 ]] || exit 2
if grep -qx bad-signature "$4"; then
  echo "$4: invalid signature (code or signature have been modified)" >&2
  exit 1
fi
"""

FAKE_UNAME = """#!/bin/sh
echo Darwin
"""

# infocmp -A <dir> <name>, as the checker calls it: like the real one, it reads
# <dir>/<first letter in hex>/<name> and nothing else, and refuses a file that
# holds no entry (here, an empty one).
FAKE_INFOCMP = r"""#!/usr/bin/env bash
set -euo pipefail
[[ $# -eq 3 && "$1" == -A ]] || exit 2
entry="$2/$(printf '%02x' "'${3:0:1}")/$3"
if [[ ! -s "$entry" ]]; then
  echo "infocmp: couldn't open terminfo file $entry." >&2
  exit 1
fi
echo "$3|fake entry,"
"""


class CheckMacosPortableDeps(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.work = Path(self.tmp.name)
        tools = self.work / "tools"
        tools.mkdir()
        for name, script in (("otool", FAKE_OTOOL), ("codesign", FAKE_CODESIGN), ("uname", FAKE_UNAME),
                             ("fake-infocmp", FAKE_INFOCMP)):
            tool = tools / name
            tool.write_bytes(script.encode("utf-8"))
            tool.chmod(tool.stat().st_mode | stat.S_IXUSR)
        self.env = dict(os.environ, PATH=f"{tools}{os.pathsep}{os.environ['PATH']}",
                        DSD_MACOS_INFOCMP=str(tools / "fake-infocmp"))
        self.stage = self.work / "dsd-neo-macos"
        (self.stage / "bin").mkdir(parents=True)
        (self.stage / "lib").mkdir()
        for entry in ("78/xterm-256color", "74/tmux-256color"):
            self.write(f"share/terminfo/{entry}", "compiled entry")
        (self.stage / "dsd-neo.sh").write_bytes(
            b'export TERMINFO_DIRS="$DIR/share/terminfo:/usr/share/terminfo"\n')
        self.write("bin/dsd-neo", "dep @rpath/libusb-1.0.0.dylib", "dep /usr/lib/libSystem.B.dylib",
                   "rpath @executable_path/../lib", "minos 26.0")
        self.write("lib/libusb-1.0.0.dylib", "dep @rpath/libusb-1.0.0.dylib", "rpath @loader_path", "minos 26.0")
        (self.stage / "dylibs-manifest.txt").write_bytes(b"libusb-1.0.0.dylib\n")

    def tearDown(self):
        self.tmp.cleanup()

    def write(self, rel, *lines):
        (self.stage / rel).parent.mkdir(parents=True, exist_ok=True)
        (self.stage / rel).write_bytes(("\n".join(lines) + "\n").encode("utf-8"))

    def run_check(self, *args):
        return subprocess.run([BASH, str(CHECK), str(self.stage), *args], env=self.env,
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

    def test_version_before_later_load_commands_is_read(self):
        # otool goes on writing long after the version's load command; the
        # check reads it all, so the pipe does not end otool early.
        self.write("bin/dsd-neo", "dep @rpath/libusb-1.0.0.dylib", "dep /usr/lib/libSystem.B.dylib",
                   "rpath @executable_path/../lib", "minos 26.0", "later 0.0")
        self.write("lib/libusb-1.0.0.dylib", "dep @rpath/libusb-1.0.0.dylib", "rpath @loader_path",
                   "legacy 26.0", "later 0.0")
        result = self.run_check("26.0")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("check passed", result.stdout)

    def test_file_without_a_version_fails(self):
        self.write("lib/libusb-1.0.0.dylib", "dep @rpath/libusb-1.0.0.dylib", "rpath @loader_path")
        result = self.run_check("26.0")
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn("is built for macOS <unknown>", result.stderr)

    def test_invalid_signature_fails(self):
        self.write("lib/libusb-1.0.0.dylib", "dep @rpath/libusb-1.0.0.dylib", "rpath @loader_path", "minos 26.0",
                   "bad-signature")
        result = self.run_check("26.0")
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn("lib/libusb-1.0.0.dylib has an invalid code signature", result.stderr)

    def test_empty_terminfo_entry_fails(self):
        (self.stage / "share/terminfo/78/xterm-256color").write_bytes(b"")
        result = self.run_check("26.0")
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn("bundled terminfo database has no readable xterm-256color entry", result.stderr)

    def test_missing_tmux_terminfo_entry_fails(self):
        (self.stage / "share/terminfo/74/tmux-256color").unlink()
        result = self.run_check("26.0")
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn("bundled terminfo database has no readable tmux-256color entry", result.stderr)

    def test_missing_dependency_still_fails(self):
        self.write("bin/dsd-neo", "dep @rpath/libssl.3.dylib", "rpath @executable_path/../lib", "minos 26.0")
        result = self.run_check("26.0")
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn("loads @rpath/libssl.3.dylib but", result.stderr)


if __name__ == "__main__":
    unittest.main()
