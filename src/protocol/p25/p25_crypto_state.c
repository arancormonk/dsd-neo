// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/state.h>
#include <dsd-neo/protocol/p25/p25_crypto.h>

#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

void
p25_crypto_reset_slot(dsd_state* state, int slot) {
    if (!state || slot < 0 || slot > 1) {
        return;
    }

    if (slot == 0) {
        state->payload_algid = 0;
        state->payload_keyid = 0;
        state->payload_miP = 0ULL;
    } else {
        state->payload_algidR = 0;
        state->payload_keyidR = 0;
        state->payload_miN = 0ULL;
    }
    state->p25_crypto_state[slot] = DSD_P25_CRYPTO_UNKNOWN;
    state->p25_p2_audio_allowed[slot] = 0;
    state->p25_p2_enc_lockout_muted[slot] = 0U;

    if (slot == 0) {
        state->DMRvcL = 0;
        state->bit_counterL = 0;
        state->dropL = 256;
        state->p25vc = 0;
        state->octet_counter = 0;
        DSD_MEMSET(state->ks_octetL, 0, sizeof(state->ks_octetL));
        DSD_MEMSET(state->ks_bitstreamL, 0, sizeof(state->ks_bitstreamL));
    } else {
        state->DMRvcR = 0;
        state->bit_counterR = 0;
        state->dropR = 256;
        DSD_MEMSET(state->ks_octetR, 0, sizeof(state->ks_octetR));
        DSD_MEMSET(state->ks_bitstreamR, 0, sizeof(state->ks_bitstreamR));
    }

    if (state->keyloader == 1) {
        if (slot == 0) {
            state->R = 0ULL;
        } else {
            state->RR = 0ULL;
        }
        state->A1[slot] = 0ULL;
        state->A2[slot] = 0ULL;
        state->A3[slot] = 0ULL;
        state->A4[slot] = 0ULL;
        state->aes_key_loaded[slot] = 0;
        state->aes_key_segments[slot] = 0U;
    }
}
