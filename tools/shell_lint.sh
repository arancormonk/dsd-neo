#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(git rev-parse --show-toplevel 2> /dev/null || pwd)
cd "$ROOT_DIR"

usage() {
  cat << 'USAGE'
Usage: tools/shell_lint.sh [--] [files...]

Runs shellcheck and shfmt in diff/check mode for repository shell scripts.
When no files are supplied, tracked shell scripts and git hooks are checked.
USAGE
}

FILES=()
while [[ $# -gt 0 ]]; do
  case "$1" in
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

if ! command -v shellcheck > /dev/null 2>&1; then
  echo "shellcheck not found. Install shellcheck to run shell linting." >&2
  exit 1
fi
if ! command -v shfmt > /dev/null 2>&1; then
  echo "shfmt not found. Install shfmt to run shell formatting checks." >&2
  exit 1
fi

if [[ ${#FILES[@]} -eq 0 ]]; then
  mapfile -t FILES < <(
    {
      git ls-files '*.sh' '.githooks/*'
      git ls-files --others --exclude-standard -- '*.sh' '.githooks/*'
      git grep -Il '^#!.*sh' -- ':!build/**' ':!src/third_party/**' ':!vcpkg_installed/**' || true
    } | sort -u
  )
fi

FILTERED=()
for f in "${FILES[@]}"; do
  case "$f" in
    build/* | src/third_party/* | vcpkg_installed/*) continue ;;
  esac
  if [[ -f "$f" ]]; then
    FILTERED+=("$f")
  fi
done

if [[ ${#FILTERED[@]} -eq 0 ]]; then
  echo "No shell scripts to lint."
  exit 0
fi

LOG_FILE=".shell-lint.local.out"
set +e
{
  echo "shellcheck files: ${#FILTERED[@]}"
  shellcheck "${FILTERED[@]}"
  echo "shfmt files: ${#FILTERED[@]}"
  shfmt -d -i 2 -ci -sr "${FILTERED[@]}"
} 2>&1 | tee "$LOG_FILE"
rc=${PIPESTATUS[0]}
set -e

if [[ $rc -eq 0 ]]; then
  echo "Shell lint completed. Full output in $LOG_FILE"
else
  echo "Shell lint failed. See $LOG_FILE for details." >&2
fi
exit "$rc"
