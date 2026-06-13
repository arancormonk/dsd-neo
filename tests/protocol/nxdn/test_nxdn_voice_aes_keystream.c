// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Regression coverage for NXDN cipher 3 voice AES-OFB keystream alignment.
 */

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/crypto/aes.h>
#include <stdint.h>
#include <stdio.h>
#include "dsd-neo/core/state_fwd.h"

void dsd_mbe_init_nxdn_cipher23_keystream(dsd_state* state);
// NOLINTNEXTLINE(misc-use-internal-linkage)
void LFSRN(const char* buffer_in, char* buffer_out, dsd_state* state);
// NOLINTNEXTLINE(misc-use-internal-linkage)
uint16_t ComputeCrcCCITT16d(const uint8_t* buf, uint32_t len);

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
LFSRN(const char* buffer_in, char* buffer_out, dsd_state* state) {
    (void)buffer_in;
    (void)buffer_out;
    (void)state;
}

uint16_t
// NOLINTNEXTLINE(misc-use-internal-linkage)
ComputeCrcCCITT16d(const uint8_t* buf, uint32_t len) {
    (void)buf;
    (void)len;
    return 0U;
}

static int
expect_int(const char* tag, int got, int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %d want %d\n", tag, got, want);
        return 1;
    }
    return 0;
}

static uint8_t
bit_from_byte(const uint8_t* bytes, size_t bit) {
    return (uint8_t)((bytes[bit / 8U] >> (7U - (bit % 8U))) & 1U);
}

static int
expect_bits_match(const char* tag, const uint8_t* got, const uint8_t* bytes, size_t bits) {
    for (size_t i = 0U; i < bits; i++) {
        uint8_t want = bit_from_byte(bytes, i);
        if (got[i] != want) {
            DSD_FPRINTF(stderr, "%s: bit %zu got %u want %u\n", tag, i, (unsigned)got[i], (unsigned)want);
            return 1;
        }
    }
    return 0;
}

static int
expect_bits_differ(const char* tag, const uint8_t* got, const uint8_t* bytes, size_t bits) {
    for (size_t i = 0U; i < bits; i++) {
        if (got[i] != bit_from_byte(bytes, i)) {
            return 0;
        }
    }
    DSD_FPRINTF(stderr, "%s: bits unexpectedly matched\n", tag);
    return 1;
}

int
main(void) {
    static dsd_state state;
    uint8_t expected[15U * 16U];
    int rc = 0;

    DSD_MEMSET(&state, 0, sizeof(state));
    for (size_t i = 0U; i < sizeof(state.aes_iv); i++) {
        state.aes_iv[i] = (uint8_t)(0x10U + i);
    }
    for (size_t i = 0U; i < sizeof(state.aes_key); i++) {
        state.aes_key[i] = (uint8_t)(0xA0U + (i * 3U));
    }

    state.nxdn_cipher_type = 0x03U;
    state.nxdn_new_iv = 1U;
    state.nxdn_part_of_frame = 0U;

    DSD_MEMSET(expected, 0, sizeof(expected));
    aes_ofb_keystream_output(state.aes_iv, state.aes_key, expected, 2, 15);

    dsd_mbe_init_nxdn_cipher23_keystream(&state);

    rc |= expect_int("aes-new-iv-cleared", (int)state.nxdn_new_iv, 0);
    rc |= expect_int("aes-bit-counter-reset", (int)state.bit_counterL, 0);
    rc |= expect_bits_match("aes-discard-16-byte-block", state.ks_bitstreamL, expected + 16U, 128U);
    rc |= expect_bits_differ("aes-not-old-8-byte-offset", state.ks_bitstreamL, expected + 8U, 64U);

    if (rc == 0) {
        printf("NXDN_VOICE_AES_KEYSTREAM: OK\n");
    }
    return rc;
}
