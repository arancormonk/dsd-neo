#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
#
# tools/lib/check_runner.sh is the give-up logic behind every local push gate:
# it records each check's result and prints the verdict the pre-push hook and
# tools/quality_preflight.sh exit on. The failure modes that matter are the
# silent ones - a verdict that reports success because the recorded results went
# missing, a report skipped by an early exit, a check whose streaming leaks into
# the next one, an interrupt that leaves the analyzer running - so every case
# here drives the library through a throwaway script and reads back what it
# printed and what it exited with.
set -euo pipefail

ROOT_DIR=$(git rev-parse --show-toplevel)
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
# Keep the runner's own mktemp -d inside the throwaway directory.
export TMPDIR="$WORK"

failures=0
fail() {
  echo "FAIL: $1" >&2
  failures=$((failures + 1))
}

# write_case NAME BODY: a script that sources the runner and then runs BODY.
write_case() {
  local name="$1"
  local body="$2"
  {
    echo '#!/usr/bin/env bash'
    echo 'set -euo pipefail'
    printf 'source %q\n' "$ROOT_DIR/tools/lib/check_runner.sh"
    printf '%s\n' "$body"
  } > "$WORK/${name}.sh"
}

# run_case NAME: run it, leaving the status in rc and the output in $WORK/NAME.out.
rc=0
run_case() {
  local name="$1"
  set +e
  bash "$WORK/${name}.sh" > "$WORK/${name}.out" 2>&1
  rc=$?
  set -e
}

# expect_out CASE TEXT MESSAGE / refute_out CASE TEXT MESSAGE
expect_out() {
  grep -qF -- "$2" "$WORK/${1}.out" || fail "$3"
}
refute_out() {
  if grep -qF -- "$2" "$WORK/${1}.out"; then
    fail "$3"
  fi
}

# --- The verdict fails closed when the recorded results are gone. ---
#
# runner_report used to answer "nothing failed" by testing an empty RUNNER_DIR's
# failed.txt, which is a path that does not exist, so a vanished state directory
# read as a clean run and the caller printed "all checks passed".
# shellcheck disable=SC2016 # The body is expanded by the script it is written into, not here.
write_case fail_open '
runner_init t 0
run_check "a real failing check" false
rm -rf "$RUNNER_DIR"
if runner_report; then
  echo "VERDICT: all checks passed"
else
  echo "VERDICT: failed"
fi
'
run_case fail_open
expect_out fail_open "VERDICT: failed" "an unreadable state directory reported success"
expect_out fail_open "runner state directory is missing" "the missing state directory was not named"

# --- A failed check is reported with the output it produced. ---
write_case report '
runner_init t 0
run_check "check that fails" bash -c "echo IMPORTANT DIAGNOSTIC; exit 3"
run_check "check that passes" bash -c "echo QUIET SUCCESS"
if runner_report; then echo "VERDICT: passed"; else echo "VERDICT: failed"; fi
'
run_case report
expect_out report "==> FAIL  check that fails" "the failing check has no status line"
expect_out report "IMPORTANT DIAGNOSTIC" "the failing check's captured output was not reported"
expect_out report "VERDICT: failed" "a recorded failure did not fail the report"
refute_out report "QUIET SUCCESS" "a passing check's captured output should stay in its log"

# --- An early exit still prints the report before the logs are removed. ---
#
# mark_missing_tool exits under DSD_HOOK_FAIL_ON_MISSING_TOOLS=1, which fires the
# EXIT trap; the cleanup used to delete every log without reporting first, so the
# run failed with no way to see what had already failed.
write_case early_exit '
runner_init t 0
run_check "check that fails" bash -c "echo IMPORTANT DIAGNOSTIC; exit 1"
runner_note_missing scan-build
echo "failing because FAIL_ON_MISSING_TOOLS=1" >&2
exit 1
'
run_case early_exit
[[ $rc -eq 1 ]] || fail "the early exit should fail the run, got rc=$rc"
expect_out early_exit "IMPORTANT DIAGNOSTIC" "the early exit discarded the recorded logs"
expect_out early_exit "missing tools: scan-build" "the early exit discarded the missing-tool list"

# --- Serial mode stops at the first failure, but reports and exits like lane mode. ---
write_case serial '
runner_init t 1
run_check "check that fails" bash -c "echo SERIAL DIAGNOSTIC; exit 7"
echo "NOT REACHED"
'
run_case serial
[[ $rc -eq 1 ]] || fail "serial mode should exit 1 like lane mode, got rc=$rc"
expect_out serial "SERIAL DIAGNOSTIC" "serial mode did not stream the failing check"
expect_out serial "1 check(s) failed" "serial mode skipped the failure report"
refute_out serial "NOT REACHED" "serial mode continued past a failure"

# --- Streaming is per call and reaches neither the check nor the next one. ---
#
# The streaming switch used to be an environment variable set as a prefix on the
# run_check call, which exported it into the check's own environment: a nested
# gate inherited it, streamed all of its lanes onto one terminal and recorded
# every failure with an empty log.
#
# The check reports every RUNNER_ name it can see, but a CI runner exports its
# own (RUNNER_OS, RUNNER_TEMP, ...) into the whole job, so only a name this test
# was not itself started with counts as one the runner leaked.
env | sed -n 's/^\(RUNNER_[A-Z_]*\)=.*/\1/p' | sort -u > "$WORK/runner_env_ambient.txt"

