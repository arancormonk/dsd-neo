// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Shared P25 voice crypto classification and slot-isolation helpers.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_P25_P25_CRYPTO_H_H
#define DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_P25_P25_CRYPTO_H_H

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state.h>
#include <stdint.h>

#include "dsd-neo/core/state_fwd.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DSD_P25_CRYPTO_PHASE1 = 1,
    DSD_P25_CRYPTO_PHASE2 = 2,
} dsd_p25_crypto_phase;

/** Return non-zero only for clear or supported decryptable voice. */
static inline int
p25_crypto_audio_ready(const dsd_state* state, int slot) {
    if (!state || slot < 0 || slot > 1) {
        return 0;
    }
    return state->p25_crypto_state[slot] == DSD_P25_CRYPTO_CLEAR
           || state->p25_crypto_state[slot] == DSD_P25_CRYPTO_DECRYPTABLE;
}

/** Return non-zero when stereo duplication must not fill this companion slot. */
static inline int
p25_crypto_companion_suppressed(const dsd_state* state, int slot) {
    if (!state || slot < 0 || slot > 1) {
        return 0;
    }
    return state->p25_crypto_state[slot] == DSD_P25_CRYPTO_ENCRYPTED_PENDING
           || state->p25_crypto_state[slot] == DSD_P25_CRYPTO_BLOCKED;
}

/** Start a voice-call classification window from grant service options. */
void p25_crypto_begin_voice_call(dsd_state* state, dsd_p25_crypto_phase phase, int slot, int svc_bits, int force_clear);

/** Mark an in-progress call as encryption-pending without overwriting ALGID/KID/MI. */
void p25_crypto_mark_encrypted_pending(dsd_state* state, int slot);

/**
 * Resolve definitive HDU/LDU2/MAC_PTT/ESS crypto metadata.
 *
 * Imported key material for @p keyid is activated before decryptability is
 * tested. ALGID 0 remains non-definitive; only ALGID 0x80 confirms clear voice.
 */
dsd_p25_crypto_state p25_crypto_resolve(dsd_opts* opts, dsd_state* state, dsd_p25_crypto_phase phase, int slot,
                                        int algid, int keyid, uint64_t mi, int talkgroup);

/** Convert a still-pending classification into a silent timeout block. */
void p25_crypto_block_pending(dsd_state* state, int slot);

/** Reset crypto classification and metadata at a call boundary. */
void p25_crypto_reset_slot(dsd_state* state, int slot);

#ifdef __cplusplus
}
#endif
#endif /* DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_P25_P25_CRYPTO_H_H */
