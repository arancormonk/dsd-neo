#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
#
# tools/check_workflow_download_pins.sh keeps the inline archlinux:base-devel
# digests in the workflows equal to ARCHLINUX_BASE_DEVEL_IMAGE. The jobs that run
# in the image name it inline (a container image cannot read the env file), and
# the copies drifted from the pin once. It also keeps a job that pulls its
# container with credentials off pull requests, where Dependabot and forks have
# no secrets and GitHub rejects the empty ones. The script runs inside a throwaway
# repository holding the real pins file and one workflow, so each case edits only
# the workflow.
set -euo pipefail

ROOT_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)

# shellcheck source=tests/tools/missing_tool.sh
source "$(dirname -- "$0")/missing_tool.sh"
if ! command -v git > /dev/null 2>&1 || ! command -v rg > /dev/null 2>&1; then
  missing_tool "git or ripgrep not available"
fi

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

REPO="$WORK/repo"
mkdir -p "$REPO/tools" "$REPO/.github/workflows"
git -C "$REPO" init -q
cp "$ROOT_DIR/tools/check_workflow_download_pins.sh" "$REPO/tools/"
cp "$ROOT_DIR/tools/ci-dependency-pins.env" "$REPO/tools/"
: > "$REPO/.github/workflows/linux-appimage.yaml"

# shellcheck source=tools/ci-dependency-pins.env
# shellcheck disable=SC1091
source "$ROOT_DIR/tools/ci-dependency-pins.env"
pinned="$ARCHLINUX_BASE_DEVEL_IMAGE"
other="archlinux:base-devel@sha256:$(printf '0%.0s' {1..64})"

failures=0
fail() {
  echo "FAIL: $1" >&2
  failures=$((failures + 1))
}

# run_case NAME IMAGE...: one workflow naming each IMAGE inline; the verdict is
# left in rc and the output in $WORK/NAME.out.
rc=0
run_case() {
  local name="$1"
  shift
  {
    echo "jobs:"
    local i=0 image
    for image in "$@"; do
      echo "  job$i:"
      echo "    container:"
      echo "      image: $image"
      i=$((i + 1))
    done
  } > "$REPO/.github/workflows/ci.yaml"
  rc=0
  (cd "$REPO" && ./tools/check_workflow_download_pins.sh) > "$WORK/$name.out" 2>&1 || rc=$?
}

run_case match "$pinned" "$pinned"
if [[ $rc -ne 0 ]]; then
  fail "inline digests equal to the pin failed the check"
  cat "$WORK/match.out" >&2
fi

run_case drift "$pinned" "$other"
if [[ $rc -eq 0 ]]; then
  fail "an inline digest that differs from ARCHLINUX_BASE_DEVEL_IMAGE passed"
fi
if ! grep -qF "Inline archlinux:base-devel image differs from ARCHLINUX_BASE_DEVEL_IMAGE" "$WORK/drift.out"; then
  fail "the drift was not reported"
  cat "$WORK/drift.out" >&2
fi
if ! grep -qF "$other" "$WORK/drift.out"; then
  fail "the drifted reference was not shown"
fi

run_case lone "$other"
if [[ $rc -eq 0 ]]; then
  fail "a single drifted inline digest passed"
fi

# run_kotlin_case NAME CHECK_LINE: a workflow that downloads the Kotlin compiler,
# followed by CHECK_LINE; the verdict is left in rc.
run_kotlin_case() {
  local name="$1" check="$2"
  {
    echo "jobs:"
    echo "  job0:"
    echo "    steps:"
    echo "      - run: |"
    # shellcheck disable=SC2016 # the workflow, not this test, expands these
    echo '          curl -fsSL -o kotlin.zip "https://github.com/JetBrains/kotlin/releases/download/v${KOTLIN_COMPILER_VERSION}/kotlin-compiler-${KOTLIN_COMPILER_VERSION}.zip"'
    echo "          $check"
    echo "          unzip -q kotlin.zip"
  } > "$REPO/.github/workflows/ci.yaml"
  rc=0
  (cd "$REPO" && ./tools/check_workflow_download_pins.sh) > "$WORK/$name.out" 2>&1 || rc=$?
}

# shellcheck disable=SC2016 # the workflow, not this test, expands these
run_kotlin_case kotlin_verified 'echo "${KOTLIN_COMPILER_SHA256}  kotlin.zip" | sha256sum -c -'
if [[ $rc -ne 0 ]]; then
  fail "a Kotlin download verified against KOTLIN_COMPILER_SHA256 failed the check"
  cat "$WORK/kotlin_verified.out" >&2
fi

run_kotlin_case kotlin_unverified 'true'
if [[ $rc -eq 0 ]]; then
  fail "an unverified Kotlin download passed"
