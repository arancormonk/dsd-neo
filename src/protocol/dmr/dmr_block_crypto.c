// SPDX-License-Identifier: ISC
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "dmr_block_crypto.h"
#include <dsd-neo/core/bp.h>
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
        /* Preserve legacy malformed-length fallback semantics, bounded to the actual buffer. */
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

    if (state->currentslot == 0) {
        ctx->alg = state->payload_algid;
        ctx->kid = state->payload_keyid;
        ctx->mi = (unsigned long long)state->payload_mi;
        ctx->rkey = dmr_block_rkey_at(state, state->payload_keyid);
    } else {
        ctx->alg = state->payload_algidR;
        ctx->kid = state->payload_keyidR;
        ctx->mi = (unsigned long long)state->payload_miR;
        ctx->rkey = dmr_block_rkey_at(state, state->payload_keyidR);
    }

    dmr_block_load_aes_key(state, ctx->kid, ctx->aes_key);
    ctx->aes_key_loaded = dmr_block_bytes_any_nonzero(ctx->aes_key, sizeof(ctx->aes_key));

    if (ctx->rkey == 0ULL && state->R != 0ULL) {
        ctx->rkey = state->R;
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

void
dmr_block_crypto_print_info(const dmr_block_crypto_ctx* ctx) {
    if (ctx == NULL) {
        return;
    }

    DSD_FPRINTF(stderr, "\\n PDU ALG: %02X; Key ID: %02X;", ctx->alg, ctx->kid);
    if (ctx->alg != 0 && ctx->mi != 0ULL) {
        DSD_FPRINTF(stderr, " MI(32): %08llX;", ctx->mi);
    }
    if (ctx->alg == 0) {
        DSD_FPRINTF(stderr, " Moto BP;");
    } else if (ctx->alg == 1) {
        DSD_FPRINTF(stderr, " RC4;");
    } else if (ctx->alg == 2) {
        DSD_FPRINTF(stderr, " DES;");
    } else if (ctx->alg == 4) {
        DSD_FPRINTF(stderr, " AES128;");
    } else if (ctx->alg == 5) {
        DSD_FPRINTF(stderr, " AES256;");
    } else if (ctx->alg == 7) {
        DSD_FPRINTF(stderr, " VTX STD;");
    }
    if (ctx->rkey != 0ULL && ctx->alg != 0) {
        DSD_FPRINTF(stderr, " Key: %s;", DSD_SECRET_REDACTED);
    }
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
    aes_ofb_keystream_output(maes, ctx->aes_key, stream, (ctx->alg == 5) ? 2 : 0, nblocks);
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
    aes_ecb_decrypt_blocks(payload, ctx->aes_key, payload, (ctx->alg == 5) ? 2 : 0, nblocks);
    return 1;
}

static uint8_t
dmr_block_crypto_apply_bp(dsd_state* state, uint8_t slot, const dmr_block_crypto_ctx* ctx) {
    if (ctx->alg != 0 || state->K == 0 || state->K > 0xFFULL) {
        return 0;
    }

    const uint16_t bp_key = BPK[state->K];
    DSD_FPRINTF(stderr, " Key: %s;", DSD_SECRET_REDACTED);
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
dmr_block_crypto_decrypt_payload(dsd_state* state, uint8_t slot, const dmr_block_crypto_ctx* ctx) {
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
        des_multi_keystream_output(ctx->mi, ctx->rkey, stream, 1, nblocks);
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

    return dmr_block_crypto_apply_bp(state, slot, ctx);
}
