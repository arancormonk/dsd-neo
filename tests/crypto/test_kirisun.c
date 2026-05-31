// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/state.h>
#include <dsd-neo/crypto/dmr_keystream.h>
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
expect_int(const char* label, int got, int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %d, want %d\n", label, got, want);
        return 1;
    }
    return 0;
}

static int
test_kirisun_universal_vector(void) {
    static const uint8_t expect[] = {0x53, 0xBB, 0xE9, 0x2A, 0xB4, 0x79, 0x45, 0x62, 0x16, 0xBA, 0xDB, 0xD6, 0xF3, 0xA5,
                                     0x56, 0xB1, 0xA6, 0x6A, 0x85, 0xF2, 0x87, 0x7C, 0x5C, 0xF2, 0x9E, 0xC3, 0xE7, 0x2E,
                                     0xE3, 0x33, 0x3F, 0xFE, 0x58, 0x1E, 0x03, 0x26, 0xAE, 0xB3, 0x27, 0x84};
    static dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));
    state.currentslot = 0;
    state.payload_mi = 0x11223344ULL;
    state.A1[0] = 0x3333333333333333ULL;
    state.A2[0] = 0x3333333333333333ULL;
    state.A3[0] = 0x3333333333333333ULL;
    state.A4[0] = 0x3333333333333333ULL;
    state.aes_key_segments[0] = 4U;

    kirisun_uni_keystream_creation(&state);
    return expect_int("kirisun universal derived loaded", state.aes_key_loaded[0], 1)
           | expect_bytes("kirisun universal", state.ks_octetL, expect, sizeof(expect));
}

static int
test_kirisun_advanced_vector(void) {
    static const uint8_t expect[] = {0x23, 0x87, 0x8D, 0xE2, 0xC6, 0x4A, 0x00, 0x84, 0x92, 0x3E, 0xE9, 0x93, 0x7C, 0x00,
                                     0x5E, 0xA2, 0xC1, 0x72, 0xE7, 0xFB, 0x00, 0xBB, 0xF8, 0x4B, 0x10, 0x74, 0xCC, 0x00,
                                     0x84, 0xF2, 0xFB, 0x42, 0x3B, 0x60, 0x00, 0xEA, 0xF8, 0xCD, 0xC1, 0x46};
    static dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));
    state.currentslot = 1;
    state.payload_miR = 0xA1B2C3D4ULL;
    state.A1[1] = 0xDC1A7E9F9BF312DBULL;
    state.A2[1] = 0xF45010CEC5F7A53AULL;
    state.A3[1] = 0xC407D0BFA803617BULL;
    state.A4[1] = 0xE426A7254DA9390DULL;
    state.aes_key_segments[1] = 4U;

    kirisun_adv_keystream_creation(&state);
    return expect_int("kirisun advanced derived loaded", state.aes_key_loaded[1], 1)
           | expect_bytes("kirisun advanced", state.ks_octetR, expect, sizeof(expect));
}

static int
test_kirisun_incomplete_key_clears_loaded(void) {
    static dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMSET(state.ks_octetL, 0xA5, sizeof(state.ks_octetL));
    state.currentslot = 0;
    state.payload_mi = 0x11223344ULL;
    state.A1[0] = 0x3333333333333333ULL;
    state.aes_key_loaded[0] = 1;
    state.aes_key_segments[0] = 1U;

    kirisun_uni_keystream_creation(&state);

    uint8_t expect[sizeof(state.ks_octetL)];
    DSD_MEMSET(expect, 0xA5, sizeof(expect));
    return expect_int("kirisun incomplete clears loaded", state.aes_key_loaded[0], 0)
           | expect_bytes("kirisun incomplete leaves stream", state.ks_octetL, expect, sizeof(expect));
}

static int
test_kirisun_zero_word_clears_loaded(void) {
    static dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMSET(state.ks_octetL, 0xA5, sizeof(state.ks_octetL));
    state.currentslot = 0;
    state.payload_mi = 0x11223344ULL;
    state.A1[0] = 0x3333333333333333ULL;
    state.A2[0] = 0x3333333333333333ULL;
    state.A3[0] = 0x3333333333333333ULL;
    state.A4[0] = 0x0000000000000000ULL;
    state.aes_key_loaded[0] = 1;
    state.aes_key_segments[0] = 4U;

    uint8_t previous[sizeof(state.ks_octetL)];
    DSD_MEMSET(previous, 0xA5, sizeof(previous));
    kirisun_uni_keystream_creation(&state);

    return expect_int("kirisun zero word clears loaded", state.aes_key_loaded[0], 0)
           | expect_bytes("kirisun zero word leaves stream", state.ks_octetL, previous, sizeof(previous));
}

static int
test_kirisun_all_zero_key_clears_loaded(void) {
    static dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMSET(state.ks_octetL, 0xA5, sizeof(state.ks_octetL));
    state.currentslot = 0;
    state.payload_mi = 0x11223344ULL;
    state.aes_key_loaded[0] = 1;
    state.aes_key_segments[0] = 4U;

    kirisun_uni_keystream_creation(&state);

    uint8_t expect[sizeof(state.ks_octetL)];
    DSD_MEMSET(expect, 0xA5, sizeof(expect));
    return expect_int("kirisun all-zero clears loaded", state.aes_key_loaded[0], 0)
           | expect_bytes("kirisun all-zero leaves stream", state.ks_octetL, expect, sizeof(expect));
}

int
main(void) {
    int rc = 0;
    rc |= test_kirisun_universal_vector();
    rc |= test_kirisun_advanced_vector();
    rc |= test_kirisun_incomplete_key_clears_loaded();
    rc |= test_kirisun_zero_word_clears_loaded();
    rc |= test_kirisun_all_zero_key_clears_loaded();
    return rc;
}
