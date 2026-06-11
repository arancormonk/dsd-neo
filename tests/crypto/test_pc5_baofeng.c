// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/state.h>
#include <dsd-neo/crypto/dmr_keystream.h>
#include <dsd-neo/crypto/pc5.h>
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
expect_bits_string(const char* label, const short* bits, const char* want) {
    for (size_t i = 0; want[i] != '\0'; i++) {
        int got = bits[i] & 1;
        int expected = want[i] - '0';
        if (got != expected) {
            DSD_FPRINTF(stderr, "%s: bit %zu expected %d, got %d\n", label, i, expected, got);
            return 1;
        }
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
expect_char_short_bits(const char* label, const char got[49], const short want[49]) {
    for (int i = 0; i < 49; i++) {
        int got_bit = ((unsigned char)got[i]) & 1U;
        int want_bit = want[i] & 1;
        if (got_bit != want_bit) {
            DSD_FPRINTF(stderr, "%s: bit %d expected %d, got %d\n", label, i, want_bit, got_bit);
            return 1;
        }
    }
    return 0;
}

static int
expect_pc5_schedule(const char* label, const PC5Context* got, const PC5Context* want) {
    if (got->rounds != want->rounds || memcmp(got->perm, want->perm, sizeof(want->perm)) != 0
        || memcmp(got->new1, want->new1, sizeof(want->new1)) != 0
        || memcmp(got->decal, want->decal, sizeof(want->decal)) != 0
        || memcmp(got->rngxor, want->rngxor, sizeof(want->rngxor)) != 0
        || memcmp(got->rngxor2, want->rngxor2, sizeof(want->rngxor2)) != 0
        || memcmp(got->tab, want->tab, sizeof(want->tab)) != 0 || memcmp(got->inv, want->inv, sizeof(want->inv)) != 0
        || memcmp(got->permut, want->permut, sizeof(want->permut)) != 0
        || memcmp(got->numbers, want->numbers, sizeof(want->numbers)) != 0) {
        DSD_FPRINTF(stderr, "%s: schedule mismatch\n", label);
        return 1;
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
u64_to_bytes_be_local(uint64_t val, unsigned char* out) {
    for (int i = 0; i < 8; i++) {
        out[i] = (unsigned char)((val >> (56 - 8 * i)) & 0xFF);
    }
}

static void
build_pc5_128_key(uint64_t k1, uint64_t k2, unsigned char key2[16]) {
    unsigned char key1[16];
    DSD_MEMSET(key1, 0, sizeof(key1));
    u64_to_bytes_be_local(k1, &key1[0]);
    u64_to_bytes_be_local(k2, &key1[8]);

    for (int i = 0; i < 16; i++) {
        key2[i] = key1[15 - i];
    }
}

static int
test_pc5_decrypt_frame_vector(void) {
    static const char expect[] = "0110111111011011011100101111011110110100000100110";
    unsigned char key[16];
    DSD_MEMSET(key, 0, sizeof(key));
    build_pc5_128_key(0x0123456789ABCDEFULL, 0xFEDCBA9876543210ULL, key);

    DSD_MEMSET(&ctxpc5, 0, sizeof(ctxpc5));
    create_keys_pc5(&ctxpc5, key, sizeof(key));
    ctxpc5.rounds = PC5_NBROUND;

    short frame[49];
    for (int i = 0; i < 49; i++) {
        frame[i] = (short)((i * 7 + 1) & 1);
    }
    decrypt_frame_49_pc5(frame);

    return expect_bits_string("pc5 decrypt vector", ctxpc5.bits, expect);
}

static int
test_baofeng_128_key_schedule(void) {
    PC5Context expected;
    DSD_MEMSET(&expected, 0, sizeof(expected));
    unsigned char key[16];
    DSD_MEMSET(key, 0, sizeof(key));
    build_pc5_128_key(0x0123456789ABCDEFULL, 0xFEDCBA9876543210ULL, key);
    create_keys_pc5(&expected, key, sizeof(key));
    expected.rounds = PC5_NBROUND;

    static dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMSET(&ctxpc5, 0, sizeof(ctxpc5));
    int parse_rc = baofeng_ap_pc5_keystream_creation(&state, "0123456789ABCDEF FEDCBA9876543210", 0);

    int rc = 0;
    rc |= expect_int("baofeng 128 parse", parse_rc, 0);
    rc |= expect_int("baofeng 128 flag", state.baofeng_ap, 1);
    rc |= expect_pc5_schedule("baofeng 128", &ctxpc5, &expected);
    return rc;
}

static int
test_baofeng_256_key_schedule_uses_ascii_hex(void) {
    PC5Context expected;
    DSD_MEMSET(&expected, 0, sizeof(expected));
    const unsigned char key_ascii[] = "000102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1F";
    create_keys_pc5(&expected, key_ascii, strlen((const char*)key_ascii));
    expected.rounds = PC5_NBROUND;

    static dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMSET(&ctxpc5, 0, sizeof(ctxpc5));
    int parse_rc = baofeng_ap_pc5_keystream_creation(
        &state, "0001020304050607 08090A0B0C0D0E0F 1011121314151617 18191A1B1C1D1E1F", 0);

    int rc = 0;
    rc |= expect_int("baofeng 256 parse", parse_rc, 0);
    rc |= expect_int("baofeng 256 flag", state.baofeng_ap, 1);
    rc |= expect_pc5_schedule("baofeng 256", &ctxpc5, &expected);
    return rc;
}

static int
test_baofeng_256_key_schedule_preserves_ascii_case(void) {
    PC5Context expected;
    DSD_MEMSET(&expected, 0, sizeof(expected));
    const unsigned char key_ascii[] = "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f";
    create_keys_pc5(&expected, key_ascii, strlen((const char*)key_ascii));
    expected.rounds = PC5_NBROUND;

    static dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMSET(&ctxpc5, 0, sizeof(ctxpc5));
    int parse_rc = baofeng_ap_pc5_keystream_creation(
        &state, "0001020304050607 08090a0b0c0d0e0f 1011121314151617 18191a1b1c1d1e1f", 0);

    int rc = 0;
    rc |= expect_int("baofeng 256 lowercase parse", parse_rc, 0);
    rc |= expect_int("baofeng 256 lowercase flag", state.baofeng_ap, 1);
    rc |= expect_pc5_schedule("baofeng 256 lowercase", &ctxpc5, &expected);
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
test_baofeng_apply_skips_silence_and_zero_tail(void) {
    static dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMSET(&ctxpc5, 0, sizeof(ctxpc5));
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
test_baofeng_apply_decrypts_voice_frame(void) {
    static dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMSET(&ctxpc5, 0, sizeof(ctxpc5));
    (void)baofeng_ap_pc5_keystream_creation(&state, "0123456789ABCDEF FEDCBA9876543210", 0);

    char frame[49];
    short expected[49];
    for (int i = 0; i < 49; i++) {
        frame[i] = (char)((i * 7 + 1) & 1);
        expected[i] = (short)(frame[i] & 1);
    }
    decrypt_frame_49_pc5(expected);

    int rc = 0;
    rc |= expect_int("baofeng apply voice frame", baofeng_pc5_apply_frame49(&state, frame), 1);
    rc |= expect_char_short_bits("baofeng apply voice bits", frame, expected);
    return rc;
}

static int
test_baofeng_key_log_respects_show_keys(void) {
    static dsd_state state;
    int rc = 0;

    dsd_test_capture_stderr cap;
    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMSET(&ctxpc5, 0, sizeof(ctxpc5));
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
    DSD_MEMSET(&ctxpc5, 0, sizeof(ctxpc5));
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
    DSD_MEMSET(&ctxpc5, 0, sizeof(ctxpc5));
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
    rc |= test_pc5_decrypt_frame_vector();
    rc |= test_baofeng_128_key_schedule();
    rc |= test_baofeng_256_key_schedule_uses_ascii_hex();
    rc |= test_baofeng_256_key_schedule_preserves_ascii_case();
    rc |= test_baofeng_rejects_invalid_hex();
    rc |= test_baofeng_apply_skips_silence_and_zero_tail();
    rc |= test_baofeng_apply_decrypts_voice_frame();
    rc |= test_baofeng_key_log_respects_show_keys();
    return rc;
}
