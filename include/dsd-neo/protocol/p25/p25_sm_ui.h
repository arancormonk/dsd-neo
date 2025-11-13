// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Small helpers for tagging/logging P25 state-machine status in the shared
 * dsd_state structure and stderr (when verbose).
 */

#pragma once

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#ifdef __cplusplus
extern "C" {
#endif

// Record a short status tag and optionally log a concise line to stderr when
// verbose > 1. Tags are also pushed into a small ring buffer for UI display.
void p25_sm_log_status(dsd_opts* opts, dsd_state* state, const char* tag);

#ifdef __cplusplus
}
#endif
