#!/usr/bin/env bash
set -euo pipefail

# Run Semgrep for additional SAST checks. Default mode is advisory (non-erroring).
# Use --strict to fail on findings.
# Excludes third-party code under src/third_party.

ROOT_DIR=$(git rev-parse --show-toplevel 2>/dev/null || pwd)
cd "$ROOT_DIR"

usage() {
  cat <<'USAGE'
Usage: tools/semgrep.sh [--strict] [--config <config>] [--] [paths...]

Options:
  --strict          Fail on findings (--error).
  --config CONFIG   Semgrep config/rule pack (default: p/default).

Arguments:
  paths...          Optional paths to scan. Default: src include apps tests tools
USAGE
}

STRICT=0
CONFIG="p/default"
TARGETS=()

while [[ $# -gt 0 ]]; do
  case "$1" in
    --strict) STRICT=1; shift ;;
    --config)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for --config" >&2
        exit 2
      fi
      CONFIG="$2"
      shift 2
      ;;
    -h|--help) usage; exit 0 ;;
    --) shift; TARGETS+=("$@"); break ;;
    -*)
      echo "Unknown option: $1" >&2
      usage >&2
      exit 2
      ;;
    *) TARGETS+=("$1"); shift ;;
  esac
done

if ! command -v semgrep >/dev/null 2>&1; then
  echo "semgrep not found. Install with: pipx install semgrep (or pip install semgrep)." >&2
  exit 1
fi

if [[ ${#TARGETS[@]} -eq 0 ]]; then
  TARGETS=(src include apps tests tools)
fi

LOG_FILE=".semgrep.local.out"

ARGS=(
  --config "$CONFIG"
  --metrics=off
  --disable-version-check
  --exclude src/third_party
  --exclude src/third_party/**
  --exclude build
)
if [[ $STRICT -eq 1 ]]; then
  ARGS+=(--error)
fi

set +e
semgrep "${ARGS[@]}" "${TARGETS[@]}" 2>&1 | tee "$LOG_FILE"
rc=${PIPESTATUS[0]}
set -e

if [[ $rc -eq 0 ]]; then
  echo "Semgrep completed. Full output in $LOG_FILE"
else
  echo "Semgrep found issues (or failed). See $LOG_FILE for details." >&2
fi

exit "$rc"
