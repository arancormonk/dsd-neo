#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
#
# tools/ci_changed_files.sh decides which files the pull-request jobs check, and a
# job whose list comes back empty skips its work and passes. Three ways the list
# came back wrong:
#  - git quotes a name with non-ASCII bytes, a quote or a tab unless asked for raw
#    (-z) output, and a quoted name matched no pattern and failed the existence
#    check, so the file dropped out of every list;
#  - a git diff that failed came back as an empty list with exit status 0;
#  - the lists were written to .ci/changed-files/ inside the checkout by
#    following any symlink the pull request had committed there, so a link to
#    /dev/null emptied a list (clang-format then checked nothing); and a
#    directory there must fail rather than swallow the list.
# A name with a newline in it cannot be carried by the line-based lists, so the
# helper must fail on it rather than drop it.
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

# Planted symlinks in the default output directory do not empty the lists.
git checkout -q --detach "$base"
mkdir -p .ci/changed-files
for list in format_files.txt semgrep_targets.txt; do
  ln -s /dev/null ".ci/changed-files/$list"
done
printf 'int planted;\n' > src/core/base.c
git add -A
git commit -q -m "plant links"
planted=$(git rev-parse HEAD)
GITHUB_OUTPUT="$WORK/planted.out" bash "$SCRIPT" --base "$base" --head "$planted" \
  --no-header-expansion > /dev/null
for list in format_files.txt semgrep_targets.txt; do
  [[ ! -L ".ci/changed-files/$list" ]] || fail "$list is still a symlink"
  grep -qx 'src/core/base.c' ".ci/changed-files/$list" || fail "$list lost src/core/base.c"
done
echo "PASS symlinks planted in the output directory do not empty the lists"
git checkout -q -f --detach "$base"
git clean -q -fdx

# A directory, or a link to one, where a list goes fails the helper: the rename
# would put the list inside it, and the path would read as an empty list.
expect_refused() {
  local label="$1"
  local head="$2"
  if GITHUB_OUTPUT="$WORK/refused.out" bash "$SCRIPT" --base "$base" --head "$head" \
    --no-header-expansion > /dev/null 2> "$WORK/refused.err"; then
    fail "$label: exited 0"
  fi
  [[ ! -s "$WORK/refused.out" ]] || fail "$label: outputs written: $(cat "$WORK/refused.out")"
  echo "PASS $label"
}
mkdir -p .ci/changed-files/format_files.txt
printf 'x\n' > .ci/changed-files/format_files.txt/keep
printf 'int dir_case;\n' > src/core/base.c
git add -A
git commit -q -m "directory at a list path"
expect_refused "a directory at a list path fails the helper" "$(git rev-parse HEAD)"
grep -q 'is a directory' "$WORK/refused.err" || fail "directory not reported: $(cat "$WORK/refused.err")"
git checkout -q -f --detach "$base"
git clean -q -fdx
mkdir -p .ci/changed-files elsewhere
printf 'x\n' > elsewhere/keep
ln -s ../../elsewhere .ci/changed-files/format_files.txt
printf 'int dir_link_case;\n' > src/core/base.c
git add -A
git commit -q -m "link to a directory at a list path"
expect_refused "a link to a directory at a list path fails the helper" "$(git rev-parse HEAD)"
git checkout -q -f --detach "$base"
git clean -q -fdx

# A changed name with a newline in it fails the helper.
newline_name="src/core/line"$'\n'"break.c"
printf 'int newline;\n' > "$newline_name"
git add -A
git commit -q -m "newline name"
expect_refused "a changed name with a newline fails the helper" "$(git rev-parse HEAD)"
grep -q 'contains a newline' "$WORK/refused.err" || fail "newline not reported: $(cat "$WORK/refused.err")"
git checkout -q -f --detach "$base"
git clean -q -fdx

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
