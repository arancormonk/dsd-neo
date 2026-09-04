#!/usr/bin/env bash
set -euo pipefail

# Full local guardrail set: the pre-push checks with missing tools fatal and
# scan-build enabled, then the whole-tree lint, gitleaks and a fuzz smoke pass.
#
# Phases, in the default lane mode:
#   1. tools/preflight_ci.sh streams in the foreground (it runs its own
#      concurrent lanes) while gitleaks, which the hook never runs, scans in the
#      background.
#   2. The fuzz-asan-debug configure and build stream in the foreground while
#      the unscoped whole-tree lint (CMake format, shell, workflow, zizmor, OSV)
#      runs in the background. Those tools may already have run scoped inside
#      the hook, so they wait for it to finish rather than share its log files.
#   3. The fuzz smoke pass.
# Every failure is reported at the end. DSD_HOOK_SERIAL=1 runs everything one
# after another in the foreground and stops at the first failure.
#
# Environment (also read by the pre-push hook):
#   DSD_HOOK_JOBS=N              Worker budget (default: detected core count).
#   DSD_HOOK_SERIAL=1            Sequential, stream everything, fail fast.
#   DSD_HOOK_SCAN_BUILD_FRESH=1  Clean scan-build rebuild instead of incremental.

ROOT_DIR=$(git rev-parse --show-toplevel 2> /dev/null || pwd)
cd "$ROOT_DIR"

export DSD_HOOK_FAIL_ON_MISSING_TOOLS=1
export DSD_HOOK_RUN_SCAN_BUILD=1

# shellcheck disable=SC1091 # Sourced from the repo root at runtime; linted as its own file.
source "$ROOT_DIR/tools/lib/check_runner.sh"
runner_init quality-preflight "${DSD_HOOK_SERIAL:-0}"
JOBS=$(runner_detect_jobs)

PREFLIGHT_ARGS=("$@")

# shellcheck disable=SC2329 # Started through runner_spawn.
lane_gitleaks() {
  run_check "gitleaks secret scan" tools/gitleaks.sh
}

# shellcheck disable=SC2329 # Started through runner_spawn.
lane_lint() {
  run_check "CMake format (full tree)" tools/cmake_format_check.sh
  run_check "shell lint (full tree)" tools/shell_lint.sh
  run_check "workflow lint (full tree)" tools/workflow_lint.sh
  run_check "zizmor workflow security" tools/zizmor.sh
  run_check "OSV dependency scan" tools/osv_scan.sh
}

# shellcheck disable=SC2329 # Invoked through run_check.
fuzz_configure_and_build() {
  cmake --preset fuzz-asan-debug
  cmake --build --preset fuzz-asan-debug -j "$JOBS"
}

echo "quality-preflight: jobs=${JOBS} serial=${DSD_HOOK_SERIAL:-0}"

# Phase 1: the pre-push checks, with gitleaks alongside.
runner_spawn gitleaks lane_gitleaks
RUNNER_STREAM=1 run_check "pre-push checks (tools/preflight_ci.sh)" tools/preflight_ci.sh "${PREFLIGHT_ARGS[@]}"
runner_wait_all

# Phase 2: the fuzz build, with the whole-tree lint alongside.
runner_spawn lint lane_lint
RUNNER_STREAM=1 run_check "fuzz-asan-debug configure and build" fuzz_configure_and_build
runner_wait_all

# Phase 3: the fuzz smoke pass.
RUNNER_STREAM=1 run_check "fuzz smoke (tools/fuzz_smoke.sh)" tools/fuzz_smoke.sh --no-build

if runner_report; then
  echo "quality-preflight: all checks passed."
  exit 0
fi
exit 1
