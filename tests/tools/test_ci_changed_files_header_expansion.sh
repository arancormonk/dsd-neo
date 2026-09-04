#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
#
# tools/ci_changed_files.sh expands a changed header to the translation units
# that include it, capped per language. The cap used to be one alphabetical cut
# across the whole repository, so a header with more than five src/ includers
# never reached a tests/ TU on a pull request; an IWYU verdict that only renders
# from a tests/ includer then failed the full-tree push run on main after #471
# merged. The cap now applies per top-level tree, so every tree that includes the
# header is represented. This builds a throwaway repository in which src/ alone
# overflows the cap and checks the tests/ includer still lands in analysis_tus.
set -euo pipefail

if ! command -v rg > /dev/null 2>&1; then
  echo "SKIP: rg not found; header include expansion needs it" >&2
  exit 0
fi

ROOT_DIR=$(git rev-parse --show-toplevel)
SCRIPT="$ROOT_DIR/tools/ci_changed_files.sh"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

cd "$WORK"
git init -q .
git config user.email test@example.invalid
git config user.name test
git config commit.gpgsign false

mkdir -p include/dsd-neo/core src/core tests/core apps/cli
printf 'struct row;\n' > include/dsd-neo/core/widget.h
for n in a b c d e f g; do
  printf '#include <dsd-neo/core/widget.h>\n' > "src/core/$n.c"
done
printf '#include <dsd-neo/core/widget.h>\n' > src/core/z.cpp
printf '#include <dsd-neo/core/widget.h>\n' > tests/core/test_widget.c
printf '#include <dsd-neo/core/widget.h>\n' > apps/cli/main.c
printf 'int unrelated;\n' > src/core/unrelated.c
git add -A
git commit -q -m base
base=$(git rev-parse HEAD)

printf 'struct row;\nstruct other;\n' > include/dsd-neo/core/widget.h
git commit -q -am "touch header"
head=$(git rev-parse HEAD)

bash "$SCRIPT" --base "$base" --head "$head" --out-dir out > /dev/null 2>&1
mapfile -t tus < out/analysis_tus.txt

has() {
  local want="$1"
  local t=""
  for t in "${tus[@]}"; do
    [[ "$t" == "$want" ]] && return 0
  done
  return 1
}

fail=0
# Every tree that includes the header contributes, even though src/ alone
# overflows the per-language cap.
has tests/core/test_widget.c || {
  echo "FAIL: tests/ includer dropped from analysis_tus" >&2
  fail=1
}
has apps/cli/main.c || {
  echo "FAIL: apps/ includer dropped from analysis_tus" >&2
  fail=1
}
has src/core/z.cpp || {
  echo "FAIL: C++ includer dropped from analysis_tus" >&2
  fail=1
}
# The cap still bounds each tree: seven src/ C includers, at most five chosen.
src_c=0
for t in "${tus[@]}"; do
  [[ "$t" == src/*.c ]] && src_c=$((src_c + 1))
done
if [[ $src_c -ne 5 ]]; then
  echo "FAIL: expected 5 src/ C includers under the cap, got $src_c" >&2
  fail=1
fi
has src/core/unrelated.c && {
  echo "FAIL: non-includer listed in analysis_tus" >&2
  fail=1
}

if [[ $fail -ne 0 ]]; then
  printf 'analysis_tus:\n%s\n' "${tus[@]}" >&2
  exit 1
fi
echo "PASS: header expansion samples every tree under the per-language cap"
