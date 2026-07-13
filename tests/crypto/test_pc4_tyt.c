// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/state.h>
#include <dsd-neo/crypto/dmr_keystream.h>
#include <stdint.h>
#include <stdio.h>
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

static int
expect_int(const char* label, int got, int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: expected %d, got %d\n", label, want, got);
        return 1;
    }
    return 0;
}

static int
expect_ambe_rows(const char* label, char got[4][24], const char* const want[4]) {
    for (int row = 0; row < 4; row++) {
        for (int col = 0; col < 24; col++) {
            int expected = want[row][col] - '0';
            int actual = got[row][col] & 1;
            if (actual != expected) {
                DSD_FPRINTF(stderr, "%s: row %d col %d expected %d, got %d\n", label, row, col, expected, actual);
                return 1;
            }
        }
    }
    return 0;
}

static int
expect_char_frame(const char* label, const char got[49], const char want[49]) {
    for (int i = 0; i < 49; i++) {
        int actual = ((unsigned char)got[i]) & 1U;
        int expected = ((unsigned char)want[i]) & 1U;
        if (actual != expected) {
            DSD_FPRINTF(stderr, "%s: bit %d expected %d, got %d\n", label, i, expected, actual);
            return 1;
        }
    }
    return 0;
}

static int
expect_frame_string(const char* label, const char got[49], const char* want) {
    for (int i = 0; i < 49; i++) {
        int actual = ((unsigned char)got[i]) & 1U;
        int expected = want[i] - '0';
        if (actual != expected) {
            DSD_FPRINTF(stderr, "%s: bit %d expected %d, got %d\n", label, i, expected, actual);
            return 1;
        }
    }
    return 0;
}

static void
fill_default_silence(char frame[49]) {
    static const uint64_t k_ambe_default_silence = 0xF801A99F8CE080ULL;
    for (int i = 0; i < 49; i++) {
        frame[i] = (char)((k_ambe_default_silence >> (55 - i)) & 1U);
    }
}

static void
fill_zero_tail(char frame[49]) {
    for (int i = 0; i < 24; i++) {
        frame[i] = (char)((i + 1) & 1U);
    }
    for (int i = 24; i < 44; i++) {
        frame[i] = 0;
    }
    for (int i = 44; i < 49; i++) {
        frame[i] = (char)(i & 1U);
    }
}

static void
load_voice_frame(char frame[49]) {
    for (int i = 0; i < 49; i++) {
        frame[i] = (char)((i * 7 + 1) & 1);
    }
}

static void
load_tyt16_fixture(char frame[4][24]) {
    for (int row = 0; row < 4; row++) {
        for (int col = 0; col < 24; col++) {
            frame[row][col] = (char)(((row * 7) + (col * 3) + 1) & 1);
        }
    }
}

static int
test_tyt16_codeword_keystream_vectors(void) {
    static dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));
    state.H = 0xBEEFULL;

    char first_frame[4][24];
    load_tyt16_fixture(first_frame);
    tyt16_ambe2_codeword_keystream(&state, first_frame, 0);
    static const char* const expect_first[4] = {"010101110011001100110011", "101110111010101010101011",
                                                "010101110110101010101010", "101010101010100101010101"};

    char later_frame[4][24];
    load_tyt16_fixture(later_frame);
    tyt16_ambe2_codeword_keystream(&state, later_frame, 1);
    static const char* const expect_later[4] = {"010101001100110011001100", "111011101110101010101011",
                                                "010111011100101010101010", "101010101010100101010101"};

    int rc = 0;
    rc |= expect_ambe_rows("tyt16 first frame", first_frame, expect_first);
    rc |= expect_ambe_rows("tyt16 later frame", later_frame, expect_later);
    return rc;
}

