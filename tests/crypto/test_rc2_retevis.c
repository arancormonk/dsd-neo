// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/state.h>
#include <dsd-neo/crypto/dmr_keystream.h>
#include <dsd-neo/crypto/rc2.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

static int
expect_int(const char* label, int got, int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: expected %d, got %d\n", label, want, got);
        return 1;
    }
    return 0;
}

static int
expect_bits_string(const char* label, const uint8_t* bits, const char* want) {
    for (size_t i = 0; want[i] != '\0'; i++) {
        int got = bits[i] & 1;
        int expected = want[i] - '0';
        if (got != expected) {
            DSD_FPRINTF(stderr, "%s: bit %zu expected %d, got %d\n", label, i, expected, got);
            return 1;
        }
    }
    return 0;
}

static int
expect_char_frame(const char* label, const char got[49], const char want[49]) {
    for (int i = 0; i < 49; i++) {
        int got_bit = ((unsigned char)got[i]) & 1U;
        int want_bit = ((unsigned char)want[i]) & 1U;
        if (got_bit != want_bit) {
            DSD_FPRINTF(stderr, "%s: bit %d expected %d, got %d\n", label, i, want_bit, got_bit);
            return 1;
        }
    }
    return 0;
}

static int
expect_char_bits(const char* label, const char got[49], const uint8_t want[49]) {
    for (int i = 0; i < 49; i++) {
        int got_bit = ((unsigned char)got[i]) & 1U;
        int want_bit = want[i] & 1U;
        if (got_bit != want_bit) {
            DSD_FPRINTF(stderr, "%s: bit %d expected %d, got %d\n", label, i, want_bit, got_bit);
            return 1;
        }
    }
    return 0;
}

static int
expect_rc2_schedule(const char* label, const CryptoContext* got, const CryptoContext* want) {
    if (got == NULL) {
        DSD_FPRINTF(stderr, "%s: missing context\n", label);
        return 1;
    }
    if (got->internal_zero != want->internal_zero || memcmp(got->keys, want->keys, sizeof(want->keys)) != 0
        || memcmp(got->rc2.xkey, want->rc2.xkey, sizeof(want->rc2.xkey)) != 0) {
        DSD_FPRINTF(stderr, "%s: schedule mismatch\n", label);
        return 1;
    }
    return 0;
}

static void
fill_default_silence(char frame[49]) {
    static const uint64_t k_ambe_default_silence = 0xF801A99F8CE080ULL;
    for (int i = 0; i < 49; i++) {
        frame[i] = (char)((k_ambe_default_silence >> (55 - i)) & 1U);
    }
}

static void
u64_to_bytes_be_local(uint64_t val, unsigned char* out) {
    for (int i = 0; i < 8; i++) {
        out[i] = (unsigned char)((val >> (56 - 8 * i)) & 0xFF);
    }
}

static void
build_rc2_128_key(uint64_t k1, uint64_t k2, unsigned char key2[16]) {
    unsigned char key1[16];
    DSD_MEMSET(key1, 0, sizeof(key1));
    u64_to_bytes_be_local(k1, &key1[0]);
    u64_to_bytes_be_local(k2, &key1[8]);

    for (int i = 0; i < 16; i++) {
        key2[i] = key1[15 - i];
    }
}

static void
append_ascii_hex_chunk(uint64_t value, unsigned char* out) {
    for (int i = 0; i < 16; i++) {
        uint8_t nibble = (uint8_t)((value >> (60 - (i * 4))) & 0xFU);
        out[i] = (unsigned char)(nibble <= 9 ? (nibble + 0x30U) : (nibble + 0x37U));
    }
}