# leaked_names CASE: the RUNNER_ names the check saw that the runner added.
leaked_names() {
  sed -n 's/^RUNNER_SEEN \(RUNNER_[A-Z_]*\)$/\1/p' "$WORK/${1}.out" | sort -u \
    > "$WORK/${1}.seen.txt"
  comm -13 "$WORK/runner_env_ambient.txt" "$WORK/${1}.seen.txt" | tr '\n' ' '
}

dump_runner_env='bash -c "echo STREAMED NOW; env | sed -n \"s/^\(RUNNER_[A-Z_]*\)=.*/RUNNER_SEEN \1/p\""'

write_case stream "
runner_init t 0
run_check --stream 'streamed check' ${dump_runner_env}
run_check 'captured check' bash -c 'echo CAPTURED NOT STREAMED'
if runner_report; then echo 'VERDICT: passed'; else echo 'VERDICT: failed'; fi
"
run_case stream
expect_out stream "STREAMED NOW" "--stream did not stream the check's output"
stream_leaked=$(leaked_names stream)
[[ -z "$stream_leaked" ]] || fail "a RUNNER_ variable reached the check's environment: ${stream_leaked}"
refute_out stream "CAPTURED NOT STREAMED" "--stream stayed on for the next check"
expect_out stream "VERDICT: passed" "two passing checks did not pass"

# --- ...and the check above only means something if a leak is still visible. ---
#
# An exported RUNNER_ variable in the runner's own scope is what the streaming
# switch used to be, so it stands in for the regression: the same check has to
# report it, or filtering out the CI runner's variables has blinded the case.
write_case stream_leak "
export RUNNER_LEAK_PROBE=1
runner_init t 0
run_check --stream 'streamed check' ${dump_runner_env}
"
run_case stream_leak
[[ "$(leaked_names stream_leak)" == "RUNNER_LEAK_PROBE " ]] ||
  fail "the leak check no longer sees a RUNNER_ variable exported into a check"

# --- RUNNER_LAST_RC lets a caller gate follow-up work on the check it just ran. ---
#
# quality_preflight skips the fuzz smoke pass when the fuzz build failed; without
# this the smoke pass ran on binaries an earlier build had left behind.
# shellcheck disable=SC2016 # The body is expanded by the script it is written into, not here.
write_case last_rc '
runner_init t 0
run_check "failing build" bash -c "exit 4"
echo "AFTER_BUILD=$RUNNER_LAST_RC"
run_check "passing step" true
echo "AFTER_STEP=$RUNNER_LAST_RC"
runner_report || true
'
run_case last_rc
expect_out last_rc "AFTER_BUILD=4" "RUNNER_LAST_RC did not carry the failing check's status"
expect_out last_rc "AFTER_STEP=0" "RUNNER_LAST_RC did not carry the passing check's status"

# --- A lane that exits on its own is recorded, never silently passed. ---
write_case lane_abort '
lane_a() { run_check "inside lane" true; exit 9; }
runner_init t 0
runner_spawn a lane_a
runner_wait_all
if runner_report; then echo "VERDICT: passed"; else echo "VERDICT: failed"; fi
'
run_case lane_abort
expect_out lane_abort "lane a aborted (rc=9)" "an aborted lane was not recorded"
expect_out lane_abort "VERDICT: failed" "an aborted lane did not fail the report"

# --- A TERM during a check takes the check down with it, promptly. ---
#
# bash runs a trap only between commands, so a check run in the foreground
# deferred INT/TERM for as long as the check took - minutes, for the whole
# pre-push suite or a scan-build - and killing the runner left the analyzer
# running. Checks are background jobs in their own process group now.
write_case signal "
runner_init t 0
run_check 'long check' bash -c 'echo \$\$ > ${WORK}/child.pid; sleep 60'
"
rm -f "$WORK/child.pid"
bash "$WORK/signal.sh" > "$WORK/signal.out" 2>&1 &
signal_pid=$!
for _ in $(seq 1 100); do
  if [[ -s "$WORK/child.pid" ]]; then
    break
  fi
  sleep 0.1
done
child_pid=$(cat "$WORK/child.pid" 2> /dev/null || echo "")
[[ -n "$child_pid" ]] || fail "the long check never started"
kill -TERM "$signal_pid" 2> /dev/null || true
waited=0
while kill -0 "$signal_pid" 2> /dev/null && [[ $waited -lt 100 ]]; do
  sleep 0.1
  waited=$((waited + 1))
done
if kill -0 "$signal_pid" 2> /dev/null; then
  fail "the runner ignored SIGTERM while a check was running"
  kill -KILL "$signal_pid" 2> /dev/null || true
fi
set +e
wait "$signal_pid"
signal_rc=$?
set -e
[[ $signal_rc -eq 130 ]] || fail "an interrupted run should exit 130, got rc=$signal_rc"
grep -qF "interrupted" "$WORK/signal.out" || fail "the interrupt was not reported"
sleep 0.2
if [[ -n "$child_pid" ]] && kill -0 "$child_pid" 2> /dev/null; then
  fail "the check outlived the runner that was told to stop"
  kill -KILL "$child_pid" 2> /dev/null || true
fi

if [[ $failures -ne 0 ]]; then
  echo "check_runner: ${failures} assertion(s) failed" >&2
  exit 1
fi
echo "check_runner: all assertions passed"
