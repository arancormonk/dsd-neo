#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
#
# .githooks/pre-push hands its checks one path per line, so a pushed name with a
# newline in it would split into two paths that do not exist: skipped with a
# warning for a ref that is not checked out, or reported as local deletions for
# the one that is. The hook must refuse the push and say why. It must also leave
# nothing behind in the temporary directory. A change to a pinned or vendored
# dependency, a deletion included, must run the per-TU analyzers over the whole
# tree, and reconfigure first so a changed pin is refetched. This runs the hook,
# with tools/push_changed_files.sh, in a throwaway repository; every case but the
# last two ends before the hook loads its check runner.
set -euo pipefail

ROOT_DIR=$(git rev-parse --show-toplevel)
# The default-tree cases must not inherit the caller's analysis tree; the one case
# that needs another sets it itself.
unset DSD_ANALYSIS_BUILD_DIR
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

# A change to a pinned or vendored dependency runs the per-TU analyzers over the
# whole tree, as tools/ci_changed_files.sh asks of CI; an ordinary source change
# does not. The hook prints its scope before it starts the checks, and this
# repository has no check runner, so it stops right after that line.
git checkout -q --detach "$base"
printf 'int edited;\n' > base.c
git commit -q -am 'ordinary source change'
run_hook "$(git rev-parse HEAD)" "$base" || true
grep -q 'analysis_full=0' "$WORK/out" || fail "a source change asked for the full tree: $(cat "$WORK/out")"
echo "PASS an ordinary source change keeps the analysis to the changed files"
for path in tools/ci-dependency-pins.env cmake/Pffft.cmake src/third_party/ezpwd/rs \
  android/third_party/libusb/libusb.h android/third_party/librtlsdr/rtl-sdr.h \
  android/third_party/libairspy/airspy.h; do
  git checkout -q -f --detach "$base"
  mkdir -p "$(dirname "$path")"
  printf 'changed\n' > "$path"
  git add -A
  git commit -q -m "change $path"
  run_hook "$(git rev-parse HEAD)" "$base" || true
  grep -q 'analysis_full=1' "$WORK/out" || fail "$path did not ask for the full tree: $(cat "$WORK/out")"
  echo "PASS a change to $path analyzes the whole tree"
done

# A push that only deletes a dependency file asks for the full tree as well, where
# the per-ref path list (additions and changes) is empty.
git checkout -q -f --detach "$base"
mkdir -p src/third_party/ezpwd
printf 'vendored\n' > src/third_party/ezpwd/rs
git add -A
git commit -q -m 'vendored file'
vendored=$(git rev-parse HEAD)
git rm -q src/third_party/ezpwd/rs
git commit -q -m 'drop vendored file'
run_hook "$(git rev-parse HEAD)" "$vendored" || true
grep -q 'analysis_full=1' "$WORK/out" || fail "deleting a vendored file did not ask for the full tree: $(cat "$WORK/out")"
echo "PASS deleting a vendored file analyzes the whole tree"

# Moving a file out of a dependency removes it from the dependency, though git would
# call it a rename.
git checkout -q -f --detach "$vendored"
mkdir -p docs
git mv src/third_party/ezpwd/rs docs/rs
git commit -q -m 'move vendored file out'
run_hook "$(git rev-parse HEAD)" "$vendored" || true
grep -q 'analysis_full=1' "$WORK/out" || fail "moving a vendored file out did not ask for the full tree: $(cat "$WORK/out")"
echo "PASS moving a file out of a vendored snapshot analyzes the whole tree"

# A full-tree run reconfigures dev-debug even where a compile database exists, so a
# changed pin (PFFFT) is refetched before the analyzers read it; a scoped run keeps
# the existing database. These two cases run the hook to the end with its check
# runner and a stub cmake that records its calls; the checks it starts have no
# scripts here and fail, which is not what is being tested.
mkdir -p tools/lib "$WORK/bin"
cp "$ROOT_DIR/tools/lib/check_runner.sh" tools/lib/
cat > "$WORK/bin/cmake" << 'STUB'
#!/usr/bin/env bash
printf '%s\n' "$*" >> "$CMAKE_CALLS"
STUB
chmod +x "$WORK/bin/cmake"
# run_full_hook LOCAL_SHA REMOTE_SHA: the hook in serial mode with the stub cmake;
# its calls go to $WORK/cmake.calls.
run_full_hook() {
  rm -f "$WORK/cmake.calls"
  : > "$WORK/cmake.calls"
  printf 'refs/heads/topic %s refs/heads/topic %s\n' "$1" "$2" |
    PATH="$WORK/bin:$PATH" CMAKE_CALLS="$WORK/cmake.calls" DSD_HOOK_SERIAL=1 TMPDIR="$hook_tmp" \
      bash .githooks/pre-push origin > "$WORK/out" 2>&1 || true
}
git checkout -q -f --detach "$base"
mkdir -p build/dev-debug
printf '[]\n' > build/dev-debug/compile_commands.json
printf 'PFFFT_SHA=changed\n' > pins.env
mkdir -p tools
mv pins.env tools/ci-dependency-pins.env
git add tools/ci-dependency-pins.env
git commit -q -m 'pin change'
run_full_hook "$(git rev-parse HEAD)" "$base"
grep -qx -- '--preset dev-debug -B build/dev-debug' "$WORK/cmake.calls" ||
  fail "a full-tree run kept a stale compile database: $(cat "$WORK/cmake.calls") $(cat "$WORK/out")"
echo "PASS a full-tree run reconfigures dev-debug over an existing compile database"
# The analyzers read DSD_ANALYSIS_BUILD_DIR when it is set; that tree is refreshed.
mkdir -p build/other
printf '[]\n' > build/other/compile_commands.json
DSD_ANALYSIS_BUILD_DIR=build/other run_full_hook "$(git rev-parse HEAD)" "$base"
grep -qx -- '--preset dev-debug -B build/other' "$WORK/cmake.calls" ||
  fail "a full-tree run refreshed a tree the analyzers do not read: $(cat "$WORK/cmake.calls")"
echo "PASS a full-tree run reconfigures the tree DSD_ANALYSIS_BUILD_DIR names"
git checkout -q -f --detach "$base"
printf 'int scoped;\n' > base.c
git commit -q -am 'scoped change'
run_full_hook "$(git rev-parse HEAD)" "$base"
[[ ! -s "$WORK/cmake.calls" ]] || fail "a scoped run reconfigured: $(cat "$WORK/cmake.calls")"
grep -q 'analysis_full=0' "$WORK/out" || fail "the scoped run did not reach its checks: $(cat "$WORK/out")"
echo "PASS a scoped run keeps the existing compile database"
