#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat << 'USAGE'
Usage: tools/cleanup_local.sh [--dry-run] [--keep-vcpkg]

Removes local generated output such as build directories, tool logs, coverage
reports, and ignored log/output files. The script skips tracked files and does
not run git reset, git checkout, or git clean.

Options:
  --dry-run      Print what would be removed without deleting anything.
  --keep-vcpkg   Keep vcpkg_installed/.
  -h, --help     Show this help.
USAGE
}

DRY_RUN=0
KEEP_VCPKG=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --dry-run)
      DRY_RUN=1
      shift
      ;;
    --keep-vcpkg)
      KEEP_VCPKG=1
      shift
      ;;
    -h | --help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown option: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

if ! ROOT_DIR=$(git rev-parse --show-toplevel 2> /dev/null); then
  echo "cleanup_local.sh must be run from inside a git worktree." >&2
  exit 2
fi
cd "$ROOT_DIR"

CLEAN_PATHS=()
declare -A SEEN_PATHS=()

queue_path() {
  local path=$1

  path=${path#./}
  if [[ -z "$path" ]]; then
    return
  fi
  if [[ -n "${SEEN_PATHS[$path]+seen}" ]]; then
    return
  fi

  CLEAN_PATHS+=("$path")
  SEEN_PATHS["$path"]=1
}

has_tracked_content() {
  local path=$1

  if git ls-files --error-unmatch -- "$path" > /dev/null 2>&1; then
    return 0
  fi

  if [[ -d "$path" ]] && [[ -n "$(git ls-files -- "$path")" ]]; then
    return 0
  fi

  return 1
}

remove_path() {
  local path=$1

  case "$path" in
    "" | "." | ".." | /*)
      echo "Skipping unsafe path: $path" >&2
      return
      ;;
  esac

  if [[ ! -e "$path" ]]; then
    return
  fi

  if has_tracked_content "$path"; then
    echo "Skipping tracked path: $path"
    return
  fi

  if [[ $DRY_RUN -eq 1 ]]; then
    echo "Would remove: $path"
  else
    echo "Removing: $path"
    rm -rf -- "$path"
  fi
}

FIXED_PATHS=(
  build
  compile_commands.json
  .cache
  .ccache
  .ci
  .deps
  .deps-arch-toolchain
  .vs
  logs
  .strict-analysis
  .strict-checkpoints
  .scan-build.local
  .cppcheck-build
  coverage.info
  coverage.src.info
  coverage.protocol.info
  coverage_html
  coverage_protocol_html
  .gitleaks.sarif
  .osv-scanner.sarif
  .zizmor.sarif
  .zizmor-pedantic.sarif
)

if [[ $KEEP_VCPKG -eq 0 ]]; then
  FIXED_PATHS+=(vcpkg_installed)
fi

for path in "${FIXED_PATHS[@]}"; do
  queue_path "$path"
done

while IFS= read -r -d '' path; do
  queue_path "$path"
done < <(
  git ls-files -z --others --ignored --exclude-standard -- \
    '*.log' \
    '*.out' \
    '*.gdb' \
    '.preflight-baseline*'
)

if [[ ${#CLEAN_PATHS[@]} -eq 0 ]]; then
  echo "Nothing to clean."
  exit 0
fi

for path in "${CLEAN_PATHS[@]}"; do
  remove_path "$path"
done
