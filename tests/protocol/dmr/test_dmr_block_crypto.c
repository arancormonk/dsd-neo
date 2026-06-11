// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/bp.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/crypto/aes.h>
#include <dsd-neo/crypto/des.h>
#include <dsd-neo/crypto/rc4.h>
#include <dsd-neo/protocol/dmr/dmr.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "dmr_block_crypto.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "test_support.h"

static int g_lfsr_calls = 0;

void
LFSR128d(dsd_state* state) {
    g_lfsr_calls++;
    uint8_t* iv = (state->currentslot == 0) ? state->aes_iv : state->aes_ivR;
    for (int i = 0; i < 16; i++) {
        iv[i] = (uint8_t)i;
    }
}

static int
expect_int(const char* label, int got, int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %d want %d\n", label, got, want);
        return 1;
    }
    return 0;
}

static int
expect_u8(const char* label, uint8_t got, uint8_t want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got 0x%02X want 0x%02X\n", label, got, want);
        return 1;
    }
    return 0;
}

static int
expect_bytes(const char* label, const uint8_t* got, const uint8_t* want, size_t len) {
    if (memcmp(got, want, len) != 0) {
        DSD_FPRINTF(stderr, "%s: byte mismatch\n", label);
        return 1;
    }
    return 0;
}

static int
expect_contains(const char* label, const char* got, const char* needle) {
    if (strstr(got, needle) == NULL) {
        DSD_FPRINTF(stderr, "%s: expected output to contain \"%s\", got \"%s\"\n", label, needle, got);
        return 1;
    }
    return 0;
}

static int
capture_print_info(const dmr_block_crypto_ctx* ctx, int show_keys, char* out, size_t out_size) {
    dsd_test_capture_stderr cap;
    if (out == NULL || out_size == 0U) {
        return 1;
    }
    out[0] = '\0';
    if (dsd_test_capture_stderr_begin(&cap, "dsdneo_dmr_block_crypto") != 0) {
        DSD_FPRINTF(stderr, "capture stderr begin failed\n");
        return 1;
    }

    dmr_block_crypto_print_info(ctx, show_keys);

    if (dsd_test_capture_stderr_end(&cap) != 0) {
        DSD_FPRINTF(stderr, "capture stderr end failed\n");
        return 1;
    }

    FILE* fp = fopen(cap.path, "rb");
    if (fp == NULL) {
        DSD_FPRINTF(stderr, "capture read failed\n");
        (void)remove(cap.path);
        return 1;
    }
    size_t nread = fread(out, 1U, out_size - 1U, fp);
    out[nread] = '\0';
    (void)fclose(fp);
    (void)remove(cap.path);
    return 0;
}

static void
seed_window(dsd_state* state, uint8_t slot, uint8_t start, uint8_t poc) {
    DSD_MEMSET(state, 0, sizeof(*state));
    state->currentslot = slot;
    state->data_ks_start[slot] = start;
    state->data_block_poc[slot] = poc;
    state->data_byte_ctr[slot] = 48;
}

static void
seed_key_array(dsd_state* state, int kid, unsigned long long k1, unsigned long long k2, unsigned long long k3,
               unsigned long long k4) {
    state->rkey_array[kid + 0x000] = k1;
    state->rkey_array[kid + 0x101] = k2;
    state->rkey_array[kid + 0x201] = k3;
    state->rkey_array[kid + 0x301] = k4;
}

static void
fill_pattern(uint8_t* out, size_t len, uint8_t seed) {
    for (size_t i = 0; i < len; i++) {
        out[i] = (uint8_t)(seed + (uint8_t)(i * 3U));
    }
}

static void
xor_stream_into_payload(dsd_state* state, uint8_t slot, int start, const uint8_t* plaintext, const uint8_t* stream,
                        size_t len) {
    for (size_t i = 0; i < len; i++) {
        state->dmr_pdu_sf[slot][start + (int)i] = (uint8_t)(plaintext[i] ^ stream[i]);
    }
}

static int
test_print_info_reveals_full_aes128_key(void) {
    static dsd_state state;
    dmr_block_crypto_ctx ctx;
    char output[256];
    const uint8_t slot = 0;
    const int kid = 0x23;
    int rc = 0;

    seed_window(&state, slot, 0, 28);
    state.payload_algid = 4;
    state.payload_keyid = kid;
    seed_key_array(&state, kid, 0x0123456789ABCDEFULL, 0ULL, 0xDEADBEEFDEADBEEFULL, 0ULL);

    dmr_block_crypto_load_ctx(&state, slot, 1, 24, &ctx);
    rc |= expect_int("aes128 print key loaded", ctx.aes_key_loaded, 1);
    if (capture_print_info(&ctx, 1, output, sizeof output) != 0) {
        return 1;
    }
    rc |= expect_contains("aes128 print full key", output, "AES128; Key: 0123456789ABCDEF0000000000000000;");
    return rc;
}

