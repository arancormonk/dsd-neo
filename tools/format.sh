#!/usr/bin/env bash
set -euo pipefail

shopt -s globstar nullglob

# Include all C/C++ sources/headers across src, include, tests, examples, apps,
# plus any C/C++ headers at the repo root. CI excludes build/ via git; our
# globs already avoid it by scoping to project dirs.
files=(
  src/**/*.{c,cc,cxx,cpp,h,hpp}
  include/**/*.{h,hpp}
  tests/**/*.{c,cc,cxx,cpp,h,hpp}
  examples/**/*.{c,cc,cxx,cpp,h,hpp}
  apps/**/*.{c,cc,cxx,cpp,h,hpp}
  *.{c,cc,cxx,cpp,h,hpp}
)
if command -v clang-format >/dev/null 2>&1; then
  if [ ${#files[@]} -eq 0 ]; then
    echo "No C/C headers found to format."
    exit 0
  fi
  clang-format -i "${files[@]}"
  echo "Formatted ${#files[@]} files."
else
  echo "clang-format not found. Install it to run formatting." >&2
  exit 1
fi
