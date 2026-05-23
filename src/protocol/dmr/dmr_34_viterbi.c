// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * dmr_34_viterbi.c
 * Normative DMR 3/4 decoder (hard-decision Viterbi) compatible with existing dmr_34() packing.
 */

#include <stddef.h>
#include <stdint.h>

#include <dsd-neo/core/safe_api.h>
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

enum { R34_T = 49, R34_S = 8, R34_K = 32 };

static const int R34_INF = 1000000000;

typedef struct {
    int metric;
    uint8_t state;
    uint8_t rank;
} r34_cand_idx;

static int
r34_validate_end_state(int force_end_state, int end_state) {
    if (force_end_state && (end_state < 0 || end_state > 7)) {
        return -1;
    }
    return 0;
}

static void
r34_deinterleave_dibits(const uint8_t* dibits98, uint8_t dibits_dei[98]) {
    for (int i = 0; i < 98; i++) {
        dibits_dei[s_interleave[i]] = (uint8_t)(dibits98[i] & 0x3u);
    }
}

static void
r34_pack_nibbles(const uint8_t dibits_dei[98], uint8_t nibs[R34_T]) {
    for (int i = 0; i < R34_T; i++) {
        uint8_t d0 = dibits_dei[i * 2 + 0] & 0x3u;
        uint8_t d1 = dibits_dei[i * 2 + 1] & 0x3u;
        nibs[i] = (uint8_t)((d0 << 2) | d1);
    }
}

static void
r34_prepare_nibbles(const uint8_t* dibits98, uint8_t nibs[R34_T]) {
    uint8_t dibits_dei[98];
    r34_deinterleave_dibits(dibits98, dibits_dei);
    r34_pack_nibbles(dibits_dei, nibs);
}

static void
r34_map_nibbles_to_points(const uint8_t nibs[R34_T], uint8_t obs_point[R34_T]) {
    for (int i = 0; i < R34_T; i++) {
        obs_point[i] = s_constellation_map[nibs[i] & 0x0F];
    }
}

static void
r34_deinterleave_reliability(const uint8_t* reliab98, uint8_t reliab_dei[98]) {
    for (int i = 0; i < 98; i++) {
        reliab_dei[s_interleave[i]] = reliab98[i];
    }
}

static void
r34_split_reliability(const uint8_t reliab_dei[98], uint8_t rhi[R34_T], uint8_t rlo[R34_T]) {
    for (int i = 0; i < R34_T; i++) {
        rhi[i] = reliab_dei[i * 2 + 0];
        rlo[i] = reliab_dei[i * 2 + 1];
    }
}

static void
r34_set_uniform_reliability(uint8_t rhi[R34_T], uint8_t rlo[R34_T], uint8_t value) {
    for (int i = 0; i < R34_T; i++) {
        rhi[i] = value;
        rlo[i] = value;
    }
}

static void
r34_prepare_reliability_weights(const uint8_t* reliab98, uint8_t rhi[R34_T], uint8_t rlo[R34_T], int weighted) {
    if (weighted) {
        uint8_t reliab_dei[98];
        r34_deinterleave_reliability(reliab98, reliab_dei);
        r34_split_reliability(reliab_dei, rhi, rlo);
        return;
    }
    r34_set_uniform_reliability(rhi, rlo, 1);
}

static void
r34_metric_reset_1d(int metric[R34_S]) {
    for (int s = 0; s < R34_S; s++) {
        metric[s] = R34_INF;
    }
}

static void
r34_metric_copy_1d(int dst[R34_S], const int src[R34_S]) {
    for (int s = 0; s < R34_S; s++) {
        dst[s] = src[s];
    }
}

static void
r34_metric_init_1d(int metric[R34_S]) {
    r34_metric_reset_1d(metric);
    metric[0] = 0;
}

static void
r34_metric_reset_2d(int metric[R34_S][R34_K]) {
    for (int s = 0; s < R34_S; s++) {
        for (int r = 0; r < R34_K; r++) {
            metric[s][r] = R34_INF;
        }
    }
}

static void
r34_metric_copy_2d(int dst[R34_S][R34_K], int src[R34_S][R34_K]) {
    for (int s = 0; s < R34_S; s++) {
        for (int r = 0; r < R34_K; r++) {
            dst[s][r] = src[s][r];
        }
    }
}

static void
r34_metric_init_2d(int metric[R34_S][R34_K]) {
    r34_metric_reset_2d(metric);
    metric[0][0] = 0;
}

static int
r34_select_end_state(const int metric_prev[R34_S], int force_end_state, int end_state, int* best_s) {
    if (force_end_state) {
        *best_s = end_state;
        if (metric_prev[*best_s] >= R34_INF) {
            return -1;
        }
        return 0;
    }

    int best_m = metric_prev[0];
    int best = 0;
    for (int s = 1; s < R34_S; s++) {
        if (metric_prev[s] < best_m) {
            best_m = metric_prev[s];
            best = s;
        }
    }
    *best_s = best;
    return 0;
}

