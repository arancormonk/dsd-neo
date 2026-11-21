#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
#
# tools/check_test_tmp_leftovers.sh over a made-up build tree whose path has a
# space and glob characters in it: empty per-test folders pass; a leftover named
# 0, a hidden file, a folder, and a link (named, not followed) each fail naming
# their test, and two tests' leftovers are each named under their own test; an
# entry that is not a folder, a linked test folder, a folder no test was given,
# a test folder the names file lists but that is missing, a walk that fails
# part-way, and a linked test-tmp each fail; a CR LF names file reads the same; a
# missing, empty or unreadable test-tmp or names file is an error. The check
# deletes nothing.
set -euo pipefail

ROOT_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
CHECK="$ROOT_DIR/tools/check_test_tmp_leftovers.sh"

WORK=$(mktemp -d)
trap 'chmod -R u+rwx "$WORK" 2> /dev/null || true; rm -rf "$WORK"' EXIT
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

mkdir -p "$BUILD"
run 2 "test-tmp is missing"
mkdir -p "$TMP/CLEAN_A" "$TMP/CLEAN_B" "$TMP/CLEAN_A_B"
run 2 "test-tmp.names is missing"
printf 'CLEAN_A\nCLEAN_A_B\nCLEAN_B\n' > "$BUILD/test-tmp.names"
run 0 "3 test temp folders .* all empty"

touch "$TMP/CLEAN_A/0"
run 1 "CLEAN_A left 1 entries in its temp folder:"$'\n'"  0"
rm "$TMP/CLEAN_A/0"

touch "$TMP/CLEAN_B/.hidden" "$TMP/CLEAN_A_B/b"
mkdir "$TMP/CLEAN_A/sub dir"
touch "$TMP/CLEAN_A/sub dir/inner"
run 1 "CLEAN_A left 1 entries in its temp folder:"$'\n'"  sub dir"$'\n'".*CLEAN_A_B left 1 entries in its temp folder:"$'\n'"  b"$'\n'".*CLEAN_B left 1 entries in its temp folder:"$'\n'"  \.hidden"
[[ -e "$TMP/CLEAN_A/sub dir/inner" && -e "$TMP/CLEAN_B/.hidden" ]] || fail "the check deleted a leftover"
rm -r "$TMP/CLEAN_A/sub dir" "$TMP/CLEAN_B/.hidden" "$TMP/CLEAN_A_B/b"

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

mv "$TMP/CLEAN_B" "$WORK/clean_b"
run 1 "1 of the folders .* are missing"
ln -s "$WORK/clean_b" "$TMP/CLEAN_B"
run 1 "CLEAN_B in .* is not a test's folder"
rm "$TMP/CLEAN_B"
mv "$WORK/clean_b" "$TMP/CLEAN_B"

# A walk that fails part-way fails the check rather than reading as empty: a find that lists one folder and then
# reports an error, whatever the user's privileges would let the real one read.
mkdir "$WORK/shim"
cat > "$WORK/shim/find" << 'SHIM'
#!/usr/bin/env bash
echo "$1/CLEAN_A"
echo "find: '$1/CLEAN_B': Permission denied" >&2
exit 1
SHIM
chmod +x "$WORK/shim/find"
out=$(PATH="$WORK/shim:$PATH" "$CHECK" "$BUILD" 2>&1) && rc=0 || rc=$?
[[ $rc -eq 1 && "$out" == *"could not be read completely"* && "$out" == *"Permission denied"* ]] ||
  fail "a failed walk did not fail the check (exit $rc): $out"

# A folder no test was given, even an empty one, is a leftover.
mkdir "$TMP/EXTRA"
run 1 "EXTRA in .* is not a registered test's folder"
rmdir "$TMP/EXTRA"

# A list written on Windows (CR LF) names the same folders.
printf 'CLEAN_A\r\nCLEAN_A_B\r\nCLEAN_B\r\n' > "$BUILD/test-tmp.names"
run 0 "3 test temp folders .* all empty"
# A list that cannot be read, or lists nothing, is an error, not an empty set.
: > "$BUILD/test-tmp.names"
run 2 "could not be read, or lists no tests"
printf 'CLEAN_A\nCLEAN_A_B\nCLEAN_B\n' > "$BUILD/test-tmp.names"
chmod 000 "$BUILD/test-tmp.names"
if [[ ! -r "$BUILD/test-tmp.names" ]]; then
  run 2 "could not be read, or lists no tests"
fi
chmod 644 "$BUILD/test-tmp.names"

mv "$TMP" "$WORK/real-tmp"
ln -s "$WORK/real-tmp" "$TMP"
run 2 "test-tmp is missing or not a folder"
rm "$TMP"
mv "$WORK/real-tmp" "$TMP"

run 0 "all empty"

if [[ $failures -ne 0 ]]; then
  echo "TOOLS_CHECK_TEST_TMP_LEFTOVERS: $failures failure(s)" >&2
  exit 1
fi
echo "TOOLS_CHECK_TEST_TMP_LEFTOVERS: OK"