static int
test_rc2_decrypt_frame_vector(void) {
    static const char expect[] = "1101011111000001010101011000010100000011000100000";
    unsigned char key[16];
    DSD_MEMSET(key, 0, sizeof(key));
    build_rc2_128_key(0x736B9A9C5645288BULL, 0x243AD5CB8701EF8AULL, key);

    CryptoContext ctx;
    DSD_MEMSET(&ctx, 0, sizeof(ctx));
    create_keys_rc2(&ctx, key, sizeof(key));

    uint8_t bits[49];
    for (int i = 0; i < 49; i++) {
        bits[i] = (uint8_t)((i * 5 + 1) & 1);
    }
    decrypt_rc2(&ctx, bits);

    return expect_bits_string("rc2 decrypt vector", bits, expect);
}

static int
test_retevis_128_key_schedule(void) {
    CryptoContext expected;
    DSD_MEMSET(&expected, 0, sizeof(expected));
    unsigned char key[16];
    DSD_MEMSET(key, 0, sizeof(key));
    build_rc2_128_key(0x736B9A9C5645288BULL, 0x243AD5CB8701EF8AULL, key);
    create_keys_rc2(&expected, key, sizeof(key));

    static dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));
    char input[] = "736B9A9C5645288B 243AD5CB8701EF8A";
    retevis_rc2_keystream_creation(&state, input, 0);

    int rc = 0;
    rc |= expect_int("retevis 128 flag", state.retevis_ap, 1);
    rc |= expect_rc2_schedule("retevis 128", (const CryptoContext*)state.rc2_context, &expected);
    free(state.rc2_context);
    return rc;
}

static int
test_retevis_256_key_schedule(void) {
    static const uint64_t chunks[4] = {0x1122334455667788ULL, 0x99AABBCCDDEEFF11ULL, 0x1122334455667788ULL,
                                       0x99AABBCCDDEEFF11ULL};
    CryptoContext expected;
    DSD_MEMSET(&expected, 0, sizeof(expected));
    unsigned char key[64];
    DSD_MEMSET(key, 0, sizeof(key));
    for (size_t i = 0; i < 4U; i++) {
        append_ascii_hex_chunk(chunks[i], key + (i * 16U));
    }
    create_keys_rc2(&expected, key, sizeof(key));

    static dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));
    char input[] = "1122334455667788 99AABBCCDDEEFF11 1122334455667788 99AABBCCDDEEFF11";
    retevis_rc2_keystream_creation(&state, input, 0);

    int rc = 0;
    rc |= expect_int("retevis 256 flag", state.retevis_ap, 1);
    rc |= expect_rc2_schedule("retevis 256", (const CryptoContext*)state.rc2_context, &expected);
    free(state.rc2_context);
    return rc;
}

static int
test_retevis_256_trailing_zero_chunks_key_schedule(void) {
    static const uint64_t chunks[4] = {0x1122334455667788ULL, 0x99AABBCCDDEEFF11ULL, 0x0000000000000000ULL,
                                       0x0000000000000000ULL};
    CryptoContext expected;
    DSD_MEMSET(&expected, 0, sizeof(expected));
    unsigned char key[64];
    DSD_MEMSET(key, 0, sizeof(key));
    for (size_t i = 0; i < 4U; i++) {
        append_ascii_hex_chunk(chunks[i], key + (i * 16U));
    }
    create_keys_rc2(&expected, key, sizeof(key));

    static dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));
    char input[] = "1122334455667788 99AABBCCDDEEFF11 0000000000000000 0000000000000000";
    retevis_rc2_keystream_creation(&state, input, 0);

    int rc = 0;
    rc |= expect_int("retevis 256 zero chunks flag", state.retevis_ap, 1);
    rc |= expect_rc2_schedule("retevis 256 zero chunks", (const CryptoContext*)state.rc2_context, &expected);
    free(state.rc2_context);
    return rc;
}

