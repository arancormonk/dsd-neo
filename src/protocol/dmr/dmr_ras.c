// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief RAS (Restricted Access to System) on the DMR carrier: two signature bursts confirm it.
 *
 * A RAS system computes the CRC of its control and data bursts over a secret as well, so they fail the standard check;
 * the burst handler recognises one by its signature (dmr_ras_signature(): reserved bits set, a BPTC codeword within
 * the unique decoding radius, the standard check failed). One such burst can be noise; two on the same carrier confirm
 * that the carrier uses RAS, which the received identity shows (DSD_RXID_DMR_RAS). The evidence belongs to the carrier,
 * so every carrier change clears it with the indicator (dmr_ras_reset()).
 */

#include <dsd-neo/core/received_identity.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/protocol/dmr/dmr.h>

/* Signature bursts that confirm RAS on the carrier. */
#define DMR_RAS_CONFIRM_SIGNATURES 2U

void
dmr_ras_note_signature(dsd_state* state) {
    if (state == NULL) {
        return;
    }
    /* A burst of no carrier (read across a carrier boundary, or cut by an input interruption) is no evidence. */
    if (state->input_interrupted != 0
        || (state->frame_dispatch_active && dsd_state_frame_read_lost(state, state->frame_dispatch_carrier_seq))) {
        return;
    }
    if (state->dmr_ras_signatures < DMR_RAS_CONFIRM_SIGNATURES) {
        state->dmr_ras_signatures++;
    }
    if (state->dmr_ras_signatures >= DMR_RAS_CONFIRM_SIGNATURES) {
        /* Every signature after the confirmation notes it again: a record another family's frame replaced shows it
           once this carrier's next one arrives. */
        dsd_rxid_note_dmr_ras(state);
    }
}

int
dmr_ras_confirmed(const dsd_state* state) {
    return state != NULL && state->dmr_ras_signatures >= DMR_RAS_CONFIRM_SIGNATURES;
}

void
dmr_ras_reset(dsd_state* state) {
    if (state == NULL) {
        return;
    }
    state->dmr_ras_signatures = 0U;
    dsd_rxid_forget_dmr_ras(state);
}

void
dmr_ras_note_color_code(dsd_state* state, unsigned int color_code) {
    /* 16 is "not decoded" (a carrier boundary leaves it): the first code a carrier sends changes nothing. */
    if (state != NULL && state->dmr_color_code <= 15U && state->dmr_color_code != color_code) {
        dmr_ras_reset(state);
    }
}
