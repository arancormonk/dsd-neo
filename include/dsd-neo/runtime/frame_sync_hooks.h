// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Runtime hook table for frame-sync side effects.
 *
 * DSP frame-sync code may need to trigger protocol-specific actions without
 * depending directly on protocol headers. The engine installs the real hook
 * functions at startup; the runtime provides safe no-op wrappers until then.
 * Long protocol loops also use this boundary to check scan deadlines without
 * depending on engine headers or retuning before decoder cleanup finishes.
 */
#ifndef DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_FRAME_SYNC_HOOKS_H_
#define DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_FRAME_SYNC_HOOKS_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    void (*p25_sm_try_tick)(dsd_opts* opts, dsd_state* state);
    void (*p25_sm_release)(dsd_opts* opts, dsd_state* state);
    void (*p25_sm_vc_sync)(dsd_opts* opts, const dsd_state* state);
    void (*p25_sm_vc_no_sync)(dsd_opts* opts, const dsd_state* state);
    void (*eot_cc)(dsd_opts* opts, dsd_state* state);
    void (*no_carrier)(dsd_opts* opts, dsd_state* state);
    int (*scan_visit_should_yield)(const dsd_opts* opts, dsd_state* state);
    int (*signal_search_should_yield)(const dsd_opts* opts, dsd_state* state);
    void (*analog_call_block)(dsd_opts* opts, dsd_state* state, const float* samples, size_t count, int rate,
                              int accepted);
    void (*receive_policy_boundary)(dsd_opts* opts, dsd_state* state, int filters_were_enabled);
    void (*replay_retune)(dsd_opts* opts, dsd_state* state);
    int (*decoder_ticks_due)(const dsd_opts* opts, const dsd_state* state);
    int (*p25_probe_in_flight)(const dsd_opts* opts, const dsd_state* state);
} dsd_frame_sync_hooks;

/** The engine's conventional NFM lifecycle sees accepted processed blocks even with muted speakers. */
void dsd_frame_sync_hook_analog_call_block(dsd_opts* opts, dsd_state* state, const float* samples, size_t count,
                                           int rate, int accepted);
/** The effective receive policy has just changed (issue #637); @p filters_were_enabled says whether any filter was on
 * before. */
void dsd_frame_sync_hook_receive_policy_boundary(dsd_opts* opts, dsd_state* state, int filters_were_enabled);
int dsd_frame_sync_hook_signal_search_should_yield(const dsd_opts* opts, dsd_state* state);

/** Whether decoder-thread trunking timers are due (a startup control-channel hunt armed or running, or DMR recovery
 * under way): a frame-sync hunt read that sits on a silent input leaves it for them after a stream pause, as it does
 * for a queued command (issue #703). 0 when nothing is installed. Call on the decoder thread outside the SM guard. */
int dsd_frame_sync_hook_decoder_ticks_due(const dsd_opts* opts, const dsd_state* state);

/** Whether a P25-driven control-channel probe is in flight: trunking on, the P25 state machine hunting (the startup
 * hunt, or recovery under a P25 owner) and no other protocol owning the control channel. While one is, the frame-sync
 * SPS hunt steps only between the P25 profiles (4800/4 -> 6000/4), the sequence a P25-only rotation runs. 0 when
 * nothing is installed. The hunt asks once per step, on the decoder thread, holding the P25 SM tick guard or not: the
 * engine's answer reads the machine under the guard, taking it when the caller does not hold it. */
int dsd_frame_sync_hook_p25_probe_in_flight(const dsd_opts* opts, const dsd_state* state);

/** Install a copy of @p hooks; NULL clears every slot. */
void dsd_frame_sync_hooks_set(const dsd_frame_sync_hooks* hooks);

void dsd_frame_sync_hook_p25_sm_try_tick(dsd_opts* opts, dsd_state* state);
void dsd_frame_sync_hook_p25_sm_release(dsd_opts* opts, dsd_state* state);
void dsd_frame_sync_hook_p25_sm_vc_sync(dsd_opts* opts, const dsd_state* state);
void dsd_frame_sync_hook_p25_sm_vc_no_sync(dsd_opts* opts, const dsd_state* state);
void dsd_frame_sync_hook_eot_cc(dsd_opts* opts, dsd_state* state);
void dsd_frame_sync_hook_no_carrier(dsd_opts* opts, dsd_state* state);

/** Maintain scan visit clocks at a complete protocol frame boundary. Returns nonzero
 * when a long decoder loop must unwind so the engine can advance the scan. Does not
 * retune or release call state. Call on the decoder thread under its existing SM guard. */
int dsd_frame_sync_hook_scan_visit_should_yield(const dsd_opts* opts, dsd_state* state);

/** Note the I/Q replay centre the sample just read was captured on (dsd_opts_note_iq_replay_center()), and, when that
 * adopts a new centre over another one -- a retune the capture recorded -- run the replay-retune hook before the sample
 * is returned to the protocol, so nothing the new carrier carries is decoded on the old one's state (issue #575). Read
 * paths call it on the decoder thread after each replay sample; it costs one compare unless the centre moved. */
void dsd_frame_sync_note_replay_center(dsd_opts* opts, dsd_state* state, uint32_t center_hz);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_FRAME_SYNC_HOOKS_H_ */
