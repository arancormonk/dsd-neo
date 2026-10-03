#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
#
# tools/ci_changed_files.sh decides which files the pull-request jobs check, and a
# job whose list comes back empty skips its work and passes. Two ways the list
# came back wrong:
#  - git quotes a name with non-ASCII bytes, a quote or a tab unless asked for raw
#    (-z) output, and a quoted name matched no pattern and failed the existence
#    check, so the file dropped out of every list;
#  - a git diff that failed came back as an empty list with exit status 0.
# This builds a throwaway repository and checks each case.
set -euo pipefail

ROOT_DIR=$(git rev-parse --show-toplevel)
SCRIPT="$ROOT_DIR/tools/ci_changed_files.sh"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

cd "$WORK"
git init -q .
git config user.email test@example.invalid
git config user.name test
git config commit.gpgsign false
# The quoting case needs git's default quoting, whatever the global config says.
git config core.quotePath true

mkdir -p src/core docs
printf 'int base;\n' > src/core/base.c
printf 'notes\n' > docs/notes.md
git add -A
git commit -q -m base
base=$(git rev-parse HEAD)

# Odd names reach every list they belong to, raw.
odd=("src/core/caf"$'\xc3\xa9'".c" 'src/core/quote"name.c' "src/core/tab"$'\t'"name.c")
for name in "${odd[@]}"; do
  printf 'int x;\n' > "$name"
done
git add -A
git commit -q -m "odd names"
odd_head=$(git rev-parse HEAD)
GITHUB_OUTPUT="$WORK/odd.out" bash "$SCRIPT" --base "$base" --head "$odd_head" \
  --no-header-expansion --out-dir "$WORK/odd" > /dev/null
mapfile -t format_files < "$WORK/odd/format_files.txt"
mapfile -t semgrep_targets < "$WORK/odd/semgrep_targets.txt"
mapfile -t expected < <(printf '%s\n' "${odd[@]}" | sort -u)
[[ "${format_files[*]}" == "${expected[*]}" ]] || fail "format_files: ${format_files[*]}"
[[ "${semgrep_targets[*]}" == "${expected[*]}" ]] || fail "semgrep_targets: ${semgrep_targets[*]}"
grep -qx 'format_files=3' "$WORK/odd.out" || fail "format_files count: $(cat "$WORK/odd.out")"
echo "PASS raw names with non-ASCII, quote and tab"

# A diff that fails fails the helper. Both commits resolve, so it gets past its ref
# checks, but git diff rejects an invalid diff setting.
git config diff.context notanumber
if GITHUB_OUTPUT="$WORK/broken.out" bash "$SCRIPT" --base "$base" --head "$odd_head" \
  --no-header-expansion --out-dir "$WORK/broken" > /dev/null 2> "$WORK/broken.err"; then
  fail "a failing git diff exited 0: $(cat "$WORK/broken.out" 2> /dev/null)"
fi
grep -q 'git diff between' "$WORK/broken.err" || fail "no diff error reported: $(cat "$WORK/broken.err")"
[[ ! -s "$WORK/broken.out" ]] || fail "outputs written after a failing diff: $(cat "$WORK/broken.out")"
echo "PASS a failing diff fails the helper instead of reporting no changes"
