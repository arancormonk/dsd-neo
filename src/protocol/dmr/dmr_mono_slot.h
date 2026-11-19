// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* The over-the-air slot the single-slot DMR decoder follows on its one lane (issue #637). */
#ifndef DSD_NEO_PROTOCOL_DMR_MONO_SLOT_H
#define DSD_NEO_PROTOCOL_DMR_MONO_SLOT_H

#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <stdint.h>

#include "dsd-neo/core/opts_fwd.h"
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

/* Decoding that is not the single-slot decoder's: nothing is proven. */
static inline void
dmr_mono_slot_forget(dsd_state* state) {
    state->dmr_mono_rx_slot = DSD_DMR_MONO_RX_NONE;
    state->dmr_mono_rx_candidate = DSD_DMR_MONO_RX_NONE;
}

/* Whether a burst decoded on its own (outside the dual-slot voice loop) is folded onto lane 0, its LC and its PI header
   alike: where the voice its call carries is decoded on that one lane, MS mode and the single-slot decoder (-fr, -nm)
   without trunking. Under trunking the single-slot decoder runs a BS carrier's bursts on both slots
   (dmr_bootstrap_mono()), so such a burst stays on the slot its CACH proved, as on the dual-slot decoder. */
static inline int
dmr_mono_folds_standalone_burst(const dsd_opts* opts, const dsd_state* state) {
    return state->dmr_ms_mode == 1 || (opts->dmr_mono == 1 && opts->trunk_enable == 0 && state->dmr_stereo == 0);
}

/* Whether a standalone burst's over-the-air slot @p burst_slot (0: slot 1, 1: slot 2) is known. The MS path puts every
   burst on index 0 unless a direct-mode sync word named slot 2: there a burst's slot is known only when its sync word
   named it (dsd_state::dmr_dm_sync_slot); one whose slot is not known (an MS-sourced burst, dmr_mono_slot_begin()) is
   taken for the lane's, as before. A repeater's burst names its slot by its checked CACH. */
static inline int
dmr_mono_burst_slot_known(const dsd_state* state, uint8_t burst_slot) {
    return state->dmr_ms_mode != 1 || state->dmr_dm_sync_slot == (uint8_t)(burst_slot + 1U);
}

/* The over-the-air slot (1 or 2) of lane 0's active call, or 0 when lane 0 has no active call or its slot is not
   known. */
static inline uint8_t
dmr_mono_lane_call_slot(const dsd_state* state) {
    dsd_call_snapshot call;
    if (dsd_call_state_get(state, 0U, &call) <= 0 || call.phase != DSD_CALL_PHASE_ACTIVE) {
        return 0U;
    }
    return call.receive_identity.slot <= 2U ? call.receive_identity.slot : 0U;
}

/* Whether lane 0 carries an active call known to be of the other over-the-air slot than @p burst_slot: its receive slot
   is known and is not the burst's, and the burst's slot is known (dmr_mono_burst_slot_known()). A standalone burst the
   single-slot decoder would fold onto lane 0 (dmr_mono_folds_standalone_burst()) says nothing of that call: its
   terminator acts on the other index (1), as the other slot's does on the dual-slot decoder, and does not end it. */
static inline int
dmr_mono_lane_call_is_other_slot(const dsd_state* state, uint8_t burst_slot) {
    if (!dmr_mono_burst_slot_known(state, burst_slot)) {
        return 0;
    }
    const uint8_t lane = dmr_mono_lane_call_slot(state);
    return lane != 0U && lane != (uint8_t)(burst_slot + 1U);
}

#endif
