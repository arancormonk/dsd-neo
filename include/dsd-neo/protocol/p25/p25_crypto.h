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

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <stdint.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DSD_P25_CRYPTO_PHASE1 = 1,
    DSD_P25_CRYPTO_PHASE2 = 2,
} dsd_p25_crypto_phase;

/** Return the standard display name for a known P25 algorithm ID, or NULL. */
const char* p25_algid_name(uint8_t algid);

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

/** Return non-zero only when non-clear metadata has completed classification. */
static inline int
p25_crypto_metadata_is_confirmed_encrypted(const dsd_state* state, int slot) {
    if (!state || slot < 0 || slot > 1) {
        return 0;
    }
    const int algid = slot == 0 ? state->payload_algid : state->payload_algidR;
    if (algid == 0 || algid == 0x80 || (slot == 0 && state->p25_p1_crypto_conflict.active)) {
        return 0;
    }
    return state->p25_crypto_state[slot] == DSD_P25_CRYPTO_DECRYPTABLE
           || state->p25_crypto_state[slot] == DSD_P25_CRYPTO_BLOCKED;
}

/**
 * Return non-zero when a P25 voice frame may reach the vocoder.
 *
 * Clear and decryptable calls are always permitted. During encrypted-call
 * following, the user-controlled P25 unmute override and reverse mute may
 * explicitly permit undeciphered audio. Encryption lockout takes precedence so
 * classification probes remain silent until a key or clear metadata resolves.
 */
static inline int
p25_crypto_audio_permitted(const dsd_opts* opts, const dsd_state* state, int slot) {
    if (!state || slot < 0 || slot > 1) {
        return 0;
    }
    if (slot == 0 && state->p25_p1_identity_pending) {
        return 0;
    }
    if (slot == 0 && state->p25_p1_crypto_conflict.active) {
        return 0;
    }
    if (p25_crypto_audio_ready(state, slot)) {
        return 1;
    }
    if (!opts || opts->trunk_tune_enc_calls == 0) {
        return 0;
    }

    return opts->unmute_encrypted_p25 == 1 || opts->reverse_mute == 1;
}

/** Start a voice-call classification window from grant service options. */
void p25_crypto_begin_voice_call(dsd_state* state, dsd_p25_crypto_phase phase, int slot, int svc_bits, int force_clear);

/** Mark an in-progress call as encryption-pending without overwriting ALGID/KID/MI. */
void p25_crypto_mark_encrypted_pending(dsd_state* state, int slot);

/**
 * Quarantine a retained Phase 1 tuple that contradicts explicit-clear service
 * options. Returns non-zero while the tuple is awaiting another HDU/LDU2
 * observation.
 */
int p25_crypto_p1_defer_clear_conflict(dsd_state* state, int svc_bits);

/**
 * Resolve definitive HDU/LDU2/MAC_PTT/ESS crypto metadata.
 *
 * Imported key material for @p keyid is activated before decryptability is
 * tested. ALGID 0 remains non-definitive; only ALGID 0x80 confirms clear voice.
 * A Phase 1 non-clear tuple that contradicts explicit-clear service options
 * remains pending until another FEC-accepted tuple repeats the ALGID and KID.
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
