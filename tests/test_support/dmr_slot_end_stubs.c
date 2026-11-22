// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* For the fixtures that compile dmr_csbk.c and dmr_slot_end.c without the DMR encryption sources or the keyring: the
 * end of one slot's call (dmr_slot_end_call()) resets that slot's encryption class, late-entry vendor mode, PI pre-call
 * record and talkgroup key autoload through these. They clear the same per-slot fields the real ones do, so a fixture
 * can see which slot was reset. */

#include <dsd-neo/core/keyring.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/protocol/dmr/dmr.h>
#include <stdint.h>
#include "dsd-neo/core/state_fwd.h"

void
dmr_enc_class_reset(dsd_state* state, uint8_t slot) {
    const uint8_t idx = slot != 0U ? 1U : 0U;
    state->dmr_enc_class[idx] = DMR_ENC_CLASS_NONE;
    state->dmr_enc_class_est[idx] = 0U;
    state->dmr_enc_class_pending[idx] = DMR_ENC_CLASS_NONE;
}

void
dmr_le_vendor_reset(dsd_state* state, uint8_t slot) {
    state->dmr_le_vendor[slot != 0U ? 1U : 0U] = DMR_LE_VENDOR_NONE;
}

void
dmr_pi_precall_reset(dsd_state* state, uint8_t slot) {
    const uint8_t idx = slot != 0U ? 1U : 0U;
    state->dmr_pi_precall_rx[idx] = 0U;
    state->dmr_pi_precall_algid[idx] = 0U;
    state->dmr_pi_precall_kid[idx] = 0U;
    state->dmr_pi_precall_m[idx] = 0.0;
    state->dmr_crypto_inherited[idx] = 0U;
    state->dmr_crypto_inherited_algid[idx] = 0;
    state->dmr_crypto_inherited_keyid[idx] = 0;
}

void
keyring_dmr_tg_autoload_forget_slot(dsd_state* state, uint8_t slot) {
    (void)state;
    (void)slot;
}
