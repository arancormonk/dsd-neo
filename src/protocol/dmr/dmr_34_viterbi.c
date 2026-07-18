// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * dmr_34_viterbi.c
 * Normative DMR 3/4 decoder (hard-decision Viterbi).
 */

#include <stddef.h>
#include <stdint.h>

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/fec/trellis34.h>
#include <dsd-neo/platform/posix_compat.h>
#include <dsd-neo/protocol/dmr/r34_viterbi.h>
#include "dmr_r34_internal.h"

enum { R34_T = 49, R34_S = 8, R34_K = 32 };

static const int R34_INF = 1000000000;

typedef struct {
    int metric;
    uint8_t state;
    uint8_t rank;
} r34_cand_idx;

static void
r34_deinterleave_dibits(const uint8_t* dibits98, uint8_t dibits_dei[98]) {
    for (int i = 0; i < 98; i++) {
        dibits_dei[dsd_trellis_interleave_98[i]] = (uint8_t)(dibits98[i] & 0x3u);
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
        obs_point[i] = dsd_trellis34_constellation[nibs[i] & 0x0F];
    }
}

static void
r34_deinterleave_reliability(const uint8_t* reliab98, uint8_t reliab_dei[98]) {
    for (int i = 0; i < 98; i++) {
        reliab_dei[dsd_trellis_interleave_98[i]] = reliab98[i];
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
        int prev_s = (int)back_state[t][s][r];
        int prev_r = (int)back_rank[t][s][r];
        s = prev_s;
        r = prev_r;
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
                uint8_t expect = dsd_trellis34_fsm[ps * 8 + ns];
                int distance = dsd_popcount64((uint64_t)((expect ^ obs_point[t]) & 0x0FU));
                int m = metric_prev[ps] + distance;
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
                uint8_t expect_point = dsd_trellis34_fsm[ps * 8 + ns];
                uint8_t expect_nib = dsd_trellis34_inverse_constellation[expect_point];
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
            uint8_t expect_point = dsd_trellis34_fsm[ps * 8 + ns];
            expect_nib_tbl[ps][ns] = dsd_trellis34_inverse_constellation[expect_point];
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
    for (int r = 0; r < R34_K; r++) {
        if (metric_prev[0][r] < R34_INF) {
            idx[idx_n].metric = metric_prev[0][r];
            idx[idx_n].state = 0;
            idx[idx_n].rank = (uint8_t)r;
            idx_n++;
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

int
dmr_r34_viterbi_decode(const uint8_t* dibits98, uint8_t out_bytes18[18]) {
    if (!dibits98 || !out_bytes18) {
        return -1;
    }

    uint8_t nibs[R34_T];
    uint8_t obs_point[R34_T];
    int metric_prev[R34_S];
    uint8_t backptr[R34_T][R34_S];
    uint8_t states[R34_T];

    r34_prepare_nibbles(dibits98, nibs);
    r34_map_nibbles_to_points(nibs, obs_point);
    r34_run_viterbi_hard(obs_point, metric_prev, backptr);

    r34_traceback_states(backptr, 0, states);
    r34_pack_states_to_bytes(states, out_bytes18);

    return 0;
}

// Soft-decision variant using per-dibit reliability.
int
dmr_r34_viterbi_decode_soft(const uint8_t* dibits98, const uint8_t* reliab98, uint8_t out_bytes18[18]) {
    if (!dibits98 || !reliab98 || !out_bytes18) {
        return -1;
    }

    uint8_t nibs[R34_T];
    uint8_t rhi[R34_T];
    uint8_t rlo[R34_T];
    int metric_prev[R34_S];
    uint8_t backptr[R34_T][R34_S];
    uint8_t states[R34_T];

    r34_prepare_nibbles(dibits98, nibs);
    r34_prepare_reliability_weights(reliab98, rhi, rlo, 1);
    r34_run_viterbi_soft(nibs, rhi, rlo, metric_prev, backptr);

    r34_traceback_states(backptr, 0, states);
    r34_pack_states_to_bytes(states, out_bytes18);
    return 0;
}

int
dmr_r34_candidate_metric(const uint8_t* dibits98, const uint8_t* reliab98, const uint8_t bytes18[18], int* out_metric) {
    if (dibits98 == NULL || bytes18 == NULL || out_metric == NULL) {
        return -1;
    }

    uint8_t nibs[R34_T];
    uint8_t rhi[R34_T];
    uint8_t rlo[R34_T];
    uint8_t states[R34_T];
    int metric = 0;

    r34_prepare_nibbles(dibits98, nibs);
    r34_prepare_reliability_weights(reliab98, rhi, rlo, reliab98 != NULL);

    for (size_t group = 0; group < 6U; group++) {
        const size_t byte_index = group * 3U;
        uint32_t packed = ((uint32_t)bytes18[byte_index] << 16) | ((uint32_t)bytes18[byte_index + 1U] << 8)
                          | (uint32_t)bytes18[byte_index + 2U];
        for (size_t item = 0; item < 8U; item++) {
            states[group * 8U + item] = (uint8_t)((packed >> (21U - (item * 3U))) & 0x7U);
        }
    }
    states[48] = 0;

    for (int t = 0; t < R34_T; t++) {
        uint8_t previous = (t == 0) ? 0 : states[t - 1];
        uint8_t next = states[t];
        uint8_t point = dsd_trellis34_fsm[(previous * R34_S) + next];
        uint8_t expected_nibble = dsd_trellis34_inverse_constellation[point];
        metric += r34_weighted_nibble_cost(expected_nibble, nibs[t], rhi[t], rlo[t]);
    }

    *out_metric = metric;
    return 0;
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
