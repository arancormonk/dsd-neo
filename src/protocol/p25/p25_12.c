// SPDX-License-Identifier: ISC
/*-------------------------------------------------------------------------------
 * p25_12.c
 * P25p1 1/2 Rate Simple Trellis Decoder
 *
 * LWVMOBILE
 * 2023-10 DSD-FME Florida Man Edition
 *-----------------------------------------------------------------------------*/

#include <dsd-neo/fec/trellis34.h>
#include <dsd-neo/protocol/p25/p25_12.h>
#include <stdint.h>
#include <string.h>
#include "dsd-neo/core/safe_api.h"

//this is a dibit-pair to trellis dibit transition matrix (SDRTrunk and Ossmann)
//when evaluating hamming distance, we want to use this xor the dibit-pair nib,
//and not the fsm xor the constellation point
static const uint8_t p25_dtm[16] = {2, 12, 1, 15, 14, 0, 13, 3, 9, 7, 10, 4, 5, 11, 6, 8};

static uint32_t
p25_llr_bit_cost(int16_t llr, int expected_bit) {
    if (expected_bit) {
        return (llr < 0) ? (uint32_t)(-llr) : 0U;
    }
    return (llr > 0) ? (uint32_t)llr : 0U;
}

enum { P25_12_N_SYMS = 49, P25_12_N_ST = 4 };

static void
p25_12_insert_survivor(uint32_t metrics[P25_12_MAX_CANDIDATES], uint8_t backptrs[P25_12_MAX_CANDIDATES],
                       uint32_t candidate_metric, uint8_t candidate_backptr) {
    int insert_at = -1;
    for (int i = 0; i < P25_12_MAX_CANDIDATES; i++) {
        if (candidate_metric < metrics[i]) {
            insert_at = i;
            break;
        }
    }
    if (insert_at < 0) {
        return;
    }
    for (int i = P25_12_MAX_CANDIDATES - 1; i > insert_at; i--) {
        metrics[i] = metrics[i - 1];
        backptrs[i] = backptrs[i - 1];
    }
    metrics[insert_at] = candidate_metric;
    backptrs[insert_at] = candidate_backptr;
}

static void
p25_12_reset_metrics(uint32_t metrics[P25_12_N_ST][P25_12_MAX_CANDIDATES]) {
    for (int state = 0; state < P25_12_N_ST; state++) {
        for (int rank = 0; rank < P25_12_MAX_CANDIDATES; rank++) {
            metrics[state][rank] = UINT32_MAX;
        }
    }
}

static void
p25_12_pack_path_bytes(const uint8_t states[P25_12_N_SYMS], uint8_t out[12]) {
    for (int i = 0; i < 12; i++) {
        out[i] = (uint8_t)((states[(i * 4) + 0] << 6) | (states[(i * 4) + 1] << 4) | (states[(i * 4) + 2] << 2)
                           | states[(i * 4) + 3]);
    }
}

static int
p25_12_candidate_exists(const p25_12_candidate_t* candidates, int count, const uint8_t bytes[12]) {
    for (int i = 0; i < count; i++) {
        if (memcmp(candidates[i].bytes, bytes, 12) == 0) {
            return 1;
        }
    }
    return 0;
}

