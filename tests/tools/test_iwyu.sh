#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
#
# tools/iwyu.sh decides from IWYU's output whether a translation unit compiled,
# and for as long as the check existed it could not tell: its pattern sat in a
# quoted heredoc as "(^|\\s)error:", which Python reads as a literal backslash
# before the s, so only output that began with "error:" counted. A clang
# diagnostic begins with its location ("file.c:1:2: error:"), and IWYU 0.27
# exits 0 after one and goes on to analyse the unit, so a unit that did not
# compile under IWYU's clang passed the strict gate as clean. A fake
# include-what-you-use on PATH prints canned output in the shapes the real one
# produces, and each case checks how the script classifies it.
set -euo pipefail

ROOT_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)

if ! command -v git > /dev/null 2>&1 || ! command -v python3 > /dev/null 2>&1; then
  echo "SKIP: git or python3 not available"
  exit 0
fi

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

REPO="$WORK/repo"
mkdir -p "$REPO/tools/lib" "$WORK/bin"
git -C "$REPO" init -q
# The script resolves its root with git, sources the worker-sizing library from
# there, and refuses to run without the C-headers mapping, so the throwaway
# repository needs its own copies. The fake never reads the mapping.
cp "$ROOT_DIR/tools/lib/jobs.sh" "$REPO/tools/lib/jobs.sh"
cp "$ROOT_DIR/tools/iwyu-c-headers.imp" "$REPO/tools/iwyu-c-headers.imp"

# Fake IWYU: picks its canned output by the unit's name. Each error case exits 0,
# which is what IWYU 0.27 does after a -Werror diagnostic, so the verdict has to
# come from the text. Suggestions exit 1 under --error=1, as the real one does.
cat > "$WORK/bin/include-what-you-use" << 'FAKE'
#!/usr/bin/env bash
set -euo pipefail
unit=""
error_flag=0
# IWYU 0.26 and earlier do not list --use_c_headers (added in 0.27) and reject
# it as an unrecognized option. FAKE_IWYU_ARGS_LOG records each unit's options.
for arg in "$@"; do
  if [[ "$arg" == "--help" ]]; then
    echo "   --mapping_file=<filename>: gives iwyu a mapping file."
    if [[ "${FAKE_IWYU_HAS_C_HEADERS:-1}" == 1 ]]; then
      echo "   --use_c_headers: suggest C standard library headers in C++ mode"
    fi
    exit 0
  fi
done
if [[ -n "${FAKE_IWYU_ARGS_LOG:-}" ]]; then
  printf '%s\n' "$*" >> "$FAKE_IWYU_ARGS_LOG"
fi
for arg in "$@"; do
  case "$arg" in
    --use_c_headers)
      if [[ "${FAKE_IWYU_HAS_C_HEADERS:-1}" != 1 ]]; then
        echo "error: unknown argument: '--use_c_headers'" >&2
        exit 1
      fi
      ;;
    *.c) unit=$(basename "$arg") ;;
    --error=1) error_flag=1 ;;
  esac
done
case "$unit" in
  werror.c)
    cat << 'OUT'
werror.c:1:2: error: unused variable 'u' [-Werror,-Wunused-variable]
    1 |  int u;
      |  ^

(werror.c has correct #includes/fwd-decls)
OUT
    ;;
  header.c)
    cat << 'OUT'
In file included from header.c:1:
include/bad.h:2:10: fatal error: 'gone.h' file not found
    2 | #include "gone.h"
      |          ^~~~~~~~

(header.c has correct #includes/fwd-decls)
OUT
    ;;
  cmdline.c)
    cat << 'OUT'
In file included from <built-in>:417:
<command line>:1:12: error: invalid token in macro parameter list
    1 | #define X( 1
      |            ^

(cmdline.c has correct #includes/fwd-decls)
OUT
    ;;
  driver.c)
    cat << 'OUT'
error: unknown argument: '-mno-such-option'

(driver.c has correct #includes/fwd-decls)
OUT
    ;;
  suggest.c)
    # The word "error" in a symbol comment, an include path and a removal line.
    cat << 'OUT'

suggest.c should add these lines:
#include "fec/error_correction.h"  // for error::code, error::kind

suggest.c should remove these lines:
- #include <error.h>  // lines 3-3

The full include-list for suggest.c:
#include "fec/error_correction.h"  // for error::code, error::kind
#include <stdio.h>                 // for printf
---
OUT
    exit "$error_flag"
    ;;
  clean.c)
    echo "(clean.c has correct #includes/fwd-decls)"
    ;;
  *)
    echo "fake include-what-you-use: unexpected unit '$unit'" >&2
    exit 99
    ;;
esac
exit 0
FAKE
chmod +x "$WORK/bin/include-what-you-use"

units=(werror header cmdline driver suggest clean)
{
  echo "["
  sep=""
  for u in "${units[@]}"; do
    : > "$REPO/$u.c"
    printf '%s  {"directory": "%s", "command": "cc -c %s.c -o %s.o", "file": "%s.c"}' \
      "$sep" "$REPO" "$u" "$u" "$u"
    sep=$',\n'
  done
  printf '\n]\n'
} > "$REPO/compile_commands.json"

failures=0
fail() {
  echo "FAIL: $1" >&2
  failures=$((failures + 1))
}

# run_case NAME ARGS...: run the script inside the throwaway repository with the
# fake first on PATH, leaving the exit status in rc and the output in
# $WORK/NAME.out.
rc=0
run_case() {
  local name="$1"
  shift
  rc=0
  (cd "$REPO" && PATH="$WORK/bin:$PATH" "$ROOT_DIR/tools/iwyu.sh" --jobs 1 "$@") > "$WORK/${name}.out" 2>&1 || rc=$?
}

