// SPDX-License-Identifier: ISC
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "dmr_block_crypto.h"
#include <dsd-neo/core/bp.h>
#include <dsd-neo/core/keyring.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/crypto/aes.h>
#include <dsd-neo/crypto/des.h>
#include <dsd-neo/crypto/rc4.h>
#include <dsd-neo/protocol/dmr/dmr.h>
#include <stdint.h>
#include <stdio.h>
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/secret_redaction.h"

enum {
    DMR_BLOCK_CRYPTO_STREAM_BYTES = 129 * 24,
    DMR_AES_BLOCK_BYTES = 16,
    DMR_AES_OFB_DISCARD_BYTES = 16,
};

static unsigned long long
dmr_block_rkey_at(const dsd_state* state, int index) {
    const int capacity = (int)(sizeof(state->rkey_array) / sizeof(state->rkey_array[0]));
    if (state == NULL || index < 0 || index >= capacity) {
        return 0ULL;
    }
    return state->rkey_array[index];
}

static void
dmr_block_store_u64_be(unsigned long long value, uint8_t* out) {
    for (int i = 0; i < 8; i++) {
        out[i] = (uint8_t)((value >> (56 - (i * 8))) & 0xFFU);
    }
}

static int
dmr_block_bytes_any_nonzero(const uint8_t* bytes, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (bytes[i] != 0U) {
            return 1;
        }
    }
    return 0;
}

static void
dmr_block_load_aes_key(const dsd_state* state, int key_id, uint8_t aes_key[32]) {
    unsigned long long parts[4] = {
        dmr_block_rkey_at(state, key_id + 0x000),
        dmr_block_rkey_at(state, key_id + 0x101),
        dmr_block_rkey_at(state, key_id + 0x201),
        dmr_block_rkey_at(state, key_id + 0x301),
    };

    if (parts[0] == 0ULL && parts[1] == 0ULL && parts[2] == 0ULL && parts[3] == 0ULL) {
        parts[0] = state->K1;
        parts[1] = state->K2;
        parts[2] = state->K3;
        parts[3] = state->K4;
    }

    for (int i = 0; i < 4; i++) {
        dmr_block_store_u64_be(parts[i], aes_key + ((size_t)i * 8U));
    }
}

static void
dmr_block_crypto_clamp_window(const dsd_state* state, uint8_t slot, dmr_block_crypto_ctx* ctx) {
    const int cap = (int)(sizeof(state->dmr_pdu_sf[slot]) / sizeof(state->dmr_pdu_sf[slot][0]));
    if (ctx->start < 0) {
        ctx->start = 0;
    }
    if (ctx->start > cap) {
        ctx->start = cap;
    }
    const int max_end = cap - ctx->start;
    if (ctx->end < 0 || ctx->end > max_end) {
        /* Malformed lengths consume the remaining bounded buffer window. */
        ctx->end = max_end;
    }
}

void
dmr_block_crypto_load_ctx(const dsd_state* state, uint8_t slot, int blocks, uint8_t block_len,
                          dmr_block_crypto_ctx* ctx) {
    if (ctx == NULL) {
        return;
    }
    DSD_MEMSET(ctx, 0, sizeof(*ctx));
    if (state == NULL || slot >= 2U) {
        return;
    }

    ctx->start = (int)state->data_ks_start[slot];
    ctx->end = ((blocks + 1) * block_len) - 4 - (int)state->data_block_poc[slot] - ctx->start;
    dmr_block_crypto_clamp_window(state, slot, ctx);

    // The caller passes `slot`; the identity fields are selected by state->currentslot. They
    // agree at every call site today -- normalizing here keeps the new map lookup from
    // silently resolving for a different slot than the alg/kid it is resolving for.
    const uint8_t id_slot = (uint8_t)((state->currentslot == 1) ? 1 : 0);

    if (id_slot == 0U) {
        ctx->alg = state->payload_algid;
        ctx->signaled_kid = state->payload_keyid;
        ctx->mi = (unsigned long long)state->payload_mi;
    } else {
        ctx->alg = state->payload_algidR;
        ctx->signaled_kid = state->payload_keyidR;
        ctx->mi = (unsigned long long)state->payload_miR;
    }

    // Same key for the call's data as for its voice: --dmr-tg-key-csv is keyed on the data
    // header's target, whose group flag was recorded when the header was parsed.
    //
    // Only a signaled id that round-trips through the resolver's uint8_t goes through it -- the
    // same guard dsd_mbe.c and dsd_file.c apply. payload_keyid is shared across protocols and
    // P25 writes a full 16-bit KID into it (p25_crypto.c), so narrowing unconditionally would
    // make ctx->kid index rkey_array[id & 0xFF] and decrypt the PDU with an unrelated key.
    ctx->kid = ctx->signaled_kid;
    if (ctx->signaled_kid >= 0 && ctx->signaled_kid <= 0xFF) {
        ctx->kid = (int)keyring_dmr_effective_kid(state, (uint32_t)state->dmr_lrrp_target[id_slot],
                                                  state->dmr_data_target_is_group[id_slot] != 0U,
                                                  (uint8_t)ctx->signaled_kid, &ctx->mapped);
    }
    ctx->rkey = dmr_block_rkey_at(state, ctx->kid);

    dmr_block_load_aes_key(state, ctx->kid, ctx->aes_key);
    ctx->aes_key_loaded = dmr_block_bytes_any_nonzero(ctx->aes_key, sizeof(ctx->aes_key));

    // Slot-correct fallback: RR keys slot 2. Reading R for both slots decrypted slot 2 with
    // slot 1's key whenever the resolved id had nothing imported.
    const unsigned long long slot_rkey = (id_slot == 0U) ? state->R : state->RR;
    if (ctx->rkey == 0ULL && slot_rkey != 0ULL) {
        ctx->rkey = slot_rkey;
    }

    ctx->rc4_iv[0] = (uint8_t)((ctx->rkey & 0xFF00000000ULL) >> 32U);
    ctx->rc4_iv[1] = (uint8_t)((ctx->rkey & 0xFF000000ULL) >> 24U);
    ctx->rc4_iv[2] = (uint8_t)((ctx->rkey & 0xFF0000ULL) >> 16U);
    ctx->rc4_iv[3] = (uint8_t)((ctx->rkey & 0xFF00ULL) >> 8U);
    ctx->rc4_iv[4] = (uint8_t)((ctx->rkey & 0xFFULL) >> 0U);
    ctx->rc4_iv[5] = (uint8_t)((ctx->mi & 0xFF000000ULL) >> 24U);
    ctx->rc4_iv[6] = (uint8_t)((ctx->mi & 0xFF0000ULL) >> 16U);
    ctx->rc4_iv[7] = (uint8_t)((ctx->mi & 0xFF00ULL) >> 8U);
    ctx->rc4_iv[8] = (uint8_t)((ctx->mi & 0xFFULL) >> 0U);
}

