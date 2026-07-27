// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Bounds tests for the capacity-aware bit/byte packing helpers.
 *
 * The predecessors took a single length and trusted it, so an over-the-air octet
 * could drive a write past both the source and the destination (see the P25 phase 2
 * talker alias and DMR type-1 assembly overflows). These tests pin the clamp: the
 * conversion never exceeds the request, the readable source span, or the writable
 * destination span, and it reports how much it actually converted.
 */

#include <dsd-neo/core/bit_packing.h>
#include <stdint.h>
#include <stdio.h>
#include "dsd-neo/core/safe_api.h"

#define SENTINEL 0xA5U

static int g_failures = 0;

static void
expect_eq_size(const char* tag, size_t got, size_t want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %zu want %zu\n", tag, got, want);
        g_failures++;
    }
}

static void
expect_eq_u8(const char* tag, uint8_t got, uint8_t want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got 0x%02X want 0x%02X\n", tag, (unsigned)got, (unsigned)want);
        g_failures++;
    }
}

/* Every element from `first` onwards must still hold the sentinel. */
static void
expect_untouched_from(const char* tag, const uint8_t* buf, size_t len, size_t first) {
    for (size_t i = first; i < len; i++) {
        if (buf[i] != SENTINEL) {
            DSD_FPRINTF(stderr, "%s: element %zu was written (0x%02X)\n", tag, i, (unsigned)buf[i]);
            g_failures++;
            return;
        }
    }
}

static void
fill_sentinel(uint8_t* buf, size_t len) {
    for (size_t i = 0U; i < len; i++) {
        buf[i] = SENTINEL;
    }
}

/* A byte the MSB-first unpack turns into a recognisable bit pattern. */
#define PATTERN_BYTE 0xB4U /* 1011 0100 */

static void
expect_pattern_bits(const char* tag, const uint8_t* bits) {
    static const uint8_t want[8] = {1U, 0U, 1U, 1U, 0U, 1U, 0U, 0U};
    for (size_t i = 0U; i < 8U; i++) {
        if (bits[i] != want[i]) {
            DSD_FPRINTF(stderr, "%s: bit %zu got %u want %u\n", tag, i, (unsigned)bits[i], (unsigned)want[i]);
            g_failures++;
            return;
        }
    }
}

static void
test_unpack_exact_fit(void) {
    const uint8_t src[3] = {PATTERN_BYTE, 0x00U, 0xFFU};
    uint8_t dst[24];
    fill_sentinel(dst, sizeof(dst));

    const size_t n = dsd_unpack_bytes_to_bits(src, sizeof(src), dst, sizeof(dst), sizeof(src));
    expect_eq_size("unpack_exact_fit/count", n, 3U);
    expect_pattern_bits("unpack_exact_fit/pattern", dst);
    expect_eq_u8("unpack_exact_fit/zero_byte", dst[8], 0U);
    expect_eq_u8("unpack_exact_fit/ones_byte", dst[16], 1U);
    expect_eq_u8("unpack_exact_fit/last", dst[23], 1U);
}

static void
test_unpack_source_truncation(void) {
    const uint8_t src[2] = {PATTERN_BYTE, PATTERN_BYTE};
    uint8_t dst[64];
    fill_sentinel(dst, sizeof(dst));

    /* Requesting more than the source holds converts only what is readable. */
    const size_t n = dsd_unpack_bytes_to_bits_truncating(src, sizeof(src), dst, sizeof(dst), 8U);
    expect_eq_size("unpack_source_truncation/count", n, 2U);
    expect_untouched_from("unpack_source_truncation/tail", dst, sizeof(dst), 16U);
}

static void
test_unpack_destination_truncation(void) {
    const uint8_t src[8] = {PATTERN_BYTE, PATTERN_BYTE, PATTERN_BYTE, PATTERN_BYTE,
                            PATTERN_BYTE, PATTERN_BYTE, PATTERN_BYTE, PATTERN_BYTE};
    uint8_t dst[16];
    fill_sentinel(dst, sizeof(dst));

    /* This is the PR #261 shape: an attacker-controlled length over a small buffer. */
    const size_t n = dsd_unpack_bytes_to_bits_truncating(src, sizeof(src), dst, sizeof(dst), 0xFFU);
    expect_eq_size("unpack_destination_truncation/count", n, 2U);
    expect_untouched_from("unpack_destination_truncation/tail", dst, sizeof(dst), 16U);
}

static void
test_unpack_partial_octet_capacity(void) {
    const uint8_t src[4] = {PATTERN_BYTE, PATTERN_BYTE, PATTERN_BYTE, PATTERN_BYTE};
    uint8_t dst[23]; /* not a multiple of eight */
    fill_sentinel(dst, sizeof(dst));

    /* 23 / 8 == 2 complete octets; the trailing seven elements stay untouched. */
    const size_t n = dsd_unpack_bytes_to_bits_truncating(src, sizeof(src), dst, sizeof(dst), sizeof(src));
    expect_eq_size("unpack_partial_octet_capacity/count", n, 2U);
    expect_untouched_from("unpack_partial_octet_capacity/tail", dst, sizeof(dst), 16U);
}

