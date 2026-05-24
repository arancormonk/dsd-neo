#!/usr/bin/env bash
set -euo pipefail

# Run Lizard complexity checks for project-owned code.
# Strict mode fails when any Lizard threshold warning is emitted.

ROOT_DIR=$(git rev-parse --show-toplevel 2> /dev/null || pwd)
cd "$ROOT_DIR"

usage() {
  cat << 'USAGE'
Usage: tools/lizard.sh [--strict] [--ccn N] [--length N] [--arguments N] [--] [paths...]

Options:
  --strict       Fail when threshold warnings are emitted.
  --ccn N        Cyclomatic complexity warning threshold (default: 15).
  --length N     Function length warning threshold (default: 1000; strict: 120).
  --arguments N  Parameter count warning threshold (default: 100; strict: 13).

Arguments:
  paths...       Optional paths to scan. Default: src include apps
USAGE
}

STRICT=0
CCN=15
LENGTH=1000
ARGUMENTS=100
CCN_SET=0
LENGTH_SET=0
ARGUMENTS_SET=0
TARGETS=()

while [[ $# -gt 0 ]]; do
  case "$1" in
    --strict)
      STRICT=1
      shift
      ;;
    --ccn)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for --ccn" >&2
        exit 2
      fi
      CCN="$2"
      CCN_SET=1
      shift 2
      ;;
    --length)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for --length" >&2
        exit 2
      fi
      LENGTH="$2"
      LENGTH_SET=1
      shift 2
      ;;
    --arguments)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for --arguments" >&2
        exit 2
      fi
      ARGUMENTS="$2"
      ARGUMENTS_SET=1
      shift 2
      ;;
    -h | --help)
      usage
      exit 0
      ;;
    --)
      shift
      TARGETS+=("$@")
      break
      ;;
    -*)
      echo "Unknown option: $1" >&2
      usage >&2
      exit 2
      ;;
    *)
      TARGETS+=("$1")
      shift
      ;;
  esac
done

if [[ $STRICT -eq 1 ]]; then
  # Keep the current maximum CCN as the strict ceiling, but make length and
  # parameter-count budgets useful enough to catch future growth.
  [[ $CCN_SET -eq 0 ]] && CCN=15
  [[ $LENGTH_SET -eq 0 ]] && LENGTH=120
  [[ $ARGUMENTS_SET -eq 0 ]] && ARGUMENTS=13
fi

if ! command -v lizard > /dev/null 2>&1; then
  echo "lizard not found. Install with: python -m pip install lizard." >&2
  exit 1
fi

if [[ ${#TARGETS[@]} -eq 0 ]]; then
  TARGETS=(src include apps)
fi

LOG_FILE=".lizard.local.out"
ARGS=(
  -l cpp
  -C "$CCN"
  -L "$LENGTH"
  -a "$ARGUMENTS"
  -x "build/*"
  -x "./build/*"
  -x "src/third_party/*"
  -x "./src/third_party/*"
)

if [[ $STRICT -eq 0 ]]; then
  ARGS+=(-i -1)
fi

set +e
lizard "${ARGS[@]}" "${TARGETS[@]}" 2>&1 | tee "$LOG_FILE"
rc=${PIPESTATUS[0]}
set -e

if [[ $rc -eq 0 ]]; then
  echo "Lizard completed. Full output in $LOG_FILE"
else
  echo "Lizard found threshold warnings or failed. See $LOG_FILE for details." >&2
fi

exit "$rc"
