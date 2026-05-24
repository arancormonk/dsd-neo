#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(git rev-parse --show-toplevel 2> /dev/null || pwd)
cd "$ROOT_DIR"

usage() {
  cat << 'USAGE'
Usage: tools/workflow_lint.sh [--] [workflow-files...]

Runs actionlint over GitHub Actions workflow files.
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

if ! command -v actionlint > /dev/null 2>&1; then
  echo "actionlint not found. Install actionlint to lint GitHub Actions workflows." >&2
  exit 1
fi

if [[ ${#FILES[@]} -eq 0 ]]; then
  mapfile -t FILES < <(
    {
      git ls-files '.github/workflows/*.yml' '.github/workflows/*.yaml'
      git ls-files --others --exclude-standard -- '.github/workflows/*.yml' '.github/workflows/*.yaml'
    } | sort -u
  )
fi

FILTERED=()
for f in "${FILES[@]}"; do
  if [[ -f "$f" ]]; then
    FILTERED+=("$f")
  fi
done

if [[ ${#FILTERED[@]} -eq 0 ]]; then
  echo "No workflow files to lint."
  exit 0
fi

LOG_FILE=".actionlint.local.out"
set +e
actionlint "${FILTERED[@]}" 2>&1 | tee "$LOG_FILE"
rc=${PIPESTATUS[0]}
set -e

if [[ $rc -eq 0 ]]; then
  echo "Workflow lint completed. Full output in $LOG_FILE"
else
  echo "Workflow lint failed. See $LOG_FILE for details." >&2
fi
exit "$rc"
