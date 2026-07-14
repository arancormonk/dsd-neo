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
#include "test_support.h"

static int
expect_int(const char* label, int got, int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: expected %d, got %d\n", label, want, got);
        return 1;
    }
    return 0;
}

static int
expect_char_frame(const char* label, const char got[49], const char want[49]) {
    for (int i = 0; i < 49; i++) {
        int got_bit = ((unsigned char)got[i]) & 1U;
        int want_bit = ((unsigned char)want[i]) & 1U;
        if (got_bit != want_bit) {
            DSD_FPRINTF(stderr, "%s: bit %d expected %d, got %d\n", label, i, want_bit, got_bit);
            return 1;
        }
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
expect_frame_string(const char* label, const char got[49], const char* want) {
    for (int i = 0; i < 49; i++) {
        int got_bit = ((unsigned char)got[i]) & 1U;
        int want_bit = want[i] - '0';
        if (got_bit != want_bit) {
            DSD_FPRINTF(stderr, "%s: bit %d expected %d, got %d\n", label, i, want_bit, got_bit);
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
load_voice_frame(char frame[49]) {
    for (int i = 0; i < 49; i++) {
        frame[i] = (char)((i * 7 + 1) & 1);
    }
}

static int
test_baofeng_128_apply_vector(void) {
    static const char expect[] = "0110111111011011011100101111011110110100000100110";
    static dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));
    int parse_rc = baofeng_ap_pc5_keystream_creation(&state, "0123456789ABCDEF FEDCBA9876543210", 0);

    char frame[49];
    load_voice_frame(frame);

    int rc = 0;
    rc |= expect_int("baofeng 128 parse", parse_rc, 0);
    rc |= expect_int("baofeng 128 flag", state.baofeng_ap, 1);
    rc |= expect_int("baofeng 128 apply", baofeng_pc5_apply_frame49(&state, frame), 1);
    rc |= expect_frame_string("baofeng 128 vector", frame, expect);
    return rc;
}

static int
test_baofeng_128_lowercase_apply_vector(void) {
    static const char expect[] = "0110111111011011011100101111011110110100000100110";
    static dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));
    int parse_rc = baofeng_ap_pc5_keystream_creation(&state, "0123456789abcdef fedcba9876543210", 0);

    char frame[49];
    load_voice_frame(frame);

    int rc = 0;
    rc |= expect_int("baofeng 128 lowercase parse", parse_rc, 0);
    rc |= expect_int("baofeng 128 lowercase flag", state.baofeng_ap, 1);
    rc |= expect_int("baofeng 128 lowercase apply", baofeng_pc5_apply_frame49(&state, frame), 1);
    rc |= expect_frame_string("baofeng 128 lowercase vector", frame, expect);
    return rc;
}

static int
test_baofeng_256_apply_vector_uses_ascii_hex(void) {
    static const char expect[] = "1011101110110010100111001011000101011000011001111";
    static dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));
    int parse_rc = baofeng_ap_pc5_keystream_creation(
        &state, "0001020304050607 08090A0B0C0D0E0F 1011121314151617 18191A1B1C1D1E1F", 0);

    char frame[49];
    load_voice_frame(frame);

    int rc = 0;
    rc |= expect_int("baofeng 256 parse", parse_rc, 0);
    rc |= expect_int("baofeng 256 flag", state.baofeng_ap, 1);
    rc |= expect_int("baofeng 256 apply", baofeng_pc5_apply_frame49(&state, frame), 1);
    rc |= expect_frame_string("baofeng 256 vector", frame, expect);
    return rc;
}

static int
test_baofeng_256_apply_vector_preserves_ascii_case(void) {
    static const char expect[] = "0101111010110000000010000110011111111110100100111";
    static dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));
    int parse_rc = baofeng_ap_pc5_keystream_creation(
        &state, "0001020304050607 08090a0b0c0d0e0f 1011121314151617 18191a1b1c1d1e1f", 0);

    char frame[49];
    load_voice_frame(frame);

    int rc = 0;
    rc |= expect_int("baofeng 256 lowercase parse", parse_rc, 0);
    rc |= expect_int("baofeng 256 lowercase flag", state.baofeng_ap, 1);
    rc |= expect_int("baofeng 256 lowercase apply", baofeng_pc5_apply_frame49(&state, frame), 1);
    rc |= expect_frame_string("baofeng 256 lowercase vector", frame, expect);
    return rc;
}

static int
test_baofeng_rejects_invalid_hex(void) {
    static dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));
    int parse_rc = baofeng_ap_pc5_keystream_creation(&state, "0123456789ABCDEZ FEDCBA9876543210", 0);

    int rc = 0;
    rc |= expect_int("baofeng invalid parse", parse_rc, -1);
    rc |= expect_int("baofeng invalid flag", state.baofeng_ap, 0);
    return rc;
}

static int
test_baofeng_rejects_null_empty_and_bad_length(void) {
    static dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));
    state.baofeng_ap = 1;

    int rc = 0;
    rc |= expect_int("baofeng null state", baofeng_ap_pc5_keystream_creation(NULL, "0123456789ABCDEF", 0), -1);
    rc |= expect_int("baofeng null input", baofeng_ap_pc5_keystream_creation(&state, NULL, 0), -1);
    rc |= expect_int("baofeng null input preserves flag", state.baofeng_ap, 1);

    DSD_MEMSET(&state, 0, sizeof(state));
    rc |= expect_int("baofeng empty input", baofeng_ap_pc5_keystream_creation(&state, "", 0), -1);
    rc |= expect_int("baofeng empty flag", state.baofeng_ap, 0);

    DSD_MEMSET(&state, 0, sizeof(state));
    rc |= expect_int("baofeng bad length", baofeng_ap_pc5_keystream_creation(&state, "0123456789ABCDEF", 0), -1);
    rc |= expect_int("baofeng bad length flag", state.baofeng_ap, 0);

    DSD_MEMSET(&state, 0, sizeof(state));
    rc |= expect_int("baofeng overlong input",
                     baofeng_ap_pc5_keystream_creation(
                         &state, "0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEFF0", 0),
                     -1);
    rc |= expect_int("baofeng overlong flag", state.baofeng_ap, 0);
    return rc;
}

