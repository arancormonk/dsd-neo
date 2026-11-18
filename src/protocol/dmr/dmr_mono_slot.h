// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* The over-the-air slot the single-slot DMR decoder follows on its one lane (issue #637). */
#ifndef DSD_NEO_PROTOCOL_DMR_MONO_SLOT_H
#define DSD_NEO_PROTOCOL_DMR_MONO_SLOT_H

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/position_attribution.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <stdint.h>

#include "dsd-neo/core/state_fwd.h"

/* Note one burst's CACH TACT while the single-slot decoder follows a repeater (BS sync): @p tact_ok whether it passed
   its Hamming check, @p slot its slot bit. Hamming(7,4) lets most corrupted words through, so a slot is proven only
   when two bursts in a row name it. A burst naming the other slot sends the lane back to Checking (the decoder may have
   moved to a call there) until two bursts prove it again; a failed TACT changes nothing; a direct-mode (MS) carrier
   has no CACH and no slot. */
static inline void
dmr_mono_slot_note(dsd_state* state, int tact_ok, unsigned int slot) {
    if (!DSD_SYNC_IS_DMR_BS(state->synctype)) {
        return; /* No CACH: dmr_mono_slot_begin() took the slot from the sync word, or left it Checking. */
    }
    if (state->dmr_mono_rx_slot == DSD_DMR_MONO_RX_NONE) {
        state->dmr_mono_rx_slot = DSD_DMR_MONO_RX_PENDING;
    }
    if (!tact_ok) {
        return;
    }
    const uint8_t named = (uint8_t)(DSD_DMR_MONO_RX_TS1 + (slot & 1U));
    if (state->dmr_mono_rx_slot >= DSD_DMR_MONO_RX_TS1 && named != state->dmr_mono_rx_slot) {
        /* The decoder may have moved to the other timeslot (a new call there, the repeater carrier kept): the lane is
           Checking again until two bursts in a row prove which slot it now carries. */
        state->dmr_mono_rx_slot = DSD_DMR_MONO_RX_PENDING;
    } else if (state->dmr_mono_rx_slot == DSD_DMR_MONO_RX_PENDING && state->dmr_mono_rx_candidate == named) {
        state->dmr_mono_rx_slot = named;
    }
    state->dmr_mono_rx_candidate = named;
}

/* The single-slot decoder starts a superframe on its one lane. On a repeater (BS sync) the slot is Checking until its
   bursts prove it, and a slot already proven on this carrier stays proven. A direct-mode sync word names its slot
   (dsd_state::dmr_dm_sync_slot); any other burst (an MS-sourced one) names none, so its slot stays Checking rather
   than being taken for TS1. */
static inline void
dmr_mono_slot_begin(dsd_state* state) {
    if (DSD_SYNC_IS_DMR_BS(state->synctype)) {
        if (state->dmr_mono_rx_slot == DSD_DMR_MONO_RX_NONE) {
            state->dmr_mono_rx_slot = DSD_DMR_MONO_RX_PENDING;
        }
        return;
    }
    if (state->dmr_dm_sync_slot == 1U || state->dmr_dm_sync_slot == 2U) {
        state->dmr_mono_rx_slot = (uint8_t)(DSD_DMR_MONO_RX_TS1 + state->dmr_dm_sync_slot - 1U);
    } else {
        state->dmr_mono_rx_slot = DSD_DMR_MONO_RX_PENDING;
    }
    state->dmr_mono_rx_candidate = DSD_DMR_MONO_RX_NONE;
}

/* Whether the single-slot output folds a burst's LCs onto lane 0 (dmr_flco()): the single-slot reader's own bursts, and
   a standalone burst while the single-slot output is on. */
static inline int
dmr_single_slot_folds_lcs(const dsd_opts* opts, const dsd_state* state) {
    return state->dmr_ms_mode == 1 || (opts->dmr_mono == 1 && state->dmr_stereo == 0);
}

/* The burst on state->currentslot is being read (issue #800, fail closed): the sender evidence and data header binding
   of another sender kind on its lanes are revoked (dsd_position_dmr_note_lane_burst(); the embedded GPS evidence lane
   is lane 0 where the single-slot output folds the burst's LCs, the data lane the burst's slot). Every DMR burst
   reader calls it once the burst's slot is known. */
static inline void
dmr_note_burst_lane(const dsd_opts* opts, dsd_state* state) {
    const uint8_t lane = (uint8_t)(state->currentslot & 1);
    dsd_position_dmr_note_lane_burst(state, dmr_single_slot_folds_lcs(opts, state) ? 0U : lane, lane);
}

/* Decoding that is not the single-slot decoder's: nothing is proven. */
static inline void
dmr_mono_slot_forget(dsd_state* state) {
    state->dmr_mono_rx_slot = DSD_DMR_MONO_RX_NONE;
    state->dmr_mono_rx_candidate = DSD_DMR_MONO_RX_NONE;
}

#endif
