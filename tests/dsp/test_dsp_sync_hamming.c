// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/dsp/sync_hamming.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "dsd-neo/core/safe_api.h"
#include "dsp/sync_hamming_internal.h"

static int
expect_int_eq(const char* label, int actual, int expected) {
    if (actual != expected) {
        DSD_FPRINTF(stderr, "FAIL: %s: got %d expected %d\n", label, actual, expected);
        return 0;
    }
    return 1;
}

/* Verbatim copy of the per-dibit implementation the bit-plane paths replaced. */
static int
ref_decode_dibit(char dibit_char) {
    int d = (unsigned char)dibit_char;
    if (d >= '0' && d <= '3') {
        d -= '0';
    }
    return d;
}

static int
ref_invert_dibit(int d) {
    switch (d) {
        case 0: return 2;
        case 1: return 3;
        case 2: return 0;
        default: return 1;
    }
}

static int
ref_rotate_dibit(int d) {
    switch (d & 0x3) {
        case 0: return 1;
        case 1: return 3;
        case 2: return 0;
        default: return 2;
    }
}

static int
ref_hamming_distance(const char* buf, const char* pat, int len) {
    int ham = 0;
    for (int i = 0; i < len; i++) {
        int d = ref_decode_dibit(buf[i]);
        int expect = (unsigned char)pat[i] - '0';
        if (d != expect) {
            ham++;
        }
    }
    return ham;
}

static int
ref_qpsk_remaps(const char* buf, const char* pat_norm, const char* pat_inv, int len) {
    int hamming[5][2] = {{0}};
    for (int k = 0; k < len; k++) {
        int remaps[5];
        const int expected[2] = {pat_norm[k] - '0', pat_inv[k] - '0'};
        int d = ref_decode_dibit(buf[k]);
        remaps[0] = d;
        remaps[1] = ref_invert_dibit(d);
        remaps[2] = ((d & 1) << 1) | ((d & 2) >> 1);
        remaps[3] = d ^ 0x3;
        remaps[4] = ref_rotate_dibit(d);
        for (int remap = 0; remap < 5; remap++) {
            hamming[remap][0] += (remaps[remap] != expected[0]);
            hamming[remap][1] += (remaps[remap] != expected[1]);
        }
    }
    int best = hamming[0][0];
    for (int remap = 0; remap < 5; remap++) {
        for (int ref = 0; ref < 2; ref++) {
            if (hamming[remap][ref] < best) {
                best = hamming[remap][ref];
            }
        }
    }
    return best;
}

static uint32_t g_seed = 0x2468aceU;

static uint32_t
next_random(void) {
    g_seed = g_seed * 1664525U + 1013904223U;
    return g_seed >> 8;
}

/* Bytes the fast-path checks treat specially: just outside the accepted 0..3 and '0'..'3' ranges,
 * bit-4/bit-5 mismatches, and high bytes (negative as plain char on most targets). */
static const unsigned char k_odd_bytes[] = {0x04, 0x07, 0x0C, 0x10, 0x13, 0x20, 0x23, 0x2F, 0x34, 0x37, 0x3C, 0x40,
                                            0x43, 0x70, 0x73, 0x80, 0xB0, 0xB3, 0xC0, 0xF0, 0xF3, 0xFC, 0xFF, 'A'};

enum {
    MODE_ASCII = 0,
    MODE_NUMERIC,
    MODE_MIXED,
    MODE_RARE_ODD,
    MODE_NUMERIC_RARE_ODD,
    MODE_ANY,
    MODE_COUNT,
};

static char
random_byte(int mode) {
    const uint32_t r = next_random();
    switch (mode) {
        case MODE_ASCII: return (char)('0' + (r & 3U));
        case MODE_NUMERIC: return (char)(r & 3U);
        case MODE_MIXED: return (char)(((r & 4U) ? '0' : 0) + (int)(r & 3U));
        case MODE_RARE_ODD:
            if ((r & 0x3FU) == 0U) {
                return (char)k_odd_bytes[(r >> 6) % sizeof(k_odd_bytes)];
            }
            return (char)('0' + (r & 3U));
        case MODE_NUMERIC_RARE_ODD:
            if ((r & 0x3FU) == 0U) {
                return (char)k_odd_bytes[(r >> 6) % sizeof(k_odd_bytes)];
            }
            return (char)(r & 3U);
        default: return (char)(r & 0xFFU);
    }
}

