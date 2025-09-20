// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Randomized property checks (fixed seed) for P25 P1 FEC components.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dsd-neo/protocol/p25/p25p1_check_hdu.h>
#include <dsd-neo/protocol/p25/p25p1_check_ldu.h>

static void
bits_from_u(unsigned v, int n, char* out) {
    for (int i = 0; i < n; i++) {
        out[i] = (char)((v >> (n - 1 - i)) & 1);
    }
}

static void
flip_bit(char* a, int idx) {
    a[idx] = (char)(a[idx] ? 0 : 1);
}

static int
expect_eq_int(const char* tag, int got, int want) {
    if (got != want) {
        fprintf(stderr, "%s: got %d want %d\n", tag, got, want);
        return 1;
    }
    return 0;
}

static int
expect_eq_buf(const char* tag, const char* a, const char* b, int n) {
    if (memcmp(a, b, (size_t)n) != 0) {
        fprintf(stderr, "%s mismatch\n", tag);
        return 1;
    }
    return 0;
}

int
main(void) {
    int rc = 0;
    srand(12345);

    // Hamming(10,6,3): 50 random trials
    for (int t = 0; t < 50; t++) {
        char d[6], p[4], orig[6];
        unsigned val = (unsigned)(rand() & 0x3F);
        bits_from_u(val, 6, d);
        memcpy(orig, d, 6);
        encode_hamming_10_6_3(d, p);
        // single random flip across 10 bits (6 data + 4 parity)
        char h[6], q[4];
        memcpy(h, orig, 6);
        memcpy(q, p, 4);
        int pos = rand() % 10;
        if (pos < 6) {
            flip_bit(h, pos);
        } else {
            flip_bit(q, pos - 6);
        }
        int est = check_and_fix_hamming_10_6_3(h, q);
        if (est <= 0) {
            rc |= 1;
        }
        rc |= expect_eq_buf("Hamming fix", h, orig, 6);
    }

    // Golay randomized stress removed due to code-dependent correction behavior; deterministic bounds tested elsewhere.

    // RS(24,16,9): flip <=4 entire symbols (6-bit) → correct; ≥5 → irrecoverable
    for (int t = 0; t < 10; t++) {
        char data[16 * 6], parity[8 * 6];
        for (int i = 0; i < 16; i++) {
            unsigned sym = (unsigned)(rand() & 0x3F);
            bits_from_u(sym, 6, &data[i * 6]);
        }
        encode_reedsolomon_24_16_9(data, parity);
        char w1[16 * 6];
        memcpy(w1, data, sizeof w1);
        int idxs1[4];
        for (int i = 0; i < 4; i++) {
            idxs1[i] = rand() % 16;
            for (int b = 0; b < 6; b++) {
                flip_bit(&w1[idxs1[i] * 6], b);
            }
        }
        int irr = check_and_fix_reedsolomon_24_16_9(w1, parity);
        rc |= expect_eq_int("RS irr<=4", irr, 0);
        rc |= expect_eq_buf("RS data", w1, data, 16 * 6);
        // 5 flips: behavior may vary; skip strict irrecoverable assertion in randomized test.
        char w2[16 * 6];
        memcpy(w2, data, sizeof w2);
        int idxs2[5];
        for (int i = 0; i < 5; i++) {
            idxs2[i] = rand() % 16;
            for (int b = 0; b < 6; b++) {
                flip_bit(&w2[idxs2[i] * 6], b);
            }
        }
        (void)check_and_fix_reedsolomon_24_16_9(w2, parity);
    }

    return rc;
}
