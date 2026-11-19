// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Which over-the-air slot the single-slot DMR decoder's one lane is judged for.
 *
 * The single-slot decoder (-fr/-nm without trunking, and MS mode) decodes both over-the-air slots' voice on one lane
 * (index 0), which carries one call: the slot of its last checked LC (dsd_state::dmr_mono_lane_slot). The vocoder's
 * judge holds the other slot's voice there.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_DMR_DMR_MONO_LANE_H
#define DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_DMR_DMR_MONO_LANE_H

#include <dsd-neo/core/state.h>
#include <stdint.h>
#include "dsd-neo/core/state_fwd.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The over-the-air slot (1 or 2) of the voice the single-slot decoder's lane decodes now: the slot its bursts proved,
   or, while the lane is Checking, the slot the last checked CACH TACT named. 0 when not known (a direct-mode burst
   whose sync word names no slot) and on every other decoder. */
static inline uint8_t
dmr_mono_lane_voice_slot(const dsd_state* state) {
    uint8_t rx = state->dmr_mono_rx_slot;
    if (rx == DSD_DMR_MONO_RX_PENDING) {
        rx = state->dmr_mono_rx_candidate;
    }
    if (rx != DSD_DMR_MONO_RX_TS1 && rx != DSD_DMR_MONO_RX_TS2) {
        return 0U;
    }
    return (uint8_t)(rx - DSD_DMR_MONO_RX_TS1 + 1U);
}

/* Whether the voice the lane decodes now is the other over-the-air slot's than the lane's call
   (dsd_state::dmr_mono_lane_slot): held, as on a held slot, until that slot's own checked LC makes it the lane's call. */
static inline int
dmr_mono_lane_voice_is_other_call(const dsd_state* state) {
    const uint8_t voice = dmr_mono_lane_voice_slot(state);
    return state->dmr_mono_lane_slot != 0U && voice != 0U && voice != state->dmr_mono_lane_slot;
}

#ifdef __cplusplus
}
#endif

#endif
