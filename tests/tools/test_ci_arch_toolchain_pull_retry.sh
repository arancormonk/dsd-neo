#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
#
# tools/ci_arch_toolchain.sh must survive a transient registry failure: Docker
# Hub answering the image manifest fetch with a 5xx failed a CI job whose only
# step is this wrapper, and a manual re-run was the only recovery. A fake
# `docker` on PATH counts pulls and runs, so the wrapper's retry and its give-up
# are both observable without a daemon.
set -euo pipefail

ROOT_DIR=$(git rev-parse --show-toplevel)
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

cat > "$WORK/docker" << 'FAKE'
#!/usr/bin/env bash
# Fake docker: `pull` fails until FAKE_DOCKER_PULL_FAILURES pulls have been
# refused, then succeeds; `run` records its image argument and exits 0.
set -euo pipefail
log="$FAKE_DOCKER_LOG"
case "$1" in
  pull)
    n=$(( $(cat "$log.pulls" 2>/dev/null || echo 0) + 1 ))
    echo "$n" > "$log.pulls"
    if [[ "$n" -le "${FAKE_DOCKER_PULL_FAILURES:-0}" ]]; then
      echo "docker: Error response from daemon: received unexpected HTTP status: 500 Internal Server Error" >&2
      exit 1
    fi
    ;;
  run)
    printf '%s\n' "$@" > "$log.run"
    ;;
  *)
    echo "fake docker: unexpected subcommand $1" >&2
    exit 99
    ;;
esac
FAKE
chmod +x "$WORK/docker"

failures=0
fail() {
  echo "FAIL: $1" >&2
  failures=$((failures + 1))
}

run_wrapper() {
  # Backoff pinned to zero so a five-attempt retry costs nothing here.
  env PATH="$WORK:$PATH" FAKE_DOCKER_LOG="$WORK/log" \
    CI_ARCH_PULL_BACKOFF_S=0 "$@" \
    bash "$ROOT_DIR/tools/ci_arch_toolchain.sh" true > "$WORK/out" 2> "$WORK/err"
}

# --- Two transient failures, then success: the wrapper keeps pulling and runs. ---
rm -f "$WORK/log.pulls" "$WORK/log.run"
if ! run_wrapper env FAKE_DOCKER_PULL_FAILURES=2 CI_ARCH_PULL_ATTEMPTS=5; then
  fail "the wrapper gave up although the third pull succeeded"
fi
pulls=$(cat "$WORK/log.pulls" 2> /dev/null || echo 0)
[[ "$pulls" == "3" ]] || fail "expected 3 pull attempts, saw $pulls"
[[ -f "$WORK/log.run" ]] || fail "docker run never happened after the pull succeeded"
if [[ -f "$WORK/log.run" ]]; then
  # shellcheck source=tools/ci-dependency-pins.env
  # shellcheck disable=SC1091
  source "$ROOT_DIR/tools/ci-dependency-pins.env"
  grep -qxF "$ARCHLINUX_BASE_DEVEL_IMAGE" "$WORK/log.run" || fail "docker run did not receive the pinned image"
fi
grep -q "retrying" "$WORK/err" || fail "a retried pull was not reported on stderr"

# --- Every pull fails: the wrapper gives up after its attempt budget, without running. ---
rm -f "$WORK/log.pulls" "$WORK/log.run"
if run_wrapper env FAKE_DOCKER_PULL_FAILURES=99 CI_ARCH_PULL_ATTEMPTS=2; then
  fail "the wrapper reported success although every pull failed"
fi
pulls=$(cat "$WORK/log.pulls" 2> /dev/null || echo 0)
[[ "$pulls" == "2" ]] || fail "expected exactly 2 pull attempts before giving up, saw $pulls"
[[ ! -f "$WORK/log.run" ]] || fail "docker run happened although the image never arrived"
grep -q "failed after 2 attempts" "$WORK/err" || fail "the give-up message is missing"

if [[ "$failures" -ne 0 ]]; then
  echo "$failures failure(s)" >&2
  exit 1
fi
echo "OK"