static void
r34_traceback_states(uint8_t backptr[R34_T][R34_S], int end_state, uint8_t states[R34_T]) {
    int s = end_state;
    for (int t = R34_T - 1; t >= 0; t--) {
        states[t] = (uint8_t)s;
        s = backptr[t][s];
    }
}

static void
r34_traceback_state_rank(uint8_t back_state[R34_T][R34_S][R34_K], uint8_t back_rank[R34_T][R34_S][R34_K], int state,
                         int rank, uint8_t states[R34_T]) {
    int s = state;
    int r = rank;
    for (int t = R34_T - 1; t >= 0; t--) {
        states[t] = (uint8_t)s;
        s = (int)back_state[t][s][r];
        r = (int)back_rank[t][s][r];
    }
}

static void
r34_pack_states_to_bytes(const uint8_t states[R34_T], uint8_t out_bytes18[18]) {
    for (int g = 0; g < 6; g++) {
        uint32_t temp = 0;
        for (int k = 0; k < 8; k++) {
            temp = (temp << 3) | (uint32_t)(states[g * 8 + k] & 0x7u);
        }
        out_bytes18[g * 3 + 0] = (uint8_t)((temp >> 16) & 0xFF);
        out_bytes18[g * 3 + 1] = (uint8_t)((temp >> 8) & 0xFF);
        out_bytes18[g * 3 + 2] = (uint8_t)(temp & 0xFF);
    }
}

static int
r34_weighted_nibble_cost(uint8_t expect_nib, uint8_t obs_nib, uint8_t rhi, uint8_t rlo) {
    uint8_t x = (uint8_t)(expect_nib ^ obs_nib);
    int cost = 0;
    if (x & 0x8) {
        cost += rhi;
    }
    if (x & 0x4) {
        cost += rhi;
    }
    if (x & 0x2) {
        cost += rlo;
    }
    if (x & 0x1) {
        cost += rlo;
    }
    return cost;
}

static int
r34_unweighted_list_cost(uint8_t expect_nib, uint8_t obs_nib) {
    uint8_t x = (uint8_t)(expect_nib ^ obs_nib);
    int bitcnt = ((x >> 0) & 1u) + ((x >> 1) & 1u) + ((x >> 2) & 1u) + ((x >> 3) & 1u);
    if (x != 0U) {
        // prioritize minimizing symbol mismatches, then break ties by bit mismatches.
        return 256 + bitcnt;
    }
    return 0;
}

static void
r34_run_viterbi_hard(const uint8_t obs_point[R34_T], int metric_prev[R34_S], uint8_t backptr[R34_T][R34_S]) {
    int metric_curr[R34_S];

    r34_metric_init_1d(metric_prev);
    for (int t = 0; t < R34_T; t++) {
        r34_metric_reset_1d(metric_curr);
        for (int ps = 0; ps < R34_S; ps++) {
            if (metric_prev[ps] >= R34_INF) {
                continue;
            }
            for (int ns = 0; ns < R34_S; ns++) {
                uint8_t expect = s_fsm[ps * 8 + ns];
                int m = metric_prev[ps] + hamming4(expect, obs_point[t]);
                if (m < metric_curr[ns]) {
                    metric_curr[ns] = m;
                    backptr[t][ns] = (uint8_t)ps;
                }
            }
        }
        r34_metric_copy_1d(metric_prev, metric_curr);
    }
}

static void
r34_run_viterbi_soft(const uint8_t nibs[R34_T], const uint8_t rhi[R34_T], const uint8_t rlo[R34_T],
                     int metric_prev[R34_S], uint8_t backptr[R34_T][R34_S]) {
    int metric_curr[R34_S];

    r34_metric_init_1d(metric_prev);
    for (int t = 0; t < R34_T; t++) {
        r34_metric_reset_1d(metric_curr);
        for (int ps = 0; ps < R34_S; ps++) {
            if (metric_prev[ps] >= R34_INF) {
                continue;
            }
            for (int ns = 0; ns < R34_S; ns++) {
                uint8_t expect_point = s_fsm[ps * 8 + ns];
                uint8_t expect_nib = s_unmap_point_to_nibble[expect_point];
                int cost = r34_weighted_nibble_cost(expect_nib, nibs[t], rhi[t], rlo[t]);
                int m = metric_prev[ps] + cost;
                if (m < metric_curr[ns]) {
                    metric_curr[ns] = m;
                    backptr[t][ns] = (uint8_t)ps;
                }
            }
        }
        r34_metric_copy_1d(metric_prev, metric_curr);
    }
}

