#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Print the float/double member names the Semgrep no-floating-point-equality
rule matches, as a regex alternation.

Semgrep cannot resolve a struct member's declared type across headers, so the
rule lists the scalar float/double members of the shared option, state, config,
demod and service structs by name. Run this after changing those structs and
paste the output into the $FLOAT_FIELD metavariable-regex in semgrep/dsd-neo.yml.
Names that are also declared with an integer type anywhere in the tree are
dropped so an int member with the same name does not trip the rule.
"""

import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HEADERS = (
    "include/dsd-neo/core/opts.h",
    "include/dsd-neo/core/state.h",
    "include/dsd-neo/runtime/config.h",
    "include/dsd-neo/dsp/demod_state.h",
    "src/app_control/services.h",
)
SCALAR = re.compile(r"^\s*(?:const\s+)?(?:std::atomic<)?(?:float|double)>?\s+([A-Za-z_][A-Za-z0-9_]*)\s*;", re.M)
INTEGER_TYPES = r"\b(?:int|unsigned|short|long|size_t|bool|char|u?int\d+_t)\s+"


def main() -> int:
    names = set()
    for header in HEADERS:
        names |= set(SCALAR.findall((ROOT / header).read_text()))
    files = subprocess.run(["git", "-C", str(ROOT), "ls-files", "include", "src", "apps", "tests", "android"],
                           capture_output=True, text=True, check=True).stdout.split()
    tree = "\n".join((ROOT / f).read_text(errors="ignore") for f in files
                     if f.endswith((".h", ".hpp", ".c", ".cpp")))
    kept = [n for n in sorted(names) if not re.search(INTEGER_TYPES + re.escape(n) + r"\b(?!\s*\()", tree)]
    sys.stdout.write("|".join(kept) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
