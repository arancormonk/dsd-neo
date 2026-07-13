// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * P25 Phase 2 RS(63,35) FACCH/SACCH correction limit tests.
 *
 * Valid shortened codewords are constructed via ezpwd::RS<63,35> then symbol
 * errors are injected to test t=14 correction capacity through the canonical
 * erasure-aware decoders.
 */

#include <exception>
#include <stdint.h>
#include <stdio.h>
#include <vector>
#include "dsd-neo/core/safe_api.h"
#include "ezpwd/rs"

extern "C" {
int ez_rs28_facch(int payload[156], int parity[114], const int* erasures, int n_erasures);
int ez_rs28_sacch(int payload[180], int parity[132], const int* erasures, int n_erasures);
}

static const int FACCH_FIXED_ERASURES[18] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 54, 55, 56, 57, 58, 59, 60, 61, 62};
static const int SACCH_FIXED_ERASURES[11] = {0, 1, 2, 3, 4, 57, 58, 59, 60, 61, 62};

static void
sym_to_bits6(uint8_t s, int* out6) {
    for (int i = 0; i < 6; i++) {
        out6[i] = (int)((s >> (5 - i)) & 1);
    }
}

static int
expect_ge(const char* tag, int got, int minv) {
    if (got < minv) {
        DSD_FPRINTF(stderr, "%s: got %d < %d\n", tag, got, minv);
        return 1;
    }
    return 0;
}

static int
expect_lt(const char* tag, int got, int maxv) {
    if (got >= maxv) {
        DSD_FPRINTF(stderr, "%s: got %d >= %d\n", tag, got, maxv);
        return 1;
    }
    return 0;
}

int
main(void) try {
    int rc = 0;
    ezpwd::RS<63, 35> rs;

    // Build a systematic cw = data || parity
    std::vector<uint8_t> data(35), parity(28);
    for (size_t i = 0; i < data.size(); i++) {
        data[i] = (uint8_t)((i * 9 + 1) & 0x3F);
    }
    rs.encode(data, parity);
    std::vector<uint8_t> cw;
    cw.reserve(63);
    cw.insert(cw.end(), data.begin(), data.end());
    cw.insert(cw.end(), parity.begin(), parity.end());

    // Make working copies to inject symbol errors within the used ranges
    std::vector<uint8_t> cw_facch = cw;
    std::vector<uint8_t> cw_sacch = cw;

    // Inject 5 symbol errors across the FACCH used portion (0..44)
    for (int i = 0; i < 5; i++) {
        int idx = (i * 3) % 45; // 0..44
        cw_facch[idx] ^= 0x3F;  // flip all 6 bits to simulate worst-case symbol error
    }

    // Inject 15 symbol errors across the SACCH used portion (0..51)
    for (int i = 0; i < 15; i++) {
        int idx = (i * 2 + 5) % 52; // 0..51
        cw_sacch[idx] ^= 0x3F;
    }

    // FACCH mapping: payload ← cw[0..25], parity ← cw[26..44]
    {
        int payload[156] = {0};
        int parity_bits[114] = {0};
        int pbits = 0, qbits = 0;
        for (int i = 0; i <= 25; i++) {
            int b6[6];
            sym_to_bits6(cw_facch[i], b6);
            for (int b = 0; b < 6; b++) {
                payload[pbits++] = b6[b];
            }
        }
        for (int i = 26; i <= 44; i++) {
            int b6[6];
            sym_to_bits6(cw_facch[i], b6);
            for (int b = 0; b < 6; b++) {
                parity_bits[qbits++] = b6[b];
            }
        }
        int ec = ez_rs28_facch(payload, parity_bits, FACCH_FIXED_ERASURES, 18);
        rc |= expect_ge("FACCH t<=14", ec, 0);
    }

    // SACCH mapping: payload ← cw[0..29], parity ← cw[30..51]
    {
        int payload[180] = {0};
        int parity_bits[132] = {0};
        int pbits = 0, qbits = 0;
        for (int i = 0; i <= 29; i++) {
            int b6[6];
            sym_to_bits6(cw_sacch[i], b6);
            for (int b = 0; b < 6; b++) {
                payload[pbits++] = b6[b];
            }
        }
        for (int i = 30; i <= 51; i++) {
            int b6[6];
            sym_to_bits6(cw_sacch[i], b6);
            for (int b = 0; b < 6; b++) {
                parity_bits[qbits++] = b6[b];
            }
        }
        int ec = ez_rs28_sacch(payload, parity_bits, SACCH_FIXED_ERASURES, 11);
        rc |= expect_lt("SACCH t>=15 fails", ec, 0);
    }

    return rc;
} catch (const std::exception& e) {

    DSD_FPRINTF(stderr, "Unhandled exception: %s\n", e.what());
    return 1;
} catch (...) {
    DSD_FPRINTF(stderr, "Unhandled exception\n");
    return 1;
}
