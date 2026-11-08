// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file
 * @brief P25 traffic channel activity for a Signal search.
 */
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_P25_P25_TRAFFIC_ACTIVITY_H
#define DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_P25_P25_TRAFFIC_ACTIVITY_H

#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/runtime/signal_search_hooks.h>
#include <stddef.h>
#include "dsd-neo/core/state_fwd.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Report a P25 Phase 2 MAC PDU whose real checksum passed as Signal search evidence (issue #804).
 *
 * Any such MAC is CRC-checked P25 traffic on the carrier, MAC_HANGTIME, MAC_IDLE and MAC_END_PTT as well as PTT and
 * ACTIVE, so a voice channel heard between or after calls confirms its detection. Call it only for a verdict that
 * passed the MAC CRC itself, never for a word -F let through. It is traffic channel evidence, not control channel
 * activity (p25_sm_note_cc_activity()): no control channel time stamp moves. The engine judges it by the receive
 * policy for @p slot and drops it outside a receiving visit or during an input interruption.
 *
 * @param state Decoder state.
 * @param slot Voice slot the MAC belongs to: a FACCH's own slot, a SACCH's opposite one (p25_sacch_to_voice_slot()).
 */
static inline void
p25_sm_note_traffic_activity(const dsd_state* state, int slot) {
    if (!state) {
        return;
    }
    int sync = DSD_SYNC_P25P2_POS;
    if (DSD_SYNC_IS_P25P2(state->synctype)) {
        sync = state->synctype;
    } else if (DSD_SYNC_IS_P25P2(state->lastsynctype)) {
        sync = state->lastsynctype;
    }
    dsd_signal_search_note_evidence_at(state, "P25", sync, slot, 0U, 0U, NULL);
}

#ifdef __cplusplus
}
#endif

#endif // DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_P25_P25_TRAFFIC_ACTIVITY_H
