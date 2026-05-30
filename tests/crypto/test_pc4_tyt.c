// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/bit_packing.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/crypto/dmr_keystream.h>
#include <dsd-neo/crypto/pc4.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

uint64_t
convert_bits_into_output(const uint8_t* input, int len) {
    uint64_t value = 0;
    for (int i = 0; i < len; i++) {
        value = (value << 1) | (uint64_t)(input[i] & 1U);
    }
    return value;
}

void
pack_bit_array_into_byte_array(const uint8_t* input, uint8_t* output, int len) {
    for (int i = 0; i < len; i++) {
        output[i] = (uint8_t)convert_bits_into_output(input + ((size_t)i * 8U), 8);
    }
}

void
unpack_byte_array_into_bit_array(const uint8_t* input, uint8_t* output, int len) {
    for (int i = 0; i < len; i++) {
        for (int bit = 0; bit < 8; bit++) {
            output[(i * 8) + bit] = (uint8_t)((input[i] >> (7 - bit)) & 1U);
        }
    }
}

static int
expect_int(const char* label, int got, int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: expected %d, got %d\n", label, want, got);
        return 1;
    }
    return 0;
}

static int
expect_bits_string(const char* label, const short* bits, const char* want) {
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
expect_pc4_schedule(const char* label, const PC4Context* got, const PC4Context* want) {
    if (got->rounds != want->rounds || memcmp(got->keys, want->keys, sizeof(want->keys)) != 0
        || memcmp(got->perm, want->perm, sizeof(want->perm)) != 0
        || memcmp(got->new1, want->new1, sizeof(want->new1)) != 0
        || memcmp(got->array, want->array, sizeof(want->array)) != 0
        || memcmp(got->array2, want->array2, sizeof(want->array2)) != 0
        || memcmp(got->decal, want->decal, sizeof(want->decal)) != 0
        || memcmp(got->rngxor, want->rngxor, sizeof(want->rngxor)) != 0
        || memcmp(got->rngxor2, want->rngxor2, sizeof(want->rngxor2)) != 0
        || memcmp(got->tab, want->tab, sizeof(want->tab)) != 0 || memcmp(got->inv, want->inv, sizeof(want->inv)) != 0
        || memcmp(got->permut, want->permut, sizeof(want->permut)) != 0) {
        DSD_FPRINTF(stderr, "%s: schedule mismatch\n", label);
        return 1;
    }
    return 0;
}

static void
build_pc4_128_key(uint64_t k1, uint64_t k2, unsigned char key2[16]) {
    unsigned char key1[16];
    DSD_MEMSET(key1, 0, sizeof(key1));
    u64_to_bytes_be(k1, &key1[0]);
    u64_to_bytes_be(k2, &key1[8]);

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
test_pc4_decrypt_frame_vector(void) {
    static const char expect[] = "1001100011110001111101001011001100111110001000101";
    unsigned char key[16];
    DSD_MEMSET(key, 0, sizeof(key));
    build_pc4_128_key(0x736B9A9C5645288BULL, 0x243AD5CB8701EF8AULL, key);

    DSD_MEMSET(&g_pc4_context, 0, sizeof(g_pc4_context));
    create_keys(&g_pc4_context, key, sizeof(key));
    g_pc4_context.rounds = nbround;

    short frame[49];
    for (int i = 0; i < 49; i++) {
        frame[i] = (short)((i * 3 + 1) & 1);
    }
    decrypt_frame_49(frame);

    return expect_bits_string("pc4 decrypt vector", g_pc4_context.bits, expect);
}

static int
test_tyt_ap_128_key_schedule(void) {
    PC4Context expected;
    DSD_MEMSET(&expected, 0, sizeof(expected));
    unsigned char key[16];
    DSD_MEMSET(key, 0, sizeof(key));
    build_pc4_128_key(0x736B9A9C5645288BULL, 0x243AD5CB8701EF8AULL, key);
    create_keys(&expected, key, sizeof(key));
    expected.rounds = nbround;

    dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMSET(&g_pc4_context, 0, sizeof(g_pc4_context));
    char input[] = "736B9A9C5645288B 243AD5CB8701EF8A";
    tyt_ap_pc4_keystream_creation(&state, input);

    int rc = 0;
    rc |= expect_int("tyt ap 128 flag", state.tyt_ap, 1);
    rc |= expect_pc4_schedule("tyt ap 128", &g_pc4_context, &expected);
    return rc;
}

static int
test_tyt_ap_256_key_schedule(void) {
    static const uint64_t chunks[4] = {0x0123456789ABCDEFULL, 0xFEDCBA9876543210ULL, 0x1111222233334444ULL,
                                       0x5555666677778888ULL};
    PC4Context expected;
    DSD_MEMSET(&expected, 0, sizeof(expected));
    unsigned char key[64];
    DSD_MEMSET(key, 0, sizeof(key));
    for (size_t i = 0; i < 4U; i++) {
        append_ascii_hex_chunk(chunks[i], key + (i * 16U));
    }
    create_keys(&expected, key, sizeof(key));
    expected.rounds = nbround;

    dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMSET(&g_pc4_context, 0, sizeof(g_pc4_context));
    char input[] = "0123456789ABCDEF FEDCBA9876543210 1111222233334444 5555666677778888";
    tyt_ap_pc4_keystream_creation(&state, input);

    int rc = 0;
    rc |= expect_int("tyt ap 256 flag", state.tyt_ap, 1);
    rc |= expect_pc4_schedule("tyt ap 256", &g_pc4_context, &expected);
    return rc;
}

static int
test_tyt_ap_256_trailing_zero_chunks_key_schedule(void) {
    static const uint64_t chunks[4] = {0x0123456789ABCDEFULL, 0xFEDCBA9876543210ULL, 0x0000000000000000ULL,
                                       0x0000000000000000ULL};
    PC4Context expected;
    DSD_MEMSET(&expected, 0, sizeof(expected));
    unsigned char key[64];
    DSD_MEMSET(key, 0, sizeof(key));
    for (size_t i = 0; i < 4U; i++) {
        append_ascii_hex_chunk(chunks[i], key + (i * 16U));
    }
    create_keys(&expected, key, sizeof(key));
    expected.rounds = nbround;

    dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMSET(&g_pc4_context, 0, sizeof(g_pc4_context));
    char input[] = "0123456789ABCDEF FEDCBA9876543210 0000000000000000 0000000000000000";
    tyt_ap_pc4_keystream_creation(&state, input);

    int rc = 0;
    rc |= expect_int("tyt ap 256 zero chunks flag", state.tyt_ap, 1);
    rc |= expect_pc4_schedule("tyt ap 256 zero chunks", &g_pc4_context, &expected);
    return rc;
}

static int
test_tyt_ep_aes_vector(void) {
    static const char expect[] = "0001100011000000101000001110110101000101101011001";
    dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMSET(&g_pc4_context, 0, sizeof(g_pc4_context));
    char input[] = "736B9A9C5645288B 243AD5CB8701EF8A";

    tyt_ep_aes_keystream_creation(&state, input);

    int rc = 0;
    rc |= expect_int("tyt ep flag", state.tyt_ep, 1);
    rc |= expect_bits_string("tyt ep aes", g_pc4_context.bits, expect);
    return rc;
}

int
main(void) {
    int rc = 0;
    rc |= test_pc4_decrypt_frame_vector();
    rc |= test_tyt_ap_128_key_schedule();
    rc |= test_tyt_ap_256_key_schedule();
    rc |= test_tyt_ap_256_trailing_zero_chunks_key_schedule();
    rc |= test_tyt_ep_aes_vector();
    return rc;
}