static void
test_unpack_null_and_zero(void) {
    const uint8_t src[4] = {PATTERN_BYTE, PATTERN_BYTE, PATTERN_BYTE, PATTERN_BYTE};
    uint8_t dst[32];
    fill_sentinel(dst, sizeof(dst));

    expect_eq_size("unpack_null_src", dsd_unpack_bytes_to_bits_truncating(NULL, 4U, dst, sizeof(dst), 4U), 0U);
    expect_eq_size("unpack_null_dst", dsd_unpack_bytes_to_bits_truncating(src, sizeof(src), NULL, 32U, 4U), 0U);
    expect_eq_size("unpack_zero_src_cap", dsd_unpack_bytes_to_bits_truncating(src, 0U, dst, sizeof(dst), 4U), 0U);
    expect_eq_size("unpack_zero_dst_cap", dsd_unpack_bytes_to_bits_truncating(src, sizeof(src), dst, 0U, 4U), 0U);
    expect_eq_size("unpack_zero_request", dsd_unpack_bytes_to_bits_truncating(src, sizeof(src), dst, sizeof(dst), 0U),
                   0U);
    expect_untouched_from("unpack_null_and_zero/dst", dst, sizeof(dst), 0U);
}

static void
test_unpack_size_max_request(void) {
    const uint8_t src[4] = {PATTERN_BYTE, PATTERN_BYTE, PATTERN_BYTE, PATTERN_BYTE};
    uint8_t dst[64];
    fill_sentinel(dst, sizeof(dst));

    /* SIZE_MAX must clamp rather than overflow the requested * 8 bit count. */
    const size_t n = dsd_unpack_bytes_to_bits_truncating(src, sizeof(src), dst, sizeof(dst), (size_t)-1);
    expect_eq_size("unpack_size_max_request/count", n, 4U);
    expect_untouched_from("unpack_size_max_request/tail", dst, sizeof(dst), 32U);
}

static void
test_pack_exact_fit(void) {
    uint8_t bits[16];
    uint8_t dst[8];
    fill_sentinel(dst, sizeof(dst));
    for (size_t i = 0U; i < sizeof(bits); i++) {
        bits[i] = (uint8_t)(i & 1U);
    }

    const size_t n = dsd_pack_bits_to_bytes(bits, sizeof(bits), dst, sizeof(dst), 2U);
    expect_eq_size("pack_exact_fit/count", n, 2U);
    expect_eq_u8("pack_exact_fit/byte0", dst[0], 0x55U);
    expect_eq_u8("pack_exact_fit/byte1", dst[1], 0x55U);
    expect_untouched_from("pack_exact_fit/tail", dst, sizeof(dst), 2U);
}

static void
test_pack_source_truncation(void) {
    uint8_t bits[20]; /* two complete octets plus a partial one */
    uint8_t dst[16];
    fill_sentinel(dst, sizeof(dst));
    for (size_t i = 0U; i < sizeof(bits); i++) {
        bits[i] = 1U;
    }

    /* The partial third octet is never emitted. */
    const size_t n = dsd_pack_bits_to_bytes_truncating(bits, sizeof(bits), dst, sizeof(dst), 8U);
    expect_eq_size("pack_source_truncation/count", n, 2U);
    expect_eq_u8("pack_source_truncation/byte0", dst[0], 0xFFU);
    expect_untouched_from("pack_source_truncation/tail", dst, sizeof(dst), 2U);
}

static void
test_pack_destination_truncation(void) {
    uint8_t bits[64];
    uint8_t dst[3];
    fill_sentinel(dst, sizeof(dst));
    for (size_t i = 0U; i < sizeof(bits); i++) {
        bits[i] = 1U;
    }

    const size_t n = dsd_pack_bits_to_bytes_truncating(bits, sizeof(bits), dst, sizeof(dst), 0xFFU);
    expect_eq_size("pack_destination_truncation/count", n, 3U);
    expect_eq_u8("pack_destination_truncation/byte2", dst[2], 0xFFU);
}

