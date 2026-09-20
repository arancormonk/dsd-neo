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
#   3. The fuzz smoke pass, if the build in phase 2 produced binaries to smoke.
# Every failure is reported at the end. DSD_HOOK_SERIAL=1 runs everything one
# after another in the foreground and stops at the first failure.
#
# Environment (also read by the pre-push hook):
#   DSD_HOOK_JOBS=N              Worker budget shared across the run (default:
#                                detected core count). Each check that runs keeps
#                                at least one worker; DSD_HOOK_SERIAL=1 is what
#                                bounds a small machine.
#   DSD_HOOK_SERIAL=1            Sequential, stream everything, fail fast.
#   DSD_HOOK_SCAN_BUILD_REUSE=1  Incremental scan-build: only the translation
#                                units the build recompiles are analyzed.

ROOT_DIR=$(git rev-parse --show-toplevel 2> /dev/null || pwd)
cd "$ROOT_DIR"

export DSD_HOOK_FAIL_ON_MISSING_TOOLS=1
export DSD_HOOK_RUN_SCAN_BUILD=1

# shellcheck disable=SC1091 # Sourced from the repo root at runtime; linted as its own file.
source "$ROOT_DIR/tools/lib/check_runner.sh"
runner_init quality-preflight "${DSD_HOOK_SERIAL:-0}"
JOBS=$(runner_detect_jobs)

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

# run_check clears errexit around the command it runs, and that reaches inside a
# shell function, so each step reports its own failure rather than relying on
# `set -e` to stop the next one.
# shellcheck disable=SC2329 # Invoked through run_check.
fuzz_configure_and_build() {
  cmake --preset fuzz-asan-debug || return $?
  cmake --build --preset fuzz-asan-debug -j "$JOBS" || return $?
}

echo "quality-preflight: jobs=${JOBS} serial=${RUNNER_SERIAL}"

# Phase 1: the pre-push checks, with gitleaks alongside.
runner_spawn gitleaks lane_gitleaks
run_check --stream "pre-push checks (tools/preflight_ci.sh)" tools/preflight_ci.sh "$@"
runner_wait_all

# Phase 2: the fuzz build, with the whole-tree lint alongside.
runner_spawn lint lane_lint
run_check --stream "fuzz-asan-debug configure and build" fuzz_configure_and_build
fuzz_build_rc=$RUNNER_LAST_RC
runner_wait_all

# Phase 3: the fuzz smoke pass. --no-build runs whatever binaries are in the
# tree, which after a failed build are an earlier run's, so a pass there would
# be an assertion about a build that does not exist.
if [[ $fuzz_build_rc -eq 0 ]]; then
  run_check --stream "fuzz smoke (tools/fuzz_smoke.sh)" tools/fuzz_smoke.sh --no-build
else
  echo "quality-preflight: skipping the fuzz smoke pass (the fuzz build failed)." >&2
  runner_note_skipped "fuzz smoke pass: the fuzz build failed"
fi

# The pre-push checks run here with DSD_HOOK_FAIL_ON_MISSING_TOOLS=1 (exported
# above), so a tool missing inside them arrives as a failed check. The gaps this
# script opens itself are its own to record, which is what the skip above does.
if runner_report; then
  if [[ "$RUNNER_MISSING" == "1" || "$RUNNER_SKIPPED" == "1" ]]; then
    echo "quality-preflight: no check failed, but the analyses listed above did not run." >&2
    exit 1
  fi
  echo "quality-preflight: all checks passed."
  exit 0
fi
exit 1
