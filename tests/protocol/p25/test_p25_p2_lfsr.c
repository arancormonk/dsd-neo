// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

// Simple verification test for P25p2 TDMA frame scrambler LFSR taps/width
//
// Verifies that the keystream generated from a fixed seed (WACN|SYSID|NAC)
// matches precomputed 128-bit vectors at two offsets:
//   - offset 0: start at index 20 (post-sync)
//   - offset 1: start at index 20 + 360
// The taps/width under test correspond to polynomial:
//   x^44 + x^34 + x^20 + x^15 + x^9 + x^4 + 1 (MSB-first)

#include <dsd-neo/core/state.h>
#include <dsd-neo/protocol/p25/p25_lfsr.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

static void
gen_lfsr_keystream(uint32_t wacn, uint16_t sysid, uint16_t nac, uint8_t* out_bits, int nbits) {
    // MSB→LSB seed: WACN[20] | SYSID[12] | NAC[12]
    unsigned long long seed =
        ((unsigned long long)wacn << 24) | ((unsigned long long)sysid << 12) | (unsigned long long)nac;
    for (int i = 0; i < nbits; i++) {
        unsigned long long out = (seed >> 43) & 0x1ULL;
        out_bits[i] = (uint8_t)out;
        unsigned long long bit =
            ((seed >> 33) ^ (seed >> 19) ^ (seed >> 14) ^ (seed >> 8) ^ (seed >> 3) ^ (seed >> 43)) & 0x1ULL;
        seed = (seed << 1) | bit;
    }
}

static void
pack_bits_msb8(const uint8_t* bits, int nbits, uint8_t* out_bytes) {
    for (int i = 0; i < nbits / 8; i++) {
        uint8_t v = 0;
        for (int b = 0; b < 8; b++) {
            v = (uint8_t)((v << 1) | (bits[i * 8 + b] & 1));
        }
        out_bytes[i] = v;
    }
}

static int
expect_bytes(const char* tag, const uint8_t* got, const uint8_t* want, int len) {
    if (memcmp(got, want, (size_t)len) == 0) {
        return 0;
    }

    DSD_FPRINTF(stderr, "%s mismatch\n got: ", tag);
    for (int i = 0; i < len; i++) {
        DSD_FPRINTF(stderr, "%02X", got[i]);
    }
    DSD_FPRINTF(stderr, "\n exp: ");
    for (int i = 0; i < len; i++) {
        DSD_FPRINTF(stderr, "%02X", want[i]);
    }
    DSD_FPRINTF(stderr, "\n");
    return 1;
}

static int
test_p25_voice_lfsr_helpers(void) {
    int rc = 0;
    dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));

    state.currentslot = 0;
    state.payload_miP = 0x0123456789ABCDEFULL;
    LFSRP(&state);
    if (state.payload_miP != 0x20B125E779D0F34EULL) {
        DSD_FPRINTF(stderr, "slot 0 LFSRP got 0x%016llX\n", state.payload_miP);
        rc = 1;
    }

    state.currentslot = 1;
    state.payload_miN = 0xFEDCBA9876543210ULL;
    const uint8_t exp_slot1_iv[16] = {0xFE, 0xDC, 0xBA, 0x98, 0x76, 0x54, 0x32, 0x10,
                                      0x20, 0xB0, 0xDA, 0x04, 0x85, 0xD4, 0xF0, 0xC2};
    LFSR128(&state);
    rc |= expect_bytes("slot 1 LFSR128", state.aes_ivR, exp_slot1_iv, 16);

    state.currentslot = 1;
    state.payload_miP = 0x1122334455667788ULL;
    const uint8_t exp_slot0_iv[16] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
                                      0xCA, 0x31, 0x57, 0x69, 0x15, 0x0C, 0x83, 0x08};
    p25_lfsr128_slot(&state, 0);
    rc |= expect_bytes("explicit slot 0 LFSR128", state.aes_iv, exp_slot0_iv, 16);
    if (state.currentslot != 1) {
        DSD_FPRINTF(stderr, "explicit slot helper changed currentslot to %d\n", state.currentslot);
        rc = 1;
    }

    return rc;
}

