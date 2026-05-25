// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * P25 Phase 1 Confirmed Data (3/4) decoder (MBF)
 *
 * This implementation mirrors the lightweight 3/4 trellis decoder used in
 * our DMR path, adapted for P25 MBF Confirmed Data blocks. It expects 98
 * dibits and produces 18 bytes per block laid out as:
 *
 *  byte[0]: [DBSN(7 bits, MSB..bit1)] | [CRC9 MSB at bit0]
 *  byte[1]: [CRC9 low 8 bits]
 *  byte[2..17]: 16 bytes (128 bits) of payload
 *
 * The hard-decision path keeps the legacy repair walk; the soft-decision
 * path uses a full 8-state Viterbi search and a CRC-guided list interface for
 * MDPDU callers.
 */

#include <dsd-neo/protocol/p25/p25p1_mbf34.h>
#include <string.h>
#include "dsd-neo/core/safe_api.h"

static const uint8_t p25_mbf34_interleave[98] = {
    0,  1,  8,  9,  16, 17, 24, 25, 32, 33, 40, 41, 48, 49, 56, 57, 64, 65, 72, 73, 80, 81, 88, 89, 96,
    97, 2,  3,  10, 11, 18, 19, 26, 27, 34, 35, 42, 43, 50, 51, 58, 59, 66, 67, 74, 75, 82, 83, 90, 91,
    4,  5,  12, 13, 20, 21, 28, 29, 36, 37, 44, 45, 52, 53, 60, 61, 68, 69, 76, 77, 84, 85, 92, 93, 6,
    7,  14, 15, 22, 23, 30, 31, 38, 39, 46, 47, 54, 55, 62, 63, 70, 71, 78, 79, 86, 87, 94, 95};

// Dibit-pair nibble to constellation point permutation (bijective)
static const uint8_t p25_constellation_map[16] = {11, 12, 0, 7, 14, 9, 5, 2, 10, 13, 1, 6, 15, 8, 4, 3};

// Finite-state machine mapping: (state*8 + tribit) -> constellation point
static const uint8_t p25_fsm[64] = {0, 8,  4, 12, 2, 10, 6, 14, 4, 12, 2, 10, 6, 14, 0, 8, 1, 9,  5, 13, 3, 11,
                                    7, 15, 5, 13, 3, 11, 7, 15, 1, 9,  3, 11, 7, 15, 1, 9, 5, 13, 7, 15, 1, 9,
                                    5, 13, 3, 11, 2, 10, 6, 14, 0, 8,  4, 12, 6, 14, 0, 8, 4, 12, 2, 10};

enum {
    P25_MBF34_N_SYMS = 49,
    P25_MBF34_N_ST = 8,
};

static uint32_t
llr_bit_cost(int16_t llr, int expected_bit) {
    if (expected_bit) {
        return (llr < 0) ? (uint32_t)(-llr) : 0U;
    }
    return (llr > 0) ? (uint32_t)llr : 0U;
}

typedef struct {
    uint8_t valid;
    uint32_t metric;
    uint8_t states[49];
} p25_mbf34_path_t;

static void
p25_mbf34_insert_path(p25_mbf34_path_t paths[P25_MBF34_MAX_CANDIDATES], const p25_mbf34_path_t* candidate) {
    int insert_at = -1;
    for (int i = 0; i < P25_MBF34_MAX_CANDIDATES; i++) {
        if (!paths[i].valid || candidate->metric < paths[i].metric) {
            insert_at = i;
            break;
        }
    }
    if (insert_at < 0) {
        return;
    }
    for (int i = P25_MBF34_MAX_CANDIDATES - 1; i > insert_at; i--) {
        paths[i] = paths[i - 1];
    }
    paths[insert_at] = *candidate;
}

static void
p25_mbf34_pack_path_bytes(const uint8_t states[49], uint8_t out[18]) {
    for (int i = 0; i < 6; i++) {
        uint32_t tmp = (states[(i * 8) + 0] << 21) | (states[(i * 8) + 1] << 18) | (states[(i * 8) + 2] << 15)
                       | (states[(i * 8) + 3] << 12) | (states[(i * 8) + 4] << 9) | (states[(i * 8) + 5] << 6)
                       | (states[(i * 8) + 6] << 3) | states[(i * 8) + 7];
        out[(i * 3) + 0] = (uint8_t)((tmp >> 16) & 0xFF);
        out[(i * 3) + 1] = (uint8_t)((tmp >> 8) & 0xFF);
        out[(i * 3) + 2] = (uint8_t)(tmp & 0xFF);
    }
}

