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

    dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));
    char input[] = "736B9A9C5645288B 243AD5CB8701EF8A";
    retevis_rc2_keystream_creation(&state, input);

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

    dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));
    char input[] = "1122334455667788 99AABBCCDDEEFF11 1122334455667788 99AABBCCDDEEFF11";
    retevis_rc2_keystream_creation(&state, input);

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

    dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));
    char input[] = "1122334455667788 99AABBCCDDEEFF11 0000000000000000 0000000000000000";
    retevis_rc2_keystream_creation(&state, input);

    int rc = 0;
    rc |= expect_int("retevis 256 zero chunks flag", state.retevis_ap, 1);
    rc |= expect_rc2_schedule("retevis 256 zero chunks", (const CryptoContext*)state.rc2_context, &expected);
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
    return rc;
}