static int
check_all_exact(const char* label, const char* buf, const char* pn, const char* pi, int len) {
    const int ref_h = ref_hamming_distance(buf, pn, len);
    const int ref_q = ref_qpsk_remaps(buf, pn, pi, len);
    const int got[5] = {
        dsd_sync_hamming_distance(buf, pn, len),
        dsd_sync_hamming_distance_scalar(buf, pn, len),
        dsd_qpsk_sync_hamming_with_remaps(buf, pn, pi, len),
        dsd_qpsk_sync_hamming_with_remaps_portable(buf, pn, pi, len),
        dsd_qpsk_sync_hamming_with_remaps_scalar(buf, pn, pi, len),
    };
    static const char* names[5] = {"hamming", "hamming_scalar", "remaps", "remaps_portable", "remaps_scalar"};
    for (int k = 0; k < 5; k++) {
        const int want = k < 2 ? ref_h : ref_q;
        if (got[k] != want) {
            DSD_FPRINTF(stderr, "FAIL: %s %s len=%d: got %d expected %d\n", label, names[k], len, got[k], want);
            return 0;
        }
    }
    return 1;
}

/* Runs the comparison on exact-size heap copies too, so sanitizer builds flag any read past the window. */
static int
check_all(const char* label, const char* buf, const char* pn, const char* pi, int len) {
    if (!check_all_exact(label, buf, pn, pi, len)) {
        return 0;
    }
    if (len <= 0) {
        return 1;
    }
    char* copies[3] = {(char*)malloc((size_t)len), (char*)malloc((size_t)len), (char*)malloc((size_t)len)};
    int ok = copies[0] && copies[1] && copies[2];
    if (ok) {
        DSD_MEMCPY(copies[0], buf, (size_t)len);
        DSD_MEMCPY(copies[1], pn, (size_t)len);
        DSD_MEMCPY(copies[2], pi, (size_t)len);
        ok = check_all_exact(label, copies[0], copies[1], copies[2], len);
    } else {
        DSD_FPRINTF(stderr, "FAIL: %s: allocation\n", label);
    }
    for (int k = 0; k < 3; k++) {
        free(copies[k]);
    }
    return ok;
}

static int
test_known_values(void) {
    int ok = 1;
    ok &= expect_int_eq("hamming: identical", dsd_sync_hamming_distance("0123", "0123", 4), 0);
    ok &= expect_int_eq("hamming: one mismatch", dsd_sync_hamming_distance("0122", "0123", 4), 1);

    const char buf_num_ok[] = {0, 1, 2, 3};
    const char buf_num_bad[] = {0, 1, 2, 2};
    ok &= expect_int_eq("hamming: numeric identical", dsd_sync_hamming_distance(buf_num_ok, "0123", 4), 0);
    ok &= expect_int_eq("hamming: numeric one mismatch", dsd_sync_hamming_distance(buf_num_bad, "0123", 4), 1);

    const char* pat = "0123";
    ok &= expect_int_eq("remap: invert", dsd_qpsk_sync_hamming_with_remaps("2301", pat, pat, 4), 0);
    ok &= expect_int_eq("remap: swap", dsd_qpsk_sync_hamming_with_remaps("0213", pat, pat, 4), 0);
    ok &= expect_int_eq("remap: xor3", dsd_qpsk_sync_hamming_with_remaps("3210", pat, pat, 4), 0);
    ok &= expect_int_eq("remap: rot", dsd_qpsk_sync_hamming_with_remaps("2031", pat, pat, 4), 0);

    /* Out-of-range receive bytes: invert sends them to 1 and rotate reads d & 3. */
    ok &= expect_int_eq("remap: odd byte invert", dsd_qpsk_sync_hamming_with_remaps("\x07", "1", "1", 1), 0);
    ok &= expect_int_eq("remap: odd byte rotate", dsd_qpsk_sync_hamming_with_remaps("\x04", "1", "0", 1), 0);
    ok &= expect_int_eq("hamming: odd byte", dsd_sync_hamming_distance("\x04", "0", 1), 1);
    return ok;
}