static int
p25_mbf34_candidate_exists(const p25_mbf34_candidate_t* candidates, int count, const uint8_t bytes[18]) {
    for (int i = 0; i < count; i++) {
        if (memcmp(candidates[i].bytes, bytes, 18) == 0) {
            return 1;
        }
    }
    return 0;
}

static void
p25_mbf34_insert_candidate(p25_mbf34_candidate_t* candidates, int* count, int max_candidates, const uint8_t bytes[18],
                           uint32_t metric) {
    if (p25_mbf34_candidate_exists(candidates, *count, bytes)) {
        return;
    }
    int insert_at = *count;
    for (int i = 0; i < *count; i++) {
        if (metric < candidates[i].metric) {
            insert_at = i;
            break;
        }
    }
    if (*count < max_candidates) {
        (*count)++;
    } else if (insert_at >= max_candidates) {
        return;
    }
    for (int i = *count - 1; i > insert_at; i--) {
        candidates[i] = candidates[i - 1];
    }
    DSD_MEMCPY(candidates[insert_at].bytes, bytes, 18);
    candidates[insert_at].metric = metric;
}

static void
build_inverse_constellation(uint8_t inverse_map[16]) {
    DSD_MEMSET(inverse_map, 0, 16);
    for (int i = 0; i < 16; i++) {
        inverse_map[p25_constellation_map[i] & 0xF] = (uint8_t)i;
    }
}

static void
p25_mbf34_deinterleave_llr(const int16_t bit_llr[196], int16_t llr_deint[196]) {
    DSD_MEMSET(llr_deint, 0, sizeof(int16_t) * 196U);
    for (int i = 0; i < 98; i++) {
        int p = p25_mbf34_interleave[i];
        llr_deint[(p * 2) + 0] = bit_llr[(i * 2) + 0];
        llr_deint[(p * 2) + 1] = bit_llr[(i * 2) + 1];
    }
}

static uint32_t
p25_mbf34_branch_cost(const int16_t llr_deint[196], int base, uint8_t expect) {
    return llr_bit_cost(llr_deint[base + 0], (expect >> 3) & 1) + llr_bit_cost(llr_deint[base + 1], (expect >> 2) & 1)
           + llr_bit_cost(llr_deint[base + 2], (expect >> 1) & 1) + llr_bit_cost(llr_deint[base + 3], expect & 1);
}

static void
p25_mbf34_expand_paths(const int16_t llr_deint[196], const uint8_t inverse_map[16],
                       p25_mbf34_path_t prev[P25_MBF34_N_ST][P25_MBF34_MAX_CANDIDATES]) {
    p25_mbf34_path_t curr[P25_MBF34_N_ST][P25_MBF34_MAX_CANDIDATES];
    for (int i = 0; i < P25_MBF34_N_SYMS; i++) {
        DSD_MEMSET(curr, 0, sizeof(curr));
        int base = i * 4;
        for (int prev_st = 0; prev_st < P25_MBF34_N_ST; prev_st++) {
            for (int rank = 0; rank < P25_MBF34_MAX_CANDIDATES; rank++) {
                if (!prev[prev_st][rank].valid) {
                    continue;
                }
                for (int next = 0; next < P25_MBF34_N_ST; next++) {
                    uint8_t point = p25_fsm[(prev_st * 8) + next] & 0xF;
                    uint8_t expect = inverse_map[point] & 0xF;
                    p25_mbf34_path_t candidate = prev[prev_st][rank];
                    candidate.metric += p25_mbf34_branch_cost(llr_deint, base, expect);
                    candidate.states[i] = (uint8_t)next;
                    p25_mbf34_insert_path(curr[next], &candidate);
                }
            }
        }
        DSD_MEMCPY(prev, curr, sizeof(curr));
    }
}

