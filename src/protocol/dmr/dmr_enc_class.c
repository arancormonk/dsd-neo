// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Per-slot hysteresis for the DMR service-option privacy bit.
 *
 * The privacy bit reaches the decoder over channels of very different
 * reliability: a voice LC header behind a 16-bit masked CRC, an embedded LC
 * behind a 5-bit checksum (a miscorrected BPTC payload passes it roughly one
 * time in 32), and on RAS systems no verifiable CRC at all. Acting on each
 * observation directly lets a single corrupt LC mute a clear call, flap the
 * published classification at the embedded-LC cadence, and -- under
 * --enc-lockout -- permanently block the talkgroup and force the channel
 * released. This module makes the applied classification sticky: strong
 * evidence applies at once, weak evidence applies tentatively and must repeat
 * before it can flip an established classification, and the lockout only acts
 * on an established encrypted classification.
 */

#include <dsd-neo/core/state.h>
#include <dsd-neo/protocol/dmr/dmr.h>
#include <stdint.h>

#include "dsd-neo/core/state_fwd.h"

static uint8_t
dmr_enc_class_slot_index(uint8_t slot) {
    return (uint8_t)(slot != 0U ? 1U : 0U);
}

/* Feed one service-option observation for the slot and return the SO byte
 * with the privacy bit replaced by the retained classification. `strong`
 * marks evidence trustworthy on its own (a CRC-verified voice LC header or a
 * checksum-verified PI); weak evidence establishes only by repetition. */
unsigned int
dmr_enc_class_observe(dsd_state* state, uint8_t slot, unsigned int so, int strong) {
    const uint8_t idx = dmr_enc_class_slot_index(slot);
    const uint8_t obs = (so & 0x40U) != 0U ? DMR_ENC_CLASS_ENC : DMR_ENC_CLASS_CLEAR;

    if (strong) {
        state->dmr_enc_class[idx] = obs;
        state->dmr_enc_class_est[idx] = 1U;
        state->dmr_enc_class_pending[idx] = DMR_ENC_CLASS_NONE;
    } else if (state->dmr_enc_class[idx] == DMR_ENC_CLASS_NONE) {
        // First observation of the transmission: apply tentatively so late
        // entry still classifies immediately, but leave it uncorroborated so
        // a lone corrupt LC cannot arm the lockout.
        state->dmr_enc_class[idx] = obs;
        state->dmr_enc_class_est[idx] = 0U;
        state->dmr_enc_class_pending[idx] = DMR_ENC_CLASS_NONE;
    } else if (obs == state->dmr_enc_class[idx]) {
        // A matching repeat corroborates; embedded LC repeats every 360 ms,
        // so a real classification establishes within one repeat.
        state->dmr_enc_class_est[idx] = 1U;
        state->dmr_enc_class_pending[idx] = DMR_ENC_CLASS_NONE;
    } else if (state->dmr_enc_class_est[idx] == 0U || state->dmr_enc_class_pending[idx] == obs) {
        // Contradicting a value that never corroborated, or the second
        // consecutive contradiction of an established classification: the
        // newer observation wins (a tentative replacement stays tentative; a
        // corroborated flip keeps the slot established).
        state->dmr_enc_class[idx] = obs;
        state->dmr_enc_class_pending[idx] = DMR_ENC_CLASS_NONE;
    } else {
        // Lone contradiction of an established classification: quarantine it
        // until it repeats, keeping the established bit applied.
        state->dmr_enc_class_pending[idx] = obs;
    }

    if (state->dmr_enc_class[idx] == DMR_ENC_CLASS_ENC) {
        return so | 0x40U;
    }
    return so & ~0x40U;
}

/* Strong out-of-band evidence (a checksum-verified PI header, the heal of an
 * unverified terminator restoring live crypto): apply and corroborate. */
void
dmr_enc_class_force(dsd_state* state, uint8_t slot, int encrypted) {
    const uint8_t idx = dmr_enc_class_slot_index(slot);
    state->dmr_enc_class[idx] = encrypted ? DMR_ENC_CLASS_ENC : DMR_ENC_CLASS_CLEAR;
    state->dmr_enc_class_est[idx] = 1U;
    state->dmr_enc_class_pending[idx] = DMR_ENC_CLASS_NONE;
}

void
dmr_enc_class_reset(dsd_state* state, uint8_t slot) {
    const uint8_t idx = dmr_enc_class_slot_index(slot);
    state->dmr_enc_class[idx] = DMR_ENC_CLASS_NONE;
    state->dmr_enc_class_est[idx] = 0U;
    state->dmr_enc_class_pending[idx] = DMR_ENC_CLASS_NONE;
}

int
dmr_enc_class_established_enc(const dsd_state* state, uint8_t slot) {
    const uint8_t idx = dmr_enc_class_slot_index(slot);
    return state->dmr_enc_class[idx] == DMR_ENC_CLASS_ENC && state->dmr_enc_class_est[idx] != 0U;
}

int
dmr_enc_class_established_clear(const dsd_state* state, uint8_t slot) {
    const uint8_t idx = dmr_enc_class_slot_index(slot);
    return state->dmr_enc_class[idx] == DMR_ENC_CLASS_CLEAR && state->dmr_enc_class_est[idx] != 0U;
}
