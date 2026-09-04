// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Regression vectors for the NXDN Viterbi hard/soft convolution decoder.
 */

#include <dsd-neo/protocol/nxdn/nxdn_convolution.h>
#include <stdint.h>
#include <stdio.h>
#include "dsd-neo/core/safe_api.h"

static int
expect_byte(const char* tag, size_t index, uint8_t got, uint8_t want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s[%zu]: got 0x%02X want 0x%02X\n", tag, index, got, want);
        return 1;
    }
    return 0;
}

static int
expect_buffer(const char* tag, const uint8_t* got, const uint8_t* want, size_t len) {
    int rc = 0;
    for (size_t i = 0U; i < len; i++) {
        rc |= expect_byte(tag, i, got[i], want[i]);
    }
    return rc;
}

static void
decode_hard_symbols(const uint8_t* symbols, size_t pair_count, unsigned int out_bits, uint8_t* out, size_t out_len) {
    DSD_MEMSET(out, 0, out_len);

    CNXDNConvolution_init();
    CNXDNConvolution_start();
    for (size_t i = 0U; i < pair_count; i++) {
        CNXDNConvolution_decode(symbols[i * 2U], symbols[(i * 2U) + 1U]);
    }
    CNXDNConvolution_chainback(out, out_bits);
}

static void
decode_soft_symbols(const uint8_t* symbols, const uint8_t* reliabilities, size_t pair_count, unsigned int out_bits,
                    uint8_t* out, size_t out_len) {
    DSD_MEMSET(out, 0, out_len);

    CNXDNConvolution_init();
    CNXDNConvolution_start();
    for (size_t i = 0U; i < pair_count; i++) {
        CNXDNConvolution_decode_soft(symbols[i * 2U], symbols[(i * 2U) + 1U], reliabilities[i * 2U],
                                     reliabilities[(i * 2U) + 1U]);
    }
    CNXDNConvolution_chainback(out, out_bits);
}

static int
test_hard_decode_vector(void) {
    static const uint8_t symbols[] = {0, 0, 0, 2, 2, 2, 2, 0, 0, 0, 2, 0, 0, 2, 2, 2, 0, 0, 0, 2, 2, 0, 0, 0};
    static const uint8_t expected[] = {0x88U, 0xE0U};

    uint8_t out[sizeof(expected)];
    decode_hard_symbols(symbols, sizeof(symbols) / 2U, 12U, out, sizeof(out));
    return expect_buffer("hard-decode", out, expected, sizeof(expected));
}

static int
test_soft_decode_vector(void) {
    static const uint8_t symbols[] = {0, 0, 0, 2, 2, 2, 2, 0, 0, 0, 2, 0, 0, 2, 2, 2, 0, 0, 0, 2, 2, 0, 0, 0};
    static const uint8_t reliabilities[sizeof(symbols)] = {
        255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
        255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    };
    static const uint8_t expected[] = {0x03U, 0x00U};

    uint8_t out[sizeof(expected)];
    decode_soft_symbols(symbols, reliabilities, sizeof(symbols) / 2U, 12U, out, sizeof(out));
    return expect_buffer("soft-decode", out, expected, sizeof(expected));
}

static int
test_soft_decode_clamps_large_metrics(void) {
    static const uint8_t symbols[] = {9, 9, 2, 0, 0, 2, 2, 2, 0, 0, 2, 0, 0, 2, 2, 2, 0, 0, 0, 2, 2, 0, 0, 0};
    static const uint8_t reliabilities[sizeof(symbols)] = {
        255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
        255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    };
    static const uint8_t expected[] = {0xD3U, 0x00U};

    uint8_t out[sizeof(expected)];
    decode_soft_symbols(symbols, reliabilities, sizeof(symbols) / 2U, 12U, out, sizeof(out));
    return expect_buffer("soft-clamp", out, expected, sizeof(expected));
}

static int
test_chainback_zero_bits_preserves_output(void) {
    uint8_t out[2] = {0xA5U, 0x5AU};

    CNXDNConvolution_init();
    CNXDNConvolution_start();
    CNXDNConvolution_chainback(out, 0U);

    int rc = 0;
    rc |= expect_byte("zero-chainback", 0U, out[0], 0xA5U);
    rc |= expect_byte("zero-chainback", 1U, out[1], 0x5AU);
    return rc;
}

int
main(void) {
    int rc = 0;

    rc |= test_hard_decode_vector();
    rc |= test_soft_decode_vector();
    rc |= test_soft_decode_clamps_large_metrics();
    rc |= test_chainback_zero_bits_preserves_output();

    if (rc == 0) {
        printf("NXDN_CONVOLUTION: OK\n");
    }
    return rc;
}
