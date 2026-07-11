// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/keyring.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/vocoder.h>
#include <dsd-neo/protocol/p25/p25_crypto.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <stddef.h>
#include <stdint.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

static int
p25_crypto_slot_valid(int slot) {
    return slot == 0 || slot == 1;
}

static int
p25_crypto_slot_algid(const dsd_state* state, int slot) {
    return slot == 0 ? state->payload_algid : state->payload_algidR;
}

static int
p25_crypto_slot_keyid(const dsd_state* state, int slot) {
    return slot == 0 ? state->payload_keyid : state->payload_keyidR;
}

static uint64_t
p25_crypto_slot_mi(const dsd_state* state, int slot) {
    return slot == 0 ? state->payload_miP : state->payload_miN;
}

static void
p25_crypto_store_metadata(dsd_state* state, int slot, int algid, int keyid, uint64_t mi) {
    if (slot == 0) {
        state->payload_algid = algid;
        state->payload_keyid = keyid;
        state->payload_miP = mi;
        return;
    }
    state->payload_algidR = algid;
    state->payload_keyidR = keyid;
    state->payload_miN = mi;
}

static void
p25_crypto_set_state(dsd_state* state, int slot, dsd_p25_crypto_state crypto_state) {
    if (!state || !p25_crypto_slot_valid(slot)) {
        return;
    }
    state->p25_crypto_state[slot] = crypto_state;
    state->p25_p2_enc_lockout_muted[slot] =
        (uint8_t)((crypto_state == DSD_P25_CRYPTO_ENCRYPTED_PENDING || crypto_state == DSD_P25_CRYPTO_BLOCKED) ? 1U
                                                                                                               : 0U);
    if (!p25_crypto_audio_ready(state, slot)) {
        state->p25_p2_audio_allowed[slot] = 0;
    }
}

static void
p25_crypto_reset_stream_state(dsd_state* state, dsd_p25_crypto_phase phase, int slot) {
    if (slot == 0) {
        state->DMRvcL = 0;
        state->bit_counterL = 0;
        state->dropL = (phase == DSD_P25_CRYPTO_PHASE1) ? 267 : 256;
        DSD_MEMSET(state->ks_octetL, 0, sizeof(state->ks_octetL));
        DSD_MEMSET(state->ks_bitstreamL, 0, sizeof(state->ks_bitstreamL));
        if (phase == DSD_P25_CRYPTO_PHASE1) {
            state->p25vc = 0;
            state->octet_counter = 0;
        }
        return;
    }

    state->DMRvcR = 0;
    state->bit_counterR = 0;
    state->dropR = 256;
    DSD_MEMSET(state->ks_octetR, 0, sizeof(state->ks_octetR));
    DSD_MEMSET(state->ks_bitstreamR, 0, sizeof(state->ks_bitstreamR));
}

static int
p25_crypto_algorithm_supported(dsd_p25_crypto_phase phase, int algid) {
    if (algid == 0xAA || algid == 0x81 || algid == 0x84 || algid == 0x89) {
        return 1;
    }
    if (phase == DSD_P25_CRYPTO_PHASE1 && (algid == 0x83 || algid == 0x9F)) {
        return 1;
    }
    return 0;
}

static int
p25_crypto_imported_aes_segments_complete(const dsd_state* state, int slot, unsigned int required_segments) {
    static const int offsets[4] = {0x000, 0x101, 0x201, 0x301};
    const int key_id = p25_crypto_slot_keyid(state, slot);
    const size_t rkey_count = sizeof(state->rkey_array) / sizeof(state->rkey_array[0]);

    for (unsigned int i = 0; i < required_segments; i++) {
        const int index = key_id + offsets[i];
        if (index < 0 || (size_t)index >= rkey_count
            || (state->rkey_array_loaded[index] == 0U && state->rkey_array[index] == 0ULL)) {
            return 0;
        }
    }
    return 1;
}

static int
p25_crypto_has_complete_key(const dsd_state* state, dsd_p25_crypto_phase phase, int slot, int algid) {
    if (!p25_crypto_algorithm_supported(phase, algid)) {
        return 0;
    }

    const uint64_t scalar_key = slot == 0 ? state->R : state->RR;
    if (algid == 0xAA || algid == 0x81 || algid == 0x9F) {
        return scalar_key != 0ULL;
    }

    if (state->aes_key_loaded[slot] != 1) {
        return 0;
    }
    const unsigned int required_segments = algid == 0x89 ? 2U : (algid == 0x83 ? 3U : 4U);
    if (state->aes_key_segments[slot] < required_segments) {
        return 0;
    }
    return state->keyloader != 1 || p25_crypto_imported_aes_segments_complete(state, slot, required_segments);
}

typedef struct {
    int algid;
    int keyid;
    uint64_t mi;
    dsd_p25_crypto_state state;
} p25_crypto_snapshot;

static int
p25_crypto_resolve_inputs_valid(const dsd_state* state, dsd_p25_crypto_phase phase, int slot) {
    return state && p25_crypto_slot_valid(slot) && (phase == DSD_P25_CRYPTO_PHASE1 || phase == DSD_P25_CRYPTO_PHASE2);
}

static p25_crypto_snapshot
p25_crypto_capture_snapshot(const dsd_state* state, int slot) {
    p25_crypto_snapshot snapshot = {
        .algid = p25_crypto_slot_algid(state, slot),
        .keyid = p25_crypto_slot_keyid(state, slot),
        .mi = p25_crypto_slot_mi(state, slot),
        .state = state->p25_crypto_state[slot],
    };
    return snapshot;
}