# expect_compile_error UNIT ARGS...: the unit fails the run, is counted and named
# as a compile failure rather than as a suggestion, and its diagnostic is shown.
expect_compile_error() {
  local unit="$1" diagnostic="$2" before=$failures
  shift 2
  run_case "$unit" "$@" -- "$unit.c"
  if [[ $rc -eq 0 ]]; then
    fail "$unit.c ($*): a compile error passed the run"
  fi
  if ! grep -qE "IWYU summary: analyzed=1 suggested=0 compile_errors=1 fatal=1" "$WORK/$unit.out"; then
    fail "$unit.c ($*): the summary did not count one compile error and no suggestion"
  fi
  if ! grep -qF "iwyu: ERROR: $unit.c did not compile under IWYU" "$WORK/$unit.out"; then
    fail "$unit.c ($*): the unit was not named as a compile failure"
  fi
  if ! grep -qF -- "$diagnostic" "$WORK/$unit.out"; then
    fail "$unit.c ($*): the diagnostic was not shown"
  fi
  if [[ $failures -ne $before ]]; then
    cat "$WORK/$unit.out" >&2
  fi
}

# The case that passed before: a located error, exit status 0, a clean analysis.
expect_compile_error werror "werror.c:1:2: error: unused variable 'u'" --strict
# A compile error fails the run without --strict too.
expect_compile_error werror "werror.c:1:2: error: unused variable 'u'"
# A fatal error, located in a header the unit includes.
expect_compile_error header "include/bad.h:2:10: fatal error: 'gone.h' file not found" --strict
# A location that is not a file path and has a space in it.
expect_compile_error cmdline "<command line>:1:12: error: invalid token" --strict
# The driver's bare "error:" at the start of a line.
expect_compile_error driver "error: unknown argument: '-mno-such-option'" --strict

# Suggestions fail --strict as suggestions. The word "error" in the symbol
# comments and the include paths is not a diagnostic.
run_case suggest_strict --strict -- suggest.c
if [[ $rc -eq 0 ]]; then
  fail "suggest.c (--strict): suggestions passed the run"
fi
if ! grep -qE "IWYU summary: analyzed=1 suggested=1 compile_errors=0 fatal=1" "$WORK/suggest_strict.out"; then
  fail "suggest.c (--strict): not counted as one suggestion and no compile error"
  cat "$WORK/suggest_strict.out" >&2
fi
if grep -qF "did not compile under IWYU" "$WORK/suggest_strict.out"; then
  fail "suggest.c (--strict): suggestions were reported as a compile failure"
fi

# Without --strict, suggestions are advice and the run passes.
run_case suggest -- suggest.c
if [[ $rc -ne 0 ]]; then
  fail "suggest.c: suggestions failed a run without --strict"
  cat "$WORK/suggest.out" >&2
fi
if ! grep -qE "IWYU summary: analyzed=1 suggested=1 compile_errors=0 fatal=0" "$WORK/suggest.out"; then
  fail "suggest.c: not counted as one non-fatal suggestion"
fi

# A clean unit passes --strict, so the cases above are not failing on the setup.
run_case clean --strict -- clean.c
if [[ $rc -ne 0 ]]; then
  fail "clean.c (--strict): a clean unit failed the run"
  cat "$WORK/clean.out" >&2
fi
if ! grep -qE "IWYU summary: analyzed=1 suggested=0 compile_errors=0 fatal=0" "$WORK/clean.out"; then
  fail "clean.c (--strict): the summary was not clean"
fi

# --use_c_headers exists only in IWYU 0.27 and later. A fake whose --help lists
# it is passed the option and the C-headers mapping; one that does not is passed
# neither and the unit analyses clean instead of failing on an unknown option.
for has in 1 0; do
  log="$WORK/args_$has.log"
  : > "$log"
  rc=0
  (cd "$REPO" && FAKE_IWYU_HAS_C_HEADERS=$has FAKE_IWYU_ARGS_LOG="$log" PATH="$WORK/bin:$PATH" \
    "$ROOT_DIR/tools/iwyu.sh" --jobs 1 --strict -- clean.c) > "$WORK/headers_$has.out" 2>&1 || rc=$?
  if [[ $rc -ne 0 ]]; then
    fail "clean.c (--strict, c-headers support=$has): the run failed"
    cat "$WORK/headers_$has.out" >&2
  fi
  if [[ ! -s "$log" ]]; then
    fail "clean.c (c-headers support=$has): the fake was never run on the unit"
  fi
  if [[ $has -eq 1 ]]; then
    grep -qF -- "--use_c_headers" "$log" || fail "an IWYU that lists --use_c_headers was not passed it"
    grep -qF -- "iwyu-c-headers.imp" "$log" || fail "an IWYU that lists --use_c_headers was not passed the C-headers mapping"
  else
    if grep -qF -- "--use_c_headers" "$log"; then
      fail "an IWYU without --use_c_headers was passed it"
    fi
    if grep -qF -- "iwyu-c-headers.imp" "$log"; then
      fail "an IWYU without --use_c_headers was passed the C-headers mapping"
    fi
  fi
done

# One run over everything: each failing unit is counted once, in its own class.
run_case all --strict
if [[ $rc -eq 0 ]]; then
  fail "a run over every unit passed"
fi
if ! grep -qE "IWYU summary: analyzed=6 suggested=1 compile_errors=4 fatal=5" "$WORK/all.out"; then
  fail "a run over every unit miscounted its failures"
  cat "$WORK/all.out" >&2
fi

if [[ $failures -ne 0 ]]; then
  echo "TOOLS_IWYU: $failures failure(s)" >&2
  exit 1
fi
echo "TOOLS_IWYU: OK"
