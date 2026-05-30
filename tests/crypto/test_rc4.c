// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/state.h>
#include <dsd-neo/crypto/rc4.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

static int
expect_bytes(const char* label, const uint8_t* got, const uint8_t* want, size_t len) {
    if (memcmp(got, want, len) != 0) {
        DSD_FPRINTF(stderr, "%s: byte mismatch\n", label);
        return 1;
    }
    return 0;
}

static int
test_rc4_voice_known_vector(void) {
    const uint8_t key[] = {'K', 'e', 'y'};
    const uint8_t cipher[] = {0xBB, 0xF3, 0x16, 0xE8, 0xD9, 0x40, 0xAF, 0x0A, 0xD3};
    const uint8_t expect[] = {'P', 'l', 'a', 'i', 'n', 't', 'e', 'x', 't'};
    uint8_t plain[sizeof(cipher)];
    DSD_MEMSET(plain, 0, sizeof(plain));

    rc4_voice_decrypt(0, (uint8_t)sizeof(key), (uint8_t)sizeof(cipher), key, cipher, plain);
    return expect_bytes("rc4 voice vector", plain, expect, sizeof(expect));
}

static int
test_rc4_block_known_vector(void) {
    const uint8_t key[] = {'K', 'e', 'y'};
    const uint8_t expect[] = {0xEB, 0x9F, 0x77, 0x81, 0xB7, 0x34, 0xCA, 0x72, 0xA7};
    uint8_t output[sizeof(expect)];
    DSD_MEMSET(output, 0, sizeof(output));

    rc4_block_output(0, (int)sizeof(key), (int)sizeof(output), key, output);
    return expect_bytes("rc4 block vector", output, expect, sizeof(expect));
}

static int
test_rc4_block_drop_skips_prefix(void) {
    const uint8_t key[] = {'K', 'e', 'y'};
    const uint8_t expect[] = {0x77, 0x81, 0xB7, 0x34, 0xCA, 0x72, 0xA7};
    uint8_t output[sizeof(expect)];
    DSD_MEMSET(output, 0, sizeof(output));

    rc4_block_output(2, (int)sizeof(key), (int)sizeof(output), key, output);
    return expect_bytes("rc4 block drop prefix", output, expect, sizeof(expect));
}

static int
test_hytera_enhanced_setup_slot_selection(void) {
    static const uint8_t expect[] = {0x13, 0x9A, 0xC2, 0xA2, 0x51, 0x9C, 0x63, 0x86, 0x6B, 0x62,
                                     0xF3, 0xE9, 0xAB, 0xB6, 0xB9, 0x09, 0xCA, 0x23, 0x33, 0xEE};
    static dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));
    state.currentslot = 1;

    hytera_enhanced_rc4_setup(NULL, &state, 0x0102030405ULL, 0xA0A1A2A3A4ULL);
    return expect_bytes("hytera enhanced slot 2", state.ks_octetR, expect, sizeof(expect));
}

int
main(void) {
    int rc = 0;
    rc |= test_rc4_voice_known_vector();
    rc |= test_rc4_block_known_vector();
    rc |= test_rc4_block_drop_skips_prefix();
    rc |= test_hytera_enhanced_setup_slot_selection();
    return rc;
}
