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

/** What a processed conventional NFM monitor block is to the engine's call (issue #756), the analog_call_block hook's
 * @c input. BREAK: the block cannot belong to a call at all -- it straddles a boundary or mixes two rates, trunking
 * or digital sync owns the decoder, received-tone detection is not running, a retune is unresolved or a boundary the
 * tap noted has moved since its last read (dsd_analog_rx_boundaries_current()), or the tone policy rejected the
 * reception. RECEPTION: the block is the call's audio, as the per-call recording writes it. HOLDABLE: none of the
 * BREAK reasons, but nothing to record either: the carrier has closed (after its hangover) or the tone policy is still
 * checking it. PENDING: a HOLDABLE block whose carrier is open while the tone policy checks it (issue #667); it is a
 * HOLDABLE block to a call, and the lead-in of a reception that has none yet. 0 and 1 keep the meaning the hook's old
 * accepted flag gave them. */
enum {
    DSD_ANALOG_CALL_BREAK = 0,
    DSD_ANALOG_CALL_RECEPTION = 1,
    DSD_ANALOG_CALL_HOLDABLE = 2,
    DSD_ANALOG_CALL_PENDING = 3,
};

/** One processed conventional NFM monitor block as the analog_call_block hook hands it to the engine's call (issues
 * #756, #668). Every pointer is borrowed for the synchronous call only. */
typedef struct {
    /** The block after the voice band-pass and gain stage and the sink gate: what the per-call recording writes. */
    const float* samples;
    /** The same samples before the sink gate, or NULL: filled only while per-call recording (-P) is on with a pre-roll
        (opts->analog_record_preroll_ms > 0), so the pre-roll and the opening block hold the same audio whatever the
        speaker does. */
    const float* heard;
    size_t count;
    /** The rate the block ran at; 0 for a block that mixed two rates. */
    int sample_rate;
    /** What the block is to the call, DSD_ANALOG_CALL_*. */
    int input;
    /** The first sample from which the tone gate stayed OFF or ALLOWED to the block's end, at tap-read precision (a
        verdict published after a read describes that read's samples, from its start); @c count when the block ends
        under a gate that does not pass. In a block that ends under a passing gate the samples before it are silence
        in @c samples and @c heard, and were neither played nor taken by the gain stage. A block is clean when it is
        0. */
    size_t eligible_from;
    /** The block's first samples, from its start, over which the sink gate was still ramping @c samples in from a ramp
        the block before began (a gain below 1 on samples the gate hears, until it reaches 1 or the gate closes); 0
        when the block began at full gain or from silence. @c heard holds them at full level, so a recording that took
        the block before as heard goes on as heard up to here, and from @c samples after, with no dip at the seam. */
    size_t ramp_end;
    /** 1 when some tap read starting at or after @c eligible_from found the carrier itself, not only its hangover:
        the receiver's own per-read decision (under a per-sample squelch, open at the read's end or opening inside it),
        never the level of the whole block or the playback gate. */
    int qualified_carrier;
    /** The decode-domain wall time at the block's end (dsd_decode_now_realtime_s()), read before the block's raw WAV
        write; 0 while per-call recording (-P) is off. */
    double wall_end_s;
} dsd_analog_call_block;

typedef struct {
    void (*p25_sm_try_tick)(dsd_opts* opts, dsd_state* state);
    void (*p25_sm_release)(dsd_opts* opts, dsd_state* state);
    void (*p25_sm_vc_sync)(dsd_opts* opts, const dsd_state* state);
    void (*p25_sm_vc_no_sync)(dsd_opts* opts, const dsd_state* state);
    void (*eot_cc)(dsd_opts* opts, dsd_state* state);
    void (*no_carrier)(dsd_opts* opts, dsd_state* state);
    int (*scan_visit_should_yield)(const dsd_opts* opts, dsd_state* state);
    int (*signal_search_should_yield)(const dsd_opts* opts, dsd_state* state);
    void (*analog_call_block)(dsd_opts* opts, dsd_state* state, const dsd_analog_call_block* block);
    void (*receive_policy_boundary)(dsd_opts* opts, dsd_state* state, int filters_were_enabled);
    void (*replay_retune)(dsd_opts* opts, dsd_state* state);
    int (*decoder_ticks_due)(const dsd_opts* opts, const dsd_state* state);
    int (*scan_owns_hunt)(const dsd_opts* opts, const dsd_state* state);
    int (*p25_probe_in_flight)(const dsd_opts* opts, const dsd_state* state);
    int (*signal_search_probe)(const dsd_opts* opts, const dsd_state* state);
    int (*tick_guard_enter)(void);
    void (*tick_guard_leave)(void);
    void (*scan_quiet_setting_changed)(dsd_state* state, int turned_on);
    int (*scan_quiet_service)(const dsd_opts* opts, dsd_state* state);
} dsd_frame_sync_hooks;

/** The engine's conventional NFM lifecycle sees every processed monitor block, even with muted speakers, with what the
 * block is to the call (@p block, its input DSD_ANALOG_CALL_*). */