static int
test_retevis_rejects_malformed_keys(void) {
    int rc = 0;

    static dsd_state bad_char_state;
    DSD_MEMSET(&bad_char_state, 0, sizeof(bad_char_state));
    bad_char_state.retevis_ap = 1;
    bad_char_state.rc2_context = malloc(sizeof(CryptoContext));
    if (bad_char_state.rc2_context == NULL) {
        DSD_FPRINTF(stderr, "allocation failed\n");
        return 1;
    }
    char bad_char_input[] = "736B9A9C5645288B 243AD5CB8701EF8Z";
    retevis_rc2_keystream_creation(&bad_char_state, bad_char_input, 0);
    rc |= expect_int("retevis bad hex disables flag", bad_char_state.retevis_ap, 0);
    rc |= expect_int("retevis bad hex frees context", bad_char_state.rc2_context == NULL, 1);

    static dsd_state bad_length_state;
    DSD_MEMSET(&bad_length_state, 0, sizeof(bad_length_state));
    bad_length_state.retevis_ap = 1;
    bad_length_state.rc2_context = malloc(sizeof(CryptoContext));
    if (bad_length_state.rc2_context == NULL) {
        DSD_FPRINTF(stderr, "allocation failed\n");
        return 1;
    }
    char bad_length_input[] = "736B9A9C5645288B";
    retevis_rc2_keystream_creation(&bad_length_state, bad_length_input, 0);
    rc |= expect_int("retevis bad length disables flag", bad_length_state.retevis_ap, 0);
    rc |= expect_int("retevis bad length frees context", bad_length_state.rc2_context == NULL, 1);

    return rc;
}

static int
test_retevis_apply_skips_silence_and_zero_tail(void) {
    static dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));
    char input[] = "736B9A9C5645288B 243AD5CB8701EF8A";
    retevis_rc2_keystream_creation(&state, input, 0);

    int rc = 0;
    char silence[49];
    fill_default_silence(silence);
    char original_silence[49];
    DSD_MEMCPY(original_silence, silence, sizeof(original_silence));
    rc |= expect_int("retevis skip silence applied", retevis_rc2_apply_frame49(&state, silence), 0);
    rc |= expect_char_frame("retevis skip silence frame", silence, original_silence);

    char zero_tail[49] = {0};
    for (int i = 0; i < 24; i++) {
        zero_tail[i] = (char)((i + 1) & 1);
    }
    for (int i = 44; i < 49; i++) {
        zero_tail[i] = (char)(i & 1);
    }
    char original_zero_tail[49];
    DSD_MEMCPY(original_zero_tail, zero_tail, sizeof(original_zero_tail));
    rc |= expect_int("retevis skip zero-tail applied", retevis_rc2_apply_frame49(&state, zero_tail), 0);
    rc |= expect_char_frame("retevis skip zero-tail frame", zero_tail, original_zero_tail);

    free(state.rc2_context);
    return rc;
}

static int
test_retevis_apply_decrypts_voice_frame(void) {
    static dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));
    char input[] = "736B9A9C5645288B 243AD5CB8701EF8A";
    retevis_rc2_keystream_creation(&state, input, 0);

    char frame[49];
    uint8_t expected[49];
    for (int i = 0; i < 49; i++) {
        frame[i] = (char)((i * 5 + 1) & 1);
        expected[i] = (uint8_t)(frame[i] & 1);
    }

    CryptoContext expected_ctx = *(CryptoContext*)state.rc2_context;
    decrypt_rc2(&expected_ctx, expected);

    int rc = 0;
    rc |= expect_int("retevis apply voice frame", retevis_rc2_apply_frame49(&state, frame), 1);
    rc |= expect_char_bits("retevis apply voice bits", frame, expected);

    free(state.rc2_context);
    return rc;
}

int
main(void) {
    int rc = 0;
    rc |= test_rc2_decrypt_frame_vector();
    rc |= test_retevis_128_key_schedule();
    rc |= test_retevis_256_key_schedule();
    rc |= test_retevis_256_trailing_zero_chunks_key_schedule();
    rc |= test_retevis_rejects_malformed_keys();
    rc |= test_retevis_apply_skips_silence_and_zero_tail();
    rc |= test_retevis_apply_decrypts_voice_frame();
    return rc;
}
