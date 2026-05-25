#!/usr/bin/env bash
set -euo pipefail

# Run Semgrep for additional SAST checks. Default mode is advisory (non-erroring).
# Use --strict to fail on findings.
# Excludes third-party code under src/third_party. Strict mode also loads
# project-specific guardrail rules from semgrep/dsd-neo.yml.

ROOT_DIR=$(git rev-parse --show-toplevel 2> /dev/null || pwd)
cd "$ROOT_DIR"

usage() {
  cat << 'USAGE'
Usage: tools/semgrep.sh [--strict] [--config <config>] [--] [paths...]

Options:
  --strict          Fail on findings (--error).
  --config CONFIG   Semgrep config/rule pack (default: p/default; strict also
                    adds p/c, p/security-audit, and semgrep/dsd-neo.yml).
                    May be supplied multiple times.

Arguments:
  paths...          Optional paths to scan. Default: src include apps tests tools .github/workflows

Environment:
  DSD_SEMGREP_SARIF_OUT  Optional SARIF output path for GitHub code scanning.
USAGE
}

STRICT=0
CONFIGS=("p/default")
CUSTOM_CONFIGS=0
TARGETS=()

while [[ $# -gt 0 ]]; do
  case "$1" in
    --strict)
      STRICT=1
      shift
      ;;
    --config)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for --config" >&2
        exit 2
      fi
      if [[ $CUSTOM_CONFIGS -eq 0 ]]; then
        CONFIGS=()
        CUSTOM_CONFIGS=1
      fi
      CONFIGS+=("$2")
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

if ! command -v semgrep > /dev/null 2>&1; then
  echo "semgrep not found. Install with: pipx install semgrep (or pip install semgrep)." >&2
  exit 1
fi

if [[ ${#TARGETS[@]} -eq 0 ]]; then
  TARGETS=(src include apps tests tools .github/workflows)
fi

LOG_FILE=".semgrep.local.out"

ARGS=(
  --metrics=off
  --disable-version-check
  --no-git-ignore
  --exclude .deps-arch-toolchain
  --exclude .deps-arch-toolchain/**
  --exclude src/third_party
  --exclude src/third_party/**
  --exclude vcpkg_installed
  --exclude vcpkg_installed/**
  --exclude build
  --exclude build/**
)
if [[ $STRICT -eq 1 ]]; then
  ARGS+=(--error)
  if [[ $CUSTOM_CONFIGS -eq 0 ]]; then
    CONFIGS+=(p/c p/security-audit semgrep/dsd-neo.yml)
  fi
fi
for config in "${CONFIGS[@]}"; do
  ARGS+=(--config "$config")
done
if [[ -n "${DSD_SEMGREP_SARIF_OUT:-}" ]]; then
  ARGS+=(--sarif --output "$DSD_SEMGREP_SARIF_OUT")
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
