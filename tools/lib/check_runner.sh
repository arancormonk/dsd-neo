# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 DSD-neo contributors
# shellcheck shell=bash
#
# Shared runner for the local quality gates (.githooks/pre-push and
# tools/quality_preflight.sh). Sourced, never executed.
#
# The runner has two modes, selected by RUNNER_SERIAL:
#   0 (default)  checks run inside concurrent "lanes" (background subshells);
#                each check's output is captured to a log, one status line is
#                printed when it finishes, and every failure is reported at the
#                end. Nothing exits early, so one run shows everything to fix.
#   1            checks run one after another in the foreground, streaming
#                their output, and the first failure exits. This is the
#                bisect/parity switch and matches the historical behaviour.
#
# Callers run `runner_init PREFIX SERIAL`, define lane functions made of
# run_check calls, start them with runner_spawn, join them with
# runner_wait_all and finish with runner_report.
#
# Interrupts: every lane is started under job control (set -m) so that it owns
# its own process group. A background job of a non-interactive shell ignores
# SIGINT, so the parent's INT/TERM trap forwards SIGTERM to each lane's group,
# which reaches the analyzer processes underneath, then removes the log dir.
#
# The functions and variables below are used by the sourcing scripts, not here.
# shellcheck disable=SC2329,SC2034

RUNNER_PREFIX="${RUNNER_PREFIX:-runner}"
RUNNER_SERIAL="${RUNNER_SERIAL:-0}"
RUNNER_STREAM="${RUNNER_STREAM:-0}"
RUNNER_LANE="${RUNNER_LANE:-main}"
RUNNER_DIR=""
RUNNER_SEQ=0
RUNNER_LANE_PIDS=()
RUNNER_LANE_NAMES=()

# Detected core count, honouring DSD_HOOK_JOBS. macOS has no nproc.
runner_detect_jobs() {
  local jobs="${DSD_HOOK_JOBS:-}"
  if [[ -z "$jobs" ]]; then
    jobs=$(nproc 2> /dev/null || sysctl -n hw.ncpu 2> /dev/null || echo 4)
  fi
  if [[ ! "$jobs" =~ ^[1-9][0-9]*$ ]]; then
    echo "${RUNNER_PREFIX}: invalid DSD_HOOK_JOBS value: ${jobs}" >&2
    return 2
  fi
  printf '%s\n' "$jobs"
}

runner_cleanup() {
  if [[ -n "$RUNNER_DIR" && -d "$RUNNER_DIR" ]]; then
    rm -rf "$RUNNER_DIR"
  fi
}

runner_on_signal() {
  trap '' INT TERM
  local pid=""
  for pid in "${RUNNER_LANE_PIDS[@]}"; do
    kill -TERM -- "-${pid}" 2> /dev/null || kill -TERM "$pid" 2> /dev/null || true
  done
  wait 2> /dev/null || true
  echo "${RUNNER_PREFIX}: interrupted." >&2
  runner_cleanup
  exit 130
}

# runner_init PREFIX SERIAL
runner_init() {
  RUNNER_PREFIX="${1:-runner}"
  RUNNER_SERIAL="${2:-0}"
  RUNNER_DIR=$(mktemp -d "${TMPDIR:-/tmp}/dsd-neo-${RUNNER_PREFIX}.XXXXXX")
  RUNNER_LANE_PIDS=()
  RUNNER_LANE_NAMES=()
  trap runner_cleanup EXIT
  trap runner_on_signal INT TERM
}

# Record a tool that is not installed; the report lists them once.
runner_note_missing() {
  printf '%s\n' "$1" >> "${RUNNER_DIR}/missing.txt"
}