/* Every receive byte against every pattern byte, alone and planted at each position of a valid window. */
static int
test_exhaustive_bytes(void) {
    static const unsigned char inv_bytes[] = {'0', '1', '2', '3', 0x00, 0x34, 0x80, 0xFF};
    char pn[64];
    char pi[64];
    char buf[64];
    for (int b = 0; b < 256; b++) {
        for (int p = 0; p < 256; p++) {
            for (size_t q = 0; q < sizeof(inv_bytes); q++) {
                buf[0] = (char)b;
                pn[0] = (char)p;
                pi[0] = (char)inv_bytes[q];
                if (!check_all("exhaustive len 1", buf, pn, pi, 1)) {
                    return 0;
                }
            }
        }
    }
    static const int lens[] = {7, 8, 9, 15, 16, 17, 20, 24, 31, 32, 33, 48, 63, 64};
    for (size_t li = 0; li < sizeof(lens) / sizeof(lens[0]); li++) {
        const int len = lens[li];
        for (int pos = 0; pos < len; pos++) {
            for (int b = 0; b < 256; b++) {
                /* Uniform windows matter: one ASCII byte among numeric ones (or the reverse) must not
                 * hide a byte the fast path wrongly accepts. */
                const int background =
                    (b + pos) % 3 == 0 ? MODE_ASCII : ((b + pos) % 3 == 1 ? MODE_NUMERIC : MODE_MIXED);
                for (int k = 0; k < len; k++) {
                    buf[k] = random_byte(background);
                    pn[k] = random_byte(MODE_ASCII);
                    pi[k] = random_byte(MODE_ASCII);
                }
                buf[pos] = (char)b;
                if (!check_all("planted receive byte", buf, pn, pi, len)) {
                    return 0;
                }
                buf[pos] = random_byte(MODE_ASCII);
                pn[pos] = (char)b;
                if (!check_all("planted pattern byte", buf, pn, pi, len)) {
                    return 0;
                }
                pn[pos] = random_byte(MODE_ASCII);
                pi[pos] = (char)b;
                if (!check_all("planted inverse byte", buf, pn, pi, len)) {
                    return 0;
                }
            }
        }
    }
    return 1;
}

/* Random windows at every length 0..80 (above 64 is the per-dibit fallback) and alignment. */
static int
test_random_equivalence(void) {
    char storage[3][96 + 16];
    for (int len = 0; len <= 80; len++) {
        for (int trial = 0; trial < 400; trial++) {
            const int mode = trial % MODE_COUNT;
            const int pat_mode = (trial % 7 == 6) ? MODE_RARE_ODD : MODE_ASCII;
            const int offset = (int)(next_random() % 16U);
            char* buf = storage[0] + offset;
            char* pn = storage[1] + ((offset + 3) & 15);
            char* pi = storage[2] + ((offset + 9) & 15);
            for (int k = 0; k < len; k++) {
                buf[k] = random_byte(mode);
                pn[k] = random_byte(pat_mode);
                pi[k] = random_byte(pat_mode);
            }
            if (!check_all("random", buf, pn, pi, len)) {
                return 0;
            }
            /* All-match, all-mismatch, and an exact remap of the pattern. */
            for (int k = 0; k < len; k++) {
                buf[k] = pn[k];
            }
            if (!check_all("all match", buf, pn, pi, len)) {
                return 0;
            }
            for (int k = 0; k < len; k++) {
                const int d = ((unsigned char)pn[k] - '0') & 3;
                buf[k] = (char)('0' + ((d + 1) & 3));
                pi[k] = (char)('0' + ((d + 2) & 3));
            }
            if (!check_all("all mismatch", buf, pn, pi, len)) {
                return 0;
            }
            for (int k = 0; k < len; k++) {
                static const char rot_inverse[4] = {'2', '0', '3', '1'};
                buf[k] = rot_inverse[((unsigned char)pn[k] - '0') & 3];
            }
            if (!check_all("rotated", buf, pn, pi, len)) {
                return 0;
            }
        }
    }
    const char* any = "0123";
    if (!check_all("negative length", any, any, any, -5)) {
        return 0;
    }
    return 1;
}

int
main(void) {
    int ok = 1;
    ok &= test_known_values();
    ok &= test_exhaustive_bytes();
    ok &= test_random_equivalence();
    return ok ? 0 : 1;
}