static void
r34_precompute_expect_nibbles(uint8_t expect_nib_tbl[R34_S][R34_S]) {
    for (int ps = 0; ps < R34_S; ps++) {
        for (int ns = 0; ns < R34_S; ns++) {
            uint8_t expect_point = s_fsm[ps * 8 + ns];
            expect_nib_tbl[ps][ns] = s_unmap_point_to_nibble[expect_point];
        }
    }
}

static void
r34_insert_topk(int metric_curr[R34_S][R34_K], uint8_t back_state_t[R34_S][R34_K], uint8_t back_rank_t[R34_S][R34_K],
                int ns, int m, int ps, int pr) {
    for (int rr = 0; rr < R34_K; rr++) {
        if (m <= metric_curr[ns][rr]) {
            for (int sh = R34_K - 1; sh > rr; sh--) {
                metric_curr[ns][sh] = metric_curr[ns][sh - 1];
                back_state_t[ns][sh] = back_state_t[ns][sh - 1];
                back_rank_t[ns][sh] = back_rank_t[ns][sh - 1];
            }
            metric_curr[ns][rr] = m;
            back_state_t[ns][rr] = (uint8_t)ps;
            back_rank_t[ns][rr] = (uint8_t)pr;
            return;
        }
    }
}

static void
r34_viterbi_step_list(int metric_prev[R34_S][R34_K], int metric_curr[R34_S][R34_K], uint8_t back_state_t[R34_S][R34_K],
                      uint8_t back_rank_t[R34_S][R34_K], uint8_t expect_nib_tbl[R34_S][R34_S], uint8_t obs_nib,
                      uint8_t rhi, uint8_t rlo, int weighted) {
    for (int ps = 0; ps < R34_S; ps++) {
        for (int pr = 0; pr < R34_K; pr++) {
            int m0 = metric_prev[ps][pr];
            if (m0 >= R34_INF) {
                continue;
            }
            for (int ns = 0; ns < R34_S; ns++) {
                int cost = weighted ? r34_weighted_nibble_cost(expect_nib_tbl[ps][ns], obs_nib, rhi, rlo)
                                    : r34_unweighted_list_cost(expect_nib_tbl[ps][ns], obs_nib);
                r34_insert_topk(metric_curr, back_state_t, back_rank_t, ns, m0 + cost, ps, pr);
            }
        }
    }
}

static void
r34_run_viterbi_list(uint8_t nibs[R34_T], uint8_t rhi[R34_T], uint8_t rlo[R34_T], uint8_t expect_nib_tbl[R34_S][R34_S],
                     int weighted, int metric_prev[R34_S][R34_K], int metric_curr[R34_S][R34_K],
                     uint8_t back_state[R34_T][R34_S][R34_K], uint8_t back_rank[R34_T][R34_S][R34_K]) {
    r34_metric_init_2d(metric_prev);
    for (int t = 0; t < R34_T; t++) {
        r34_metric_reset_2d(metric_curr);
        DSD_MEMSET(back_state[t], 0, sizeof(back_state[t]));
        DSD_MEMSET(back_rank[t], 0, sizeof(back_rank[t]));
        r34_viterbi_step_list(metric_prev, metric_curr, back_state[t], back_rank[t], expect_nib_tbl, nibs[t], rhi[t],
                              rlo[t], weighted);
        r34_metric_copy_2d(metric_prev, metric_curr);
    }
}

static int
r34_collect_final_indices(int metric_prev[R34_S][R34_K], r34_cand_idx idx[R34_S * R34_K]) {
    int idx_n = 0;
    for (int s = 0; s < R34_S; s++) {
        for (int r = 0; r < R34_K; r++) {
            if (metric_prev[s][r] < R34_INF) {
                idx[idx_n].metric = metric_prev[s][r];
                idx[idx_n].state = (uint8_t)s;
                idx[idx_n].rank = (uint8_t)r;
                idx_n++;
            }
        }
    }
    return idx_n;
}

static void
r34_sort_indices_by_metric(r34_cand_idx idx[R34_S * R34_K], int idx_n) {
    for (int i = 1; i < idx_n; i++) {
        r34_cand_idx key = idx[i];
        int j = i - 1;
        while (j >= 0 && idx[j].metric > key.metric) {
            idx[j + 1] = idx[j];
            j--;
        }
        idx[j + 1] = key;
    }
}

static int
r34_write_list_candidates(r34_cand_idx idx[R34_S * R34_K], int idx_n, uint8_t back_state[R34_T][R34_S][R34_K],
                          uint8_t back_rank[R34_T][R34_S][R34_K], dmr_r34_candidate* out_candidates,
                          int max_candidates) {
    int out_n = idx_n;
    if (out_n > max_candidates) {
        out_n = max_candidates;
    }

    for (int ci = 0; ci < out_n; ci++) {
        uint8_t states[R34_T];
        r34_traceback_state_rank(back_state, back_rank, idx[ci].state, idx[ci].rank, states);
        r34_pack_states_to_bytes(states, out_candidates[ci].bytes18);
        out_candidates[ci].metric = idx[ci].metric;
    }

    return out_n;
}