static int
test_print_info_reveals_full_aes256_key_when_first_segment_zero(void) {
    static dsd_state state;
    dmr_block_crypto_ctx ctx;
    char output[256];
    const uint8_t slot = 1;
    const int kid = 0x24;
    int rc = 0;

    seed_window(&state, slot, 0, 28);
    state.payload_algidR = 5;
    state.payload_keyidR = kid;
    seed_key_array(&state, kid, 0ULL, 0x1112131415161718ULL, 0x2122232425262728ULL, 0x3132333435363738ULL);

    dmr_block_crypto_load_ctx(&state, slot, 1, 24, &ctx);
    rc |= expect_int("aes256 print rkey zero", ctx.rkey == 0ULL, 1);
    rc |= expect_int("aes256 print key loaded", ctx.aes_key_loaded, 1);
    if (capture_print_info(&ctx, 1, output, sizeof output) != 0) {
        return 1;
    }
    rc |= expect_contains("aes256 print full key", output,
                          "AES256; Key: 0000000000000000111213141516171821222324252627283132333435363738;");
    return rc;
}

static int
test_aes128_zero_mi_uses_reference_ecb_block_count(void) {
    static const uint8_t ciphertext[16] = {0x69, 0xC4, 0xE0, 0xD8, 0x6A, 0x7B, 0x04, 0x30,
                                           0xD8, 0xCD, 0xB7, 0x80, 0x70, 0xB4, 0xC5, 0x5A};
    static const uint8_t plaintext[16] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
                                          0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    static dsd_state state;
    dmr_block_crypto_ctx ctx;
    const uint8_t slot = 0;
    const uint8_t start = 3;
    const int kid = 0x22;
    int rc = 0;

    seed_window(&state, slot, start, 6);
    state.payload_algid = 4;
    state.payload_keyid = kid;
    state.payload_mi = 0;
    seed_key_array(&state, kid, 0x0001020304050607ULL, 0x08090A0B0C0D0E0FULL, 0, 0);

    state.dmr_pdu_sf[slot][0] = 0xE0;
    state.dmr_pdu_sf[slot][1] = 0xE1;
    state.dmr_pdu_sf[slot][2] = 0xE2;
    DSD_MEMCPY(state.dmr_pdu_sf[slot] + start, ciphertext, sizeof(ciphertext));
    DSD_MEMCPY(state.dmr_pdu_sf[slot] + start + 16, ciphertext, sizeof(ciphertext));
    DSD_MEMCPY(state.dmr_pdu_sf[slot] + start + 32, ciphertext, sizeof(ciphertext));
    state.dmr_pdu_sf[slot][start + 48] = 0xA0;

    g_lfsr_calls = 0;
    dmr_block_crypto_load_ctx(&state, slot, 1, 24, &ctx);
    rc |= expect_int("aes128 ctx start", ctx.start, start);
    rc |= expect_int("aes128 ctx end", ctx.end, 35);
    rc |= expect_int("aes128 key loaded", ctx.aes_key_loaded, 1);
    rc |= expect_int("aes128 decrypt result", dmr_block_crypto_decrypt_payload(&state, slot, &ctx, 0), 1);
    rc |= expect_int("aes128 zero-mi skips lfsr", g_lfsr_calls, 0);
    rc |= expect_int("aes128 alg normalized", state.payload_algid, 0x24);
    rc |= expect_u8("aes128 prefix byte 0", state.dmr_pdu_sf[slot][0], 0xE0);
    rc |= expect_u8("aes128 prefix byte 1", state.dmr_pdu_sf[slot][1], 0xE1);
    rc |= expect_u8("aes128 prefix byte 2", state.dmr_pdu_sf[slot][2], 0xE2);
    rc |= expect_bytes("aes128 block 0", state.dmr_pdu_sf[slot] + start, plaintext, sizeof(plaintext));
    rc |= expect_bytes("aes128 block 1", state.dmr_pdu_sf[slot] + start + 16, plaintext, sizeof(plaintext));
    rc |= expect_bytes("aes128 block 2", state.dmr_pdu_sf[slot] + start + 32, plaintext, sizeof(plaintext));
    rc |= expect_u8("aes128 trailing byte", state.dmr_pdu_sf[slot][start + 48], 0xA0);
    return rc;
}

