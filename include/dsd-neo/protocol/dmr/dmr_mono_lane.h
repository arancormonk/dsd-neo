// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Which over-the-air slot the single-slot DMR decoder's one lane is judged for.
 *
 * The single-slot decoder (-fr/-nm without trunking, and MS mode) decodes both over-the-air slots' voice on one lane
 * (index 0), whose voice hold and encryption classification belong to the slot whose own word set them
 * (dsd_state::dmr_mono_lane_owner). The vocoder's judge and the DMR decoder read them here.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_DMR_DMR_MONO_LANE_H
#define DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_DMR_DMR_MONO_LANE_H

#include <dsd-neo/core/state.h>
#include <stdint.h>

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

/* Whether the lane's hold and classification belong to another over-the-air slot than @p slot (1 or 2; 0 when not
   known): its voice, or a word of its call, says nothing of them until its own checked LC or accepted PI header. */
static inline int
dmr_mono_lane_owned_by_other_slot(const dsd_state* state, uint8_t slot) {
    return state->dmr_mono_lane_owner != 0U && slot != 0U && slot != state->dmr_mono_lane_owner;
}

#ifdef __cplusplus
}
#endif

#endif