static dsd_p25_crypto_state
p25_crypto_resolve_algid_zero(dsd_state* state, int slot) {
    const dsd_p25_crypto_state current = state->p25_crypto_state[slot];
    if (current == DSD_P25_CRYPTO_UNKNOWN || current == DSD_P25_CRYPTO_ENCRYPTED_PENDING
        || current == DSD_P25_CRYPTO_BLOCKED) {
        p25_crypto_mark_encrypted_pending(state, slot);
    }
    return state->p25_crypto_state[slot];
}

static dsd_p25_crypto_state
p25_crypto_classify_metadata(const dsd_state* state, dsd_p25_crypto_phase phase, int slot, int algid) {
    if (algid == 0x80) {
        return DSD_P25_CRYPTO_CLEAR;
    }
    return p25_crypto_has_complete_key(state, phase, slot, algid) ? DSD_P25_CRYPTO_DECRYPTABLE : DSD_P25_CRYPTO_BLOCKED;
}

static void
p25_crypto_apply_resolution(dsd_opts* opts, dsd_state* state, dsd_p25_crypto_phase phase, int slot, int algid,
                            int keyid, uint64_t mi, int talkgroup, const p25_crypto_snapshot* previous,
                            dsd_p25_crypto_state resolved) {
    const int key_identity_changed = previous->algid != algid || previous->keyid != keyid;
    const int state_changed = previous->state != resolved;
    const int purge_audio = state_changed || (previous->state == DSD_P25_CRYPTO_DECRYPTABLE && key_identity_changed);
    const int reset_stream = state_changed || key_identity_changed || previous->mi != mi;

    if (purge_audio) {
        dsd_mbe_purge_slot_audio(state, slot);
    }
    if (reset_stream) {
        p25_crypto_reset_stream_state(state, phase, slot);
    }
    p25_crypto_set_state(state, slot, resolved);

    if (phase == DSD_P25_CRYPTO_PHASE1 && opts) {
        opts->unmute_encrypted_p25 = (resolved == DSD_P25_CRYPTO_DECRYPTABLE) ? 1 : 0;
    }
    if (algid != 0x80 && opts) {
        p25_sm_emit_enc(opts, state, slot, algid, keyid, talkgroup);
    }
}

void
p25_crypto_begin_voice_call(dsd_state* state, dsd_p25_crypto_phase phase, int slot, int svc_bits, int force_clear) {
    if (!state || !p25_crypto_slot_valid(slot) || (phase != DSD_P25_CRYPTO_PHASE1 && phase != DSD_P25_CRYPTO_PHASE2)) {
        return;
    }
    if (phase == DSD_P25_CRYPTO_PHASE1) {
        slot = 0;
    }

    dsd_mbe_purge_slot_audio(state, slot);
    p25_crypto_store_metadata(state, slot, 0, 0, 0ULL);
    p25_crypto_reset_stream_state(state, phase, slot);

    const int service_options_clear = svc_bits >= 0 && (svc_bits & 0x40) == 0;
    p25_crypto_set_state(
        state, slot, (force_clear || service_options_clear) ? DSD_P25_CRYPTO_CLEAR : DSD_P25_CRYPTO_ENCRYPTED_PENDING);
    state->p25_p2_audio_allowed[slot] = 0;
}

void
p25_crypto_mark_encrypted_pending(dsd_state* state, int slot) {
    if (!state || !p25_crypto_slot_valid(slot)) {
        return;
    }

    dsd_p25_crypto_state current = state->p25_crypto_state[slot];
    if (current == DSD_P25_CRYPTO_ENCRYPTED_PENDING || current == DSD_P25_CRYPTO_BLOCKED) {
        p25_crypto_set_state(state, slot, current);
        return;
    }
    if (current == DSD_P25_CRYPTO_DECRYPTABLE) {
        return;
    }

    p25_crypto_set_state(state, slot, DSD_P25_CRYPTO_ENCRYPTED_PENDING);
    dsd_mbe_purge_slot_audio(state, slot);
}

dsd_p25_crypto_state
p25_crypto_resolve(dsd_opts* opts, dsd_state* state, dsd_p25_crypto_phase phase, int slot, int algid, int keyid,
                   uint64_t mi, int talkgroup) {
    if (!p25_crypto_resolve_inputs_valid(state, phase, slot)) {
        return DSD_P25_CRYPTO_UNKNOWN;
    }
    slot = phase == DSD_P25_CRYPTO_PHASE1 ? 0 : slot;

    if (algid == 0) {
        return p25_crypto_resolve_algid_zero(state, slot);
    }

    const p25_crypto_snapshot previous = p25_crypto_capture_snapshot(state, slot);
    p25_crypto_store_metadata(state, slot, algid, keyid, mi);

    if (algid != 0x80 && state->keyloader == 1) {
        keyring_activate_slot(opts, state, slot);
    }

    const dsd_p25_crypto_state resolved = p25_crypto_classify_metadata(state, phase, slot, algid);
    p25_crypto_apply_resolution(opts, state, phase, slot, algid, keyid, mi, talkgroup, &previous, resolved);
    return resolved;
}

void
p25_crypto_block_pending(dsd_state* state, int slot) {
    if (!state || !p25_crypto_slot_valid(slot) || state->p25_crypto_state[slot] != DSD_P25_CRYPTO_ENCRYPTED_PENDING) {
        return;
    }
    p25_crypto_set_state(state, slot, DSD_P25_CRYPTO_BLOCKED);
    dsd_mbe_purge_slot_audio(state, slot);
}
