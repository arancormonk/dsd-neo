// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * dmr_34_viterbi.c
 * Normative DMR 3/4 decoder (hard-decision Viterbi) compatible with existing dmr_34() packing.
 */

#include <stdint.h>
#include <string.h>

#include <dsd-neo/protocol/dmr/r34_viterbi.h>

// Deinterleave schedule (copy of dmr_34.c interleave[])
static const uint8_t s_interleave[98] = {0,  1,  8,  9,  16, 17, 24, 25, 32, 33, 40, 41, 48, 49, 56, 57, 64, 65, 72, 73,
                                         80, 81, 88, 89, 96, 97, 2,  3,  10, 11, 18, 19, 26, 27, 34, 35, 42, 43, 50, 51,
                                         58, 59, 66, 67, 74, 75, 82, 83, 90, 91, 4,  5,  12, 13, 20, 21, 28, 29, 36, 37,
                                         44, 45, 52, 53, 60, 61, 68, 69, 76, 77, 84, 85, 92, 93, 6,  7,  14, 15, 22, 23,
                                         30, 31, 38, 39, 46, 47, 54, 55, 62, 63, 70, 71, 78, 79, 86, 87, 94, 95};

// Nibble (dibit pair) to constellation point mapping (copy of dmr_34.c)
static const uint8_t s_constellation_map[16] = {11, 12, 0, 7, 14, 9, 5, 2, 10, 13, 1, 6, 15, 8, 4, 3};

// FSM mapping: for prev_state in [0..7], and tribit t in [0..7],
// expected constellation point code = s_fsm[prev_state*8 + t]. (copy of dmr_34.c)
static const uint8_t s_fsm[64] = {0, 8,  4, 12, 2, 10, 6, 14, 4, 12, 2, 10, 6, 14, 0, 8, 1, 9,  5, 13, 3, 11,
                                  7, 15, 5, 13, 3, 11, 7, 15, 1, 9,  3, 11, 7, 15, 1, 9, 5, 13, 7, 15, 1, 9,
                                  5, 13, 3, 11, 2, 10, 6, 14, 0, 8,  4, 12, 6, 14, 0, 8, 4, 12, 2, 10};

static inline int
hamming4(uint8_t a, uint8_t b) {
    static const uint8_t pop4[16] = {0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4};
    return (int)pop4[((a ^ b) & 0x0F)];
}

// Inverse map: point code (0..15) -> nibble (0..15) such that
// s_constellation_map[nibble] == point
static const uint8_t s_unmap_point_to_nibble[16] = {2, 10, 7, 15, 14, 6, 11, 3, 13, 5, 8, 0, 1, 9, 4, 12};