static const char*
dmr_block_crypto_alg_label(int alg) {
    switch (alg) {
        case 0: return "Moto BP";
        case 1: return "RC4";
        case 2: return "DES";
        case 4: return "AES128";
        case 5: return "AES256";
        case 7: return "VTX STD";
        default: return NULL;
    }
}

static void
dmr_block_crypto_print_key(const dmr_block_crypto_ctx* ctx, int show_keys) {
    if ((ctx->alg == 4 || ctx->alg == 5) && ctx->aes_key_loaded == 1) {
        char key_text[65];
        const size_t key_bytes = (ctx->alg == 5) ? 32U : 16U;
        DSD_FPRINTF(stderr, " Key: %s;",
                    dsd_secret_format_byte_hex(key_text, sizeof key_text, show_keys, ctx->aes_key, key_bytes));
    } else if (ctx->rkey != 0ULL && ctx->alg != 0) {
        char key_text[19];
        const unsigned width = (ctx->alg == 2) ? 16U : 10U;
        DSD_FPRINTF(stderr, " Key: %s;",
                    dsd_secret_format_hex(key_text, sizeof key_text, show_keys, ctx->rkey, width, 0));
    }
}

void
dmr_block_crypto_print_info(const dmr_block_crypto_ctx* ctx, int show_keys) {
    if (ctx == NULL) {
        return;
    }

    DSD_FPRINTF(stderr, "\n PDU ALG: %02X; Key ID: %02X;", ctx->alg, ctx->signaled_kid);
    if (ctx->mapped) {
        DSD_FPRINTF(stderr, " TG Key Map -> Key ID: %02X;", ctx->kid);
    }
    if (ctx->alg != 0 && ctx->mi != 0ULL) {
        DSD_FPRINTF(stderr, " MI(32): %08llX;", ctx->mi);
    }
    const char* alg_label = dmr_block_crypto_alg_label(ctx->alg);
    if (alg_label != NULL) {
        DSD_FPRINTF(stderr, " %s;", alg_label);
    }
    dmr_block_crypto_print_key(ctx, show_keys);
}

static void
dmr_block_crypto_apply_stream(dsd_state* state, uint8_t slot, const dmr_block_crypto_ctx* ctx,
                              const uint8_t stream[DMR_BLOCK_CRYPTO_STREAM_BYTES], int stream_offset) {
    for (int i = 0; i < ctx->end; i++) {
        state->dmr_pdu_sf[slot][i + ctx->start] ^= stream[(i + stream_offset) % DMR_BLOCK_CRYPTO_STREAM_BYTES];
    }
}

static void
dmr_block_crypto_prepare_aes_iv(dsd_state* state, uint8_t maes[16]) {
    LFSR128d(state);
    if (state->currentslot == 0) {
        DSD_MEMCPY(maes, state->aes_iv, 16);
    } else {
        DSD_MEMCPY(maes, state->aes_ivR, 16);
    }
}

static void
dmr_block_crypto_normalize_aes_algid(dsd_state* state, const dmr_block_crypto_ctx* ctx) {
    const int normalized = (ctx->alg == 5) ? 0x25 : 0x24;
    if (state->currentslot == 0) {
        state->payload_algid = normalized;
    } else {
        state->payload_algidR = normalized;
    }
}

