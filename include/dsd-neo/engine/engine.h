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
 * Close the output files of a session that will not run: the symbol capture (-c), static WAV (-w) and raw WAV (-6)
 * dsd_runtime_bootstrap() opens while it parses the options, which otherwise only the run's dsd_engine_cleanup()
 * closes, and any log or MBE file a run left open. For a host that configured a session and then gives it up, fails
 * its configure, or frees the options without running it (#782). -P opens its per-call WAVs only at the run's start
 * (issue #708); one left open is closed too, and removed when it holds no audio, as at a run's end. Each stream it
 * closes is left NULL, so a second call does nothing.
 */
void dsd_engine_release_outputs(dsd_opts* opts, dsd_state* state);
/** End calls under their old receive policy before a live edit (decoder thread). */
/** The effective receive policy has just changed (issue #637): a call it no longer admits ends; one it still admits is
 * relabelled and carries on. Codes a voice channel inherited from its control channel are forgotten when filters come
 * on. */
void dsd_engine_receive_policy_boundary(dsd_opts* opts, dsd_state* state, int filters_were_enabled);
/** Whether a followed trunk voice channel is held only by traffic the receive policy rejects (issue #637): a filter is
 * on and no admitted traffic has refreshed the channel's activity within the trunk hangtime. Continuous sync never
 * reaches noCarrier(), whose control-channel return then lets the channel go, so the frame loops yield on this. */
int dsd_engine_trunk_receive_release_due(const dsd_opts* opts, const dsd_state* state);

/**
 * Run the decode clock on the capture clock of the I/Q replay the input names (`--iq-replay`; issue #572).
 *
 * Reads the sidecar's `capture_started_utc` (dsd_iq_replay_parse_utc_seconds()) and selects the REPLAY source on it
 * (dsd_decode_clock_use_replay(), which floors it at 2000-01-01Z); a field in another form starts decode time at the
 * floor, with a warning. Then rebases the init stamps onto the capture clock (dsd_state_rebase_decode_timestamps()).
 * dsd_engine_run_with_lifecycle() calls it first, before common setup writes any record and before the stream and the
 * P25 watchdog read the clock. Does nothing for an input that is no replay.
 *
 * Selects REPLAY only on a state fresh from initState() (dsd_state::engine_fresh, which dsd_engine_run_with_lifecycle()
 * clears once it has called this). Only the init seeds are rebased, so a stamp an earlier run left in a reused state
 * (the Android service reuses its state when a start races the previous run's stopSelfLatest(), DecoderService.kt)
 * would sit ahead of the capture's wall time. On such a state the clock stays on SYSTEM, with one warning that the
 * run's replay timing is not anchored to the capture. The system clock only moves forward, so every stamp the earlier
 * run took on it stays behind now. The exception is an earlier replay of a capture stamped ahead of this device's
 * clock (recorded elsewhere, or on a clock that is behind): the leave rebases only the init seeds, so that run's
 * protocol stamps keep the capture's later time and can still sit ahead of now.
 *
 * @return 0 on success or for no replay; -1 when the sidecar does not parse (logged), which fails the run.
 */
int dsd_engine_decode_clock_enter_replay(dsd_opts* opts, dsd_state* state);

/**
 * Put the decode clock back on the system clock, and rebase the init stamps onto it, once the replay no longer feeds
 * the decoder: at the end of the run, and when app-control stops the stream to restart it or switches the input
 * away from it. First ends each slot's call (DSD_CALL_END_EXPLICIT) and commits it while decode time is still the
 * capture's, so its history row and its per-call recording and rdio-scanner sidecar are closed on the clock they were
 * stamped on. Decode-mono time goes on from the capture time the replay reached (dsd_decode_clock_use_system()), so
 * every stamp the replay took keeps ageing; wall time jumps forward to real time. A replay restarted that way runs on
 * the system clock. Does nothing unless the REPLAY source is selected. Decoder thread, with the replay stream stopped
 * or no longer read.
 */
void dsd_engine_decode_clock_leave_replay(dsd_opts* opts, dsd_state* state);

#ifdef __cplusplus
}
#endif
#endif /* DSD_NEO_INCLUDE_DSD_NEO_ENGINE_ENGINE_H_H */
