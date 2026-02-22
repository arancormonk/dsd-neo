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

    st->straight_ks = 1;
    st->straight_mod = 11;
    {
        char arg[] = "49x:F0";
        straight_mod_xor_keystream_creation(st, arg);
        rc |= expect_eq_int("len-partial-disabled", st->straight_ks, 0);
        rc |= expect_eq_int("len-partial-mod", st->straight_mod, 0);
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

    // Optional frame alignment parsing: explicit offset + step.
    {
        char arg[] = "8:F0:2:3";
        straight_mod_xor_keystream_creation(st, arg);
        rc |= expect_eq_int("frame-mode-enabled", st->straight_ks, 1);
        rc |= expect_eq_int("frame-mode-flag", st->straight_frame_mode, 1);
        rc |= expect_eq_int("frame-mode-off", st->straight_frame_off, 2);
        rc |= expect_eq_int("frame-mode-step", st->straight_frame_step, 3);
    }

    // Offset-only syntax defaults step to 49 bits per frame (then modulo len).
    {
        char arg[] = "8:F0:2";
        straight_mod_xor_keystream_creation(st, arg);
        rc |= expect_eq_int("frame-default-step-enabled", st->straight_ks, 1);
        rc |= expect_eq_int("frame-default-step-flag", st->straight_frame_mode, 1);
        rc |= expect_eq_int("frame-default-step-val", st->straight_frame_step, 1); // 49 % 8
    }

    // Malformed frame alignment fields disable the feature.
    st->straight_ks = 1;
    st->straight_mod = 8;
    {
        char arg[] = "8:F0:bad";
        straight_mod_xor_keystream_creation(st, arg);
        rc |= expect_eq_int("bad-offset-disabled", st->straight_ks, 0);
        rc |= expect_eq_int("bad-offset-mod", st->straight_mod, 0);
    }
    st->straight_ks = 1;
    st->straight_mod = 8;
    {
        char arg[] = "8:F0:2x:3";
        straight_mod_xor_keystream_creation(st, arg);
        rc |= expect_eq_int("bad-offset-partial-disabled", st->straight_ks, 0);
        rc |= expect_eq_int("bad-offset-partial-mod", st->straight_mod, 0);
    }
    st->straight_ks = 1;
    st->straight_mod = 8;
    {
        char arg[] = "8:F0:0x10:3";
        straight_mod_xor_keystream_creation(st, arg);
        rc |= expect_eq_int("bad-offset-hex-disabled", st->straight_ks, 0);
        rc |= expect_eq_int("bad-offset-hex-mod", st->straight_mod, 0);
    }
    st->straight_ks = 1;
    st->straight_mod = 8;
    {
        char arg[] = "8:F0:2:3x";
        straight_mod_xor_keystream_creation(st, arg);
        rc |= expect_eq_int("bad-step-partial-disabled", st->straight_ks, 0);
        rc |= expect_eq_int("bad-step-partial-mod", st->straight_mod, 0);
    }
    st->straight_ks = 1;
    st->straight_mod = 8;
    {
        char arg[] = "8:F0:1:2:3";
        straight_mod_xor_keystream_creation(st, arg);
        rc |= expect_eq_int("extra-fields-disabled", st->straight_ks, 0);
        rc |= expect_eq_int("extra-fields-mod", st->straight_mod, 0);
    }

    // Legacy mode: continuous modulo-N stream across frames.
    {
        char arg[] = "8:F0";
        char frame0[49];
        char frame1[49];
        memset(frame0, 0, sizeof(frame0));
        memset(frame1, 0, sizeof(frame1));
        straight_mod_xor_keystream_creation(st, arg);
        straight_mod_xor_apply_frame49(st, 0, frame0);
        straight_mod_xor_apply_frame49(st, 0, frame1);
        rc |= expect_eq_u8("legacy-frame0-byte0", bits_to_u8((const uint8_t*)frame0, 0), 0xF0U);
        rc |= expect_eq_u8("legacy-frame1-byte0", bits_to_u8((const uint8_t*)frame1, 0), 0xE1U);
        rc |= expect_eq_int("legacy-counter", st->static_ks_counter[0], 98);
    }

    // Frame mode: each AMBE frame starts at offset + n*step (mod len).
    {
        char arg[] = "8:F0:2:3";
        char frame0[49];
        char frame1[49];
        char frame2[49];
        char frame_slot1[49];
        memset(frame0, 0, sizeof(frame0));
        memset(frame1, 0, sizeof(frame1));
        memset(frame2, 0, sizeof(frame2));
        memset(frame_slot1, 0, sizeof(frame_slot1));
        straight_mod_xor_keystream_creation(st, arg);
        straight_mod_xor_apply_frame49(st, 0, frame0);
        straight_mod_xor_apply_frame49(st, 0, frame1);
        straight_mod_xor_apply_frame49(st, 0, frame2);
        straight_mod_xor_apply_frame49(st, 1, frame_slot1);
        rc |= expect_eq_u8("frame-mode-f0", bits_to_u8((const uint8_t*)frame0, 0), 0xC3U); // start 2
        rc |= expect_eq_u8("frame-mode-f1", bits_to_u8((const uint8_t*)frame1, 0), 0x1EU); // start 5
        rc |= expect_eq_u8("frame-mode-f2", bits_to_u8((const uint8_t*)frame2, 0), 0xF0U); // start 0
        rc |= expect_eq_u8("frame-mode-slot1", bits_to_u8((const uint8_t*)frame_slot1, 0),
                           0xC3U); // independent slot counter
        rc |= expect_eq_int("frame-mode-counter-slot0", st->static_ks_counter[0], 3);
        rc |= expect_eq_int("frame-mode-counter-slot1", st->static_ks_counter[1], 1);
    }

    // Large frame counters must not wrap 32-bit multiply in frame alignment.
    {
        char arg[] = "49:123456789ABC80:2:48";
        char frame0[49];
        memset(frame0, 0, sizeof(frame0));
        straight_mod_xor_keystream_creation(st, arg);
        st->static_ks_counter[0] = 1000000000;
        straight_mod_xor_apply_frame49(st, 0, frame0);

        const uint64_t frame_ctr = 1000000000ULL;
        const uint64_t mod = (uint64_t)st->straight_mod;
        const uint64_t off = (uint64_t)st->straight_frame_off;
        const uint64_t step = (uint64_t)st->straight_frame_step;
        const int expected_base = (int)((off + ((frame_ctr * step) % mod)) % mod);
        rc |= expect_eq_u8("frame-mode-overflow-safe", bits_to_u8((const uint8_t*)frame0, 0),
                           bits_to_u8(st->static_ks_bits[0], expected_base));
        rc |= expect_eq_int("frame-mode-overflow-counter", st->static_ks_counter[0], 1000000001);
    }

    if (rc == 0) {
        printf("CORE_STRAIGHT_KEYSTREAM_VALIDATION: OK\n");
    }
    free(st);
    return rc;
}
