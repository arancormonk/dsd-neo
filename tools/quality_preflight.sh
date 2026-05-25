#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(git rev-parse --show-toplevel 2> /dev/null || pwd)
cd "$ROOT_DIR"

export DSD_HOOK_FAIL_ON_MISSING_TOOLS=1
export DSD_HOOK_RUN_SCAN_BUILD=1

tools/preflight_ci.sh "$@"
tools/cmake_format_check.sh
tools/shell_lint.sh
tools/workflow_lint.sh
tools/zizmor.sh
tools/osv_scan.sh
tools/gitleaks.sh

cmake --preset fuzz-asan-debug
cmake --build --preset fuzz-asan-debug -j
tools/fuzz_smoke.sh --no-build
