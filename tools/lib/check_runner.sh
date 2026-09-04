# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
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
#                their output, and the first failure stops the run. This is the
#                bisect/parity switch: it reports the same failure list and
#                exits 1 just as lane mode does, only sooner.
#
# Callers run `runner_init PREFIX SERIAL`, define lane functions made of
# run_check calls, start them with runner_spawn, join them with
# runner_wait_all and finish with runner_report.
#
# The verdict is the set of files under RUNNER_DIR, so every function that
# reads or writes them refuses to run when that directory is missing: a runner
# that cannot record a failure must never be able to report success.
#
# Interrupts: every lane, and every check the runner starts, is a background job
# in its own process group, which the parent then waits for. A background job of
# a non-interactive shell ignores SIGINT, so the parent's HUP/INT/TERM trap
# forwards SIGTERM to each group, which reaches the analyzer processes
# underneath, then removes the log dir. Waiting on a background job rather than
# running the check in the foreground is what lets the trap run at all: bash
# defers a trap until the foreground command returns. Serial mode goes through
# the same background-and-wait path for that reason, and streams instead of
# capturing. SIGHUP is trapped alongside INT/TERM because a closed terminal
# otherwise kills the gate and leaves every analyzer it started running.

# Set by runner_init; the defaults here only cover a caller that reads one
# before calling it. None of them is read from the environment, so nothing a
# check exports can change how the runner behaves.
RUNNER_PREFIX="runner"
RUNNER_SERIAL="0"
RUNNER_LANE="main"
RUNNER_IN_LANE=0
RUNNER_DIR=""
RUNNER_SEQ=0
RUNNER_REPORTED=0
RUNNER_CHILD_PID=""
# Set by runner_report: 1 when a tool was missing, so the checks it runs did not
# run. Callers read it so a run that skipped analyses never claims to have
# passed them.
RUNNER_MISSING=0
# Exit status of the last run_check that ran in this shell. Callers gate
# follow-up work on it; a check started inside a lane cannot report back here.
RUNNER_LAST_RC=0
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

# The recorded failures are the verdict. If the directory holding them is gone,
# an empty failed.txt would read as "nothing failed", so say so and fail.
runner_require_dir() {
  if [[ -n "$RUNNER_DIR" && -d "$RUNNER_DIR" ]]; then
    return 0
  fi
  echo "${RUNNER_PREFIX}: internal error: runner state directory is missing (${RUNNER_DIR:-unset});" \
    "cannot record or report check results." >&2
  return 1
}

runner_cleanup() {
  # Lanes own their process groups, so nothing the shell exits from reaches
  # them: without this an abort between runner_spawn and runner_wait_all would
  # remove the state directory out from under analyzers that keep running and
  # keep appending to a deleted path.
  runner_stop_children
  # Something exited before runner_report ran: a missing tool under
  # DSD_HOOK_FAIL_ON_MISSING_TOOLS=1, serial mode's fail-fast, or an errexit
  # abort. Print what was recorded before the logs go away.
  if [[ "$RUNNER_REPORTED" != "1" ]] && [[ -n "$RUNNER_DIR" && -d "$RUNNER_DIR" ]]; then
    runner_report || true
  fi
  if [[ -n "$RUNNER_DIR" && -d "$RUNNER_DIR" ]]; then
    rm -rf "$RUNNER_DIR"
  fi
  RUNNER_DIR=""
}

# runner_signal_group SIGNAL PID: signal the whole process group, or the job
# alone if it did not get a group of its own.
runner_signal_group() {
  kill "-${1}" -- "-${2}" 2> /dev/null || kill "-${1}" "$2" 2> /dev/null || true
}

# runner_group_alive PID: true while any process in that group is still running.
# The group is what was signalled, and a lane's leader routinely exits while the
# analyzer underneath it does not, so testing the leader alone reports the group
# dead and skips the escalation below. Group-only on purpose: once bash has
# reaped a leader its pid can name an unrelated process, but a process group id
# stays taken for as long as the group has members.
runner_group_alive() {
  kill -0 -- "-${1}" 2> /dev/null
}