static int
test_baofeng_apply_inactive_and_null_guards(void) {
    static dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));

    char frame[49];
    char original[49];
    for (int i = 0; i < 49; i++) {
        frame[i] = (char)((i * 5 + 1) & 1);
        original[i] = frame[i];
    }

    int rc = 0;
    rc |= expect_int("baofeng apply null state", baofeng_pc5_apply_frame49(NULL, frame), 0);
    rc |= expect_char_frame("baofeng null state preserves frame", frame, original);
    rc |= expect_int("baofeng apply inactive", baofeng_pc5_apply_frame49(&state, frame), 0);
    rc |= expect_char_frame("baofeng inactive preserves frame", frame, original);
    state.baofeng_ap = 1;
    rc |= expect_int("baofeng apply null frame", baofeng_pc5_apply_frame49(&state, NULL), 0);
    return rc;
}

static int
test_baofeng_apply_skips_silence_and_zero_tail(void) {
    static dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));
    (void)baofeng_ap_pc5_keystream_creation(&state, "0123456789ABCDEF FEDCBA9876543210", 0);

    int rc = 0;
    char silence[49];
    fill_default_silence(silence);
    char original_silence[49];
    DSD_MEMCPY(original_silence, silence, sizeof(original_silence));
    rc |= expect_int("baofeng skip silence applied", baofeng_pc5_apply_frame49(&state, silence), 0);
    rc |= expect_char_frame("baofeng skip silence frame", silence, original_silence);

    char zero_tail[49] = {0};
    for (int i = 0; i < 24; i++) {
        zero_tail[i] = (char)((i + 1) & 1);
    }
    for (int i = 44; i < 49; i++) {
        zero_tail[i] = (char)(i & 1);
    }
    char original_zero_tail[49];
    DSD_MEMCPY(original_zero_tail, zero_tail, sizeof(original_zero_tail));
    rc |= expect_int("baofeng skip zero-tail applied", baofeng_pc5_apply_frame49(&state, zero_tail), 0);
    rc |= expect_char_frame("baofeng skip zero-tail frame", zero_tail, original_zero_tail);
    return rc;
}

static int
test_baofeng_key_log_respects_show_keys(void) {
    static dsd_state state;
    int rc = 0;

    dsd_test_capture_stderr cap;
    DSD_MEMSET(&state, 0, sizeof(state));
    if (dsd_test_capture_stderr_begin(&cap, "pc5_redacted") != 0) {
        DSD_FPRINTF(stderr, "failed to capture stderr for PC5 redacted log\n");
        return 1;
    }
    rc |= expect_int("baofeng redacted parse",
                     baofeng_ap_pc5_keystream_creation(&state, "0123456789ABCDEF FEDCBA9876543210", 0), 0);
    rc |= dsd_test_capture_stderr_end(&cap);
    rc |= expect_file_contains("baofeng redacted marker", cap.path, "[redacted]", 1);
    rc |= expect_file_contains("baofeng redacted key", cap.path, "0123456789ABCDEFFEDCBA9876543210", 0);
    (void)remove(cap.path);

    DSD_MEMSET(&state, 0, sizeof(state));
    if (dsd_test_capture_stderr_begin(&cap, "pc5_revealed") != 0) {
        DSD_FPRINTF(stderr, "failed to capture stderr for PC5 revealed log\n");
        return 1;
    }
    rc |= expect_int("baofeng revealed parse",
                     baofeng_ap_pc5_keystream_creation(&state, "0123456789ABCDEF FEDCBA9876543210", 1), 0);
    rc |= dsd_test_capture_stderr_end(&cap);
    rc |= expect_file_contains("baofeng revealed key", cap.path, "0123456789ABCDEFFEDCBA9876543210", 1);
    (void)remove(cap.path);

    DSD_MEMSET(&state, 0, sizeof(state));
    if (dsd_test_capture_stderr_begin(&cap, "pc5_256_revealed") != 0) {
        DSD_FPRINTF(stderr, "failed to capture stderr for PC5-256 revealed log\n");
        return 1;
    }
    rc |= expect_int("baofeng 256 revealed parse",
                     baofeng_ap_pc5_keystream_creation(
                         &state, "0001020304050607 08090A0B0C0D0E0F 1011121314151617 18191A1B1C1D1E1F", 1),
                     0);
    rc |= dsd_test_capture_stderr_end(&cap);
    rc |= expect_file_contains("baofeng 256 revealed key", cap.path,
                               "000102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1F", 1);
    (void)remove(cap.path);

    return rc;
}

int
main(void) {
    int rc = 0;
    rc |= test_baofeng_128_apply_vector();
    rc |= test_baofeng_128_lowercase_apply_vector();
    rc |= test_baofeng_256_apply_vector_uses_ascii_hex();
    rc |= test_baofeng_256_apply_vector_preserves_ascii_case();
    rc |= test_baofeng_rejects_invalid_hex();
    rc |= test_baofeng_rejects_null_empty_and_bad_length();
    rc |= test_baofeng_apply_inactive_and_null_guards();
    rc |= test_baofeng_apply_skips_silence_and_zero_tail();
    rc |= test_baofeng_key_log_respects_show_keys();
    return rc;
}