static int
test_aes256_zero_mi_uses_reference_ecb_block_count_and_manual_key_fallback(void) {
    static const uint8_t ciphertext[16] = {0x8E, 0xA2, 0xB7, 0xCA, 0x51, 0x67, 0x45, 0xBF,
                                           0xEA, 0xFC, 0x49, 0x90, 0x4B, 0x49, 0x60, 0x89};
    static const uint8_t plaintext[16] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
                                          0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    static dsd_state state;
    dmr_block_crypto_ctx ctx;
    const uint8_t slot = 1;
    const uint8_t start = 3;
    int rc = 0;

    seed_window(&state, slot, start, 6);
    state.payload_algidR = 5;
    state.payload_keyidR = 0x7A;
    state.payload_miR = 0;
    state.K1 = 0x0001020304050607ULL;
    state.K2 = 0x08090A0B0C0D0E0FULL;
    state.K3 = 0x1011121314151617ULL;
    state.K4 = 0x18191A1B1C1D1E1FULL;
    DSD_MEMCPY(state.dmr_pdu_sf[slot] + start, ciphertext, sizeof(ciphertext));
    DSD_MEMCPY(state.dmr_pdu_sf[slot] + start + 16, ciphertext, sizeof(ciphertext));
    DSD_MEMCPY(state.dmr_pdu_sf[slot] + start + 32, ciphertext, sizeof(ciphertext));
    state.dmr_pdu_sf[slot][start + 48] = 0xB0;

    g_lfsr_calls = 0;
    dmr_block_crypto_load_ctx(&state, slot, 1, 24, &ctx);
    rc |= expect_int("aes256 manual key loaded", ctx.aes_key_loaded, 1);
    rc |= expect_int("aes256 decrypt result", dmr_block_crypto_decrypt_payload(&state, slot, &ctx, 0), 1);
    rc |= expect_int("aes256 zero-mi skips lfsr", g_lfsr_calls, 0);
    rc |= expect_int("aes256 alg normalized", state.payload_algidR, 0x25);
    rc |= expect_bytes("aes256 block 0", state.dmr_pdu_sf[slot] + start, plaintext, sizeof(plaintext));
    rc |= expect_bytes("aes256 block 1", state.dmr_pdu_sf[slot] + start + 16, plaintext, sizeof(plaintext));
    rc |= expect_bytes("aes256 block 2", state.dmr_pdu_sf[slot] + start + 32, plaintext, sizeof(plaintext));
    rc |= expect_u8("aes256 trailing byte", state.dmr_pdu_sf[slot][start + 48], 0xB0);
    return rc;
}

static int
test_aes_nonzero_mi_keeps_ofb_path(void) {
    uint8_t iv[16];
    uint8_t stream[48];
    static dsd_state state;
    dmr_block_crypto_ctx ctx;
    const uint8_t slot = 0;
    const int kid = 0x33;
    int rc = 0;

    seed_window(&state, slot, 0, 28);
    state.payload_algid = 4;
    state.payload_keyid = kid;
    state.payload_mi = 0x11223344ULL;
    seed_key_array(&state, kid, 0x0001020304050607ULL, 0x08090A0B0C0D0E0FULL, 0, 0);

    g_lfsr_calls = 0;
    dmr_block_crypto_load_ctx(&state, slot, 1, 24, &ctx);
    for (int i = 0; i < 16; i++) {
        iv[i] = (uint8_t)i;
    }
    DSD_MEMSET(stream, 0, sizeof(stream));
    aes_ofb_keystream_output(iv, ctx.aes_key, stream, /*AES-128*/ 0, 3);

    rc |= expect_int("aes ofb ctx end", ctx.end, 16);
    rc |= expect_int("aes ofb decrypt result", dmr_block_crypto_decrypt_payload(&state, slot, &ctx, 0), 1);
    rc |= expect_int("aes nonzero-mi calls lfsr", g_lfsr_calls, 1);
    rc |= expect_int("aes ofb alg normalized", state.payload_algid, 0x24);
    rc |= expect_bytes("aes ofb skips discard block", state.dmr_pdu_sf[slot], stream + 16, 16);
    return rc;
}

static int
test_rc4_decrypts_window_with_key_id_lookup(void) {
    static dsd_state state;
    dmr_block_crypto_ctx ctx;
    uint8_t plaintext[32];
    uint8_t stream[32];
    const uint8_t slot = 0;
    const uint8_t start = 2;
    const int kid = 0x44;
    int rc = 0;

    seed_window(&state, slot, start, 14);
    state.payload_algid = 1;
    state.payload_keyid = kid;
    state.payload_mi = 0x01020304ULL;
    state.rkey_array[kid] = 0x0123456789ULL;

    dmr_block_crypto_load_ctx(&state, slot, 1, 24, &ctx);
    rc |= expect_int("rc4 ctx end", ctx.end, 28);
    fill_pattern(plaintext, (size_t)ctx.end, 0x31);
    DSD_MEMSET(stream, 0, sizeof(stream));
    rc4_block_output(256, 9, ctx.end, ctx.rc4_iv, stream);
    xor_stream_into_payload(&state, slot, ctx.start, plaintext, stream, (size_t)ctx.end);

    rc |= expect_int("rc4 decrypt result", dmr_block_crypto_decrypt_payload(&state, slot, &ctx, 0), 1);
    rc |= expect_bytes("rc4 plaintext", state.dmr_pdu_sf[slot] + ctx.start, plaintext, (size_t)ctx.end);
    return rc;
}

