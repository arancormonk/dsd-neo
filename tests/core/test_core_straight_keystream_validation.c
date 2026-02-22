// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/state.h>
#include <dsd-neo/crypto/dmr_keystream.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dsd-neo/core/state_fwd.h"

static int
expect_eq_int(const char* tag, int got, int want) {
    if (got != want) {
        fprintf(stderr, "%s: got %d want %d\n", tag, got, want);
        return 1;
    }
    return 0;
}

static int
expect_eq_u8(const char* tag, unsigned got, unsigned want) {
    if (got != want) {
        fprintf(stderr, "%s: got 0x%02X want 0x%02X\n", tag, got, want);
        return 1;
    }
    return 0;
}

static unsigned
bits_to_u8(const uint8_t* bits, int start) {
    unsigned v = 0U;
    for (int i = 0; i < 8; i++) {
        v = (v << 1) | (unsigned)(bits[start + i] & 1U);
    }
    return v;
}

/*
 * Provide a local parser stub required by straight_mod_xor_keystream_creation.
 * This test only needs uppercase/lowercase contiguous hex parsing.
 */
uint16_t
parse_raw_user_string(char* input, uint8_t* output, size_t out_cap) {
    if (!input || !output || out_cap == 0) {
        return 0;
    }
    size_t in_len = strlen(input);
    size_t out_idx = 0;
    size_t i = 0;
    while (i < in_len && out_idx < out_cap) {
        char hi = input[i++];
        char lo = (i < in_len) ? input[i++] : '0';
        char oct[3] = {hi, lo, '\0'};
        output[out_idx++] = (uint8_t)strtoul(oct, NULL, 16);
    }
    if ((in_len & 1U) != 0U && out_idx > 0) {
        output[out_idx - 1] = (uint8_t)(output[out_idx - 1] << 4);
    }
    return (uint16_t)out_idx;
}

void
unpack_byte_array_into_bit_array(uint8_t* input, uint8_t* output, int len) {
    int k = 0;
    for (int i = 0; i < len; i++) {
        output[k++] = (uint8_t)((input[i] >> 7) & 1U);
        output[k++] = (uint8_t)((input[i] >> 6) & 1U);
        output[k++] = (uint8_t)((input[i] >> 5) & 1U);
        output[k++] = (uint8_t)((input[i] >> 4) & 1U);
        output[k++] = (uint8_t)((input[i] >> 3) & 1U);
        output[k++] = (uint8_t)((input[i] >> 2) & 1U);
        output[k++] = (uint8_t)((input[i] >> 1) & 1U);
        output[k++] = (uint8_t)((input[i] >> 0) & 1U);
    }
}

int
main(void) {
    int rc = 0;
    dsd_state* st = (dsd_state*)calloc(1, sizeof(*st));
    if (!st) {
        fprintf(stderr, "allocation failed\n");
        return 1;
    }

    st->straight_ks = 1;
    st->straight_mod = 77;
    {
        char arg[] = "0:AA";
        straight_mod_xor_keystream_creation(st, arg);
        rc |= expect_eq_int("len-zero-disabled", st->straight_ks, 0);
        rc |= expect_eq_int("len-zero-mod", st->straight_mod, 0);
    }

    st->straight_ks = 1;
    st->straight_mod = 55;
    {
        char arg[] = "999:AA";
        straight_mod_xor_keystream_creation(st, arg);
        rc |= expect_eq_int("len-too-large-disabled", st->straight_ks, 0);
        rc |= expect_eq_int("len-too-large-mod", st->straight_mod, 0);
    }

    st->straight_ks = 1;
    st->straight_mod = 11;
    {
        char arg[] = "49";
        straight_mod_xor_keystream_creation(st, arg);
        rc |= expect_eq_int("malformed-disabled", st->straight_ks, 0);
        rc |= expect_eq_int("malformed-mod", st->straight_mod, 0);
    }

    memset(st->static_ks_bits, 0, sizeof(st->static_ks_bits));
    {
        char arg[] = "49:123456789ABC80";
        straight_mod_xor_keystream_creation(st, arg);
        rc |= expect_eq_int("valid-enabled", st->straight_ks, 1);
        rc |= expect_eq_int("valid-mod", st->straight_mod, 49);
        rc |= expect_eq_u8("slot0-first-byte", bits_to_u8(st->static_ks_bits[0], 0), 0x12U);
        rc |= expect_eq_u8("slot0-second-byte", bits_to_u8(st->static_ks_bits[0], 8), 0x34U);
        rc |= expect_eq_u8("slot1-first-byte", bits_to_u8(st->static_ks_bits[1], 0), 0x12U);
        rc |= expect_eq_int("slot0-bit48", st->static_ks_bits[0][48], 1);
        rc |= expect_eq_int("slot1-bit48", st->static_ks_bits[1][48], 1);
    }

    if (rc == 0) {
        printf("CORE_STRAIGHT_KEYSTREAM_VALIDATION: OK\n");
    }
    free(st);
    return rc;
}
