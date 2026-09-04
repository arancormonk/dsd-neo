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
 * The decoder uses a full 8-state soft-decision Viterbi search and a
 * CRC-guided list interface for MDPDU callers.
 */

#include <dsd-neo/fec/trellis34.h>
#include <dsd-neo/protocol/p25/p25p1_mbf34.h>
#include <stdint.h>
#include <string.h>
#include "dsd-neo/core/safe_api.h"

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
p25_mbf34_deinterleave_llr(const int16_t bit_llr[196], int16_t llr_deint[196]) {
    DSD_MEMSET(llr_deint, 0, sizeof(int16_t) * 196U);
    for (int i = 0; i < 98; i++) {
        int p = dsd_trellis_interleave_98[i];
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
p25_mbf34_expand_paths(const int16_t llr_deint[196], p25_mbf34_path_t prev[P25_MBF34_N_ST][P25_MBF34_MAX_CANDIDATES]) {
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
                    uint8_t point = dsd_trellis34_fsm[(prev_st * 8) + next] & 0xF;
                    uint8_t expect = dsd_trellis34_inverse_constellation[point] & 0xF;
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
p25_mbf34_run_viterbi(const int16_t llr_deint[196], uint32_t curr_metric[P25_MBF34_N_ST],
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
                uint8_t point = dsd_trellis34_fsm[(prev * 8) + next] & 0xF;
                uint8_t expect = dsd_trellis34_inverse_constellation[point] & 0xF;
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

    int16_t llr_deint[196];
    p25_mbf34_deinterleave_llr(bit_llr, llr_deint);

    p25_mbf34_path_t prev[P25_MBF34_N_ST][P25_MBF34_MAX_CANDIDATES];
    DSD_MEMSET(prev, 0, sizeof(prev));
    for (int st = 0; st < P25_MBF34_N_ST; st++) {
        prev[st][0].valid = 1;
        prev[st][0].metric = (st == 0) ? 0U : 1024U;
    }

    p25_mbf34_expand_paths(llr_deint, prev);
    return p25_mbf34_collect_candidates(prev, candidates, max_candidates);
}

int
p25_mbf34_decode_soft(const uint8_t dibits[98], const int16_t bit_llr[196], uint8_t out[18]) {
    if (!dibits || !bit_llr || !out) {
        return -1;
    }
    (void)dibits;

    int16_t llr_deint[196];
    p25_mbf34_deinterleave_llr(bit_llr, llr_deint);

    uint32_t curr_metric[P25_MBF34_N_ST];
    uint8_t backptr[P25_MBF34_N_SYMS][P25_MBF34_N_ST];
    p25_mbf34_run_viterbi(llr_deint, curr_metric, backptr);

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
