#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
#
# .githooks/pre-push hands its checks one path per line, so a pushed name with a
# newline in it would split into two paths that do not exist: skipped with a
# warning for a ref that is not checked out, or reported as local deletions for
# the one that is. The hook must refuse the push and say why. It must also leave
# nothing behind in the temporary directory. This runs the hook, with tools/push_changed_files.sh, in a
# throwaway repository; both cases end before the hook loads its check runner.
set -euo pipefail

ROOT_DIR=$(git rev-parse --show-toplevel)
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

repo="$WORK/repo"
hook_tmp="$WORK/tmp"
mkdir -p "$repo/.githooks" "$repo/tools" "$hook_tmp"
cp "$ROOT_DIR/.githooks/pre-push" "$repo/.githooks/pre-push"
cp "$ROOT_DIR/tools/push_changed_files.sh" "$repo/tools/push_changed_files.sh"
cd "$repo"
git init -q .
git config user.email test@example.invalid
git config user.name test
git config commit.gpgsign false
printf 'int base;\n' > base.c
git add -A
git commit -q -m base
base=$(git rev-parse HEAD)

# run_hook LOCAL_SHA REMOTE_SHA: the hook's exit status, with its output in $WORK/out.
run_hook() {
  local status=0
  printf 'refs/heads/topic %s refs/heads/topic %s\n' "$1" "$2" |
    TMPDIR="$hook_tmp" bash .githooks/pre-push origin > "$WORK/out" 2>&1 || status=$?
  return "$status"
}

run_hook "$base" "$base" || fail "a push with nothing to check failed: $(cat "$WORK/out")"
[[ -z "$(ls -A "$hook_tmp")" ]] || fail "temporary files left behind: $(ls -A "$hook_tmp")"
echo "PASS a push with nothing to check passes and leaves no temporary file"

printf 'int newline;\n' > "line"$'\n'"break.c"
git add -A
git commit -q -m newline
if run_hook "$(git rev-parse HEAD)" "$base"; then
  fail "a pushed name with a newline went through: $(cat "$WORK/out")"
fi
grep -q 'path with a newline in it' "$WORK/out" || fail "newline not reported: $(cat "$WORK/out")"
[[ -z "$(ls -A "$hook_tmp")" ]] || fail "temporary files left behind: $(ls -A "$hook_tmp")"
echo "PASS a pushed name with a newline is refused"