static int
test_des_decrypts_window_with_manual_key_fallback(void) {
    static dsd_state state;
    dmr_block_crypto_ctx ctx;
    uint8_t plaintext[32];
    uint8_t stream[32];
    const uint8_t slot = 1;
    const uint8_t start = 1;
    int rc = 0;

    seed_window(&state, slot, start, 19);
    state.payload_algidR = 2;
    state.payload_keyidR = 0x77;
    state.payload_miR = 0x11223344ULL;
    state.R = 0x0123456789ABCDEFULL;

    dmr_block_crypto_load_ctx(&state, slot, 1, 24, &ctx);
    rc |= expect_int("des ctx end", ctx.end, 24);
    rc |= expect_int("des manual key fallback", ctx.rkey != 0ULL, 1);
    fill_pattern(plaintext, (size_t)ctx.end, 0x52);
    DSD_MEMSET(stream, 0, sizeof(stream));
    des_multi_keystream_output(ctx.mi, ctx.rkey, stream, 1, (ctx.end / 8) + 1);
    xor_stream_into_payload(&state, slot, ctx.start, plaintext, stream, (size_t)ctx.end);

    rc |= expect_int("des decrypt result", dmr_block_crypto_decrypt_payload(&state, slot, &ctx, 0), 1);
    rc |= expect_bytes("des plaintext", state.dmr_pdu_sf[slot] + ctx.start, plaintext, (size_t)ctx.end);
    return rc;
}

static int
test_basic_privacy_decrypts_window(void) {
    static dsd_state state;
    dmr_block_crypto_ctx ctx;
    uint8_t plaintext[32];
    uint8_t stream[2];
    const uint8_t slot = 0;
    const uint8_t start = 4;
    int rc = 0;

    seed_window(&state, slot, start, 24);
    state.K = 1;
    stream[0] = (uint8_t)((BPK[state.K] >> 8U) & 0xFFU);
    stream[1] = (uint8_t)(BPK[state.K] & 0xFFU);

    dmr_block_crypto_load_ctx(&state, slot, 1, 24, &ctx);
    rc |= expect_int("bp ctx end", ctx.end, 16);
    fill_pattern(plaintext, (size_t)ctx.end, 0x73);
    for (int i = 0; i < ctx.end; i++) {
        state.dmr_pdu_sf[slot][ctx.start + i] = (uint8_t)(plaintext[i] ^ stream[i % 2]);
    }

    rc |= expect_int("bp decrypt result", dmr_block_crypto_decrypt_payload(&state, slot, &ctx, 0), 1);
    rc |= expect_bytes("bp plaintext", state.dmr_pdu_sf[slot] + ctx.start, plaintext, (size_t)ctx.end);
    return rc;
}

static int
test_aes_missing_key_still_normalizes_alg(void) {
    static dsd_state state;
    dmr_block_crypto_ctx ctx;
    const uint8_t slot = 1;
    int rc = 0;

    seed_window(&state, slot, 0, 28);
    state.payload_algidR = 5;
    state.payload_keyidR = 0x55;
    state.payload_miR = 0x01020304ULL;

    dmr_block_crypto_load_ctx(&state, slot, 1, 24, &ctx);
    rc |= expect_int("aes missing key not loaded", ctx.aes_key_loaded, 0);
    rc |= expect_int("aes missing key result", dmr_block_crypto_decrypt_payload(&state, slot, &ctx, 0), 0);
    rc |= expect_int("aes missing key alg normalized", state.payload_algidR, 0x25);
    return rc;
}

int
main(void) {
    int rc = 0;
    rc |= test_aes128_zero_mi_uses_reference_ecb_block_count();
    rc |= test_aes256_zero_mi_uses_reference_ecb_block_count_and_manual_key_fallback();
    rc |= test_aes_nonzero_mi_keeps_ofb_path();
    rc |= test_print_info_reveals_full_aes128_key();
    rc |= test_print_info_reveals_full_aes256_key_when_first_segment_zero();
    rc |= test_rc4_decrypts_window_with_key_id_lookup();
    rc |= test_des_decrypts_window_with_manual_key_fallback();
    rc |= test_basic_privacy_decrypts_window();
    rc |= test_aes_missing_key_still_normalizes_alg();
    return rc;
}
