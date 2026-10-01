// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_ENGINE_ENGINE_H_H
#define DSD_NEO_INCLUDE_DSD_NEO_ENGINE_ENGINE_H_H

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*dsd_engine_lifecycle_start_fn)(dsd_opts* opts, dsd_state* state, void* context);
typedef void (*dsd_engine_lifecycle_stop_fn)(dsd_opts* opts, dsd_state* state, void* context);

typedef struct {
    dsd_engine_lifecycle_start_fn start;
    dsd_engine_lifecycle_stop_fn stop;
    void* context;
} dsd_engine_lifecycle_hooks;

/**
 * Run the engine with optional live-run lifecycle hooks.
 *
 * The start hook is called after engine and mode-specific setup succeeds, just
 * before live processing begins. If start succeeds, the stop hook is called
 * after live processing returns and before dsd_engine_cleanup() tears down
 * engine-owned state.
 *
 * Shutdown flag: this clears the global exit flag on entry so a second run in one
 * process is not killed by the previous run's shutdown. A host that can receive a
 * stop request before this call returns to the decode loop -- anything driving the
 * engine from its own thread -- must therefore latch that request and re-assert it
 * from the start hook, which runs after the reset. Relying on the flag alone loses
 * the stop, and the run then has nothing left watching for one.
 */
int dsd_engine_run_with_lifecycle(dsd_opts* opts, dsd_state* state, const dsd_engine_lifecycle_hooks* hooks);
void dsd_engine_cleanup(dsd_opts* opts, dsd_state* state);

/**
 * Run the decode clock on the capture clock of the I/Q replay the input names (`--iq-replay`; issue #572).
 *
 * Reads the sidecar's `capture_started_utc` (dsd_iq_replay_parse_utc_seconds()) and selects the REPLAY source on it
 * (dsd_decode_clock_use_replay(), which floors it at 2000-01-01Z); a field in another form starts decode time at the
 * floor, with a warning. Then rebases the init stamps onto the capture clock (dsd_state_rebase_decode_timestamps()).
 * dsd_engine_run_with_lifecycle() calls it first, before common setup writes any record and before any other thread
 * reads the clock. Does nothing for an input that is no replay.
 *
 * @return 0 on success or for no replay; -1 when the sidecar does not parse (logged), which fails the run.
 */
int dsd_engine_decode_clock_enter_replay(dsd_opts* opts, dsd_state* state);

/**
 * Put the decode clock back on the system clock, and rebase the init stamps onto it, once the replay no longer feeds
 * the decoder: at the end of the run, and when app-control stops the stream to restart it or switches the input
 * away from it. A replay restarted that way runs on the system clock. Does nothing unless the REPLAY source is
 * selected. Decoder thread, with the replay stream stopped or no longer read.
 */
void dsd_engine_decode_clock_leave_replay(dsd_opts* opts, dsd_state* state);

#ifdef __cplusplus
}
#endif
#endif /* DSD_NEO_INCLUDE_DSD_NEO_ENGINE_ENGINE_H_H */