static uint8_t
dmr_block_crypto_apply_aes_ofb(dsd_state* state, uint8_t slot, const dmr_block_crypto_ctx* ctx) {
    uint8_t stream[DMR_BLOCK_CRYPTO_STREAM_BYTES];
    uint8_t maes[16];
    const int nblocks = (ctx->end + DMR_AES_OFB_DISCARD_BYTES + (DMR_AES_BLOCK_BYTES - 1)) / DMR_AES_BLOCK_BYTES;

    DSD_MEMSET(stream, 0, sizeof(stream));
    DSD_MEMSET(maes, 0, sizeof(maes));
    DSD_FPRINTF(stderr, "\n");
    dmr_block_crypto_prepare_aes_iv(state, maes);
    const dsd_aes_key_size key_size = (ctx->alg == 5) ? DSD_AES_KEY_256 : DSD_AES_KEY_128;
    aes_ofb_keystream_output(maes, ctx->aes_key, stream, key_size, nblocks);
    dmr_block_crypto_apply_stream(state, slot, ctx, stream, DMR_AES_OFB_DISCARD_BYTES);
    return 1;
}

static uint8_t
dmr_block_crypto_apply_aes_ecb(const dsd_state* state, uint8_t* slot_payload, uint8_t slot,
                               const dmr_block_crypto_ctx* ctx) {
    const int cap = (int)(sizeof(state->dmr_pdu_sf[slot]) / sizeof(state->dmr_pdu_sf[slot][0]));
    const int available = (ctx->start < cap) ? (cap - ctx->start) : 0;
    const int reference_blocks = (int)state->data_byte_ctr[slot] / DMR_AES_BLOCK_BYTES;
    const int nblocks =
        (reference_blocks < (available / DMR_AES_BLOCK_BYTES)) ? reference_blocks : (available / DMR_AES_BLOCK_BYTES);
    if (nblocks <= 0) {
        return 0;
    }

    uint8_t* payload = slot_payload + ctx->start;
    const dsd_aes_key_size key_size = (ctx->alg == 5) ? DSD_AES_KEY_256 : DSD_AES_KEY_128;
    aes_ecb_decrypt_blocks(payload, ctx->aes_key, payload, key_size, nblocks);
    return 1;
}

static uint8_t
dmr_block_crypto_apply_bp(dsd_state* state, uint8_t slot, const dmr_block_crypto_ctx* ctx, int show_keys) {
    if (ctx->alg != 0 || state->K == 0 || state->K > 0xFFULL) {
        return 0;
    }

    const uint16_t bp_key = BPK[state->K];
    char key_text[16];
    DSD_FPRINTF(stderr, " Key: %s;", dsd_secret_format_decimal(key_text, sizeof key_text, show_keys, state->K, 0U));
    if (bp_key == 0U) {
        return 0;
    }

    const uint8_t stream[2] = {
        (uint8_t)((bp_key >> 8U) & 0xFFU),
        (uint8_t)((bp_key >> 0U) & 0xFFU),
    };
    for (int i = 0; i < ctx->end; i++) {
        state->dmr_pdu_sf[slot][i + ctx->start] ^= stream[i % 2];
    }
    return 1;
}

uint8_t
dmr_block_crypto_decrypt_payload(dsd_state* state, uint8_t slot, const dmr_block_crypto_ctx* ctx, int show_keys) {
    if (state == NULL || ctx == NULL || slot >= 2U) {
        return 0;
    }
    if (ctx->end <= 0) {
        return 0;
    }

    if (ctx->alg == 1 && ctx->rkey != 0ULL) {
        uint8_t stream[DMR_BLOCK_CRYPTO_STREAM_BYTES];
        DSD_MEMSET(stream, 0, sizeof(stream));
        rc4_block_output(256, 9, ctx->end, ctx->rc4_iv, stream);
        dmr_block_crypto_apply_stream(state, slot, ctx, stream, 0);
        return 1;
    }

    if (ctx->alg == 2 && ctx->rkey != 0ULL) {
        uint8_t stream[DMR_BLOCK_CRYPTO_STREAM_BYTES];
        const int nblocks = (ctx->end / 8) + 1;
        DSD_MEMSET(stream, 0, sizeof(stream));
        des_ofb_keystream_output(ctx->mi, ctx->rkey, stream, nblocks);
        dmr_block_crypto_apply_stream(state, slot, ctx, stream, 0);
        return 1;
    }

    if (ctx->alg == 4 || ctx->alg == 5) {
        dmr_block_crypto_normalize_aes_algid(state, ctx);
        if (ctx->aes_key_loaded != 1) {
            return 0;
        }
        if (ctx->mi == 0ULL) {
            /*
             * Zero-MI DMR payloads use ECB, so skip unused IV/LFSR work and
             * normalize the AlgID directly.
             */
            return dmr_block_crypto_apply_aes_ecb(state, state->dmr_pdu_sf[slot], slot, ctx);
        }
        return dmr_block_crypto_apply_aes_ofb(state, slot, ctx);
    }

    return dmr_block_crypto_apply_bp(state, slot, ctx, show_keys);
}