# TERM every process group this shell started, give them a bounded grace period,
# then KILL whichever groups are still up. Called from the signal handler and
# from the EXIT trap, so no path removes the log directory while a check is
# still writing to it. Every pid tracked here was started under `set -m`, so it
# leads its own group.
runner_stop_children() {
  local pid="" waited=0 alive=0
  local pids=()
  if [[ -n "$RUNNER_CHILD_PID" ]]; then
    pids+=("$RUNNER_CHILD_PID")
  fi
  if [[ ${#RUNNER_LANE_PIDS[@]} -gt 0 ]]; then
    pids+=("${RUNNER_LANE_PIDS[@]}")
  fi
  if [[ ${#pids[@]} -eq 0 ]]; then
    return 0
  fi
  RUNNER_CHILD_PID=""
  RUNNER_LANE_PIDS=()
  RUNNER_LANE_NAMES=()
  for pid in "${pids[@]}"; do
    runner_signal_group TERM "$pid"
  done
  # Bounded: INT/TERM are ignored for the duration of the handler, so there is
  # no second Ctrl-C to escape with, and a check that does not die on SIGTERM (a
  # docker client, say) would otherwise block the `wait` below forever.
  while [[ $waited -lt 100 ]]; do
    alive=0
    for pid in "${pids[@]}"; do
      if runner_group_alive "$pid"; then
        alive=1
      fi
    done
    if [[ $alive -eq 0 ]]; then
      break
    fi
    sleep 0.1
    waited=$((waited + 1))
  done
  if [[ $alive -eq 1 ]]; then
    echo "${RUNNER_PREFIX}: a check did not stop on SIGTERM; killing it." >&2
    for pid in "${pids[@]}"; do
      if runner_group_alive "$pid"; then
        kill -KILL -- "-${pid}" 2> /dev/null || true
      fi
    done
  fi
  wait 2> /dev/null || true
}

runner_on_signal() {
  trap '' HUP INT TERM
  # Said before the stopping starts: further interrupts are ignored from here,
  # so silence for the length of the grace period reads as a lost Ctrl-C.
  echo "${RUNNER_PREFIX}: interrupted; stopping the checks." >&2
  runner_stop_children
  # A run the user stopped has no verdict to report, only half-finished checks.
  RUNNER_REPORTED=1
  runner_cleanup
  exit 130
}

# runner_init PREFIX SERIAL
runner_init() {
  RUNNER_PREFIX="${1:-runner}"
  RUNNER_SERIAL="${2:-0}"
  RUNNER_DIR=$(mktemp -d "${TMPDIR:-/tmp}/dsd-neo-${RUNNER_PREFIX}.XXXXXX")
  RUNNER_LANE="main"
  RUNNER_IN_LANE=0
  RUNNER_SEQ=0
  RUNNER_REPORTED=0
  RUNNER_CHILD_PID=""
  RUNNER_MISSING=0
  RUNNER_LAST_RC=0
  RUNNER_LANE_PIDS=()
  RUNNER_LANE_NAMES=()
  trap runner_cleanup EXIT
  trap runner_on_signal HUP INT TERM
}

# Record a tool that is not installed; the report lists them once.
runner_note_missing() {
  runner_require_dir || exit 1
  printf '%s\n' "$1" >> "${RUNNER_DIR}/missing.txt"
}

# run_check [--stream] LABEL COMMAND [ARGS...]
#
# Serial mode streams the command, and the first failure stops the run after
# the failure list has been printed. Lane mode captures stdout+stderr to a
# per-check log, prints one status line, and records failures for
# runner_report. --stream keeps a check's output on the terminal in lane mode
# (for long builds whose progress the user wants to see) while still recording
# the failure; it is a per-call argument rather than an environment variable so
# that it cannot leak into the environment of the check itself.
#
# Either way RUNNER_LAST_RC holds the check's exit status afterwards.
run_check() {
  local stream=0
  if [[ "${1:-}" == "--stream" ]]; then
    stream=1
    shift
  fi
  local label="$1"
  shift
  runner_require_dir || exit 1
  local rc=0

  if [[ "$RUNNER_SERIAL" == "1" ]]; then
    echo "==> ${label}"
    # Background job in its own process group, then wait - the same shape lane
    # mode uses, and for the same reason: bash defers a trap until the
    # foreground command returns, so a check run in the foreground swallows
    # INT/TERM for as long as it takes (minutes, for scan-build) and leaves the
    # analyzer running when the runner is finally told to stop.
    set -m
    "$@" < /dev/null &
    RUNNER_CHILD_PID=$!
    set +m
    set +e
    wait "$RUNNER_CHILD_PID"
    rc=$?
    set -e
    RUNNER_CHILD_PID=""
    RUNNER_LAST_RC=$rc
    if [[ $rc -ne 0 ]]; then
      echo "==> FAIL  ${label} (rc=${rc})"
      printf '%s\t%s\t%s\n' "$rc" "$label" "" >> "${RUNNER_DIR}/failed.txt"
      echo "${RUNNER_PREFIX}: stopping at the first failure (serial mode)." >&2
      # The EXIT trap prints the recorded failures; exit 1 in both modes so the
      # two can be compared by exit code.
      exit 1
    fi
    return 0
  fi

  RUNNER_SEQ=$((RUNNER_SEQ + 1))
  local id=""
  id=$(printf '%s-%02d' "$RUNNER_LANE" "$RUNNER_SEQ")
  local log="${RUNNER_DIR}/${id}.log"
  local start=$SECONDS
  echo "--> ${label}"
  # Outside a lane the check gets its own process group, so the INT/TERM trap
  # can take its whole process tree down with it. Inside a lane it must stay in
  # the lane's group, which is what the trap already signals.
  if [[ "$RUNNER_IN_LANE" != "1" ]]; then
    set -m
  fi
  if [[ $stream -eq 1 ]]; then
    "$@" < /dev/null &
  else
    "$@" > "$log" 2>&1 < /dev/null &
  fi
  RUNNER_CHILD_PID=$!
  if [[ "$RUNNER_IN_LANE" != "1" ]]; then
    set +m
  fi
  set +e
  wait "$RUNNER_CHILD_PID"
  rc=$?
  set -e
  RUNNER_CHILD_PID=""
  # shellcheck disable=SC2034 # Read by the sourcing script once run_check returns.
  RUNNER_LAST_RC=$rc
  if [[ $stream -eq 1 ]]; then
    log=""
  fi
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
    RUNNER_LANE="main"
    return 0
  fi
  set -m
  (
    set +m
    RUNNER_LANE="$name"
    RUNNER_IN_LANE=1
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
  if [[ ${#RUNNER_LANE_PIDS[@]} -eq 0 ]]; then
    return 0
  fi
  runner_require_dir || exit 1
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

# Print missing tools and every failed check's log. Returns 1 if anything
# failed, or if the recorded results are unreadable. Sets RUNNER_MISSING when a
# tool was absent, so the caller can say that the run skipped analyses rather
# than that it passed them.
runner_report() {
  RUNNER_REPORTED=1
  RUNNER_MISSING=0
  runner_require_dir || return 1
  local missing=""
  if [[ -s "${RUNNER_DIR}/missing.txt" ]]; then
    # shellcheck disable=SC2034 # Read by the sourcing script once runner_report returns.
    RUNNER_MISSING=1
    missing=$(sort -u "${RUNNER_DIR}/missing.txt" | tr '\n' ' ')
    echo "${RUNNER_PREFIX}: missing tools: ${missing% }" >&2
  fi
  if [[ ! -s "${RUNNER_DIR}/failed.txt" ]]; then
    return 0
  fi
  local count=0
  count=$(wc -l < "${RUNNER_DIR}/failed.txt" | tr -d ' ')
  local rc="" label="" log=""
  # The whole report goes to stderr, so a caller that redirects one stream keeps
  # the counts, the logs and the failed-check list together rather than splitting
  # the summary from what it summarizes.
  {
    echo "${RUNNER_PREFIX}: ${count} check(s) failed:"
    while IFS=$'\t' read -r rc label log; do
      echo "----- ${label} (rc=${rc}) -----"
      if [[ -n "$log" && -f "$log" ]]; then
        cat "$log"
      fi
    done < "${RUNNER_DIR}/failed.txt"
    echo "${RUNNER_PREFIX}: failed checks:"
    cut -f2 "${RUNNER_DIR}/failed.txt" | sed 's/^/  /'
  } >&2
  return 1
}