# run_check LABEL COMMAND [ARGS...]
#
# Serial mode streams the command and exits on failure, exactly as the
# pre-push hook always did. Lane mode captures stdout+stderr to a per-check
# log, prints one status line, and records failures for runner_report.
# RUNNER_STREAM=1 keeps the output streaming in lane mode (for long builds
# whose progress the user wants to see) while still recording the failure.
run_check() {
  local label="$1"
  shift
  local rc=0
  if [[ "$RUNNER_SERIAL" == "1" ]]; then
    echo "==> ${label}"
    set +e
    "$@"
    rc=$?
    set -e
    if [[ $rc -ne 0 ]]; then
      echo "${RUNNER_PREFIX}: ${label} failed." >&2
      exit "$rc"
    fi
    return 0
  fi

  RUNNER_SEQ=$((RUNNER_SEQ + 1))
  local id=""
  id=$(printf '%s-%02d' "$RUNNER_LANE" "$RUNNER_SEQ")
  local log="${RUNNER_DIR}/${id}.log"
  local start=$SECONDS
  echo "--> ${label}"
  set +e
  if [[ "$RUNNER_STREAM" == "1" ]]; then
    "$@"
    rc=$?
    log=""
  else
    "$@" > "$log" 2>&1
    rc=$?
  fi
  set -e
  local elapsed=$((SECONDS - start))
  if [[ $rc -eq 0 ]]; then
    echo "==> ok    ${label} (${elapsed}s)"
  else
    echo "==> FAIL  ${label} (${elapsed}s, rc=${rc})"
    printf '%s\t%s\t%s\n' "$rc" "$label" "$log" >> "${RUNNER_DIR}/failed.txt"
  fi
  return 0
}

# runner_spawn NAME FUNCTION
#
# Lane mode: run FUNCTION in a background subshell that owns its process group.
# Serial mode: call FUNCTION inline.
runner_spawn() {
  local name="$1"
  local fn="$2"
  if [[ "$RUNNER_SERIAL" == "1" ]]; then
    RUNNER_LANE="$name"
    "$fn"
    return 0
  fi
  set -m
  (
    set +m
    RUNNER_LANE="$name"
    RUNNER_SEQ=0
    "$fn"
  ) < /dev/null &
  RUNNER_LANE_PIDS+=("$!")
  RUNNER_LANE_NAMES+=("$name")
  set +m
}

# Join every spawned lane. A lane that exits non-zero on its own (a missing
# tool under DSD_HOOK_FAIL_ON_MISSING_TOOLS=1, or an unexpected error outside
# run_check) is recorded as a failure so it can never pass silently.
runner_wait_all() {
  local i=0
  local rc=0
  for i in "${!RUNNER_LANE_PIDS[@]}"; do
    rc=0
    wait "${RUNNER_LANE_PIDS[$i]}" || rc=$?
    if [[ $rc -ne 0 ]]; then
      echo "==> FAIL  lane ${RUNNER_LANE_NAMES[$i]} aborted (rc=${rc})"
      printf '%s\t%s\t%s\n' "$rc" "lane ${RUNNER_LANE_NAMES[$i]} aborted" "" >> "${RUNNER_DIR}/failed.txt"
    fi
  done
  RUNNER_LANE_PIDS=()
  RUNNER_LANE_NAMES=()
}

# Print missing tools and every failed check's log. Returns 1 if anything failed.
runner_report() {
  local missing=""
  if [[ -s "${RUNNER_DIR}/missing.txt" ]]; then
    missing=$(sort -u "${RUNNER_DIR}/missing.txt" | tr '\n' ' ')
    echo "${RUNNER_PREFIX}: missing tools: ${missing% }" >&2
  fi
  if [[ ! -s "${RUNNER_DIR}/failed.txt" ]]; then
    return 0
  fi
  local count=0
  count=$(wc -l < "${RUNNER_DIR}/failed.txt" | tr -d ' ')
  echo "${RUNNER_PREFIX}: ${count} check(s) failed:" >&2
  local rc="" label="" log=""
  while IFS=$'\t' read -r rc label log; do
    echo "----- ${label} (rc=${rc}) -----"
    if [[ -n "$log" && -f "$log" ]]; then
      cat "$log"
    fi
  done < "${RUNNER_DIR}/failed.txt"
  echo "${RUNNER_PREFIX}: failed checks:" >&2
  cut -f2 "${RUNNER_DIR}/failed.txt" | sed 's/^/  /' >&2
  return 1
}
