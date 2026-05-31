// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/bit_packing.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/crypto/dmr_keystream.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

void
unpack_byte_array_into_bit_array(const uint8_t* input, uint8_t* output, int len) {
    for (int i = 0; i < len; i++) {
        for (int bit = 0; bit < 8; bit++) {
            output[(i * 8) + bit] = (uint8_t)((input[i] >> (7 - bit)) & 1U);
        }
    }
}

static int
expect_int(const char* label, int got, int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: expected %d, got %d\n", label, want, got);
        return 1;
    }
    return 0;
}

static int
expect_bytes(const char* label, const uint8_t* got, const uint8_t* want, size_t len) {
    if (memcmp(got, want, len) != 0) {
        DSD_FPRINTF(stderr, "%s: byte mismatch\n", label);
        return 1;
    }
    return 0;
}

static int
expect_frame_string(const char* label, char frame[4][24], const char* want) {
    size_t pos = 0;
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 24; c++) {
            int got = frame[r][c] & 1;
            int expected = want[pos++] - '0';
            if (got != expected) {
                DSD_FPRINTF(stderr, "%s: bit %zu expected %d, got %d\n", label, pos - 1U, expected, got);
                return 1;
            }
        }
    }
    return 0;
}

static int
test_ee72_key_parse(void) {
    static const uint8_t expect[] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99};
    static dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));

    int parse_rc = connect_systems_ee72_key_creation(&state, "0x11 22 33 44 55 66 77 88 99");

    int rc = 0;
    rc |= expect_int("ee72 parse", parse_rc, 0);
    rc |= expect_int("ee72 flag", state.csi_ee, 1);
    rc |= expect_bytes("ee72 key", state.csi_ee_key, expect, sizeof(expect));
    return rc;
}

static int
test_ee72_rejects_invalid_length(void) {
    static dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));

    int parse_rc = connect_systems_ee72_key_creation(&state, "1122334455667788");

    int rc = 0;
    rc |= expect_int("ee72 invalid parse", parse_rc, -1);
    rc |= expect_int("ee72 invalid flag", state.csi_ee, 0);
    return rc;
}

static int
test_csi72_frame_transform_vector(void) {
    static const uint8_t key[] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99};
    static const char expect[] =
        "010110011001100110011001010100101010101101001010101010100100101010101010010110101010101010101010";

    static dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMCPY(state.csi_ee_key, key, sizeof(key));
    state.csi_ee = 1;

    char frame[4][24];
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 24; c++) {
            frame[r][c] = (char)(((r * 24 + c) * 3 + 1) & 1);
        }
    }

    csi72_ambe2_codeword_keystream(&state, frame);
    return expect_frame_string("csi72 frame", frame, expect);
}

int
main(void) {
    int rc = 0;
    rc |= test_ee72_key_parse();
    rc |= test_ee72_rejects_invalid_length();
    rc |= test_csi72_frame_transform_vector();
    return rc;
}