static void
p25_12_insert_candidate(p25_12_candidate_t* candidates, int* count, int max_candidates, const uint8_t bytes[12],
                        uint32_t metric) {
    if (p25_12_candidate_exists(candidates, *count, bytes)) {
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
    DSD_MEMCPY(candidates[insert_at].bytes, bytes, 12);
    candidates[insert_at].metric = metric;
}

static inline uint32_t
p25_12_symbol_cost(const int16_t* llr_dei, int base, uint8_t expect) {
    return p25_llr_bit_cost(llr_dei[base + 0], (expect >> 3) & 1)
           + p25_llr_bit_cost(llr_dei[base + 1], (expect >> 2) & 1)
           + p25_llr_bit_cost(llr_dei[base + 2], (expect >> 1) & 1) + p25_llr_bit_cost(llr_dei[base + 3], expect & 1);
}

static void
p25_12_expand_survivors_for_symbol(uint32_t curr_metric[P25_12_MAX_CANDIDATES],
                                   uint8_t curr_backptr[P25_12_MAX_CANDIDATES],
                                   const uint32_t prev_metric[P25_12_MAX_CANDIDATES], const int16_t* llr_dei,
                                   int sym_idx, int st_prev, int st_next) {
    int base = sym_idx * 4;
    uint8_t expect = p25_dtm[(st_prev << 2) | st_next] & 0xF;
    uint32_t cost = p25_12_symbol_cost(llr_dei, base, expect);

    for (int rank = 0; rank < P25_12_MAX_CANDIDATES; rank++) {
        if (prev_metric[rank] == UINT32_MAX) {
            continue;
        }
        uint32_t candidate_metric = prev_metric[rank] + cost;
        uint8_t candidate_backptr = (uint8_t)((st_prev << 3) | rank);
        p25_12_insert_survivor(curr_metric, curr_backptr, candidate_metric, candidate_backptr);
    }
}

static void
p25_12_traceback(const uint8_t* backptr, int final_state, int final_rank, uint8_t states[P25_12_N_SYMS]) {
    int state = final_state;
    int rank = final_rank;
    for (int sym_idx = P25_12_N_SYMS; sym_idx > 0;) {
        sym_idx--;
        states[sym_idx] = (uint8_t)state;
        size_t backptr_index = (((size_t)sym_idx * P25_12_N_ST + (size_t)state) * P25_12_MAX_CANDIDATES) + (size_t)rank;
        uint8_t predecessor = backptr[backptr_index];
        state = (predecessor >> 3) & 0x3;
        rank = predecessor & 0x7;
    }
}

int
p25_12_soft_llr_list(const uint8_t* input, const int16_t* bit_llr196, p25_12_candidate_t* candidates,
                     int max_candidates) {
    (void)input;
    if (bit_llr196 == NULL || candidates == NULL || max_candidates <= 0) {
        return 0;
    }
    if (max_candidates > P25_12_MAX_CANDIDATES) {
        max_candidates = P25_12_MAX_CANDIDATES;
    }

    int16_t llr_dei[196];
    DSD_MEMSET(llr_dei, 0, sizeof(llr_dei));
    for (int i = 0; i < 98; i++) {
        int p = dsd_trellis_interleave_98[i];
        llr_dei[(p * 2) + 0] = bit_llr196[(i * 2) + 0];
        llr_dei[(p * 2) + 1] = bit_llr196[(i * 2) + 1];
    }

    uint32_t metric_a[P25_12_N_ST][P25_12_MAX_CANDIDATES];
    uint32_t metric_b[P25_12_N_ST][P25_12_MAX_CANDIDATES];
    p25_12_reset_metrics(metric_a);
    p25_12_reset_metrics(metric_b);
    uint32_t (*prev_metric)[P25_12_MAX_CANDIDATES] = metric_a;
    uint32_t (*curr_metric)[P25_12_MAX_CANDIDATES] = metric_b;
    uint8_t backptr[P25_12_N_SYMS][P25_12_N_ST][P25_12_MAX_CANDIDATES];
    DSD_MEMSET(backptr, 0, sizeof(backptr));
    for (int st = 0; st < P25_12_N_ST; st++) {
        prev_metric[st][0] = (st == 0) ? 0U : 256U;
    }

    for (int i = 0; i < P25_12_N_SYMS; i++) {
        p25_12_reset_metrics(curr_metric);
        for (int st_prev = 0; st_prev < P25_12_N_ST; st_prev++) {
            for (int st_next = 0; st_next < P25_12_N_ST; st_next++) {
                p25_12_expand_survivors_for_symbol(curr_metric[st_next], backptr[i][st_next], prev_metric[st_prev],
                                                   llr_dei, i, st_prev, st_next);
            }
        }
        uint32_t (*swap_metric)[P25_12_MAX_CANDIDATES] = prev_metric;
        prev_metric = curr_metric;
        curr_metric = swap_metric;
    }

    int out_count = 0;
    for (int st = 0; st < P25_12_N_ST; st++) {
        for (int rank = 0; rank < P25_12_MAX_CANDIDATES; rank++) {
            if (prev_metric[st][rank] == UINT32_MAX) {
                continue;
            }
            uint8_t states[P25_12_N_SYMS];
            uint8_t bytes[12];
            p25_12_traceback(&backptr[0][0][0], st, rank, states);
            p25_12_pack_path_bytes(states, bytes);
            p25_12_insert_candidate(candidates, &out_count, max_candidates, bytes, prev_metric[st][rank]);
        }
    }
    return out_count;
}

int
p25_12_soft_llr(const uint8_t* input, const int16_t* bit_llr196, uint8_t treturn[12]) {
    enum { N_SYMS = 49, N_ST = 4 };

    int i, j;
    (void)input;

    int16_t llr_dei[196];
    DSD_MEMSET(llr_dei, 0, sizeof(llr_dei));
    for (i = 0; i < 98; i++) {
        int p = dsd_trellis_interleave_98[i];
        llr_dei[(p * 2) + 0] = bit_llr196[(i * 2) + 0];
        llr_dei[(p * 2) + 1] = bit_llr196[(i * 2) + 1];
    }

    uint32_t prev_metric[N_ST];
    uint32_t curr_metric[N_ST];
    uint8_t backptr[N_SYMS][N_ST];

    /* Initialize metrics: bias start at state 0 */
    for (j = 0; j < N_ST; j++) {
        prev_metric[j] = (j == 0) ? 0 : 256;
    }

    for (i = 0; i < N_SYMS; i++) {
        /* For each candidate next state, pick best predecessor */
        for (int st_next = 0; st_next < N_ST; st_next++) {
            uint32_t best = 0xFFFFFFFFu;
            uint8_t best_prev = 0;
            for (int st_prev = 0; st_prev < N_ST; st_prev++) {
                /* Expected transition nibble from (st_prev -> st_next) */
                uint8_t expect = p25_dtm[(st_prev << 2) | st_next] & 0xF;
                int base = i * 4;
                uint32_t cost = p25_llr_bit_cost(llr_dei[base + 0], (expect >> 3) & 1)
                                + p25_llr_bit_cost(llr_dei[base + 1], (expect >> 2) & 1)
                                + p25_llr_bit_cost(llr_dei[base + 2], (expect >> 1) & 1)
                                + p25_llr_bit_cost(llr_dei[base + 3], expect & 1);

                uint32_t m = prev_metric[st_prev] + cost;
                if (m < best) {
                    best = m;
                    best_prev = (uint8_t)st_prev;
                }
            }
            curr_metric[st_next] = best;
            backptr[i][st_next] = best_prev;
        }
        /* Roll metrics */
        for (j = 0; j < N_ST; j++) {
            prev_metric[j] = curr_metric[j];
        }
    }

    /* Select best ending state */
    uint32_t best_final = curr_metric[0];
    int st = 0;
    for (j = 1; j < N_ST; j++) {
        if (curr_metric[j] < best_final) {
            best_final = curr_metric[j];
            st = j;
        }
    }

    /* Traceback to recover tdibits (next states) */
    uint8_t tdibits[N_SYMS];
    for (i = N_SYMS; i > 0;) {
        i--;
        tdibits[i] = (uint8_t)st;
        st = backptr[i][st];
    }

    /* Pack first 48 tdibits into 12 bytes (MSB-first) */
    for (i = 0; i < 12; i++) {
        treturn[i] = (uint8_t)((tdibits[(i * 4) + 0] << 6) | (tdibits[(i * 4) + 1] << 4) | (tdibits[(i * 4) + 2] << 2)
                               | (tdibits[(i * 4) + 3]));
    }

    /* Return normalized metric (divide by 256 to roughly match hard-decision scale) */
    return (int)(best_final >> 8);
}

/*
 * Soft-decision 1/2-rate trellis decoder.
 * reliab98: per-dibit reliability (0=uncertain, 255=confident) parallel to input.
 */
