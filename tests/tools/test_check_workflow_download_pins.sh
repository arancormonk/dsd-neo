#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
#
# tools/check_workflow_download_pins.sh keeps the inline archlinux:base-devel
# digests in the workflows equal to ARCHLINUX_BASE_DEVEL_IMAGE. The jobs that run
# in the image name it inline (a container image cannot read the env file), and
# the copies drifted from the pin once. The script runs inside a throwaway
# repository holding the real pins file and one workflow, so each case edits only
# the workflow.
set -euo pipefail

ROOT_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)

if ! command -v git > /dev/null 2>&1 || ! command -v rg > /dev/null 2>&1; then
  echo "SKIP: git or ripgrep not available"
  exit 0
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

if [[ $failures -ne 0 ]]; then
  echo "TOOLS_CHECK_WORKFLOW_DOWNLOAD_PINS: $failures failure(s)" >&2
  exit 1
fi
echo "TOOLS_CHECK_WORKFLOW_DOWNLOAD_PINS: OK"