static int
p25_mbf34_collect_candidates(p25_mbf34_path_t prev[P25_MBF34_N_ST][P25_MBF34_MAX_CANDIDATES],
                             p25_mbf34_candidate_t* candidates, int max_candidates) {
    int out_count = 0;
    for (int st = 0; st < P25_MBF34_N_ST; st++) {
        for (int rank = 0; rank < P25_MBF34_MAX_CANDIDATES; rank++) {
            if (!prev[st][rank].valid) {
                continue;
            }
            uint8_t bytes[18];
            p25_mbf34_pack_path_bytes(prev[st][rank].states, bytes);
            p25_mbf34_insert_candidate(candidates, &out_count, max_candidates, bytes, prev[st][rank].metric);
        }
    }
    return out_count;
}

static void
p25_mbf34_run_viterbi(const int16_t llr_deint[196], const uint8_t inverse_map[16], uint32_t curr_metric[P25_MBF34_N_ST],
                      uint8_t backptr[P25_MBF34_N_SYMS][P25_MBF34_N_ST]) {
    uint32_t prev_metric[P25_MBF34_N_ST];
    for (int st = 0; st < P25_MBF34_N_ST; st++) {
        prev_metric[st] = (st == 0) ? 0U : 1024U;
    }

    for (int i = 0; i < P25_MBF34_N_SYMS; i++) {
        int base = i * 4;
        for (int next = 0; next < P25_MBF34_N_ST; next++) {
            uint32_t best = 0xFFFFFFFFU;
            uint8_t best_prev = 0;
            for (int prev = 0; prev < P25_MBF34_N_ST; prev++) {
                uint8_t point = p25_fsm[(prev * 8) + next] & 0xF;
                uint8_t expect = inverse_map[point] & 0xF;
                uint32_t metric = prev_metric[prev] + p25_mbf34_branch_cost(llr_deint, base, expect);
                if (metric < best) {
                    best = metric;
                    best_prev = (uint8_t)prev;
                }
            }
            curr_metric[next] = best;
            backptr[i][next] = best_prev;
        }
        for (int st = 0; st < P25_MBF34_N_ST; st++) {
            prev_metric[st] = curr_metric[st];
        }
    }
}

// Attempt to find a surviving/best path given a local error at position
static uint8_t
p25_fix34(uint8_t* p, uint8_t state, int position) {
    int i, j, k, best_p, best_v, survivors;

    int s[8];
    DSD_MEMSET(s, 0, sizeof(s));

    uint8_t temp_p[8];
    temp_p[0] = (p[position] ^ 1) & 0xF;
    temp_p[1] = (p[position] ^ 3) & 0xF;
    temp_p[2] = (p[position] ^ 5) & 0xF;
    temp_p[3] = (p[position] ^ 7) & 0xF;
    temp_p[4] = (p[position] ^ 9) & 0xF;
    temp_p[5] = (p[position] ^ 11) & 0xF;
    temp_p[6] = (p[position] ^ 13) & 0xF;
    temp_p[7] = (p[position] ^ 15) & 0xF;

    best_p = 0;
    best_v = 0;

    for (k = 0; k < 8; k++) {
        uint8_t temp_s = state;
        int counter = 0;
        uint8_t tri = 0;
        for (i = position; i < 49; i++) {
            const uint8_t t = (i == position) ? temp_p[k] : p[i];
            if (tri != 0xFF) {
                tri = 0xFF;
                for (j = 0; j < 8; j++) {
                    if (p25_fsm[(temp_s * 8) + j] == t) {
                        tri = temp_s = (uint8_t)j;
                        counter++;
                        break;
                    }
                }
                if (counter > best_p) {
                    best_p = counter;
                    best_v = k;
                }
                if (i == 48) {
                    s[k] = 1;
                }
            }
        }
    }

    survivors = 0;
    for (k = 0; k < 8; k++) {
        if (s[k] == 1) {
            survivors++;
        }
    }
    (void)survivors; // debug aid if needed
    return temp_p[best_v];
}

