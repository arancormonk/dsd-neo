// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_PROTOCOL_DMR_DMR_CONFIDENCE_H
#define DSD_NEO_PROTOCOL_DMR_DMR_CONFIDENCE_H

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DMR_CONFIDENCE_REJECT = -1,
    DMR_CONFIDENCE_PENDING = 0,
    DMR_CONFIDENCE_LOCKED = 1,
} dmr_confidence_result;

/** How long a scan quiet check waits for the burst that completes a colour-code lock it has a candidate for (issue
 * #834): a mobile's next burst of the slot, 60 ms on, with room for one missed. */
#define DMR_CONFIDENCE_PARTIAL_HOLD_MS 120U

void dmr_confidence_reset(dsd_state* state);
void dmr_confidence_reset_slot(dsd_state* state, unsigned int slot);
void dmr_confidence_note_voice_sync(dsd_state* state, unsigned int slot);
/* The two notes below tell a scan quiet check, through @p opts (dsd_frame_sync_hook_content_evidence(), issue #834),
   what the burst did to the gate: PROVEN where the gate's evidence is noted (a lock, a burst matching the lock, a voice
   slot opening), PARTIAL (DMR_CONFIDENCE_PARTIAL_HOLD_MS) for a burst that left the gate PENDING on an unlocked
   candidate. @p opts may be NULL (no quiet check). */
dmr_confidence_result dmr_confidence_note_voice_burst(const dsd_opts* opts, dsd_state* state, unsigned int slot,
                                                      unsigned int color_code);
dmr_confidence_result dmr_confidence_note_data_burst(const dsd_opts* opts, dsd_state* state, unsigned int color_code);
/* dmr_confidence_note_data_burst() for a control burst (a CSBK, an MBC header or continuation): the colour code counts
   toward the lock as any data burst's, but none of the gate's evidence is noted (no PROVEN, no
   dsd_state::dmr_confidence_lock_m stamp, no search evidence) until the PDU it belongs to passes the control gate. */
dmr_confidence_result dmr_confidence_note_control_burst(const dsd_opts* opts, dsd_state* state,
                                                        unsigned int color_code);
/* A control PDU from bursts that prove content passed the control gate (dmr_cspdu_ex(), which notes its PROVEN
   activity): with the gate locked, the stamp and the search evidence its bursts deferred are noted now. */
void dmr_confidence_note_control_accepted(dsd_state* state);
int dmr_confidence_voice_slot_open(const dsd_state* state, unsigned int slot);
int dmr_confidence_any_voice_open(const dsd_state* state);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_PROTOCOL_DMR_DMR_CONFIDENCE_H */
