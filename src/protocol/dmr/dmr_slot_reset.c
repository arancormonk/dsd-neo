// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* One DMR slot's call state, reset slot by slot (dmr_slot_reset.h). */

#include "dmr_slot_reset.h"

#include <dsd-neo/core/keyring.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/protocol/dmr/dmr.h>
#include <stdint.h>

#include "dmr_vcrypt.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

dmr_slot_crypto
dmr_slot_crypto_fields(dsd_state* state, uint8_t slot) {
    if (slot == 0U) {
        return (dmr_slot_crypto){
            &state->dmr_fid, &state->dmr_so, &state->payload_algid, &state->payload_keyid, &state->payload_mi,
        };
    }
    return (dmr_slot_crypto){
        &state->dmr_fidR, &state->dmr_soR, &state->payload_algidR, &state->payload_keyidR, &state->payload_miR,
    };
}

void
dmr_slot_reset_crypto(dsd_state* state, uint8_t slot) {
    const uint8_t idx = slot != 0U ? 1U : 0U;
    const dmr_slot_crypto live = dmr_slot_crypto_fields(state, idx);
    *live.fid = 0;
    *live.so = 0;
    *live.algid = 0;
    *live.keyid = 0;
    *live.mi = 0;
    dmr_enc_class_reset(state, idx);
    dmr_le_vendor_reset(state, idx);
    dmr_pi_precall_reset(state, idx);
    // The call's superframe chain ends with it (issue #729).
    dmr_vcrypt_forget(state, idx);
}

void
dmr_slot_reset_metadata(dsd_state* state, uint8_t slot) {
    const uint8_t idx = slot != 0U ? 1U : 0U;
    state->dmr_alias_block_len[idx] = 0;
    state->dmr_alias_char_size[idx] = 0;
    state->dmr_alias_format[idx] = 0;
    DSD_SNPRINTF(state->generic_talker_alias[idx], sizeof(state->generic_talker_alias[idx]), "%s", "");
    DSD_MEMSET(state->dmr_pdu_sf[idx], 0, sizeof(state->dmr_pdu_sf[idx]));
    state->dmr_embedded_gps[idx][0] = '\0';
    state->dmr_lrrp_gps[idx][0] = '\0';
}

void
dmr_slot_retire(dsd_state* state, uint8_t slot) {
    if (!state) {
        return;
    }
    const uint8_t idx = slot != 0U ? 1U : 0U;
    state->dmr_heal_valid[idx] = 0U;
    dmr_slot_reset_crypto(state, idx);
    dmr_slot_reset_metadata(state, idx);
    // The slot's next call keys from no talkgroup of the retired one's (keyring_dmr_tg_autoload_apply()).
    keyring_dmr_tg_autoload_forget_slot(state, idx);
    // The DES IV the slot's MI expanded to (dmr_vcrypt.c, LFSR64()), as a leave forgets it.
    if (idx == 0U) {
        state->payload_miP = 0;
    } else {
        state->payload_miN = 0;
    }
    dmr_slot_reset_blocks(state, idx);
    dmr_enc_class_hold_slot(state, idx);
}
