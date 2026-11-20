#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
#
# tools/check_test_tmp_leftovers.sh over a made-up build tree whose path has a
# space and glob characters in it: empty per-test folders pass; a leftover named
# 0, a hidden file, a folder, and a link (named, not followed) each fail naming
# their test; an entry that is not a folder fails; a missing test-tmp is an
# error. The check deletes nothing.
set -euo pipefail

ROOT_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
CHECK="$ROOT_DIR/tools/check_test_tmp_leftovers.sh"

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
BUILD="$WORK/build [debug] *x"
TMP="$BUILD/test-tmp"
failures=0

fail() {
  echo "FAIL: $*" >&2
  failures=$((failures + 1))
}

# run <want rc> <regex the output must match, or ""> : the check over BUILD.
run() {
  local want=$1 pattern=$2 out rc=0
  out=$("$CHECK" "$BUILD" 2>&1) || rc=$?
  if [[ $rc -ne $want ]]; then
    fail "exit $rc, expected $want: $out"
  elif [[ -n "$pattern" && ! "$out" =~ $pattern ]]; then
    fail "output does not match '$pattern': $out"
  fi
}

run 2 "does not exist"

mkdir -p "$TMP/CLEAN_A" "$TMP/CLEAN_B"
run 0 "2 test temp folders .* all empty"

touch "$TMP/CLEAN_A/0"
run 1 "CLEAN_A left 1 entries in its temp folder:"$'\n'"  0"
rm "$TMP/CLEAN_A/0"

touch "$TMP/CLEAN_B/.hidden"
mkdir "$TMP/CLEAN_A/sub dir"
touch "$TMP/CLEAN_A/sub dir/inner"
run 1 "CLEAN_A left 1 entries in its temp folder:"$'\n'"  sub dir"
run 1 "CLEAN_B left 1 entries in its temp folder:"$'\n'"  \.hidden"
[[ -e "$TMP/CLEAN_A/sub dir/inner" && -e "$TMP/CLEAN_B/.hidden" ]] || fail "the check deleted a leftover"
rm -r "$TMP/CLEAN_A/sub dir" "$TMP/CLEAN_B/.hidden"

mkdir "$WORK/outside"
touch "$WORK/outside/keep"
ln -s "$WORK/outside" "$TMP/CLEAN_A/link"
run 1 "CLEAN_A left 1 entries in its temp folder:"$'\n'"  link"
out=$("$CHECK" "$BUILD" 2>&1) || true
[[ "$out" != *keep* ]] || fail "the check followed the link: $out"
[[ -e "$WORK/outside/keep" && -L "$TMP/CLEAN_A/link" ]] || fail "the check deleted the link or its target"
rm "$TMP/CLEAN_A/link"

touch "$TMP/stray"
run 1 "stray in .* is not a test's folder"
rm "$TMP/stray"
ln -s "$WORK/outside" "$TMP/LINKED"
run 1 "LINKED in .* is not a test's folder"
rm "$TMP/LINKED"

run 0 "all empty"

if [[ $failures -ne 0 ]]; then
  echo "TOOLS_CHECK_TEST_TMP_LEFTOVERS: $failures failure(s)" >&2
  exit 1
fi
echo "TOOLS_CHECK_TEST_TMP_LEFTOVERS: OK"
