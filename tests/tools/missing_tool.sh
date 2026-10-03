# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
# shellcheck shell=bash
#
# Sourced by the tools/ tests whose subject drives an external program. Without
# that program a test cannot run: on a developer machine it skips, and passes,
# but where DSD_NEO_REQUIRE_TEST_TOOLS=1, which every CI job that runs the suite
# sets, it fails, so a missing tool can never pass a required check unseen.
missing_tool() {
  if [[ "${DSD_NEO_REQUIRE_TEST_TOOLS:-}" == 1 ]]; then
    echo "FAIL: $1 (DSD_NEO_REQUIRE_TEST_TOOLS=1)" >&2
    exit 1
  fi
  echo "SKIP: $1"
  exit 0
}
