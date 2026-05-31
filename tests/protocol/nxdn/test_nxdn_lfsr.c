// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Regression vectors for NXDN voice scrambler and AES IV LFSR behavior.
 */

#include <dsd-neo/core/state.h>
#include <dsd-neo/protocol/nxdn/nxdn_lfsr.h>
#include <stdint.h>
#include <stdio.h>
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

static int
expect_ull(const char* tag, unsigned long long got, unsigned long long want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got 0x%llX want 0x%llX\n", tag, got, want);
        return 1;
    }
    return 0;
}

static int
expect_byte(const char* tag, uint8_t got, uint8_t want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got 0x%02X want 0x%02X\n", tag, got, want);
        return 1;
    }
    return 0;
}

static int
expect_char_bit(const char* tag, int index, char got, char want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s[%d]: got %d want %d\n", tag, index, (int)got, (int)want);
        return 1;
    }
    return 0;
}

static int
test_lfsrn_voice_scrambler_vector(void) {
    static const char expected[49] = {
        1, 0, 0, 0, 0, 1, 1, 0, 1, 1, 1, 0, 0, 0, 1, 0, 0, 1, 0, 0, 0, 0, 1, 1, 0,
        0, 0, 1, 1, 0, 0, 0, 1, 1, 0, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 1, 1, 1, 1,
    };

    static dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));
    state.payload_miN = 0x1234ULL;

    char input[49];
    char output[49];
    DSD_MEMSET(output, 0, sizeof(output));
    for (int i = 0; i < 49; i++) {
        input[i] = (char)(((i * 3) + 1) & 1);
    }

    LFSRN(input, output, &state);

    int rc = expect_ull("lfsrn-advanced-seed", state.payload_miN, 0x3BDEULL);
    for (int i = 0; i < 49; i++) {
        char tag[40];
        DSD_SNPRINTF(tag, sizeof(tag), "%s", "lfsrn-output");
        rc |= expect_char_bit(tag, i, output[i], expected[i]);
    }
    return rc;
}

static int
test_lfsr128n_aes_iv_vector(void) {
    static const uint8_t expected[16] = {0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF,
                                         0x20, 0xB1, 0x25, 0xE7, 0x79, 0xD0, 0xF3, 0x4E};

    static dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));
    state.payload_miN = 0x0123456789ABCDEFULL;
    DSD_MEMSET(state.aes_iv, 0xA5, sizeof(state.aes_iv));

    LFSR128n(&state);

    int rc = 0;
    for (int i = 0; i < 16; i++) {
        char tag[40];
        DSD_SNPRINTF(tag, sizeof(tag), "lfsr128n-iv-%02d", i);
        rc |= expect_byte(tag, state.aes_iv[i], expected[i]);
    }
    return rc;
}

int
main(void) {
    int rc = 0;
    rc |= test_lfsrn_voice_scrambler_vector();
    rc |= test_lfsr128n_aes_iv_vector();

    if (rc == 0) {
        printf("NXDN_LFSR: OK\n");
    }
    return rc;
}
