#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(git rev-parse --show-toplevel 2> /dev/null || pwd)
cd "$ROOT_DIR"

usage() {
  cat << 'USAGE'
Usage: tools/cmake_format_check.sh [--fix] [--] [files...]

Checks tracked CMake files with gersemi. With --fix, rewrites files in place.
When no files are supplied, all tracked CMake files are checked.
USAGE
}

FIX=0
FILES=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --fix)
      FIX=1
      shift
      ;;
    -h | --help)
      usage
      exit 0
      ;;
    --)
      shift
      FILES+=("$@")
      break
      ;;
    -*)
      echo "Unknown option: $1" >&2
      usage >&2
      exit 2
      ;;
    *)
      FILES+=("$1")
      shift
      ;;
  esac
done

if ! command -v gersemi > /dev/null 2>&1; then
  echo "gersemi not found. Install with: python -m pip install gersemi." >&2
  exit 1
fi

is_cmake_file() {
  case "$1" in
    CMakeLists.txt | */CMakeLists.txt | CMakeLists.txt.in | */CMakeLists.txt.in | *.cmake | *.cmake.in)
      return 0
      ;;
    *)
      return 1
      ;;
  esac
}

if [[ ${#FILES[@]} -eq 0 ]]; then
  mapfile -t FILES < <(
    {
      git ls-files
      git ls-files --others --exclude-standard
    } | while IFS= read -r f; do
      if is_cmake_file "$f"; then
        printf '%s\n' "$f"
      fi
    done | sort -u
  )
fi

FILTERED=()
for f in "${FILES[@]}"; do
  f="${f#./}"
  case "$f" in
    build/* | vcpkg_installed/*) continue ;;
  esac
  if [[ -f "$f" ]] && is_cmake_file "$f"; then
    FILTERED+=("$f")
  fi
done

if [[ ${#FILTERED[@]} -eq 0 ]]; then
  echo "No CMake files to check."
  exit 0
fi

LOG_FILE=".cmake-format.local.out"
set +e
if [[ $FIX -eq 1 ]]; then
  {
    echo "gersemi fix files: ${#FILTERED[@]}"
    gersemi -i --quiet "${FILTERED[@]}"
  } 2>&1 | tee "$LOG_FILE"
else
  {
    echo "gersemi check files: ${#FILTERED[@]}"
    gersemi --check --diff --quiet "${FILTERED[@]}"
  } 2>&1 | tee "$LOG_FILE"
fi
rc=${PIPESTATUS[0]}
set -e

if [[ $rc -eq 0 ]]; then
  echo "CMake format check completed. Full output in $LOG_FILE"
else
  echo "CMake format check failed. Run tools/cmake_format_check.sh --fix." >&2
fi
exit "$rc"