fi
if ! grep -qF "Kotlin compiler download in .github/workflows/ci.yaml must be verified" "$WORK/kotlin_unverified.out"; then
  fail "the unverified Kotlin download was not reported"
  cat "$WORK/kotlin_unverified.out" >&2
fi

# run_credentials_case NAME: the workflow on stdin replaces ci.yaml; the verdict is
# left in rc and the output in $WORK/NAME.out.
run_credentials_case() {
  local name="$1"
  cat > "$REPO/.github/workflows/ci.yaml"
  rc=0
  (cd "$REPO" && ./tools/check_workflow_download_pins.sh) > "$WORK/$name.out" 2>&1 || rc=$?
}
# expect_credentials_verdict NAME WANT: WANT is "pass" or "fail".
expect_credentials_verdict() {
  local name="$1" want="$2"
  if [[ "$want" == pass && $rc -ne 0 ]]; then
    fail "$name: failed the check"
    cat "$WORK/$name.out" >&2
  elif [[ "$want" == fail ]]; then
    if [[ $rc -eq 0 ]]; then
      fail "$name: passed"
    elif ! grep -qF "can run on a pull request" "$WORK/$name.out" ||
      ! grep -qF "job build" "$WORK/$name.out"; then
      fail "$name: the job was not reported"
      cat "$WORK/$name.out" >&2
    fi
  fi
}

# GitHub rejects empty container credentials before the job starts, and Dependabot
# and fork PRs get no secrets: a job that pulls with credentials must stay off pull
# requests (#665 put them on two that did not, and every Dependabot PR failed both).
run_credentials_case pr_job_with_credentials << EOF
on:
  pull_request:
jobs:
  build:
    runs-on: ubuntu-latest
    container:
      image: $pinned
      credentials:
        username: \${{ secrets.DOCKERHUB_USERNAME }}
        password: \${{ secrets.DOCKERHUB_TOKEN }}
    steps:
      - run: true
EOF
expect_credentials_verdict pr_job_with_credentials fail

run_credentials_case pr_service_with_credentials << EOF
on:
  push:
  pull_request:
jobs:
  build:
    runs-on: ubuntu-latest
    services:
      cache:
        image: $pinned
        credentials:
          username: \${{ secrets.DOCKERHUB_USERNAME }}
          password: \${{ secrets.DOCKERHUB_TOKEN }}
    steps:
      - run: true
EOF
expect_credentials_verdict pr_service_with_credentials fail

# A step-level condition does not keep the job, and its container pull, off a PR.
run_credentials_case step_condition_only << EOF
on:
  pull_request:
jobs:
  build:
    runs-on: ubuntu-latest
    container:
      image: $pinned
      credentials:
        username: \${{ secrets.DOCKERHUB_USERNAME }}
        password: \${{ secrets.DOCKERHUB_TOKEN }}
    steps:
      - if: github.event_name != 'pull_request'
        run: true
EOF
expect_credentials_verdict step_condition_only fail

run_credentials_case job_off_pull_requests << EOF
on:
  pull_request:
jobs:
  build:
    runs-on: ubuntu-latest
    if: github.event_name != 'pull_request'
    container:
      image: $pinned
      credentials:
        username: \${{ secrets.DOCKERHUB_USERNAME }}
        password: \${{ secrets.DOCKERHUB_TOKEN }}
    steps:
      - run: true
EOF
expect_credentials_verdict job_off_pull_requests pass

# The AUR jobs' shape: the container first, and a folded condition after it.
run_credentials_case folded_condition_after_container << EOF
on:
  pull_request:
jobs:
  build:
    runs-on: ubuntu-latest
    container:
      image: $pinned
      credentials:
        username: \${{ secrets.DOCKERHUB_USERNAME }}
        password: \${{ secrets.DOCKERHUB_TOKEN }}
    if: >-
      \${{
        always() &&
        github.event_name != 'pull_request' &&
        github.repository == 'arancormonk/dsd-neo'
      }}
    steps:
      - run: true
EOF
expect_credentials_verdict folded_condition_after_container pass

# credentialed_job KEY CONDITION_LINE...: a pull_request workflow whose one job,
# under KEY, pulls with credentials and carries the given job-level lines.
credentialed_job() {
  local key="$1"
  shift
  printf 'on: [push, pull_request]\njobs:\n  %s\n    runs-on: ubuntu-latest\n' "$key"
  printf '%s\n' "$@"
  printf '    container:\n      image: %s\n      credentials:\n' "$pinned"
  # shellcheck disable=SC2016 # the workflow, not this test, expands these
  printf '        username: ${{ secrets.DOCKERHUB_USERNAME }}\n        password: ${{ secrets.DOCKERHUB_TOKEN }}\n'
  printf '    steps:\n      - run: true\n'
}

