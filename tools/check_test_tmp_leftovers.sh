#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
#
# Usage: tools/check_test_tmp_leftovers.sh [BUILD_DIR]   (default: build/dev-debug)
#
# Run after ctest. Every test runs with a temp folder of its own,
# BUILD_DIR/test-tmp/<TEST> (cmake/TestTmpdir.cmake), and must remove what it
# creates there. This walks test-tmp two levels deep in one traversal, without
# following links, and fails naming every test whose folder is not empty, with
# what it left. It fails closed: an unreadable folder, a linked test-tmp or test
# folder, an entry that is not a test's folder, or a test folder the last
# configure listed (BUILD_DIR/test-tmp.names) that is missing, are each a
# failure. It never deletes anything: the folders stay for a look, and ctest's
# next run empties them before its first test (tests/cmake/TestTmpdirPrepare.cmake).
set -euo pipefail

build_dir=${1:-build/dev-debug}
root="$build_dir/test-tmp"
names="$build_dir/test-tmp.names"

say() {
  echo "check_test_tmp_leftovers: $*" >&2
}

if [[ -L "$root" || ! -d "$root" ]]; then
  say "$root is missing or not a folder; run ctest in $build_dir first"
  exit 2
fi
if [[ ! -f "$names" ]]; then
  say "$names is missing; configure $build_dir first"
  exit 2
fi

# One walk; its errors (an unreadable folder) fail the check. Names with a newline in them are not expected here.
if ! listing=$(find "$root" -mindepth 1 -maxdepth 2 -print 2>&1); then
  say "$root could not be read completely:"
  printf '%s\n' "$listing" >&2
  exit 1
fi

failed=0
folders=""
current=""
current_count=0
report_current() {
  if [[ $current_count -gt 0 ]]; then
    say "$current left $current_count entries in its temp folder:"
    printf '%s' "$current_left" >&2
    failed=1
  fi
}
# Sorted, what is in one test's folder comes together ("<TEST>/" is a prefix no other path has). Plain bash 3.2 and
# POSIX tools: macOS ships that bash.
while IFS= read -r path; do
  [[ -n "$path" ]] || continue
  rel=${path#"$root/"}
  test_name=${rel%%/*}
  if [[ "$rel" == "$test_name" ]]; then
    folders+="$test_name"$'\n'
    if [[ -L "$path" || ! -d "$path" ]]; then
      say "$test_name in $root is not a test's folder"
      failed=1
    fi
    continue
  fi
  if [[ "$test_name" != "$current" ]]; then
    report_current
    current=$test_name
    current_count=0
    current_left=""
  fi
  current_count=$((current_count + 1))
  if [[ $current_count -le 20 ]]; then
    current_left+="  ${rel#*/}"$'\n'
  fi
done < <(printf '%s\n' "$listing" | LC_ALL=C sort)
report_current

missing=$(LC_ALL=C comm -23 <(grep -v '^$' "$names" | LC_ALL=C sort -u) <(printf '%s' "$folders" | LC_ALL=C sort -u) | wc -l | tr -d ' ')
if [[ $missing -gt 0 ]]; then
  say "$missing of the folders $names lists are missing from $root; run ctest again to make them"
  failed=1
fi

if [[ $failed -ne 0 ]]; then
  say "a test must remove every temp file and folder it creates; see docs/testing.md, \"Temporary files and directories\""
  exit 1
fi
echo "check_test_tmp_leftovers: $(printf '%s' "$folders" | grep -c .) test temp folders in $root, all empty"