void dsd_frame_sync_hook_analog_call_block(dsd_opts* opts, dsd_state* state, const dsd_analog_call_block* block);
/** The effective receive policy has just changed (issue #637); @p filters_were_enabled says whether any filter was on
 * before. */
void dsd_frame_sync_hook_receive_policy_boundary(dsd_opts* opts, dsd_state* state, int filters_were_enabled);
int dsd_frame_sync_hook_signal_search_should_yield(const dsd_opts* opts, dsd_state* state);

/** Whether decoder-thread trunking timers are due (a startup control-channel hunt armed or running, or DMR recovery
 * under way): a frame-sync hunt read that sits on a silent input leaves it for them after a stream pause, as it does
 * for a queued command (issue #703). 0 when nothing is installed. Call on the decoder thread outside the SM guard. */
int dsd_frame_sync_hook_decoder_ticks_due(const dsd_opts* opts, const dsd_state* state);

/** Whether a scan owns the hunt (issue #834): trunk scan owns the tuner, or -Y owns it. A hunt read that got no input
 * for a bounded time (a silent PCM input for a stream pause, a live RTL read that came back DSD_RTL_READ_IDLE, an idle
 * analog monitor) then ends the pass with DSD_INTERRUPT_SCAN_IDLE, so the engine loop services the scan. Asked only
 * after such a read, never while samples flow. 0 when nothing is installed. Decoder thread. */
int dsd_frame_sync_hook_scan_owns_hunt(const dsd_opts* opts, const dsd_state* state);

/** Whether a P25-driven control-channel probe is in flight: trunking on, the P25 state machine hunting (the startup
 * hunt, or recovery under a P25 owner) and no other protocol owning the control channel. While one is, the frame-sync
 * SPS hunt steps only between the P25 profiles (4800/4 -> 6000/4), the sequence a P25-only rotation runs. 0 when
 * nothing is installed. The hunt asks once per step, on the decoder thread, holding the P25 SM tick guard or not: the
 * engine's answer reads the machine under the guard, taking it when the caller does not hold it. */
int dsd_frame_sync_hook_p25_probe_in_flight(const dsd_opts* opts, const dsd_state* state);

/** Whether a Signal search probe is identifying an Auto detection (issue #804): a visit landed on a range with Auto
 * identification, no protocol evidence yet, the probe time still running. While one is, the unlocked frame-sync SPS
 * hunt tries 4800/4 -> 6000/4 -> 2400/4 -> 9600/2 -> 4800/2, so P25 Phase 2 comes inside a short probe, and spends a
 * shorter dwell on 6000/4. 0 when nothing is installed. The hunt asks only when it steps, resets its acquisition or
 * spends a probe's dwell, never per symbol, and keeps the answer in dsd_state::sps_hunt_search_order. */
int dsd_frame_sync_hook_signal_search_probe(const dsd_opts* opts, const dsd_state* state);

/** Take the P25 SM tick guard on the decoder thread for a P25p1 chain step (frame_sync_p25p1_compare.c), unless the
 * caller already holds it. Returns 1 when it took it, for dsd_frame_sync_hook_tick_guard_leave(); 0 when the caller
 * held it or nothing is installed (headless and test builds, which run no watchdog). Blocks while the watchdog holds
 * it, so it is taken only on the rare chain steps, never per symbol or per frame. */
int dsd_frame_sync_hook_tick_guard_enter(void);
/** Leave the guard dsd_frame_sync_hook_tick_guard_enter() took (@p taken). */
void dsd_frame_sync_hook_tick_guard_leave(int taken);

/**
 * The session-wide scan skip-silent setting (dsd_opts::scan_skip_silent, #834) was written. The app-control handler
 * calls it on the decoder thread after storing the value; the engine drops the quiet checks it already made. With
 * @p turned_on (off before, on now) the visit on air gets no check: the setting applies from the next visit. This
 * table lives for the whole engine run, not a trunk-scan coordinator's, so it serves -Y too. A no-op when nothing is
 * installed (headless runs, tests).
 */
void dsd_scan_quiet_setting_changed(dsd_state* state, int turned_on);

/**
 * The scan quiet check's in-hunt service (issue #834), asked at the frame-sync hunt's 300-symbol point beside the
 * trunk-scan dwell: the engine starts, collects and expires the landed channel's measurement here, on the decoder
 * thread, and answers 1 only when a quiet result it may skip on is ready. The hunt then hands back as at a pass end,
 * keeping its history, and the engine loop's tick decides. With the skip-silent setting off it is one load and 0;
 * 0 when nothing is installed.
 */
int dsd_frame_sync_hook_scan_quiet_service(const dsd_opts* opts, dsd_state* state);

/** Install a copy of @p hooks; NULL clears every slot. */
void dsd_frame_sync_hooks_set(const dsd_frame_sync_hooks* hooks);
/** Copy the installed table into @p out: a host that replaces one slot keeps the others. */
void dsd_frame_sync_hooks_get(dsd_frame_sync_hooks* out);

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
