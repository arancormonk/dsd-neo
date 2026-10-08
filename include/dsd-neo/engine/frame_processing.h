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
 * `p2_cc` itself. The no-carrier reset runs it outside trunk scan, whose per-target snapshots keep these; app-control
 * runs it on every accepted retune the user asks for to another carrier, which can sync before any no-carrier pass. */
void dsd_engine_forget_carrier_codes(dsd_state* state);
/** With trunking off, forget what the carrier on air left beyond its codes (issue #575): what the trunking-off
 * no-carrier pass forgets of it (the P25 voice frequencies a grant update wrote, the DMR rest channel and branding, the
 * NXDN site and channel plan) and the DMR grants' voice frequencies (`trunk_vc_freq[]`). A retune to another carrier
 * runs it after the outgoing calls are committed, since the next carrier can sync a call before that pass. With trunking
 * on these belong to the system the receiver follows, and nothing changes. */
void dsd_engine_forget_untrunked_carrier_state(const dsd_opts* opts, dsd_state* state);
/** An I/Q replay's retune the capture recorded, which the read path adopted (dsd_frame_sync_note_replay_center()),
 * before it returns the first sample the new carrier carries (issue #575). With trunking off it is another
 * conventional carrier, the boundary an accepted live retune is: the outgoing calls end and commit as a hop, while the
 * live codes are still the carrier's they were heard on, and then the codes go
 * (dsd_engine_forget_carrier_codes()) with the untrunked state (dsd_engine_forget_untrunked_carrier_state()). With
 * trunking on it is the system following itself, and under trunk scan a target switch whose snapshots carry the codes:
 * nothing changes. The frame-sync hook runs it on the decoder thread from inside a sample read, where no call-state
 * lock is held: the read paths run in protocol and frame-sync code, never under the call-state or event-layer lock. */
void dsd_engine_leave_replay_carrier(dsd_opts* opts, dsd_state* state);

#ifdef __cplusplus
}
#endif
#endif /* DSD_NEO_INCLUDE_DSD_NEO_ENGINE_FRAME_PROCESSING_H_H */
