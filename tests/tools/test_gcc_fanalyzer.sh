#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
#
# tools/gcc_fanalyzer.sh passed -fsyntax-only for as long as it existed. GCC's
# analyzer is an IPA pass and never runs under that flag, so the pre-push lane
# and the CI leg reported a clean run over an empty analysis, and the verdict
# regex separately missed the [-Werror=analyzer-...] spelling that --strict
# produces. Both failures are silent by nature - the script exits 0 and prints a
# summary - so the cases here drive the real script over a throwaway compilation
# database and check that a seeded defect is actually reported and counted.
set -euo pipefail

ROOT_DIR=$(git rev-parse --show-toplevel)

if ! command -v gcc > /dev/null 2>&1 || ! command -v python3 > /dev/null 2>&1; then
  echo "SKIP: gcc or python3 not available"
  exit 0
fi
if ! gcc -fanalyzer -S -o /dev/null -x c /dev/null > /dev/null 2>&1; then
  echo "SKIP: this gcc does not support -fanalyzer"
  exit 0
fi

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

REPO="$WORK/repo"
mkdir -p "$REPO/tools/lib"
git -C "$REPO" init -q
# The script resolves its root with git and sources the worker-sizing library
# from there, so the throwaway repository needs its own copy.
cp "$ROOT_DIR/tools/lib/jobs.sh" "$REPO/tools/lib/jobs.sh"

failures=0
fail() {
  echo "FAIL: $1" >&2
  failures=$((failures + 1))
}

# A double free the analyzer reports, and nothing an ordinary -Wall -Wextra
# -Wpedantic -Werror build would object to: --strict adds those, and a case that
# failed on a plain warning would prove nothing about the analyzer.
cat > "$REPO/seeded.c" << 'EOF'
#include <stdlib.h>

void seeded_double_free(void);

void
seeded_double_free(void) {
    char* p = (char*)malloc(16);
    if (p == NULL) {
        return;
    }
    free(p);
    free(p);
}
EOF

cat > "$REPO/clean.c" << 'EOF'
#include <stddef.h>

void clean_add(int* out, int a, int b);

void
clean_add(int* out, int a, int b) {
    if (out != NULL) {
        *out = a + b;
    }
}
EOF

# Leaks as plainly as seeded.c does: if C++ units ever stop being skipped, this
# case fails rather than quietly starting to analyze what GCC does not support.
cat > "$REPO/unit.cpp" << 'EOF'
#include <stdlib.h>

void cxx_leak();

void
cxx_leak() {
    char* p = (char*)malloc(16);
    (void)p;
}
EOF

cat > "$REPO/compile_commands.json" << EOF
[
  {"directory": "$REPO", "command": "gcc -c seeded.c -o seeded.o", "file": "seeded.c"},
  {"directory": "$REPO", "command": "gcc -c clean.c -o clean.o", "file": "clean.c"},
  {"directory": "$REPO", "command": "g++ -c unit.cpp -o unit.o", "file": "unit.cpp"}
]
EOF

# run_case NAME ARGS...: run the script inside the throwaway repository, leaving
# the exit status in rc and the output in $WORK/NAME.out.
rc=0
run_case() {
  local name="$1"
  shift
  rc=0
  (cd "$REPO" && "$ROOT_DIR/tools/gcc_fanalyzer.sh" "$@") > "$WORK/${name}.out" 2>&1 || rc=$?
}

# A seeded defect fails the run and is named in the output. Under -fsyntax-only
# this case passed with an empty analysis.
run_case seeded -- seeded.c
if [[ $rc -eq 0 ]]; then
  fail "a seeded double free did not fail the run"
fi
if ! grep -q -- "-Wanalyzer-double-free" "$WORK/seeded.out"; then
  fail "the double free was not reported"
fi
# A count, not exactly one: which diagnostics a given GCC reports for the same
# defect is its business, and the check is that they were counted at all.
if ! grep -qE "analyzer_diagnostics=[1-9]" "$WORK/seeded.out"; then
  fail "the summary did not count the diagnostic"
  cat "$WORK/seeded.out" >&2
fi

# --strict is how the pre-push hook and CI run it, and -Werror renames every
# diagnostic to [-Werror=analyzer-...]; the count has to survive that.
run_case strict --strict -- seeded.c
if [[ $rc -eq 0 ]]; then
  fail "--strict did not fail on a seeded double free"
fi
if ! grep -qE "analyzer_diagnostics=[1-9]" "$WORK/strict.out"; then
  fail "--strict did not count the diagnostic"
  cat "$WORK/strict.out" >&2
fi

# A clean unit still passes, so the cases above are not failing on the setup.
run_case clean --strict -- clean.c
if [[ $rc -ne 0 ]]; then
  fail "a clean translation unit failed the run"
  cat "$WORK/clean.out" >&2
fi

# GCC supports the analyzer on C only, so C++ units are skipped - and a skip of
# something the caller asked for reaches the pre-push gate as a NOTE line rather
# than passing quietly.
run_case cxx --strict -- unit.cpp
if [[ $rc -ne 0 ]]; then
  fail "a C++ translation unit was not skipped"
  cat "$WORK/cxx.out" >&2
fi
if ! grep -q "gcc-fanalyzer: NOTE: 1 requested C++ translation unit(s) were not analyzed" "$WORK/cxx.out"; then
  fail "the skipped C++ unit was not noted for the gate"
  cat "$WORK/cxx.out" >&2
fi
if ! grep -q "gcc-fanalyzer: NOTE: .*unit\.cpp" "$WORK/cxx.out"; then
  fail "the note did not name the skipped unit"
fi

if [[ $failures -ne 0 ]]; then
  echo "TOOLS_GCC_FANALYZER: $failures failure(s)" >&2
  exit 1
fi
echo "TOOLS_GCC_FANALYZER: OK"
