#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(git rev-parse --show-toplevel 2> /dev/null || pwd)
cd "$ROOT_DIR"

usage() {
  cat << 'USAGE'
Usage: tools/zizmor.sh [--pedantic|--auditor] [--min-confidence LEVEL] [--min-severity LEVEL] [--sarif-out FILE] [--no-config] [--] [inputs...]

Runs zizmor against GitHub Actions workflows and Dependabot config.
Default inputs: .github/workflows .github/dependabot.yml

Environment:
  DSD_ZIZMOR_SARIF_OUT  Optional SARIF output path.
USAGE
}

PERSONA="regular"
MIN_CONFIDENCE="high"
MIN_SEVERITY="low"
SARIF_OUT="${DSD_ZIZMOR_SARIF_OUT:-}"
NO_CONFIG=0
INPUTS=()

while [[ $# -gt 0 ]]; do
  case "$1" in
    --pedantic)
      PERSONA="pedantic"
      shift
      ;;
    --auditor)
      PERSONA="auditor"
      shift
      ;;
    --min-confidence)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for --min-confidence" >&2
        exit 2
      fi
      MIN_CONFIDENCE="$2"
      shift 2
      ;;
    --min-severity)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for --min-severity" >&2
        exit 2
      fi
      MIN_SEVERITY="$2"
      shift 2
      ;;
    --sarif-out)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for --sarif-out" >&2
        exit 2
      fi
      SARIF_OUT="$2"
      shift 2
      ;;
    --no-config)
      NO_CONFIG=1
      shift
      ;;
    -h | --help)
      usage
      exit 0
      ;;
    --)
      shift
      INPUTS+=("$@")
      break
      ;;
    -*)
      echo "Unknown option: $1" >&2
      usage >&2
      exit 2
      ;;
    *)
      INPUTS+=("$1")
      shift
      ;;
  esac
done

if ! command -v zizmor > /dev/null 2>&1; then
  echo "zizmor not found. Install with: pipx install zizmor (or python -m pip install zizmor)." >&2
  exit 1
fi

if [[ ${#INPUTS[@]} -eq 0 ]]; then
  INPUTS=(.github/workflows .github/dependabot.yml)
fi

COMMON_ARGS=(
  --offline
  --persona="$PERSONA"
  --min-confidence="$MIN_CONFIDENCE"
  --no-progress
  --color=never
)
if [[ -n "$MIN_SEVERITY" ]]; then
  COMMON_ARGS+=(--min-severity="$MIN_SEVERITY")
fi
if [[ $NO_CONFIG -eq 1 ]]; then
  COMMON_ARGS+=(--no-config)
fi

LOG_FILE=".zizmor.local.out"
ERR_FILE="$(mktemp "${TMPDIR:-/tmp}/dsd-neo-zizmor-sarif.XXXXXX")"
# shellcheck disable=SC2329 # Invoked by the EXIT trap.
cleanup() {
  rm -f "$ERR_FILE"
}
trap cleanup EXIT

set +e
zizmor "${COMMON_ARGS[@]}" --format=plain "${INPUTS[@]}" 2>&1 | tee "$LOG_FILE"
plain_rc=${PIPESTATUS[0]}

sarif_rc=0
if [[ -n "$SARIF_OUT" ]]; then
  zizmor "${COMMON_ARGS[@]}" --format=sarif "${INPUTS[@]}" > "$SARIF_OUT" 2> "$ERR_FILE"
  sarif_rc=$?
  if [[ -s "$ERR_FILE" ]]; then
    cat "$ERR_FILE" >> "$LOG_FILE"
  fi
fi
set -e

rc=$plain_rc
if [[ $rc -eq 0 && $sarif_rc -ne 0 ]]; then
  rc=$sarif_rc
fi

if [[ $rc -eq 0 ]]; then
  echo "zizmor completed. Full output in $LOG_FILE"
else
  echo "zizmor found issues or failed. See $LOG_FILE for details." >&2
fi
exit "$rc"
