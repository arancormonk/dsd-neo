// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/bp.h>
#include <dsd-neo/crypto/dmr_keystream.h>
#include <stdint.h>
#include <stdio.h>
#include "dsd-neo/core/safe_api.h"

static int
expect_eq_int(const char* tag, int got, int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %d want %d\n", tag, got, want);
        return 1;
    }
    return 0;
}

static int
expect_eq_frame(const char* tag, const char got[49], const char want[49]) {
    for (int i = 0; i < 49; i++) {
        const int got_bit = ((unsigned char)got[i]) & 1U;
        const int want_bit = ((unsigned char)want[i]) & 1U;
        if (got_bit != want_bit) {
            DSD_FPRINTF(stderr, "%s: bit %d got %d want %d\n", tag, i, got_bit, want_bit);
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
legacy_basic_privacy_apply(unsigned long long key_id, char ambe_d[49]) {
    uint64_t k = BPK[key_id];
    k = (((k & 0xFF0FULL) << 32U) + (k << 16U) + k);
    for (int j = 0; j < 48; j++) {
        const int x = (int)(((k << (unsigned)j) & 0x800000000000ULL) >> 47U);
        ambe_d[j] ^= (char)x;
    }
}

static void
legacy_hytera_bp_apply(unsigned long long k1, unsigned long long k2, unsigned long long k3, unsigned long long k4,
                       int frame_idx, char ambe_d[49]) {
    int len = 0;
    if (k2 == 0ULL) {
        len = 39;
        k1 <<= 24;
    } else {
        len = 127;
    }
    if (k4 != 0ULL) {
        len = 255;
    }

    uint8_t t_key[256] = {0};
    uint8_t p_n[882] = {0};
    for (int i = 0; i < 64; i++) {
        t_key[i] = (uint8_t)((k1 >> (63 - i)) & 1ULL);
        t_key[i + 64] = (uint8_t)((k2 >> (63 - i)) & 1ULL);
        t_key[i + 128] = (uint8_t)((k3 >> (63 - i)) & 1ULL);
        t_key[i + 192] = (uint8_t)((k4 >> (63 - i)) & 1ULL);
    }

    int pos = 0;
    for (int i = 0; i < 882; i++) {
        p_n[i] = t_key[pos];
        pos++;
        if (pos > len) {
            pos = 0;
        }
    }

    pos = frame_idx * 49;
    for (int i = 0; i < 49; i++) {
        ambe_d[i] ^= (char)(p_n[pos++] & 1U);
    }
}

static int
test_silence_detection(const char silence[49]) {
    int rc = 0;

    rc |= expect_eq_int("silence-detected", dmr_ambe49_is_default_silence(silence), 1);

    char not_silence[49];
    DSD_MEMCPY(not_silence, silence, sizeof(not_silence));
    not_silence[48] ^= 1;
    rc |= expect_eq_int("silence-rejected", dmr_ambe49_is_default_silence(not_silence), 0);

    {
        char zero_tail[49] = {0};
        zero_tail[0] = 1;
        char active[49] = {0};
        active[0] = 1;
        active[24] = 1;
        rc |= expect_eq_int("voice-stream-skip-silence", dmr_ambe49_should_skip_voice_stream(silence), 1);
        rc |= expect_eq_int("voice-stream-skip-zero-tail", dmr_ambe49_should_skip_voice_stream(zero_tail), 1);
        rc |= expect_eq_int("voice-stream-keep-active", dmr_ambe49_should_skip_voice_stream(active), 0);
    }

    return rc;
}

static int
test_voice_stream_aes_keystream(void) {
    uint8_t ks_bits[128] = {0};
    for (int i = 0; i < 128; i++) {
        ks_bits[i] = (uint8_t)(i & 1);
    }

    char frame[49];
    char expected[49];
    for (int i = 0; i < 49; i++) {
        frame[i] = (char)((i + 1) & 1);
        expected[i] = (char)(frame[i] ^ (char)(ks_bits[i] & 1U));
    }
    frame[24] = 1;
    expected[24] = (char)(frame[24] ^ (char)(ks_bits[24] & 1U));

    int rc = 0;
    long int bit_counter = 0;
    int applied = dmr_voice_stream_apply_frame49(ks_bits, &bit_counter, 0x24, frame);
    rc |= expect_eq_int("voice-stream-applied", applied, 1);
    rc |= expect_eq_int("voice-stream-aes-counter", (int)bit_counter, 56);
    rc |= expect_eq_frame("voice-stream-aes-frame", frame, expected);
    return rc;
}

static int
test_voice_stream_silence_skip(const char silence[49]) {
    uint8_t ks_bits[128] = {1};
    char frame[49];
    char original[49];
    DSD_MEMCPY(frame, silence, sizeof(frame));
    DSD_MEMCPY(original, silence, sizeof(original));

    int rc = 0;
    long int bit_counter = 0;
    int applied = dmr_voice_stream_apply_frame49(ks_bits, &bit_counter, 0x24, frame);
    rc |= expect_eq_int("voice-stream-silence-skipped", applied, 0);
    rc |= expect_eq_int("voice-stream-silence-aes-counter", (int)bit_counter, 56);
    rc |= expect_eq_frame("voice-stream-silence-frame", frame, original);

    bit_counter = 0;
    applied = dmr_voice_stream_apply_frame49(ks_bits, &bit_counter, 0x02, frame);
    rc |= expect_eq_int("voice-stream-hytera-silence-skipped", applied, 0);
    rc |= expect_eq_int("voice-stream-hytera-counter", (int)bit_counter, 49);
    rc |= expect_eq_frame("voice-stream-hytera-frame", frame, original);
    return rc;
}

static int
test_basic_privacy(void) {
    int rc = 0;
    char frame[49] = {0};
    char expected[49] = {0};
    int applied = dmr_basic_privacy_apply_frame49(42ULL, frame);
    legacy_basic_privacy_apply(42ULL, expected);
    rc |= expect_eq_int("bp-applied", applied, 1);
    rc |= expect_eq_frame("bp-bits", frame, expected);
    return rc;
}

static int
test_basic_privacy_rejects_invalid_keys(void) {
    int rc = 0;
    char frame[49] = {0};
    char expected[49] = {0};
    frame[0] = expected[0] = 1;
    int applied = dmr_basic_privacy_apply_frame49(0ULL, frame);
    rc |= expect_eq_int("bp-zero-rejected", applied, 0);
    rc |= expect_eq_frame("bp-zero-unchanged", frame, expected);

    applied = dmr_basic_privacy_apply_frame49(256ULL, frame);
    rc |= expect_eq_int("bp-oob-rejected", applied, 0);
    rc |= expect_eq_frame("bp-oob-unchanged", frame, expected);
    return rc;
}

static int
test_hytera_bp_silence_skip(const char silence[49]) {
    int rc = 0;
    char frame[49];
    char original[49];
    DSD_MEMCPY(frame, silence, sizeof(frame));
    DSD_MEMCPY(original, silence, sizeof(original));
    int frame_counter = 0;
    int applied = hytera_bp_apply_frame49(0x0123456789ULL, 0ULL, 0ULL, 0ULL, &frame_counter, frame);
    rc |= expect_eq_int("skip-silence-applied", applied, 0);
    rc |= expect_eq_int("skip-silence-counter", frame_counter, 1);
    rc |= expect_eq_frame("skip-silence-frame", frame, original);
    return rc;
}

static int
test_hytera_bp_frame_apply(void) {
    int rc = 0;
    char frame[49] = {0};
    char expected[49] = {0};
    int frame_counter = 1;
    int applied = hytera_bp_apply_frame49(0x0123456789ULL, 0ULL, 0ULL, 0ULL, &frame_counter, frame);
    legacy_hytera_bp_apply(0x0123456789ULL, 0ULL, 0ULL, 0ULL, 1, expected);
    rc |= expect_eq_int("apply-frame-applied", applied, 1);
    rc |= expect_eq_int("apply-frame-counter", frame_counter, 2);
    rc |= expect_eq_frame("apply-frame-bits", frame, expected);
    return rc;
}

static int
test_hytera_bp_frame_counter_clamp(void) {
    int rc = 0;
    char frame[49] = {0};
    char expected[49] = {0};
    int frame_counter = 99;
    int applied = hytera_bp_apply_frame49(0x0123456789ULL, 0ULL, 0ULL, 0ULL, &frame_counter, frame);
    legacy_hytera_bp_apply(0x0123456789ULL, 0ULL, 0ULL, 0ULL, 17, expected);
    rc |= expect_eq_int("clamp-applied", applied, 1);
    rc |= expect_eq_int("clamp-counter", frame_counter, 18);
    rc |= expect_eq_frame("clamp-bits", frame, expected);
    return rc;
}

int
main(void) {
    char silence[49] = {0};
    fill_default_silence(silence);

    int rc = 0;
    rc |= test_silence_detection(silence);
    rc |= test_voice_stream_aes_keystream();
    rc |= test_voice_stream_silence_skip(silence);
    rc |= test_basic_privacy();
    rc |= test_basic_privacy_rejects_invalid_keys();
    rc |= test_hytera_bp_silence_skip(silence);
    rc |= test_hytera_bp_frame_apply();
    rc |= test_hytera_bp_frame_counter_clamp();

    if (rc == 0) {
        printf("CORE_HYTERA_BP_SILENCE: OK\n");
    }
    return rc;
}
