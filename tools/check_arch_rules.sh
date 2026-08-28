#!/usr/bin/env bash
set -euo pipefail

repo_root=$(git rev-parse --show-toplevel 2> /dev/null || pwd)
cd "$repo_root"

usage() {
  cat << 'USAGE'
Usage: tools/check_arch_rules.sh

Runs the architecture guardrails in cmake/arch_rules.cmake over src/ and
include/dsd-neo/: module include boundaries (backend vs app-control vs UI vs
Qt), the frontend/app-control boundary, and forbidden constructs (exit(),
weak symbols, /alternatename pragmas, direct rtl_stream_* use from the
terminal UI). Fails with a non-zero exit when any violation is found.
USAGE
}

if [[ $# -gt 0 ]]; then
  case "$1" in
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
fi

if ! command -v cmake > /dev/null 2>&1; then
  echo "cmake not found in PATH." >&2
  exit 1
fi

exec cmake -P cmake/arch_rules.cmake
