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
#include "test_support.h"

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
expect_file_contains(const char* label, const char* path, const char* needle, int want_contains) {
    FILE* fp = fopen(path, "rb");
    if (!fp) {
        DSD_FPRINTF(stderr, "%s: failed to open capture file\n", label);
        return 1;
    }

    char buf[512];
    size_t n = fread(buf, 1U, sizeof(buf) - 1U, fp);
    fclose(fp);
    buf[n] = '\0';

    const int contains = strstr(buf, needle) != NULL;
    if (contains != want_contains) {
        DSD_FPRINTF(stderr, "%s: expected contains=%d for \"%s\", got %d in \"%s\"\n", label, want_contains, needle,
                    contains, buf);
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

    int parse_rc = connect_systems_ee72_key_creation(&state, "0x11 22 33 44 55 66 77 88 99", 0);

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

    int parse_rc = connect_systems_ee72_key_creation(&state, "1122334455667788", 0);

    int rc = 0;
    rc |= expect_int("ee72 invalid parse", parse_rc, -1);
    rc |= expect_int("ee72 invalid flag", state.csi_ee, 0);
    return rc;
}

static int
test_ee72_rejects_null_and_malformed_inputs(void) {
    static const uint8_t original_key[] = {0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8};
    static dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMCPY(state.csi_ee_key, original_key, sizeof(original_key));
    state.csi_ee = 1;

    int rc = 0;
    rc |= expect_int("ee72 null state", connect_systems_ee72_key_creation(NULL, "112233445566778899", 0), -1);
    rc |= expect_int("ee72 null input", connect_systems_ee72_key_creation(&state, NULL, 0), -1);
    rc |= expect_int("ee72 null input preserves flag", state.csi_ee, 1);
    rc |= expect_bytes("ee72 null input preserves key", state.csi_ee_key, original_key, sizeof(original_key));

    DSD_MEMSET(&state, 0, sizeof(state));
    rc |= expect_int("ee72 invalid character", connect_systems_ee72_key_creation(&state, "11223344zz66778899", 0), -1);
    rc |= expect_int("ee72 invalid character flag", state.csi_ee, 0);

    DSD_MEMSET(&state, 0, sizeof(state));
    rc |= expect_int("ee72 overlong input",
                     connect_systems_ee72_key_creation(&state, "112233445566778899AABBCCDDEEFF001122", 0), -1);
    rc |= expect_int("ee72 overlong flag", state.csi_ee, 0);
    return rc;
}

static int
test_ee72_accepts_leading_space_upper_prefix_and_lowercase(void) {
    static const uint8_t expect[] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00, 0x11, 0x22};
    static dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));

    int parse_rc = connect_systems_ee72_key_creation(&state, "\t 0Xaa bb cc dd ee ff 00 11 22", 0);

    int rc = 0;
    rc |= expect_int("ee72 lowercase parse", parse_rc, 0);
    rc |= expect_int("ee72 lowercase flag", state.csi_ee, 1);
    rc |= expect_bytes("ee72 lowercase key", state.csi_ee_key, expect, sizeof(expect));
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

static int
test_csi72_inactive_keeps_frame(void) {
    static dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));

    char frame[4][24];
    char original[4][24];
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 24; c++) {
            frame[r][c] = (char)(((r * 24 + c) + 1) & 1);
            original[r][c] = frame[r][c];
        }
    }

    csi72_ambe2_codeword_keystream(NULL, frame);
    int rc = expect_bytes("csi72 null state preserves frame", (const uint8_t*)frame, (const uint8_t*)original,
                          sizeof(frame));

    csi72_ambe2_codeword_keystream(&state, frame);
    rc |= expect_bytes("csi72 inactive state preserves frame", (const uint8_t*)frame, (const uint8_t*)original,
                       sizeof(frame));
    return rc;
}

static int
test_ee72_key_log_respects_show_keys(void) {
    static dsd_state state;
    int rc = 0;

    dsd_test_capture_stderr cap;
    DSD_MEMSET(&state, 0, sizeof(state));
    if (dsd_test_capture_stderr_begin(&cap, "ee72_redacted") != 0) {
        DSD_FPRINTF(stderr, "failed to capture stderr for EE72 redacted log\n");
        return 1;
    }
    rc |= expect_int("ee72 redacted parse", connect_systems_ee72_key_creation(&state, "112233445566778899", 0), 0);
    rc |= dsd_test_capture_stderr_end(&cap);
    rc |= expect_file_contains("ee72 redacted marker", cap.path, "[redacted]", 1);
    rc |= expect_file_contains("ee72 redacted key", cap.path, "112233445566778899", 0);
    (void)remove(cap.path);

    DSD_MEMSET(&state, 0, sizeof(state));
    if (dsd_test_capture_stderr_begin(&cap, "ee72_revealed") != 0) {
        DSD_FPRINTF(stderr, "failed to capture stderr for EE72 revealed log\n");
        return 1;
    }
    rc |= expect_int("ee72 revealed parse", connect_systems_ee72_key_creation(&state, "112233445566778899", 1), 0);
    rc |= dsd_test_capture_stderr_end(&cap);
    rc |= expect_file_contains("ee72 revealed key", cap.path, "112233445566778899", 1);
    (void)remove(cap.path);

    return rc;
}

int
main(void) {
    int rc = 0;
    rc |= test_ee72_key_parse();
    rc |= test_ee72_rejects_invalid_length();
    rc |= test_ee72_rejects_null_and_malformed_inputs();
    rc |= test_ee72_accepts_leading_space_upper_prefix_and_lowercase();
    rc |= test_csi72_frame_transform_vector();
    rc |= test_csi72_inactive_keeps_frame();
    rc |= test_ee72_key_log_respects_show_keys();
    return rc;
}
