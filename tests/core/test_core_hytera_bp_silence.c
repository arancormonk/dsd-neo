// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/crypto/dmr_keystream.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int
expect_eq_int(const char* tag, int got, int want) {
    if (got != want) {
        fprintf(stderr, "%s: got %d want %d\n", tag, got, want);
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
            fprintf(stderr, "%s: bit %d got %d want %d\n", tag, i, got_bit, want_bit);
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

int
main(void) {
    int rc = 0;

    char silence[49] = {0};
    fill_default_silence(silence);
    rc |= expect_eq_int("silence-detected", dmr_ambe49_is_default_silence(silence), 1);

    char not_silence[49];
    memcpy(not_silence, silence, sizeof(not_silence));
    not_silence[48] ^= 1;
    rc |= expect_eq_int("silence-rejected", dmr_ambe49_is_default_silence(not_silence), 0);

    {
        char frame[49];
        char original[49];
        memcpy(frame, silence, sizeof(frame));
        memcpy(original, silence, sizeof(original));
        int frame_counter = 0;
        int applied = hytera_bp_apply_frame49(0x0123456789ULL, 0ULL, 0ULL, 0ULL, &frame_counter, frame);
        rc |= expect_eq_int("skip-silence-applied", applied, 0);
        rc |= expect_eq_int("skip-silence-counter", frame_counter, 1);
        rc |= expect_eq_frame("skip-silence-frame", frame, original);
    }

    {
        char frame[49] = {0};
        char expected[49] = {0};
        int frame_counter = 1;
        int applied = hytera_bp_apply_frame49(0x0123456789ULL, 0ULL, 0ULL, 0ULL, &frame_counter, frame);
        legacy_hytera_bp_apply(0x0123456789ULL, 0ULL, 0ULL, 0ULL, 1, expected);
        rc |= expect_eq_int("apply-frame-applied", applied, 1);
        rc |= expect_eq_int("apply-frame-counter", frame_counter, 2);
        rc |= expect_eq_frame("apply-frame-bits", frame, expected);
    }

    {
        char frame[49] = {0};
        char expected[49] = {0};
        int frame_counter = 99;
        int applied = hytera_bp_apply_frame49(0x0123456789ULL, 0ULL, 0ULL, 0ULL, &frame_counter, frame);
        legacy_hytera_bp_apply(0x0123456789ULL, 0ULL, 0ULL, 0ULL, 17, expected);
        rc |= expect_eq_int("clamp-applied", applied, 1);
        rc |= expect_eq_int("clamp-counter", frame_counter, 18);
        rc |= expect_eq_frame("clamp-bits", frame, expected);
    }

    if (rc == 0) {
        printf("CORE_HYTERA_BP_SILENCE: OK\n");
    }
    return rc;
}
