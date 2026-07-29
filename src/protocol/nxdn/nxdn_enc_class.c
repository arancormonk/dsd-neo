// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Hysteresis for the NXDN cipher field.
 *
 * The 2-bit cipher type reaches the decoder from several channels -- VCALL in
 * SACCH/FACCH/CAC, the DCR SACCH-2 message, and the SCCH call option -- and
 * each sits behind FEC and a short CRC that can accept a miscorrected payload
 * at low SNR. Acting on every observation directly let a single corrupt
 * element mute a clear call (or silently unmute an encrypted one), and under
 * --enc-lockout write a session-permanent blocking talkgroup entry and force
 * the channel released -- the same single-observation defect class fixed for
 * P25 in #280. This module makes the applied cipher sticky: observations
 * apply tentatively and must repeat before they can flip an established
 * classification, user-forced scrambler modes apply as authoritative, and the
 * lockout only acts on an established non-clear classification.
 *
 * The class fields store the observed cipher plus one so 0 can mean "no
 * observation this transmission"; cipher 0 (clear) is a real observed value.
 */

#include <dsd-neo/core/state.h>
#include <dsd-neo/protocol/nxdn/nxdn.h>
#include <stdint.h>

#include "dsd-neo/core/state_fwd.h"

/* Feed one cipher observation and return the cipher value to apply. `strong`
 * marks evidence trustworthy on its own; weak evidence establishes only by
 * repetition, and a lone contradiction of an established classification is
 * quarantined until a matching repeat (one superframe cadence). */
uint8_t
nxdn_cipher_observe(dsd_state* state, uint8_t cipher, int strong) {
    const uint8_t obs = (uint8_t)((cipher & 0x03U) + 1U);

    if (strong) {
        state->nxdn_cipher_class = obs;
        state->nxdn_cipher_class_est = 1U;
        state->nxdn_cipher_class_pending = 0U;
    } else if (state->nxdn_cipher_class == 0U) {
        // First observation of the transmission: apply tentatively so late
        // entry still classifies immediately, but leave it uncorroborated so
        // a lone corrupt element cannot arm the lockout.
        state->nxdn_cipher_class = obs;
        state->nxdn_cipher_class_est = 0U;
        state->nxdn_cipher_class_pending = 0U;
    } else if (obs == state->nxdn_cipher_class) {
        // A matching repeat corroborates; VCALL repeats every superframe, so
        // a real classification establishes within one repeat.
        state->nxdn_cipher_class_est = 1U;
        state->nxdn_cipher_class_pending = 0U;
    } else if (state->nxdn_cipher_class_est == 0U || state->nxdn_cipher_class_pending == obs) {
        // Contradicting a value that never corroborated, or the second
        // consecutive contradiction of an established classification: the
        // newer observation wins (a tentative replacement stays tentative; a
        // corroborated flip keeps the classification established).
        state->nxdn_cipher_class = obs;
        state->nxdn_cipher_class_pending = 0U;
    } else {
        // Lone contradiction of an established classification: quarantine it
        // until it repeats, keeping the established value applied.
        state->nxdn_cipher_class_pending = obs;
    }

    return (uint8_t)(state->nxdn_cipher_class - 1U);
}

/* Authoritative out-of-band evidence (user-forced scrambler mode): apply to
 * both the live cipher and the classification, corroborated. */
void
nxdn_cipher_force(dsd_state* state, uint8_t cipher) {
    state->nxdn_cipher_type = cipher;
    state->nxdn_cipher_class = (uint8_t)((cipher & 0x03U) + 1U);
    state->nxdn_cipher_class_est = 1U;
    state->nxdn_cipher_class_pending = 0U;
}

void
nxdn_cipher_class_reset(dsd_state* state) {
    state->nxdn_cipher_class = 0U;
    state->nxdn_cipher_class_est = 0U;
    state->nxdn_cipher_class_pending = 0U;
}

int
nxdn_cipher_established_enc(const dsd_state* state) {
    return state->nxdn_cipher_class > 1U && state->nxdn_cipher_class_est != 0U;
}