int
dmr_r34_viterbi_decode(const uint8_t* dibits98, uint8_t out_bytes18[18]) {
    if (!dibits98 || !out_bytes18) {
        return -1;
    }

    // Step 1: deinterleave dibits using schedule
    uint8_t dibits_dei[98];
    for (int i = 0; i < 98; i++) {
        dibits_dei[s_interleave[i]] = (uint8_t)(dibits98[i] & 0x3u);
    }

    // Step 2: pack dibits into 49 nibbles: nib = (dibit0<<2)|(dibit1)
    uint8_t nibs[49];
    for (int i = 0; i < 49; i++) {
        uint8_t d0 = dibits_dei[i * 2 + 0] & 0x3u;
        uint8_t d1 = dibits_dei[i * 2 + 1] & 0x3u;
        nibs[i] = (uint8_t)((d0 << 2) | d1);
    }

    // Step 3: map to observed constellation point codes (0..15)
    uint8_t obs_point[49];
    for (int i = 0; i < 49; i++) {
        obs_point[i] = s_constellation_map[nibs[i] & 0x0F];
    }

    // Step 4: Viterbi over 8 states, time 49, transitions: prev_state -> next_state = tribit (0..7)
    // Branch metric: Hamming distance between expected point and observed point.
    enum { T = 49, S = 8 };

    const int INF = 1e9;
    int metric_prev[S];
    int metric_curr[S];
    uint8_t backptr[T][S];

    for (int s = 0; s < S; s++) {
        metric_prev[s] = INF;
    }
    metric_prev[0] = 0; // start in state 0

    for (int t = 0; t < T; t++) {
        for (int s = 0; s < S; s++) {
            metric_curr[s] = INF;
        }
        for (int ps = 0; ps < S; ps++) {
            if (metric_prev[ps] >= INF) {
                continue;
            }
            for (int ns = 0; ns < S; ns++) {
                uint8_t expect = s_fsm[ps * 8 + ns];
                int cost = hamming4(expect, obs_point[t]);
                int m = metric_prev[ps] + cost;
                if (m < metric_curr[ns]) {
                    metric_curr[ns] = m;
                    backptr[t][ns] = (uint8_t)ps;
                }
            }
        }
        for (int s = 0; s < S; s++) {
            metric_prev[s] = metric_curr[s];
        }
    }

    // Step 5: traceback from best end state
    int best_s = 0;
    int best_m = metric_prev[0];
    for (int s = 1; s < S; s++) {
        if (metric_prev[s] < best_m) {
            best_m = metric_prev[s];
            best_s = s;
        }
    }

    // Extract tribits: next state equals tribit chosen; reconstruct states s_t for each time t
    uint8_t states[T];
    int s = best_s;
    for (int t = T - 1; t >= 0; t--) {
        states[t] = (uint8_t)s; // state after consuming symbol t
        s = backptr[t][s];
    }

    // Step 6: pack first 48 tribits (states[0..47]) into 18 bytes
    for (int g = 0; g < 6; g++) {
        uint32_t temp = 0;
        for (int k = 0; k < 8; k++) {
            temp = (temp << 3) | (uint32_t)(states[g * 8 + k] & 0x7u);
        }
        out_bytes18[g * 3 + 0] = (uint8_t)((temp >> 16) & 0xFF);
        out_bytes18[g * 3 + 1] = (uint8_t)((temp >> 8) & 0xFF);
        out_bytes18[g * 3 + 2] = (uint8_t)(temp & 0xFF);
    }

    return 0;
}