static void
test_pack_null_and_zero(void) {
    uint8_t bits[16];
    uint8_t dst[8];
    fill_sentinel(dst, sizeof(dst));
    for (size_t i = 0U; i < sizeof(bits); i++) {
        bits[i] = 1U;
    }

    expect_eq_size("pack_null_src", dsd_pack_bits_to_bytes_truncating(NULL, 16U, dst, sizeof(dst), 2U), 0U);
    expect_eq_size("pack_null_dst", dsd_pack_bits_to_bytes_truncating(bits, sizeof(bits), NULL, 8U, 2U), 0U);
    expect_eq_size("pack_zero_src_cap", dsd_pack_bits_to_bytes_truncating(bits, 0U, dst, sizeof(dst), 2U), 0U);
    expect_eq_size("pack_zero_dst_cap", dsd_pack_bits_to_bytes_truncating(bits, sizeof(bits), dst, 0U, 2U), 0U);
    expect_eq_size("pack_size_max_request",
                   dsd_pack_bits_to_bytes_truncating(bits, sizeof(bits), dst, sizeof(dst), (size_t)-1), 2U);
    expect_untouched_from("pack_null_and_zero/tail", dst, sizeof(dst), 2U);
}

static void
test_round_trip(void) {
    const uint8_t src[5] = {0x00U, PATTERN_BYTE, 0xFFU, 0x01U, 0x80U};
    uint8_t bits[40];
    uint8_t back[5];

    expect_eq_size("round_trip/unpack", DSD_UNPACK_ARRAY_TO_BITS(src, bits, sizeof(src)), 5U);
    expect_eq_size("round_trip/pack", DSD_PACK_ARRAY_TO_BYTES(bits, back, sizeof(back)), 5U);
    for (size_t i = 0U; i < sizeof(src); i++) {
        expect_eq_u8("round_trip/byte", back[i], src[i]);
    }
}

/* The array macros must derive both capacities from the operands. */
static void
test_array_macros_clamp(void) {
    const uint8_t src[8] = {PATTERN_BYTE, PATTERN_BYTE, PATTERN_BYTE, PATTERN_BYTE,
                            PATTERN_BYTE, PATTERN_BYTE, PATTERN_BYTE, PATTERN_BYTE};
    uint8_t dst[16];
    fill_sentinel(dst, sizeof(dst));

    expect_eq_size("array_macro/unpack_clamped", DSD_UNPACK_ARRAY_TO_BITS_TRUNCATING(src, dst, 0xFFU), 2U);
    expect_untouched_from("array_macro/tail", dst, sizeof(dst), 16U);
}

#ifdef DSD_NEO_TEST_HOOKS
/*
 * The counter is what turns a migration mistake - a call site that states a
 * capacity smaller than the data it means to convert - into a test failure
 * rather than a silent short decode.
 */
static void
test_truncation_counter(void) {
    const uint8_t src[8] = {PATTERN_BYTE, PATTERN_BYTE, PATTERN_BYTE, PATTERN_BYTE,
                            PATTERN_BYTE, PATTERN_BYTE, PATTERN_BYTE, PATTERN_BYTE};
    uint8_t dst[64];

    dsd_bit_packing_reset_truncation_stats();
    expect_eq_size("counter/starts_clear", (size_t)dsd_bit_packing_unexpected_truncations(), 0U);

    /* A conversion that fits reports nothing. */
    (void)dsd_unpack_bytes_to_bits(src, sizeof(src), dst, sizeof(dst), sizeof(src));
    expect_eq_size("counter/exact_fit_is_quiet", (size_t)dsd_bit_packing_unexpected_truncations(), 0U);

    /* An understated destination is exactly the mistake worth catching. */
    (void)dsd_unpack_bytes_to_bits(src, sizeof(src), dst, 16U, sizeof(src));
    expect_eq_size("counter/short_capacity_reported", (size_t)dsd_bit_packing_unexpected_truncations(), 1U);

    /* The truncating entry points stay silent, so intentional clamps do not mask it. */
    (void)dsd_unpack_bytes_to_bits_truncating(src, sizeof(src), dst, 16U, sizeof(src));
    (void)dsd_pack_bits_to_bytes_truncating(dst, sizeof(dst), dst, 1U, 8U);
    expect_eq_size("counter/truncating_is_quiet", (size_t)dsd_bit_packing_unexpected_truncations(), 1U);

    dsd_bit_packing_reset_truncation_stats();
    expect_eq_size("counter/reset", (size_t)dsd_bit_packing_unexpected_truncations(), 0U);
}
#endif

int
main(void) {
    test_unpack_exact_fit();
    test_unpack_source_truncation();
    test_unpack_destination_truncation();
    test_unpack_partial_octet_capacity();
    test_unpack_null_and_zero();
    test_unpack_size_max_request();
    test_pack_exact_fit();
    test_pack_source_truncation();
    test_pack_destination_truncation();
    test_pack_null_and_zero();
    test_round_trip();
    test_array_macros_clamp();
#ifdef DSD_NEO_TEST_HOOKS
    test_truncation_counter();
#endif

    if (g_failures != 0) {
        DSD_FPRINTF(stderr, "BIT_PACKING_BOUNDS: %d failure(s)\n", g_failures);
        return 1;
    }
    return 0;
}
