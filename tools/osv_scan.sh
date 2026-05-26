#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(git rev-parse --show-toplevel 2> /dev/null || pwd)
cd "$ROOT_DIR"

usage() {
  cat << 'USAGE'
Usage: tools/osv_scan.sh [--sarif-out FILE] [--] [paths...]

Runs OSV-Scanner source dependency/vendored-code scanning.
Default path: .

Environment:
  DSD_OSV_SCANNER_IMAGE       Docker image fallback (default: pinned OSV_SCANNER_IMAGE)
  DSD_OSV_SARIF_OUT           Optional SARIF output path.
  DSD_OSV_ALLOW_NO_PACKAGES   Treat missing package metadata/lockfiles as success (default: 1).
USAGE
}

SARIF_OUT="${DSD_OSV_SARIF_OUT:-}"
TARGETS=()

while [[ $# -gt 0 ]]; do
  case "$1" in
    --sarif-out)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for --sarif-out" >&2
        exit 2
      fi
      SARIF_OUT="$2"
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

if [[ ${#TARGETS[@]} -eq 0 ]]; then
  TARGETS=(.)
fi

# shellcheck source=tools/ci-dependency-pins.env
# shellcheck disable=SC1091
source "$ROOT_DIR/tools/ci-dependency-pins.env"
PINNED_OSV_IMAGE="${OSV_SCANNER_IMAGE:?OSV_SCANNER_IMAGE is required}"
OSV_IMAGE="${DSD_OSV_SCANNER_IMAGE:-$PINNED_OSV_IMAGE}"
ALLOW_NO_PACKAGES="${DSD_OSV_ALLOW_NO_PACKAGES:-1}"
LOG_FILE=".osv-scanner.local.out"

run_osv() {
  local format="$1"
  local output_file="${2:-}"
  local args=(scan source --recursive --format "$format" --verbosity error)
  if [[ "$ALLOW_NO_PACKAGES" == "1" ]]; then
    args+=(--allow-no-lockfiles)
  fi
  if [[ -n "$output_file" ]]; then
    args+=(--output "$output_file")
  fi
  args+=("${TARGETS[@]}")

  if command -v osv-scanner > /dev/null 2>&1; then
    osv-scanner "${args[@]}"
  elif command -v docker > /dev/null 2>&1; then
    docker run --rm \
      --volume "$ROOT_DIR:/src" \
      --workdir /src \
      "$OSV_IMAGE" \
      "${args[@]}"
  else
    echo "osv-scanner not found and docker is unavailable." >&2
    return 127
  fi
}

normalize_rc() {
  local rc="$1"
  if [[ "$ALLOW_NO_PACKAGES" == "1" && "$rc" -eq 128 ]]; then
    echo "OSV-Scanner found no supported package metadata; treating as success." >&2
    return 0
  fi
  return "$rc"
}

set +e
{
  echo "OSV-Scanner targets: ${TARGETS[*]}"
  run_osv vertical
} 2>&1 | tee "$LOG_FILE"
plain_rc=${PIPESTATUS[0]}
normalize_rc "$plain_rc"
plain_rc=$?

sarif_rc=0
if [[ -n "$SARIF_OUT" ]]; then
  run_osv sarif "$SARIF_OUT" >> "$LOG_FILE" 2>&1
  sarif_rc=$?
  normalize_rc "$sarif_rc"
  sarif_rc=$?
fi
set -e

rc=$plain_rc
if [[ $rc -eq 0 && $sarif_rc -ne 0 ]]; then
  rc=$sarif_rc
fi

if [[ $rc -eq 0 ]]; then
  echo "OSV-Scanner completed. Full output in $LOG_FILE"
else
  echo "OSV-Scanner found issues or failed. See $LOG_FILE for details." >&2
fi
exit "$rc"