static int
decode_hard_impl(const uint8_t* dibits98, int force_end_state, int end_state, uint8_t out_bytes18[18]) {
    if (!dibits98 || !out_bytes18) {
        return -1;
    }
    if (r34_validate_end_state(force_end_state, end_state) != 0) {
        return -1;
    }

    uint8_t nibs[R34_T];
    uint8_t obs_point[R34_T];
    int metric_prev[R34_S];
    uint8_t backptr[R34_T][R34_S];
    uint8_t states[R34_T];
    int best_s = 0;

    r34_prepare_nibbles(dibits98, nibs);
    r34_map_nibbles_to_points(nibs, obs_point);
    r34_run_viterbi_hard(obs_point, metric_prev, backptr);

    if (r34_select_end_state(metric_prev, force_end_state, end_state, &best_s) != 0) {
        return -1;
    }

    r34_traceback_states(backptr, best_s, states);
    r34_pack_states_to_bytes(states, out_bytes18);

    return 0;
}

int
dmr_r34_viterbi_decode(const uint8_t* dibits98, uint8_t out_bytes18[18]) {
    return decode_hard_impl(dibits98, 0, 0, out_bytes18);
}

int
dmr_r34_viterbi_decode_endstate(const uint8_t* dibits98, int end_state, uint8_t out_bytes18[18]) {
    return decode_hard_impl(dibits98, 1, end_state, out_bytes18);
}

static int
decode_soft_impl(const uint8_t* dibits98, const uint8_t* reliab98, int force_end_state, int end_state,
                 uint8_t out_bytes18[18]) {
    if (!dibits98 || !reliab98 || !out_bytes18) {
        return -1;
    }
    if (r34_validate_end_state(force_end_state, end_state) != 0) {
        return -1;
    }

    uint8_t nibs[R34_T];
    uint8_t rhi[R34_T];
    uint8_t rlo[R34_T];
    int metric_prev[R34_S];
    uint8_t backptr[R34_T][R34_S];
    uint8_t states[R34_T];
    int best_s = 0;

    r34_prepare_nibbles(dibits98, nibs);
    r34_prepare_reliability_weights(reliab98, rhi, rlo, 1);
    r34_run_viterbi_soft(nibs, rhi, rlo, metric_prev, backptr);

    if (r34_select_end_state(metric_prev, force_end_state, end_state, &best_s) != 0) {
        return -1;
    }

    r34_traceback_states(backptr, best_s, states);
    r34_pack_states_to_bytes(states, out_bytes18);
    return 0;
}

// Soft-decision variant using per-dibit reliability.
int
dmr_r34_viterbi_decode_soft(const uint8_t* dibits98, const uint8_t* reliab98, uint8_t out_bytes18[18]) {
    return decode_soft_impl(dibits98, reliab98, 0, 0, out_bytes18);
}

int
dmr_r34_viterbi_decode_soft_endstate(const uint8_t* dibits98, const uint8_t* reliab98, int end_state,
                                     uint8_t out_bytes18[18]) {
    return decode_soft_impl(dibits98, reliab98, 1, end_state, out_bytes18);
}

int
dmr_r34_viterbi_decode_list(const uint8_t* dibits98, const uint8_t* reliab98, dmr_r34_candidate* out_candidates,
                            int max_candidates, int* out_count) {
    if (!dibits98 || !out_candidates || !out_count || max_candidates <= 0) {
        return -1;
    }

    uint8_t nibs[R34_T];
    uint8_t rhi[R34_T];
    uint8_t rlo[R34_T];
    uint8_t expect_nib_tbl[R34_S][R34_S];
    int metric_prev[R34_S][R34_K];
    int metric_curr[R34_S][R34_K];
    uint8_t back_state[R34_T][R34_S][R34_K];
    uint8_t back_rank[R34_T][R34_S][R34_K];
    r34_cand_idx idx[R34_S * R34_K];
    const int weighted = (reliab98 != NULL);
    int idx_n = 0;

    r34_prepare_nibbles(dibits98, nibs);
    r34_prepare_reliability_weights(reliab98, rhi, rlo, weighted);
    r34_precompute_expect_nibbles(expect_nib_tbl);
    r34_run_viterbi_list(nibs, rhi, rlo, expect_nib_tbl, weighted, metric_prev, metric_curr, back_state, back_rank);

    idx_n = r34_collect_final_indices(metric_prev, idx);
    r34_sort_indices_by_metric(idx, idx_n);
    *out_count = r34_write_list_candidates(idx, idx_n, back_state, back_rank, out_candidates, max_candidates);
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