# A condition only keeps pull requests out when no branch of it lets one through:
# the exclusion has to be a top-level && term, outside comments. The workflow goes
# in through process substitution: a pipe would run the case, and set rc, in a
# subshell.
run_credentials_case same_repo_alternative < <(credentialed_job build: \
  "    if: github.event_name != 'pull_request' || github.event.pull_request.head.repo.full_name == github.repository")
expect_credentials_verdict same_repo_alternative fail
run_credentials_case exclusion_in_a_comment < <(credentialed_job build: \
  "    if: always() # github.event_name != 'pull_request'")
expect_credentials_verdict exclusion_in_a_comment fail
run_credentials_case exclusion_inside_an_alternative < <(credentialed_job build: \
  "    if: \${{ (github.event_name != 'pull_request' || github.ref == 'refs/heads/main') && always() }}")
expect_credentials_verdict exclusion_inside_an_alternative fail
run_credentials_case parenthesized_conjunction < <(credentialed_job build: \
  "    if: \${{ (github.repository == 'arancormonk/dsd-neo' && github.event_name != 'pull_request') }}")
expect_credentials_verdict parenthesized_conjunction pass
run_credentials_case single_quoted_scalar < <(credentialed_job build: \
  "    if: 'github.event_name != ''pull_request'''")
expect_credentials_verdict single_quoted_scalar pass

# Quoted and commented job keys are jobs too.
run_credentials_case quoted_job_key < <(credentialed_job '"build":')
expect_credentials_verdict quoted_job_key fail
run_credentials_case commented_job_key < <(credentialed_job 'build: # required check')
expect_credentials_verdict commented_job_key fail

# A job does not inherit the exemption of the one before it.
run_credentials_case exemption_does_not_carry_over < <(
  credentialed_job release: "    if: github.event_name != 'pull_request'"
  credentialed_job '"build":' | sed '1,2d'
)
expect_credentials_verdict exemption_does_not_carry_over fail

run_credentials_case pull_request_target_trigger < <(
  credentialed_job build: | sed 's/^on: .*/on:\n  pull_request_target:/'
)
expect_credentials_verdict pull_request_target_trigger fail

# Credentials count in any YAML form, and a trigger list need not be indented.
run_credentials_case flow_style_container < <(
  credentialed_job build: | sed '/^      image:/,/^        password:/d' |
    sed "s|^    container:|    container: {image: $pinned, credentials: {username: u, password: p}}|"
)
expect_credentials_verdict flow_style_container fail
run_credentials_case indentless_trigger_list < <(
  credentialed_job build: | sed 's/^on: .*/on:\n- push\n- pull_request/'
)
expect_credentials_verdict indentless_trigger_list fail

# The exclusion has to name the pull-request events the workflow receives:
# pull_request_target needs its own, a reusable workflow takes its caller's event,
# and an equality with a non-PR event excludes them all.
run_credentials_case target_excluded_as_pull_request < <(
  credentialed_job build: "    if: github.event_name != 'pull_request'" |
    sed 's/^on: .*/on: [pull_request_target]/'
)
expect_credentials_verdict target_excluded_as_pull_request fail
run_credentials_case target_excluded < <(
  credentialed_job build: "    if: github.event_name != 'pull_request_target'" |
    sed 's/^on: .*/on: [pull_request_target]/'
)
expect_credentials_verdict target_excluded pass
run_credentials_case both_events_one_excluded < <(
  credentialed_job build: "    if: github.event_name != 'pull_request'" |
    sed 's/^on: .*/on: [pull_request, pull_request_target]/'
)
expect_credentials_verdict both_events_one_excluded fail
run_credentials_case reusable_workflow < <(
  credentialed_job build: "    if: github.event_name != 'pull_request'" |
    sed 's/^on: .*/on: [workflow_call]/'
)
expect_credentials_verdict reusable_workflow fail
run_credentials_case push_only_condition < <(
  credentialed_job build: "    if: github.event_name == 'push' && github.ref == 'refs/heads/main'"
)
expect_credentials_verdict push_only_condition pass

# A quoted credentials key is the same key.
run_credentials_case quoted_credentials_key < <(
  credentialed_job build: | sed 's/^      credentials:/      "credentials":/'
)
expect_credentials_verdict quoted_credentials_key fail
run_credentials_case quoted_credentials_key_in_flow < <(
  credentialed_job build: | sed '/^      image:/,/^        password:/d' |
    sed "s|^    container:|    container: {image: $pinned, 'credentials': {username: u, password: p}}|"
)
expect_credentials_verdict quoted_credentials_key_in_flow fail

