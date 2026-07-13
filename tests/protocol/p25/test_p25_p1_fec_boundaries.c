// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Focused FEC boundary tests for P25 Phase 1 components:
 * - Hamming(10,6,3): single-bit correction on hex and parity.
 * - Golay(24,12,8): correct up to 3 bit errors; fail on 4.
 * - RS(24,16,9): correct up to 4 symbol errors; fail on 5.
 */

#include <dsd-neo/fec/block_codes.h>
#include <dsd-neo/protocol/p25/p25p1_check_hdu.h>
#include <dsd-neo/protocol/p25/p25p1_check_ldu.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "dsd-neo/core/safe_api.h"

static void
bits_from_u(int value, int n_bits, char* out_bits) {
    for (int i = 0; i < n_bits; i++) {
        out_bits[i] = (char)((value >> (n_bits - 1 - i)) & 1);
    }
}

static void
symbols_from_u6(const uint8_t* values, int count, char* out_bits) {
    for (int i = 0; i < count; i++) {
        bits_from_u(values[i], 6, &out_bits[(size_t)i * 6U]);
    }
}

static void
flip_bit(char* arr, int idx) {
    arr[idx] = (char)(arr[idx] ? 0 : 1);
}

static int
expect_eq_hex(const char* tag, const char* got, const char* want, int n) {
    if (memcmp(got, want, (size_t)n) != 0) {
        DSD_FPRINTF(stderr, "%s mismatch\n", tag);
        return 1;
    }
    return 0;
}

static int
expect_eq_int(const char* tag, int got, int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %d want %d\n", tag, got, want);
        return 1;
    }
    return 0;
}

int
main(void) {
    int rc = 0;

    // Hamming(10,6,3): single-bit correction
    {
        char hex[6], orig[6];
        const char parity[4] = {0, 1, 1, 0};
        bits_from_u(0x2A /* 101010 */, 6, hex);
        DSD_MEMCPY(orig, hex, 6);

        // Flip one data bit
        char h1[6], p1[4];
        DSD_MEMCPY(h1, orig, 6);
        DSD_MEMCPY(p1, parity, 4);
        flip_bit(h1, 3);
        int est1 = hamming_10_6_3_decode(h1, p1);
        rc |= expect_eq_hex("Hamming single data-bit fix", h1, orig, 6);
        // est1 should be >=1, but we don't assert the exact value.
        if (est1 <= 0) {
            DSD_FPRINTF(stderr, "Hamming estimated errors not positive (%d)\n", est1);
            rc |= 1;
        }

        // Flip one parity bit
        char h2[6], p2[4];
        DSD_MEMCPY(h2, orig, 6);
        DSD_MEMCPY(p2, parity, 4);
        flip_bit(p2, 1);
        int est2 = hamming_10_6_3_decode(h2, p2);
        rc |= expect_eq_hex("Hamming single parity-bit fix (data unchanged)", h2, orig, 6);
        if (est2 <= 0) {
            DSD_FPRINTF(stderr, "Hamming estimated errors not positive for parity flip (%d)\n", est2);
            rc |= 1;
        }
    }

    // Golay(24,12,8): up to 3 bit errors corrected; 4 becomes irrecoverable
    {
        char dodeca[12], orig[12];
        const char parity[12] = {0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 0, 1};
        bits_from_u(0xACE /* 12 bits */, 12, dodeca);
        DSD_MEMCPY(orig, dodeca, 12);

        // 3 random flips across data+parity
        char d1[12], p1[12];
        DSD_MEMCPY(d1, orig, 12);
        DSD_MEMCPY(p1, parity, 12);
        flip_bit(d1, 0);
        flip_bit(d1, 7);
        flip_bit(p1, 4);
        int fixed = 0;
        int irr = check_and_fix_golay_24_12(d1, p1, &fixed);
        rc |= expect_eq_int("Golay(24,12) irrecoverable(<=3)", irr, 0);
        rc |= expect_eq_hex("Golay corrected data", d1, orig, 12);

        // 4 flips: expect irrecoverable
        char d2[12], p2[12];
        DSD_MEMCPY(d2, orig, 12);
        DSD_MEMCPY(p2, parity, 12);
        flip_bit(d2, 0);
        flip_bit(d2, 1);
        flip_bit(d2, 2);
        flip_bit(p2, 0);
        fixed = 0;
        irr = check_and_fix_golay_24_12(d2, p2, &fixed);
        rc |= expect_eq_int("Golay(24,12) irrecoverable(>=4)", irr, 1);
    }

    // RS(24,16,9): up to 4 symbol errors corrected; 5 fails
    {
        // Build 16 6-bit data symbols
        char data_bits[16 * 6];
        for (int i = 0; i < 16; i++) {
            int sym = (i * 7 + 3) & 0x3F;
            bits_from_u(sym, 6, &data_bits[(size_t)i * 6]);
        }
        static const uint8_t parity_symbols[8] = {0x3D, 0x18, 0x3B, 0x29, 0x16, 0x08, 0x13, 0x20};
        char parity_bits[8 * 6];
        symbols_from_u6(parity_symbols, 8, parity_bits);

        // Make a working copy and flip 4 entire symbols (invert all 6 bits)
        char w1[16 * 6];
        DSD_MEMCPY(w1, data_bits, sizeof(w1));
        int flip_syms1[] = {0, 5, 9, 15};
        for (unsigned i = 0; i < sizeof(flip_syms1) / sizeof(flip_syms1[0]); i++) {
            int s = flip_syms1[i];
            for (int b = 0; b < 6; b++) {
                flip_bit(&w1[(size_t)s * 6], b);
            }
        }
        int irr = check_and_fix_reedsolomon_24_16_9(w1, parity_bits);
        rc |= expect_eq_int("RS(24,16,9) irrecoverable(<=4 sym)", irr, 0);
        rc |= expect_eq_hex("RS(24,16,9) corrected data", w1, data_bits, ((size_t)16) * 6);

        // Flip 5 symbols
        char w2[16 * 6];
        DSD_MEMCPY(w2, data_bits, sizeof(w2));
        int flip_syms2[] = {0, 1, 4, 8, 12};
        for (unsigned i = 0; i < sizeof(flip_syms2) / sizeof(flip_syms2[0]); i++) {
            int s = flip_syms2[i];
            for (int b = 0; b < 6; b++) {
                flip_bit(&w2[(size_t)s * 6], b);
            }
        }
        irr = check_and_fix_reedsolomon_24_16_9(w2, parity_bits);
        rc |= expect_eq_int("RS(24,16,9) irrecoverable(>=5 sym)", irr, 1);
    }

    return rc;
}
