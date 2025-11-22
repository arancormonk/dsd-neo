// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file
 * @brief P25 state-machine watchdog helpers.
 */
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#pragma once

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

/*
 * Background watchdog for the P25 trunking state machine tick.
 * Ensures hangtime/CC-return logic continues even if the main
 * DSP loop is temporarily stalled by upstream I/O.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* Try to run one tick if not already in progress. Safe to call often. */
void p25_sm_try_tick(dsd_opts* opts, dsd_state* state);

/* Start/stop background 1 Hz watchdog thread (no-op if already started). */
void p25_sm_watchdog_start(dsd_opts* opts, dsd_state* state);
void p25_sm_watchdog_stop(void);

/* Returns 1 while a P25 SM tick is executing on the current thread. */
int p25_sm_in_tick(void);

#ifdef __cplusplus
}
#endif
