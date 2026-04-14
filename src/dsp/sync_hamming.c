// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Frame-sync Hamming distance helpers.
 *
 * False-alarm probability derivation
 * ----------------------------------
 * Let N be the sync-pattern length in dibits. Under the null hypothesis of
 * uniformly random 4-ary symbols, each received dibit matches the expected
 * pattern position independently with probability 1/4, so the Hamming
 * distance H is Binomial(N, 3/4):
 *
 *   P(H = k) = C(N,k) * (3/4)^k * (1/4)^(N-k)
 *
 * For a single-pattern threshold test "declare sync if H <= t" the per-trial
 * false-alarm probability is the CDF
 *
 *   Pfa_single(N, t) = sum_{k=0..t} P(H = k).
 *
 * Tabulated values (N = 24, the P25/DMR sync length):
 *
 *   t = 0: Pfa ≈ 3.55e-15
 *   t = 2: Pfa ≈ 9.12e-12
 *   t = 4: Pfa ≈ 3.26e-9
 *   t = 6: Pfa ≈ 3.9e-7
 *   t = 8: Pfa ≈ 1.4e-5
 *
 * The `_with_remaps` helper evaluates ten candidate transforms (identity,
 * invert, bit-swap, xor-0x3, 90° rotation — each against the normal and
 * inverted reference pattern) and returns the minimum. Under the Bonferroni
 * union bound this multiplies the effective per-trial false-alarm rate by at
 * most the number of candidates (<= 10), so any application threshold should
 * use the adjusted bound
 *
 *   Pfa_effective ≤ 10 * Pfa_single(N, t).
 *
 * The remap transforms are not independent (e.g. rotation and inversion share
 * orbit structure on Z/4Z), so the actual factor is smaller; 10 is a safe
 * upper bound.
 *
 * Sliding-window detection rate
 * -----------------------------
 * At symbol rate Rs the per-second false-alarm rate across a continuous
 * search is Rs * Pfa_effective. For P25 phase 1 (Rs = 4800 sym/s) and a
 * threshold of t = 4, this gives a bound of ~1.6e-4 false syncs per second,
 * or about one per 1.7 hours of noise. In practice the decoder requires
 * multiple confirming symbols/frames after the initial latch, which drives
 * the end-to-end false-sync rate far below this per-trial bound.
 *
 * Callers choosing thresholds should reason in terms of Pfa_effective and
 * tolerable false-alarm-rate per unit time at the symbol rate of the mode.
 */

#include <dsd-neo/dsp/sync_hamming.h>

int
dsd_sync_hamming_distance(const char* buf, const char* pat, int len) {
    int ham = 0;
    for (int i = 0; i < len; i++) {
        int d = (unsigned char)buf[i];
        if (d >= '0' && d <= '3') {
            d -= '0';
        }
        int expect = (unsigned char)pat[i] - '0';
        if (d != expect) {
            ham++;
        }
    }
    return ham;
}

int
dsd_qpsk_sync_hamming_with_remaps(const char* buf, const char* pat_norm, const char* pat_inv, int len) {
    int ham_ident_n = 0, ham_ident_i = 0;
    int ham_invert_n = 0, ham_invert_i = 0;
    int ham_swap_n = 0, ham_swap_i = 0;
    int ham_xor3_n = 0, ham_xor3_i = 0;
    int ham_rot_n = 0, ham_rot_i = 0;
    for (int k = 0; k < len; k++) {
        int d = (unsigned char)buf[k];
        if (d >= '0' && d <= '3') {
            d -= '0';
        }
        int expect_n = pat_norm[k] - '0';
        int expect_i = pat_inv[k] - '0';
        int d_inv = (d == 0) ? 2 : (d == 1) ? 3 : (d == 2) ? 0 : 1;
        int d_swap = ((d & 1) << 1) | ((d & 2) >> 1); /* swap bit order */
        int d_xor3 = d ^ 0x3;                         /* bitwise not in 2-bit space */
        /* 90° rotation (cyclic remap 0->1->3->2->0) */
        int d_rot;
        switch (d & 0x3) {
            case 0: d_rot = 1; break;
            case 1: d_rot = 3; break;
            case 2: d_rot = 0; break;
            default: d_rot = 2; break; /* d==3 */
        }
        if (d != expect_n) {
            ham_ident_n++;
        }
        if (d != expect_i) {
            ham_ident_i++;
        }
        if (d_inv != expect_n) {
            ham_invert_n++;
        }
        if (d_inv != expect_i) {
            ham_invert_i++;
        }
        if (d_swap != expect_n) {
            ham_swap_n++;
        }
        if (d_swap != expect_i) {
            ham_swap_i++;
        }
        if (d_xor3 != expect_n) {
            ham_xor3_n++;
        }
        if (d_xor3 != expect_i) {
            ham_xor3_i++;
        }
        if (d_rot != expect_n) {
            ham_rot_n++;
        }
        if (d_rot != expect_i) {
            ham_rot_i++;
        }
    }
    int best = ham_ident_n;
    int ham_candidates[] = {ham_ident_i, ham_invert_n, ham_invert_i, ham_swap_n, ham_swap_i,
                            ham_xor3_n,  ham_xor3_i,   ham_rot_n,    ham_rot_i};
    for (int idx = 0; idx < (int)(sizeof(ham_candidates) / sizeof(ham_candidates[0])); idx++) {
        if (ham_candidates[idx] < best) {
            best = ham_candidates[idx];
        }
    }
    return best;
}