static int
test_tyt_ap_128_apply_vector(void) {
    static const char expect[] = "1001100011110001111101001011001100111110001000101";
    static dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));
    tyt_ap_pc4_keystream_creation(&state, "736B9A9C5645288B 243AD5CB8701EF8A", 0);

    char frame[49];
    load_voice_frame(frame);

    int rc = 0;
    rc |= expect_int("tyt ap 128 flag", state.tyt_ap, 1);
    rc |= expect_int("tyt ap 128 apply", tyt_ap_pc4_apply_frame49(&state, frame), 1);
    rc |= expect_frame_string("tyt ap 128 vector", frame, expect);
    return rc;
}

static int
test_tyt_ap_256_apply_vector(void) {
    static const char expect[] = "1011101011000001010000101111011100101110010000001";
    static dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));
    tyt_ap_pc4_keystream_creation(&state, "0123456789ABCDEF FEDCBA9876543210 1111222233334444 5555666677778888", 0);

    char frame[49];
    load_voice_frame(frame);

    int rc = 0;
    rc |= expect_int("tyt ap 256 flag", state.tyt_ap, 1);
    rc |= expect_int("tyt ap 256 apply", tyt_ap_pc4_apply_frame49(&state, frame), 1);
    rc |= expect_frame_string("tyt ap 256 vector", frame, expect);
    return rc;
}

static int
test_tyt_ap_256_trailing_zero_chunks_apply_vector(void) {
    static const char expect[] = "1011101110001000011000010101111001110001111001101";
    static dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));
    tyt_ap_pc4_keystream_creation(&state, "0123456789ABCDEF FEDCBA9876543210 0000000000000000 0000000000000000", 0);

    char frame[49];
    load_voice_frame(frame);

    int rc = 0;
    rc |= expect_int("tyt ap 256 zero chunks flag", state.tyt_ap, 1);
    rc |= expect_int("tyt ap 256 zero chunks apply", tyt_ap_pc4_apply_frame49(&state, frame), 1);
    rc |= expect_frame_string("tyt ap 256 zero chunks vector", frame, expect);
    return rc;
}

static int
test_tyt_ap_rejects_malformed_keys(void) {
    int rc = 0;

    static dsd_state bad_char_state;
    DSD_MEMSET(&bad_char_state, 0, sizeof(bad_char_state));
    bad_char_state.tyt_ap = 1;
    char bad_char_input[] = "736B9A9C5645288B 243AD5CB8701EF8Z";
    tyt_ap_pc4_keystream_creation(&bad_char_state, bad_char_input, 0);
    rc |= expect_int("tyt ap bad hex disables flag", bad_char_state.tyt_ap, 0);

    static dsd_state bad_length_state;
    DSD_MEMSET(&bad_length_state, 0, sizeof(bad_length_state));
    bad_length_state.tyt_ap = 1;
    char bad_length_input[] = "736B9A9C5645288B";
    tyt_ap_pc4_keystream_creation(&bad_length_state, bad_length_input, 0);
    rc |= expect_int("tyt ap bad length disables flag", bad_length_state.tyt_ap, 0);

    return rc;
}

static int
test_tyt_ep_aes_vector(void) {
    static const char expect[] = "1011001001101010000010100100011111101111000001100";
    static dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));
    tyt_ep_aes_keystream_creation(&state, "736B9A9C5645288B 243AD5CB8701EF8A", 0);

    char frame[49];
    load_voice_frame(frame);

    int rc = 0;
    rc |= expect_int("tyt ep flag", state.tyt_ep, 1);
    rc |= expect_int("tyt ep apply", tyt_ep_aes_apply_frame49(&state, frame), 1);
    rc |= expect_frame_string("tyt ep aes vector", frame, expect);
    return rc;
}

