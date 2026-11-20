#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""tests/y_call_hold_check.py refuses a selection that runs nothing.

--sets and --hangtime narrow the default legs, and a combination outside them (or a misspelled set or call) used to
select no case at all, so the script exited 0 without starting the replay host: a validation that checked nothing
passed. A missing host or fixture directory must fail too, not count as a run. Stdlib only; runs no host.
"""

import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(os.environ.get("DSD_TEST_SOURCE_ROOT", Path(__file__).resolve().parents[2]))
SCRIPT = ROOT / "tests" / "y_call_hold_check.py"


def run_check(*args):
    """The check's exit code and combined output for @p args."""
    result = subprocess.run([sys.executable, str(SCRIPT), *args], capture_output=True, text=True, timeout=60,
                            check=False)
    return result.returncode, result.stdout + result.stderr


class EmptySelection(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="dsd-y-call-hold-check-")
        self.host = Path(self.temporary.name) / "host"
        self.host.write_text("not run\n", encoding="utf-8")
        self.fixtures = Path(self.temporary.name)

    def tearDown(self):
        self.temporary.cleanup()

    def test_no_default_leg_is_refused(self):
        code, out = run_check("--sets", "fa", "--hangtime", "2", str(self.host), str(self.fixtures))
        self.assertNotEqual(code, 0, out)
        self.assertIn("--all-legs", out)

    def test_unknown_set_is_refused(self):
        code, out = run_check("--sets", "fx", str(self.host), str(self.fixtures))
        self.assertNotEqual(code, 0, out)
        self.assertIn("fx", out)

    def test_unknown_call_is_refused(self):
        code, out = run_check(str(self.host), str(self.fixtures), "no_such_call")
        self.assertNotEqual(code, 0, out)
        self.assertIn("no_such_call", out)

    def test_missing_host_is_refused(self):
        code, out = run_check(str(self.fixtures / "missing-host"), str(self.fixtures), "dpmr")
        self.assertNotEqual(code, 0, out)
        self.assertIn("no replay host", out)

    def test_missing_fixtures_are_refused(self):
        code, out = run_check(str(self.host), str(self.fixtures / "missing"), "dpmr")
        self.assertNotEqual(code, 0, out)
        self.assertIn("no fixture directory", out)


if __name__ == "__main__":
    unittest.main()
