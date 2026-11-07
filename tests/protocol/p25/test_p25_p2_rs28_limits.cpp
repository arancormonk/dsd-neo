// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * P25 Phase 2 RS(63,35) FACCH/SACCH correction limit tests.
 *
 * Valid shortened codewords are constructed via ezpwd::RS<63,35> with the
 * shortened data symbols at zero (FACCH 0..8, SACCH 0..4), then symbol errors
 * are injected into the sent symbols to test the correction capacity through
 * the canonical decoders with only the punctured parity erased: FACCH is
 * RS(45,26,20) and corrects t=9, SACCH is RS(52,30,23) and corrects t=11.
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

static const int FACCH_FIXED_ERASURES[9] = {54, 55, 56, 57, 58, 59, 60, 61, 62};
static const int SACCH_FIXED_ERASURES[6] = {57, 58, 59, 60, 61, 62};

namespace {

struct AcchShape {
    const char* name;
    int pad;      // shortened symbols, RS index 0..pad-1
    int n_parity; // sent parity symbols, RS index 35..35+n_parity-1
};

} // namespace

static const AcchShape FACCH = {"FACCH", 9, 19};
static const AcchShape SACCH = {"SACCH", 5, 22};

static void
sym_to_bits6(uint8_t s, int* out6) {
    for (int i = 0; i < 6; i++) {
        out6[i] = (int)((s >> (5 - i)) & 1);
    }
}

static int
expect_eq(const char* tag, int got, int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %d want %d\n", tag, got, want);
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

/* Full 63-symbol codeword (data 0..34, parity 35..62) with `nonzero_pad` set in shortened symbol 0. */
static std::vector<uint8_t>
make_codeword(const AcchShape& shape, uint8_t nonzero_pad) {
    ezpwd::RS<63, 35> rs;
    std::vector<uint8_t> data(35, 0), parity(28);
    for (int i = shape.pad; i < 35; i++) {
        data[(size_t)i] = (uint8_t)((i * 9 + 1) & 0x3F);
    }
    data[0] = nonzero_pad;
    rs.encode(data, parity);
    std::vector<uint8_t> cw;
    cw.reserve(63);
    cw.insert(cw.end(), data.begin(), data.end());
    cw.insert(cw.end(), parity.begin(), parity.end());
    return cw;
}

/* Flip all six bits of `count` distinct sent symbols, spread over payload and parity. */
static std::vector<uint8_t>
with_errors(const AcchShape& shape, const std::vector<uint8_t>& cw, int count) {
    std::vector<uint8_t> rx = cw;
    int sent = 35 + shape.n_parity - shape.pad;
    for (int i = 0; i < count; i++) {
        rx[(size_t)shape.pad + (size_t)((i * 4) % sent)] ^= 0x3F;
    }
    return rx;
}

/* Decode the sent symbols of `rx`; the punctured parity is not passed on, as on air. */
static int
decode(const AcchShape& shape, const std::vector<uint8_t>& rx, int payload[180]) {
    int parity_bits[132] = {0};
    int payload_offset = 0;
    int parity_offset = 0;
    for (int i = shape.pad; i < 35; i++) {
        sym_to_bits6(rx[(size_t)i], &payload[payload_offset]);
        payload_offset += 6;
    }
    for (int i = 35; i < 35 + shape.n_parity; i++) {
        sym_to_bits6(rx[(size_t)i], &parity_bits[parity_offset]);
        parity_offset += 6;
    }
    if (shape.pad == FACCH.pad) {
        return ez_rs28_facch(payload, parity_bits, FACCH_FIXED_ERASURES, 9);
    }
    return ez_rs28_sacch(payload, parity_bits, SACCH_FIXED_ERASURES, 6);
}

static int
expect_payload(const char* tag, const AcchShape& shape, const int payload[180], const std::vector<uint8_t>& cw) {
    for (int symbol = shape.pad; symbol < 35; symbol++) {
        int expected[6];
        sym_to_bits6(cw[(size_t)symbol], expected);
        for (int bit = 0; bit < 6; bit++) {
            if (payload[((symbol - shape.pad) * 6) + bit] != expected[bit]) {
                DSD_FPRINTF(stderr, "%s: mismatch at symbol %d bit %d\n", tag, symbol, bit);
                return 1;
            }
        }
    }
    return 0;
}

static int
check_shape(const AcchShape& shape, int t) {
    int rc = 0;
    char tag[64];
    std::vector<uint8_t> cw = make_codeword(shape, 0);

    // A clean nonzero vector catches shortened-codeword index and bit-order regressions.
    {
        int payload[180] = {0};
        DSD_SNPRINTF(tag, sizeof(tag), "%s clean nonzero codeword", shape.name);
        rc |= expect_eq(tag, decode(shape, cw, payload), 0);
        rc |= expect_payload(tag, shape, payload, cw);
    }

    // t symbol errors are all corrected.
    {
        int payload[180] = {0};
        DSD_SNPRINTF(tag, sizeof(tag), "%s t=%d", shape.name, t);
        rc |= expect_eq(tag, decode(shape, with_errors(shape, cw, t), payload), t);
        rc |= expect_payload(tag, shape, payload, cw);
    }

    // t+1 symbol errors fail.
    {
        int payload[180] = {0};
        DSD_SNPRINTF(tag, sizeof(tag), "%s t+1=%d fails", shape.name, t + 1);
        rc |= expect_lt(tag, decode(shape, with_errors(shape, cw, t + 1), payload), 0);
    }

    // A codeword of the unshortened code with a nonzero shortened symbol: one error the decode can reach, but only
    // in a symbol known to be zero, so it fails.
    {
        int payload[180] = {0};
        DSD_SNPRINTF(tag, sizeof(tag), "%s pad violation rejected", shape.name);
        rc |= expect_lt(tag, decode(shape, make_codeword(shape, 0x2A), payload), 0);
    }
    return rc;
}

int
main(void) try {
    int rc = 0;
    rc |= check_shape(FACCH, 9);
    rc |= check_shape(SACCH, 11);
    return rc;
} catch (const std::exception& e) {

    DSD_FPRINTF(stderr, "Unhandled exception: %s\n", e.what());
    return 1;
} catch (...) {
    DSD_FPRINTF(stderr, "Unhandled exception\n");
    return 1;
}