static int
test_tyt_ap_apply_skips_silence_and_zero_tail(void) {
    static dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));
    state.tyt_ap = 1;

    int rc = 0;
    char inactive[49];
    for (int i = 0; i < 49; i++) {
        inactive[i] = (char)((i + 1) & 1U);
    }
    char inactive_original[49];
    DSD_MEMCPY(inactive_original, inactive, sizeof(inactive_original));
    static dsd_state inactive_state;
    DSD_MEMSET(&inactive_state, 0, sizeof(inactive_state));
    rc |= expect_int("tyt ap null state", tyt_ap_pc4_apply_frame49(NULL, inactive), 0);
    rc |= expect_int("tyt ap null frame", tyt_ap_pc4_apply_frame49(&state, NULL), 0);
    rc |= expect_int("tyt ap inactive state", tyt_ap_pc4_apply_frame49(&inactive_state, inactive), 0);
    rc |= expect_char_frame("tyt ap inactive frame", inactive, inactive_original);

    char silence[49];
    fill_default_silence(silence);
    char original_silence[49];
    DSD_MEMCPY(original_silence, silence, sizeof(original_silence));
    rc |= expect_int("tyt ap skip silence applied", tyt_ap_pc4_apply_frame49(&state, silence), 0);
    rc |= expect_char_frame("tyt ap skip silence frame", silence, original_silence);

    char zero_tail[49];
    fill_zero_tail(zero_tail);
    char original_zero_tail[49];
    DSD_MEMCPY(original_zero_tail, zero_tail, sizeof(original_zero_tail));
    rc |= expect_int("tyt ap skip zero-tail applied", tyt_ap_pc4_apply_frame49(&state, zero_tail), 0);
    rc |= expect_char_frame("tyt ap skip zero-tail frame", zero_tail, original_zero_tail);
    return rc;
}

static int
test_tyt_ep_apply_skips_silence_and_zero_tail(void) {
    static const char active_expect[] = "1011001001101010000010100100011111101111000001100";
    static dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));
    tyt_ep_aes_keystream_creation(&state, "736B9A9C5645288B 243AD5CB8701EF8A", 0);

    int rc = 0;
    char inactive[49];
    for (int i = 0; i < 49; i++) {
        inactive[i] = (char)((i + 1) & 1U);
    }
    char inactive_original[49];
    DSD_MEMCPY(inactive_original, inactive, sizeof(inactive_original));
    static dsd_state inactive_state;
    DSD_MEMSET(&inactive_state, 0, sizeof(inactive_state));
    rc |= expect_int("tyt ep null state", tyt_ep_aes_apply_frame49(NULL, inactive), 0);
    rc |= expect_int("tyt ep null frame", tyt_ep_aes_apply_frame49(&state, NULL), 0);
    rc |= expect_int("tyt ep inactive state", tyt_ep_aes_apply_frame49(&inactive_state, inactive), 0);
    rc |= expect_char_frame("tyt ep inactive frame", inactive, inactive_original);

    char silence[49];
    fill_default_silence(silence);
    char original_silence[49];
    DSD_MEMCPY(original_silence, silence, sizeof(original_silence));
    rc |= expect_int("tyt ep skip silence applied", tyt_ep_aes_apply_frame49(&state, silence), 0);
    rc |= expect_char_frame("tyt ep skip silence frame", silence, original_silence);

    char zero_tail[49];
    fill_zero_tail(zero_tail);
    char original_zero_tail[49];
    DSD_MEMCPY(original_zero_tail, zero_tail, sizeof(original_zero_tail));
    rc |= expect_int("tyt ep skip zero-tail applied", tyt_ep_aes_apply_frame49(&state, zero_tail), 0);
    rc |= expect_char_frame("tyt ep skip zero-tail frame", zero_tail, original_zero_tail);

    char active[49];
    load_voice_frame(active);
    rc |= expect_int("tyt ep active applied", tyt_ep_aes_apply_frame49(&state, active), 1);
    rc |= expect_frame_string("tyt ep active frame", active, active_expect);
    return rc;
}

int
main(void) {
    int rc = 0;
    rc |= test_tyt16_codeword_keystream_vectors();
    rc |= test_tyt_ap_128_apply_vector();
    rc |= test_tyt_ap_256_apply_vector();
    rc |= test_tyt_ap_256_trailing_zero_chunks_apply_vector();
    rc |= test_tyt_ap_rejects_malformed_keys();
    rc |= test_tyt_ep_aes_vector();
    rc |= test_tyt_ap_apply_skips_silence_and_zero_tail();
    rc |= test_tyt_ep_apply_skips_silence_and_zero_tail();
    return rc;
}