// Soft-decision variant using per-dibit reliability.
int
dmr_r34_viterbi_decode_soft(const uint8_t* dibits98, const uint8_t* reliab98, uint8_t out_bytes18[18]) {
    if (!dibits98 || !reliab98 || !out_bytes18) {
        return -1;
    }

    // Step 1: deinterleave dibits using schedule
    uint8_t dibits_dei[98];
    for (int i = 0; i < 98; i++) {
        dibits_dei[s_interleave[i]] = (uint8_t)(dibits98[i] & 0x3u);
    }

    // Step 2: pack dibits into 49 nibbles and gather reliabilities for hi/lo dibits
    uint8_t nibs[49];
    uint8_t rhi[49], rlo[49];
    for (int i = 0; i < 49; i++) {
        uint8_t d0 = dibits_dei[i * 2 + 0] & 0x3u;
        uint8_t d1 = dibits_dei[i * 2 + 1] & 0x3u;
        nibs[i] = (uint8_t)((d0 << 2) | d1);
        // reliabilities correspond to original (pre-deinterleave) positions
        // We need to fetch reliabilities consistent with deinterleave remap.
        // Since deinterleave used: dei[interleave[i]] = input[i], we reconstruct
        // by building an inverse mapping here.
    }

    // Build reliability arrays for deinterleaved indices
    uint8_t reliab_dei[98];
    for (int i = 0; i < 98; i++) {
        // dei[interleave[i]] = input[i] => reliab_dei[interleave[i]] = reliab98[i]
        reliab_dei[s_interleave[i]] = reliab98[i];
    }
    for (int i = 0; i < 49; i++) {
        rhi[i] = reliab_dei[i * 2 + 0];
        rlo[i] = reliab_dei[i * 2 + 1];
    }

    // Step 3: Viterbi with weighted branch metric in nibble space
    enum { T = 49, S = 8 };

    const int INF = 1e9;
    int metric_prev[S];
    int metric_curr[S];
    uint8_t backptr[T][S];

    for (int s = 0; s < S; s++) {
        metric_prev[s] = INF;
    }
    metric_prev[0] = 0;

    for (int t = 0; t < T; t++) {
        for (int s = 0; s < S; s++) {
            metric_curr[s] = INF;
        }
        for (int ps = 0; ps < S; ps++) {
            if (metric_prev[ps] >= INF) {
                continue;
            }
            for (int ns = 0; ns < S; ns++) {
                uint8_t expect_point = s_fsm[ps * 8 + ns];
                uint8_t expect_nib = s_unmap_point_to_nibble[expect_point];
                uint8_t obs_nib = nibs[t];
                uint8_t x = (uint8_t)(expect_nib ^ obs_nib);
                // Weighted bit mismatch cost (hi bits weight = rhi, lo bits = rlo)
                int cost = 0;
                if (x & 0x8) {
                    cost += rhi[t];
                }
                if (x & 0x4) {
                    cost += rhi[t];
                }
                if (x & 0x2) {
                    cost += rlo[t];
                }
                if (x & 0x1) {
                    cost += rlo[t];
                }
                int m = metric_prev[ps] + cost;
                if (m < metric_curr[ns]) {
                    metric_curr[ns] = m;
                    backptr[t][ns] = (uint8_t)ps;
                }
            }
        }
        for (int s = 0; s < S; s++) {
            metric_prev[s] = metric_curr[s];
        }
    }

    // Step 4: traceback
    int best_s = 0;
    int best_m = metric_prev[0];
    for (int s = 1; s < S; s++) {
        if (metric_prev[s] < best_m) {
            best_m = metric_prev[s];
            best_s = s;
        }
    }

    uint8_t states[T];
    int s = best_s;
    for (int t = T - 1; t >= 0; t--) {
        states[t] = (uint8_t)s;
        s = backptr[t][s];
    }

    // Step 5: pack 48 tribits
    for (int g = 0; g < 6; g++) {
        uint32_t temp = 0;
        for (int k = 0; k < 8; k++) {
            temp = (temp << 3) | (uint32_t)(states[g * 8 + k] & 0x7u);
        }
        out_bytes18[g * 3 + 0] = (uint8_t)((temp >> 16) & 0xFF);
        out_bytes18[g * 3 + 1] = (uint8_t)((temp >> 8) & 0xFF);
        out_bytes18[g * 3 + 2] = (uint8_t)(temp & 0xFF);
    }
    return 0;
}

// Simple encoder helper for tests
int
dmr_r34_encode(const uint8_t out_bytes18[18], uint8_t dibits98[98]) {
    if (!out_bytes18 || !dibits98) {
        return -1;
    }

    // Unpack 48 tribits from 18 bytes
    uint8_t tribits[48];
    for (int g = 0; g < 6; g++) {
        uint32_t temp = ((uint32_t)out_bytes18[g * 3 + 0] << 16) | ((uint32_t)out_bytes18[g * 3 + 1] << 8)
                        | ((uint32_t)out_bytes18[g * 3 + 2] << 0);
        for (int k = 0; k < 8; k++) {
            tribits[g * 8 + k] = (uint8_t)((temp >> (21 - 3 * k)) & 0x7u);
        }
    }

    // Generate deinterleaved dibits from trellis using FSM
    uint8_t de[98];
    uint8_t state = 0;
    for (int t = 0; t < 49; t++) {
        uint8_t tri;
        if (t < 48) {
            tri = tribits[t] & 0x7u;
        } else {
            tri = 0; // tail symbol (simple choice)
        }
        uint8_t point = s_fsm[state * 8 + tri];
        uint8_t nib = s_unmap_point_to_nibble[point];
        uint8_t d0 = (uint8_t)((nib >> 2) & 0x3u);
        uint8_t d1 = (uint8_t)(nib & 0x3u);
        de[t * 2 + 0] = d0;
        de[t * 2 + 1] = d1;
        state = tri;
    }

    // Interleave to output order: input[i] = de[ interleave[i] ]
    for (int i = 0; i < 98; i++) {
        dibits98[i] = de[s_interleave[i]];
    }
    return 0;
}
