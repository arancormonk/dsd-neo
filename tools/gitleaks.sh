#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(git rev-parse --show-toplevel 2> /dev/null || pwd)
cd "$ROOT_DIR"

usage() {
  cat << 'USAGE'
Usage: tools/gitleaks.sh

Runs Gitleaks against the repository and writes .gitleaks.sarif for code scanning.
Uses a local gitleaks binary when present, otherwise falls back to Docker.
USAGE
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  usage
  exit 0
fi

LOG_FILE=".gitleaks.local.out"
SARIF_FILE=".gitleaks.sarif"
CONFIG_FILE=".gitleaks.toml"
GITLEAKS_IMAGE="${DSD_GITLEAKS_IMAGE:-ghcr.io/gitleaks/gitleaks:v8.30.1}"

ARGS=(
  detect
  --source=.
  --config="$CONFIG_FILE"
  --redact
  --report-format=sarif
  --report-path="$SARIF_FILE"
  --exit-code=1
  --no-banner
)

set +e
if command -v gitleaks > /dev/null 2>&1; then
  gitleaks "${ARGS[@]}" 2>&1 | tee "$LOG_FILE"
  rc=${PIPESTATUS[0]}
elif command -v docker > /dev/null 2>&1; then
  docker run --rm \
    --volume "$ROOT_DIR:/repo" \
    --workdir /repo \
    "$GITLEAKS_IMAGE" \
    "${ARGS[@]}" 2>&1 | tee "$LOG_FILE"
  rc=${PIPESTATUS[0]}
else
  echo "gitleaks not found and docker is unavailable." | tee "$LOG_FILE" >&2
  rc=1
fi
set -e

if [[ $rc -eq 0 ]]; then
  echo "Gitleaks completed. Full output in $LOG_FILE"
else
  echo "Gitleaks found issues or failed. See $LOG_FILE for details." >&2
fi
exit "$rc"