# GitHub compares strings case-insensitively: == 'PULL_REQUEST' selects pull
# requests, and != 'Pull_Request' excludes them.
run_credentials_case uppercase_equality < <(
  credentialed_job build: "    if: github.event_name == 'PULL_REQUEST'"
)
expect_credentials_verdict uppercase_equality fail
run_credentials_case mixed_case_exclusion < <(
  credentialed_job build: "    if: github.event_name != 'Pull_Request'"
)
expect_credentials_verdict mixed_case_exclusion pass

# Only event names count as triggers, not an input that happens to be called
# pull_request.
run_credentials_case dispatch_input_named_pull_request < <(
  credentialed_job build: |
    sed 's/^on: .*/on:\n  workflow_dispatch:\n    inputs:\n      pull_request:\n        description: number/'
)
expect_credentials_verdict dispatch_input_named_pull_request pass

# A job the line reader cannot see into fails closed: a flow-style job with
# credentials, whatever its condition, and a container taken from an alias.
run_credentials_case flow_style_job << EOF
on: [pull_request]
jobs:
  build: {runs-on: ubuntu-latest, if: "github.event_name != 'pull_request'", container: {image: $pinned, credentials: {username: u, password: p}}, steps: [{run: "true"}]}
EOF
expect_credentials_verdict flow_style_job fail
run_credentials_case container_from_alias << EOF
on: [pull_request]
jobs:
  release:
    if: github.event_name != 'pull_request'
    runs-on: ubuntu-latest
    container: &arch
      image: $pinned
      credentials:
        username: \${{ secrets.DOCKERHUB_USERNAME }}
        password: \${{ secrets.DOCKERHUB_TOKEN }}
    steps:
      - run: true
  build:
    runs-on: ubuntu-latest
    container: *arch
    steps:
      - run: true
EOF
expect_credentials_verdict container_from_alias fail
grep -qF "job build (aliases" "$WORK/container_from_alias.out" || fail "container_from_alias: the alias was not named as the reason"
run_credentials_case anchored_job << EOF
on: [pull_request]
jobs:
  build: &build
    if: github.event_name != 'pull_request'
    runs-on: ubuntu-latest
    container:
      image: $pinned
      credentials:
        username: \${{ secrets.DOCKERHUB_USERNAME }}
        password: \${{ secrets.DOCKERHUB_TOKEN }}
    steps:
      - run: true
EOF
expect_credentials_verdict anchored_job pass

# Flow-style triggers are read in their compact and next-line forms, and triggers
# that cannot be read at all count as pull requests.
run_credentials_case compact_flow_trigger < <(
  credentialed_job build: | sed 's/^on: .*/on: {"pull_request":{}}/'
)
expect_credentials_verdict compact_flow_trigger fail
run_credentials_case next_line_flow_trigger < <(
  credentialed_job build: | sed 's/^on: .*/on:\n  [push, pull_request]/'
)
expect_credentials_verdict next_line_flow_trigger fail
run_credentials_case unreadable_trigger < <(
  credentialed_job build: | sed 's/^on: .*/on: *triggers/'
)
expect_credentials_verdict unreadable_trigger fail

# A jobs mapping, or a job, written in flow style fails closed on credentials.
run_credentials_case inline_jobs_mapping << EOF
on: [pull_request]
jobs: {build: {runs-on: ubuntu-latest, container: {image: $pinned, credentials: {username: u, password: p}}, steps: [{run: "true"}]}}
EOF
if [[ $rc -eq 0 ]]; then
  fail "inline_jobs_mapping: passed"
fi
run_credentials_case next_line_flow_job << EOF
on: [pull_request]
jobs:
  build:
    {runs-on: ubuntu-latest, container: {image: $pinned, credentials: {username: u, password: p}}, steps: [{run: "true"}]}
EOF
expect_credentials_verdict next_line_flow_job fail

run_credentials_case push_only_workflow << EOF
on:
  push:
jobs:
  build:
    runs-on: ubuntu-latest
    container:
      image: $pinned
      credentials:
        username: \${{ secrets.DOCKERHUB_USERNAME }}
        password: \${{ secrets.DOCKERHUB_TOKEN }}
    steps:
      - run: true
EOF
expect_credentials_verdict push_only_workflow pass

run_credentials_case persist_credentials_only << EOF
on:
  pull_request:
jobs:
  build:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@3d3c42e5aac5ba805825da76410c181273ba90b1 # v7.0.1
        with:
          persist-credentials: false
EOF
expect_credentials_verdict persist_credentials_only pass

if [[ $failures -ne 0 ]]; then
  echo "TOOLS_CHECK_WORKFLOW_DOWNLOAD_PINS: $failures failure(s)" >&2
  exit 1
fi
echo "TOOLS_CHECK_WORKFLOW_DOWNLOAD_PINS: OK"
