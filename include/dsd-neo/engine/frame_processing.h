// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Engine-owned frame processing entrypoints.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_ENGINE_FRAME_PROCESSING_H_H
#define DSD_NEO_INCLUDE_DSD_NEO_ENGINE_FRAME_PROCESSING_H_H

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#ifdef __cplusplus
extern "C" {
#endif

void processFrame(dsd_opts* opts, dsd_state* state);
void noCarrier(dsd_opts* opts, dsd_state* state);
/** Caller holds the P25 SM tick guard; ticks the DMR owner without acquiring it.
 * The P25 CC-return helper still defers under the held guard. */
void dsd_engine_no_carrier_locked(dsd_opts* opts, dsd_state* state);
/** Reset decoder buffers and protocol state after calls have been finalized.
 * Performs no scanner step or return-to-control-channel tuning. */
void dsd_engine_reset_no_carrier_state(dsd_opts* opts, dsd_state* state);
/** Forget the codes the carrier on air decoded (issue #575): the DMR colour code with the confidence lock it pairs with,
 * the NXDN RAN with its stand-in mark, the dPMR colour code, the Phase 1 NAC and the proof of the Phase 2 seed, never
 * `p2_cc` itself. The no-carrier reset runs it outside trunk scan, whose per-target snapshots keep these, and the
 * carrier boundary runs it (dsd_engine_carrier_boundary()), since the next carrier can sync before any no-carrier pass. */
void dsd_engine_forget_carrier_codes(dsd_state* state);
/** With trunking off, forget what the carrier on air left beyond its codes (issue #575): what the trunking-off
 * no-carrier pass forgets of it (the P25 voice frequencies a grant update wrote, the DMR rest channel and branding, the
 * NXDN site and channel plan) and the DMR grants' voice frequencies (`trunk_vc_freq[]`). The carrier boundary runs it
 * after the outgoing calls are committed, since the next carrier can sync a call before that pass. With trunking on
 * these belong to the system the receiver follows, and nothing changes. */
void dsd_engine_forget_untrunked_carrier_state(const dsd_opts* opts, dsd_state* state);

/** Forget what the decoders gathered on the carrier on air (issue #575): every confirmation gate's evidence, which
 * vouched for that carrier's transmissions, and every multi-frame assembly that could publish an identity or a code
 * (the NXDN SACCH superframe and alias, the DMR short LC, embedded LC, late-entry MI, talker alias and data blocks, the
 * P25 MAC fragments, talker aliases, Phase 2 ESS_B, partial voice superframe and staged rekey, and the ended calls' P25
 * crypto, the dPMR superframe part, the M17 LSF chunks, packet and signature, the YSF text): the next carrier's pieces
 * could otherwise complete or reuse them. The carrier boundary runs it, and so does a trunk-scan target switch, which
 * moves the tuner without the boundary: the target snapshots carry the codes, never these. */
void dsd_engine_forget_carrier_decoding(dsd_opts* opts, dsd_state* state);

/** What changed the carrier a carrier boundary leaves (issue #575). */
typedef enum {
    /* An accepted tune the user asked for to another carrier: a tap, a frequency entry, a channel cycle over a list no
       followed system owns, an import's tune, a config's new frequency. */
    DSD_CARRIER_BOUNDARY_TUNE = 0,
    /* A radio stream started on another source than the one before it (dsd_engine_note_stream_source()). */
    DSD_CARRIER_BOUNDARY_SOURCE,
    /* The input in force was replaced (state->input_boundary). */
    DSD_CARRIER_BOUNDARY_INPUT_SWITCH,
    /* A conventional scan stepped to its next row: a -Y row commit or the untyped step. */
    DSD_CARRIER_BOUNDARY_SCAN_STEP,
    /* An I/Q replay read adopted a retune its capture recorded, with trunking off. */
    DSD_CARRIER_BOUNDARY_REPLAY_RETUNE,
} dsd_carrier_boundary_kind;

/** The one carrier boundary (issue #575): every place the receiver leaves a carrier for another runs it, in one order.
 *  1. The voice channel a trunking state machine followed, if one is held, is released while its calls are still
 *     active, so the release's audio flush plays under the call's talkgroup: the P25 or DMR state machine comes to rest
 *     on its control channel without tuning (the caller's change is the move) and the shared release drops
 *     trunk_is_tuned and the voice channel frequencies (dsd_engine_release_tuned_call_state()).
 *  2. The calls heard on the carrier left end, as a hop, and commit, while the live codes are still that carrier's.
 *  3. The carrier's codes go (dsd_engine_forget_carrier_codes(): p2_cc_verified and the NAC with them, never p2_cc),
 *     with the evidence that vouched for its transmissions: every confirmation gate restarts, as the no-carrier pass
 *     restarts them, so the next carrier proves itself again; and with every multi-frame assembly that can publish an
 *     identity or a code (an NXDN SACCH superframe, a DMR short LC or talker alias, an M17 LSF...), which the next
 *     carrier's next piece could otherwise complete.
 *  4. With trunking off, what the trunking-off no-carrier pass forgets of it goes too
 *     (dsd_engine_forget_untrunked_carrier_state()).
 *  5. state->carrier_seq moves, so a decoder that buffered the left carrier's bursts drops them.
 * @p guard_held is 1 exactly when the calling thread holds the P25 SM tick guard and 0 when it does not; the guard is not
 * re-entrant, so the value must be exact. A tune, a source change or an input switch can run beside the watchdog's
 * ticks, which write the state machines, the followed assignment and the call state under that guard: the boundary then
 * holds it from its first inspection through its last step, taking it when the caller does not hold it. A scan step and
 * a replay retune run where no P25 or DMR recovery tick runs (conventional scanning, trunking off), inspect no state
 * machine and take no guard: a replay retune runs inside a sample read, whose caller may hold it (processFrame()).
 * Callers: the tune services (svc_rtl_set_freq() 0, svc_rtl_set_freq_locked() 1), the stream start
 * (dsd_engine_note_stream_source(): svc_rtl_start_locked() 1, the engine's own start 0), the channel cycles (1 when
 * run_manual_retune_guarded() took the guard), a config apply and a RadioReference import (1, guarded commands), the
 * engine's input switch (0), the scan steps and the replay hook. Exempt, and never routed here: retunes within a system
 * under trunking, trunk-scan target switches, and retunes an external controller makes over the RTL UDP port. Decoder
 * thread. */
void dsd_engine_carrier_boundary(dsd_opts* opts, dsd_state* state, dsd_carrier_boundary_kind kind, int guard_held);

/** Note the source a radio stream just started on (issue #575): its kind of device and which one, never its settings
 *  or tuning (an RTL-SDR index, an rtl_tcp host and port, an Airspy serial, a SoapySDR device, a replay capture). A start
 *  on another source than the stream before it, or of a replay, which plays its capture again from the start, is a
 *  carrier boundary (dsd_engine_carrier_boundary(), DSD_CARRIER_BOUNDARY_SOURCE); the first start of a session, and a
 *  same-source reopen for a gain, PPM, bandwidth or squelch change, are not. Every stream start runs it, so a new
 *  command that changes the source and restarts the stream needs nothing of its own. @p guard_held as for the boundary. */
void dsd_engine_note_stream_source(dsd_opts* opts, dsd_state* state, int guard_held);

/** An I/Q replay's retune the capture recorded, which the read path adopted (dsd_frame_sync_note_replay_center()),
 * before it returns the first sample the new carrier carries (issue #575). With trunking off it is another
 * conventional carrier, the boundary an accepted live retune is (dsd_engine_carrier_boundary(),
 * DSD_CARRIER_BOUNDARY_REPLAY_RETUNE): the outgoing calls end and commit as a hop, while the live codes are still the
 * carrier's they were heard on, then the codes and the untrunked state go, and the carrier count moves. With
 * trunking on it is the system following itself, and under trunk scan a target switch whose snapshots carry the codes:
 * nothing changes. The frame-sync hook runs it on the decoder thread from inside a sample read, where no call-state
 * lock is held: the read paths run in protocol and frame-sync code, never under the call-state or event-layer lock. */
void dsd_engine_leave_replay_carrier(dsd_opts* opts, dsd_state* state);

#ifdef __cplusplus
}
#endif
#endif /* DSD_NEO_INCLUDE_DSD_NEO_ENGINE_FRAME_PROCESSING_H_H */