int
p25_mbf34_decode(const uint8_t dibits[98], uint8_t out[18]) {
    if (!dibits || !out) {
        return -1;
    }

    uint32_t irr_err = 0;

    uint8_t deint[98];
    DSD_MEMSET(deint, 0, sizeof(deint));
    for (int i = 0; i < 98; i++) {
        deint[p25_mbf34_interleave[i]] = dibits[i];
    }

    uint8_t nibs[49];
    DSD_MEMSET(nibs, 0, sizeof(nibs));
    for (int i = 0; i < 49; i++) {
        nibs[i] = (uint8_t)((deint[i * 2 + 0] << 2) | (deint[i * 2 + 1]));
    }

    uint8_t point[49];
    DSD_MEMSET(point, 0xFF, sizeof(point));
    for (int i = 0; i < 49; i++) {
        point[i] = p25_constellation_map[nibs[i] & 0xF];
    }

    uint8_t state = 0;
    uint32_t tribits[49];
    DSD_MEMSET(tribits, 0xF, sizeof(tribits));

    int i = 0;
    while (i < 49) {
        for (int j = 0; j < 8; j++) {
            if (p25_fsm[(state * 8) + j] == point[i]) {
                tribits[i] = state = (uint8_t)j;
                break;
            }
        }
        if (tribits[i] > 7) {
            irr_err++;
            point[i] = p25_fix34(point, state, i);
            continue;
        }
        i++;
    }

    // Pack first 48 tribits into 18 bytes (24 bits per 8-tribit group)
    for (int group = 0; group < 6; group++) {
        uint32_t tmp = (tribits[(group * 8) + 0] << 21) | (tribits[(group * 8) + 1] << 18)
                       | (tribits[(group * 8) + 2] << 15) | (tribits[(group * 8) + 3] << 12)
                       | (tribits[(group * 8) + 4] << 9) | (tribits[(group * 8) + 5] << 6)
                       | (tribits[(group * 8) + 6] << 3) | (tribits[(group * 8) + 7] << 0);
        out[(group * 3) + 0] = (uint8_t)((tmp >> 16) & 0xFF);
        out[(group * 3) + 1] = (uint8_t)((tmp >> 8) & 0xFF);
        out[(group * 3) + 2] = (uint8_t)((tmp >> 0) & 0xFF);
    }

    (void)irr_err; // could be used for telemetry
    return 0;
}

int
p25_mbf34_decode_soft_list(const uint8_t dibits[98], const int16_t bit_llr[196], p25_mbf34_candidate_t* candidates,
                           int max_candidates) {
    if (!dibits || !bit_llr || !candidates || max_candidates <= 0) {
        return 0;
    }
    (void)dibits;
    if (max_candidates > P25_MBF34_MAX_CANDIDATES) {
        max_candidates = P25_MBF34_MAX_CANDIDATES;
    }

    uint8_t inverse_map[16];
    build_inverse_constellation(inverse_map);

    int16_t llr_deint[196];
    p25_mbf34_deinterleave_llr(bit_llr, llr_deint);

    p25_mbf34_path_t prev[P25_MBF34_N_ST][P25_MBF34_MAX_CANDIDATES];
    DSD_MEMSET(prev, 0, sizeof(prev));
    for (int st = 0; st < P25_MBF34_N_ST; st++) {
        prev[st][0].valid = 1;
        prev[st][0].metric = (st == 0) ? 0U : 1024U;
    }

    p25_mbf34_expand_paths(llr_deint, inverse_map, prev);
    return p25_mbf34_collect_candidates(prev, candidates, max_candidates);
}

int
p25_mbf34_decode_soft(const uint8_t dibits[98], const int16_t bit_llr[196], uint8_t out[18]) {
    if (!dibits || !bit_llr || !out) {
        return -1;
    }
    (void)dibits;

    uint8_t inverse_map[16];
    build_inverse_constellation(inverse_map);

    int16_t llr_deint[196];
    p25_mbf34_deinterleave_llr(bit_llr, llr_deint);

    uint32_t curr_metric[P25_MBF34_N_ST];
    uint8_t backptr[P25_MBF34_N_SYMS][P25_MBF34_N_ST];
    p25_mbf34_run_viterbi(llr_deint, inverse_map, curr_metric, backptr);

    uint32_t best_final = curr_metric[0];
    int st = 0;
    for (int i = 1; i < P25_MBF34_N_ST; i++) {
        if (curr_metric[i] < best_final) {
            best_final = curr_metric[i];
            st = i;
        }
    }

    uint8_t tribits[P25_MBF34_N_SYMS];
    for (int i = P25_MBF34_N_SYMS; i > 0;) {
        i--;
        tribits[i] = (uint8_t)st;
        st = backptr[i][st];
    }

    p25_mbf34_pack_path_bytes(tribits, out);

    return (int)(best_final >> 8);
}
