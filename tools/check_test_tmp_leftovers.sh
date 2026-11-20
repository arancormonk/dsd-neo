#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
#
# Usage: tools/check_test_tmp_leftovers.sh [BUILD_DIR]   (default: build/dev-debug)
#
# Run after ctest. Every test runs with a temp folder of its own,
# BUILD_DIR/test-tmp/<TEST> (cmake/TestTmpdir.cmake), and must remove what it
# creates there. This lists each folder one level deep, without following
# links, and fails naming every test whose folder is not empty, with what it
# left. It never deletes anything: the folders stay for a look, and ctest's next
# run empties them before its first test (tests/cmake/TestTmpdirPrepare.cmake).
set -euo pipefail

build_dir=${1:-build/dev-debug}
root="$build_dir/test-tmp"

if [[ ! -d "$root" ]]; then
  echo "check_test_tmp_leftovers: $root does not exist; run ctest in $build_dir first" >&2
  exit 2
fi

failed=0
tests=0
while IFS= read -r -d '' entry; do
  name=${entry##*/}
  if [[ -L "$entry" || ! -d "$entry" ]]; then
    echo "check_test_tmp_leftovers: $name in $root is not a test's folder" >&2
    failed=1
    continue
  fi
  tests=$((tests + 1))
  if ! left=$(find "$entry" -mindepth 1 -maxdepth 1 -print 2>&1); then
    echo "check_test_tmp_leftovers: $name: its folder could not be read: $left" >&2
    failed=1
    continue
  fi
  if [[ -n "$left" ]]; then
    count=$(printf '%s\n' "$left" | wc -l | tr -d ' ')
    echo "check_test_tmp_leftovers: $name left $count entries in its temp folder:" >&2
    shown=0
    while IFS= read -r line && [[ $shown -lt 20 ]]; do
      echo "  ${line#"$entry/"}" >&2
      shown=$((shown + 1))
    done <<< "$left"
    failed=1
  fi
done < <(find "$root" -mindepth 1 -maxdepth 1 -print0)

if [[ $failed -ne 0 ]]; then
  echo "check_test_tmp_leftovers: a test must remove every temp file and folder it creates; see docs/testing.md, \"Temporary files and directories\"" >&2
  exit 1
fi
echo "check_test_tmp_leftovers: $tests test temp folders in $root, all empty"
