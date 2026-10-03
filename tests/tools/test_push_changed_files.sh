#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
#
# tools/push_changed_files.sh lists the paths a pushed ref changes for
# .githooks/pre-push. Computed inline with `git diff --name-only ... || true`, a
# remote SHA the clone did not have made the diff fail, which read as "nothing
# changed", so the hook exited without checking anything; and git's quoting of a
# name with non-ASCII bytes, quotes or tabs kept that file out of every check.
# This builds a throwaway repository whose history gives each way of finding the
# comparison a different answer, and checks each one, the raw names, a remote
# named like an option, and that a failing diff fails.
set -euo pipefail

ROOT_DIR=$(git rev-parse --show-toplevel)
SCRIPT="$ROOT_DIR/tools/push_changed_files.sh"
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
git config core.quotePath true

# c0 -> c1 (x.c) -> c2 (y.c) -> c3 (z.c and the odd names) is the pushed branch;
# side is a commit off c1 that only a remote branch has.
mkdir -p src
for f in keep x y z; do
  printf 'int %s;\n' "$f" > "src/$f.c"
done
git add -A
git commit -q -m c0
c0=$(git rev-parse HEAD)
printf 'int x1;\n' > src/x.c
git commit -q -am c1
c1=$(git rev-parse HEAD)
printf 'int side;\n' > src/side.c
git add -A
git commit -q -m side
side=$(git rev-parse HEAD)
git checkout -q --detach "$c1"
printf 'int y2;\n' > src/y.c
git commit -q -am c2
printf 'int z3;\n' > src/z.c
odd=("src/caf"$'\xc3\xa9'".c" 'src/quote"name.c' "src/tab"$'\t'"name.c")
for name in "${odd[@]}"; do
  printf 'int odd;\n' > "$name"
done
git add -A
git commit -q -m c3
head=$(git rev-parse HEAD)

zeros=0000000000000000000000000000000000000000
missing=1234567890abcdef1234567890abcdef12345678

# expect_paths LABEL REMOTE REMOTE_SHA PATH...: the script lists exactly these paths.
expect_paths() {
  local label="$1"
  local remote="$2"
  local remote_sha="$3"
  shift 3
  local want=()
  local got=()
  mapfile -t want < <(printf '%s\n' "$@" "${odd[@]}" | sort)
  bash "$SCRIPT" "$remote" refs/heads/topic "$head" "$remote_sha" > "$WORK/paths" 2> "$WORK/err" ||
    fail "$label: exit $?: $(cat "$WORK/err")"
  mapfile -d '' -t got < "$WORK/paths"
  mapfile -t got < <(printf '%s\n' "${got[@]}" | sort)
  [[ "${got[*]}" == "${want[*]}" ]] || fail "$label: got ${got[*]}"
  echo "PASS $label"
}

git update-ref refs/remotes/origin/main "$c0"
expect_paths "existing ref: compared with the remote's SHA" origin "$c1" src/y.c src/z.c
expect_paths "new ref: compared with the remote's default branch" origin "$zeros" src/x.c src/y.c src/z.c
expect_paths "remote SHA missing from the clone: compared with the default branch" origin "$missing" \
  src/x.c src/y.c src/z.c
grep -q 'is not in this clone' "$WORK/err" || fail "missing-SHA fallback not reported: $(cat "$WORK/err")"
expect_paths "a remote named -h is a remote, not a request for help" -h "$c1" src/y.c src/z.c

git update-ref -d refs/remotes/origin/main
git update-ref refs/remotes/origin/near "$side"
git update-ref refs/remotes/origin/far "$c0"
expect_paths "no default branch: compared with the nearest merge base" origin "$zeros" src/y.c src/z.c

git update-ref -d refs/remotes/origin/near
git update-ref -d refs/remotes/origin/far
expect_paths "no remote branches: compared with the empty tree" origin "$zeros" \
  src/keep.c src/x.c src/y.c src/z.c

git config diff.context notanumber
if bash "$SCRIPT" origin refs/heads/topic "$head" "$c1" > "$WORK/paths" 2> "$WORK/err"; then
  fail "a failing git diff exited 0"
fi
grep -q 'failed for refs/heads/topic' "$WORK/err" || fail "no diff error reported: $(cat "$WORK/err")"
echo "PASS a failing diff fails instead of listing nothing"