int
main(void) {
    // Fixed seed for test
    const uint32_t WACN = 0xABCDE; // 20-bit
    const uint16_t SYSID = 0x0123; // 12-bit
    const uint16_t NAC = 0x0456;   // 12-bit

    // Generate sufficient keystream: need up to 20 + 8*360 + 128 bits
    enum { TOTAL_BITS = 20 + 8 * 360 + 128 };

    uint8_t lbits[TOTAL_BITS];
    DSD_MEMSET(lbits, 0, sizeof lbits);
    gen_lfsr_keystream(WACN, SYSID, NAC, lbits, TOTAL_BITS);

    // Expected 128-bit segments packed MSB-first
    // start + 20 (offset 0): 0x12345695b0f9ee0bfdb7924533d86141
    const uint8_t exp0[16] = {0x12, 0x34, 0x56, 0x95, 0xB0, 0xF9, 0xEE, 0x0B,
                              0xFD, 0xB7, 0x92, 0x45, 0x33, 0xD8, 0x61, 0x41};
    // start + 20 + 360 (offset 1): 0x2927afb664b5d14b8008032c26a94f26
    const uint8_t exp1[16] = {0x29, 0x27, 0xAF, 0xB6, 0x64, 0xB5, 0xD1, 0x4B,
                              0x80, 0x08, 0x03, 0x2C, 0x26, 0xA9, 0x4F, 0x26};
    // start + 20 + 4*360 (offset 4): 0xfb223a54e30a985a81e2e236bf320a98
    const uint8_t exp4[16] = {0xFB, 0x22, 0x3A, 0x54, 0xE3, 0x0A, 0x98, 0x5A,
                              0x81, 0xE2, 0xE2, 0x36, 0xBF, 0x32, 0x0A, 0x98};
    // start + 20 + 8*360 (offset 8): 0xd2b21546f7a96c2c764028e3c1e023c9
    const uint8_t exp8[16] = {0xD2, 0xB2, 0x15, 0x46, 0xF7, 0xA9, 0x6C, 0x2C,
                              0x76, 0x40, 0x28, 0xE3, 0xC1, 0xE0, 0x23, 0xC9};

    uint8_t out0[16], out1[16], out4[16], out8[16];
    pack_bits_msb8(&lbits[20], 128, out0);
    pack_bits_msb8(&lbits[20 + 360], 128, out1);
    pack_bits_msb8(&lbits[20 + 4 * 360], 128, out4);
    pack_bits_msb8(&lbits[20 + 8 * 360], 128, out8);

    int rc = 0;
    if (memcmp(out0, exp0, sizeof exp0) != 0) {
        DSD_FPRINTF(stderr, "P25p2 LFSR mismatch at offset 0 (start+20)\n");
        DSD_FPRINTF(stderr, " got: ");
        for (int i = 0; i < 16; i++) {
            DSD_FPRINTF(stderr, "%02X", out0[i]);
        }
        DSD_FPRINTF(stderr, "\n exp: ");
        for (int i = 0; i < 16; i++) {
            DSD_FPRINTF(stderr, "%02X", exp0[i]);
        }
        DSD_FPRINTF(stderr, "\n");
        rc = 1;
    }
    if (memcmp(out1, exp1, sizeof exp1) != 0) {
        DSD_FPRINTF(stderr, "P25p2 LFSR mismatch at offset 1 (start+20+360)\n");
        DSD_FPRINTF(stderr, " got: ");
        for (int i = 0; i < 16; i++) {
            DSD_FPRINTF(stderr, "%02X", out1[i]);
        }
        DSD_FPRINTF(stderr, "\n exp: ");
        for (int i = 0; i < 16; i++) {
            DSD_FPRINTF(stderr, "%02X", exp1[i]);
        }
        DSD_FPRINTF(stderr, "\n");
        rc = 1;
    }
    if (memcmp(out4, exp4, sizeof exp4) != 0) {
        DSD_FPRINTF(stderr, "P25p2 LFSR mismatch at offset 4 (start+20+4*360)\n");
        DSD_FPRINTF(stderr, " got: ");
        for (int i = 0; i < 16; i++) {
            DSD_FPRINTF(stderr, "%02X", out4[i]);
        }
        DSD_FPRINTF(stderr, "\n exp: ");
        for (int i = 0; i < 16; i++) {
            DSD_FPRINTF(stderr, "%02X", exp4[i]);
        }
        DSD_FPRINTF(stderr, "\n");
        rc = 1;
    }
    if (memcmp(out8, exp8, sizeof exp8) != 0) {
        DSD_FPRINTF(stderr, "P25p2 LFSR mismatch at offset 8 (start+20+8*360)\n");
        DSD_FPRINTF(stderr, " got: ");
        for (int i = 0; i < 16; i++) {
            DSD_FPRINTF(stderr, "%02X", out8[i]);
        }
        DSD_FPRINTF(stderr, "\n exp: ");
        for (int i = 0; i < 16; i++) {
            DSD_FPRINTF(stderr, "%02X", exp8[i]);
        }
        DSD_FPRINTF(stderr, "\n");
        rc = 1;
    }

    if (rc == 0) {
        DSD_FPRINTF(stderr, "P25p2 LFSR taps/sequence verified (4 vectors)\n");
    }
    rc |= test_p25_voice_lfsr_helpers();
    return rc;
}
