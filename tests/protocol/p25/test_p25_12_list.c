// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Focused tests for the P25 Phase 1 1/2-rate soft-decision list decoder.
 */

#include <dsd-neo/fec/trellis34.h>
#include <dsd-neo/protocol/p25/p25_12.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "dsd-neo/core/safe_api.h"

/* Fixed reference encoding of 96 2C 11 84 7E A0 39 55 C3 08 F1 6B. */
static const uint8_t k_reference_dibits[98] = {
    0U, 1U, 2U, 1U, 0U, 2U, 3U, 1U, 3U, 0U, 2U, 2U, 0U, 2U, 0U, 0U, 0U, 3U, 1U, 1U, 3U, 3U, 0U, 0U, 1U,
    1U, 1U, 3U, 0U, 1U, 3U, 0U, 2U, 1U, 0U, 3U, 2U, 2U, 3U, 3U, 0U, 0U, 1U, 1U, 0U, 2U, 2U, 0U, 3U, 1U,
    0U, 0U, 1U, 0U, 3U, 2U, 3U, 0U, 2U, 0U, 2U, 1U, 1U, 2U, 0U, 0U, 0U, 2U, 0U, 1U, 1U, 1U, 2U, 2U, 3U,
    1U, 1U, 1U, 3U, 0U, 3U, 2U, 1U, 2U, 0U, 2U, 1U, 3U, 0U, 0U, 3U, 3U, 2U, 1U, 3U, 0U, 1U, 0U,
};

static void
dibits_to_llr(const uint8_t dibits[98], int16_t bit_llr[196], int16_t magnitude) {
    for (int i = 0; i < 98; i++) {
        bit_llr[(i * 2) + 0] = ((dibits[i] >> 1) & 1) ? magnitude : (int16_t)-magnitude;
        bit_llr[(i * 2) + 1] = (dibits[i] & 1) ? magnitude : (int16_t)-magnitude;
    }
}

static void
set_dibit_llr_magnitude(const uint8_t dibits[98], int16_t bit_llr[196], int dibit_index, int16_t magnitude) {
    bit_llr[(dibit_index * 2) + 0] = ((dibits[dibit_index] >> 1) & 1) ? magnitude : (int16_t)-magnitude;
    bit_llr[(dibit_index * 2) + 1] = (dibits[dibit_index] & 1) ? magnitude : (int16_t)-magnitude;
}

enum { P25_12_REF_N_SYMS = 49, P25_12_REF_N_ST = 4 };

static const uint8_t k_p25_12_ref_dtm[16] = {2U, 12U, 1U, 15U, 14U, 0U, 13U, 3U, 9U, 7U, 10U, 4U, 5U, 11U, 6U, 8U};

typedef struct {
    uint8_t valid;
    uint32_t metric;
    uint8_t states[P25_12_REF_N_SYMS];
} p25_12_ref_path_t;

static uint32_t
p25_12_ref_llr_bit_cost(int16_t llr, int expected_bit) {
    if (expected_bit) {
        return (llr < 0) ? (uint32_t)(-llr) : 0U;
    }
    return (llr > 0) ? (uint32_t)llr : 0U;
}

static void
p25_12_ref_insert_path(p25_12_ref_path_t paths[P25_12_MAX_CANDIDATES], const p25_12_ref_path_t* candidate) {
    int insert_at = -1;
    for (int i = 0; i < P25_12_MAX_CANDIDATES; i++) {
        if (!paths[i].valid || candidate->metric < paths[i].metric) {
            insert_at = i;
            break;
        }
    }
    if (insert_at < 0) {
        return;
    }
    for (int i = P25_12_MAX_CANDIDATES - 1; i > insert_at; i--) {
        paths[i] = paths[i - 1];
    }
    paths[insert_at] = *candidate;
}

static uint32_t
p25_12_ref_symbol_cost(const int16_t llr_dei[196], int base, uint8_t expect) {
    return p25_12_ref_llr_bit_cost(llr_dei[base + 0], (expect >> 3) & 1)
           + p25_12_ref_llr_bit_cost(llr_dei[base + 1], (expect >> 2) & 1)
           + p25_12_ref_llr_bit_cost(llr_dei[base + 2], (expect >> 1) & 1)
           + p25_12_ref_llr_bit_cost(llr_dei[base + 3], expect & 1);
}

static void
p25_12_ref_expand_paths(p25_12_ref_path_t curr[P25_12_MAX_CANDIDATES], p25_12_ref_path_t prev[P25_12_MAX_CANDIDATES],
                        const int16_t llr_dei[196], int sym_idx, int st_prev, int st_next) {
    int base = sym_idx * 4;
    uint8_t expect = k_p25_12_ref_dtm[(st_prev << 2) | st_next] & 0xFU;
    uint32_t cost = p25_12_ref_symbol_cost(llr_dei, base, expect);

    for (int rank = 0; rank < P25_12_MAX_CANDIDATES; rank++) {
        if (!prev[rank].valid) {
            continue;
        }
        p25_12_ref_path_t candidate = prev[rank];
        candidate.metric += cost;
        candidate.states[sym_idx] = (uint8_t)st_next;
        p25_12_ref_insert_path(curr, &candidate);
    }
}

static void
p25_12_ref_pack_path(const uint8_t states[P25_12_REF_N_SYMS], uint8_t out[12]) {
    for (int i = 0; i < 12; i++) {
        out[i] = (uint8_t)((states[(i * 4) + 0] << 6) | (states[(i * 4) + 1] << 4) | (states[(i * 4) + 2] << 2)
                           | states[(i * 4) + 3]);
    }
}

static int
p25_12_ref_candidate_exists(const p25_12_candidate_t* candidates, int count, const uint8_t bytes[12]) {
    for (int i = 0; i < count; i++) {
        if (memcmp(candidates[i].bytes, bytes, 12) == 0) {
            return 1;
        }
    }
    return 0;
}

static void
p25_12_ref_insert_candidate(p25_12_candidate_t* candidates, int* count, int max_candidates, const uint8_t bytes[12],
                            uint32_t metric) {
    if (p25_12_ref_candidate_exists(candidates, *count, bytes)) {
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

static int
p25_12_ref_list(const int16_t bit_llr196[196], p25_12_candidate_t* candidates, int max_candidates) {
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

    p25_12_ref_path_t prev[P25_12_REF_N_ST][P25_12_MAX_CANDIDATES];
    p25_12_ref_path_t curr[P25_12_REF_N_ST][P25_12_MAX_CANDIDATES];
    DSD_MEMSET(prev, 0, sizeof(prev));
    for (int st = 0; st < P25_12_REF_N_ST; st++) {
        prev[st][0].valid = 1U;
        prev[st][0].metric = (st == 0) ? 0U : 256U;
    }

    for (int sym_idx = 0; sym_idx < P25_12_REF_N_SYMS; sym_idx++) {
        DSD_MEMSET(curr, 0, sizeof(curr));
        for (int st_prev = 0; st_prev < P25_12_REF_N_ST; st_prev++) {
            for (int st_next = 0; st_next < P25_12_REF_N_ST; st_next++) {
                p25_12_ref_expand_paths(curr[st_next], prev[st_prev], llr_dei, sym_idx, st_prev, st_next);
            }
        }
        DSD_MEMCPY(prev, curr, sizeof(prev));
    }

    int out_count = 0;
    for (int st = 0; st < P25_12_REF_N_ST; st++) {
        for (int rank = 0; rank < P25_12_MAX_CANDIDATES; rank++) {
            if (!prev[st][rank].valid) {
                continue;
            }
            uint8_t bytes[12];
            p25_12_ref_pack_path(prev[st][rank].states, bytes);
            p25_12_ref_insert_candidate(candidates, &out_count, max_candidates, bytes, prev[st][rank].metric);
        }
    }
    return out_count;
}

static int
p25_12_compare_reference(const char* label, const int16_t bit_llr[196], int max_candidates) {
    p25_12_candidate_t expected[P25_12_MAX_CANDIDATES];
    p25_12_candidate_t actual[P25_12_MAX_CANDIDATES];
    DSD_MEMSET(expected, 0, sizeof(expected));
    DSD_MEMSET(actual, 0, sizeof(actual));

    int expected_count = p25_12_ref_list(bit_llr, expected, max_candidates);
    int actual_count = p25_12_soft_llr_list(NULL, bit_llr, actual, max_candidates);
    if (actual_count != expected_count) {
        DSD_FPRINTF(stderr, "%s count mismatch max=%d actual=%d expected=%d\n", label, max_candidates, actual_count,
                    expected_count);
        return 1;
    }
    for (int i = 0; i < actual_count; i++) {
        if (actual[i].metric != expected[i].metric || memcmp(actual[i].bytes, expected[i].bytes, 12) != 0) {
            DSD_FPRINTF(stderr, "%s candidate mismatch max=%d index=%d metric=%u/%u\n", label, max_candidates, i,
                        actual[i].metric, expected[i].metric);
            return 1;
        }
        for (int j = 0; j < i; j++) {
            if (memcmp(actual[i].bytes, actual[j].bytes, 12) == 0) {
                DSD_FPRINTF(stderr, "%s duplicate candidate max=%d index=%d/%d\n", label, max_candidates, j, i);
                return 1;
            }
        }
    }
    return 0;
}

static void
p25_12_fill_seeded_llr(int16_t bit_llr[196], uint32_t seed) {
    for (int i = 0; i < 196; i++) {
        seed = (seed * 1664525U) + 1013904223U;
        int32_t value = (int32_t)((seed >> 16) & 0xFFFFU) - 32768;
        bit_llr[i] = (int16_t)value;
    }
    bit_llr[0] = INT16_MIN;
    bit_llr[1] = INT16_MAX;
    bit_llr[2] = 0;
}

static int
p25_12_test_exact_equivalence(const int16_t clean_llr[196], const int16_t noisy_llr[196]) {
    static const int limits[] = {1, 2, 3, 4, 5, 6, 7, P25_12_MAX_CANDIDATES, P25_12_MAX_CANDIDATES + 3};
    int16_t zero_llr[196];
    int16_t seeded_llr[196];
    DSD_MEMSET(zero_llr, 0, sizeof(zero_llr));

    for (size_t i = 0; i < sizeof(limits) / sizeof(limits[0]); i++) {
        int max_candidates = limits[i];
        if (p25_12_compare_reference("clean", clean_llr, max_candidates) != 0
            || p25_12_compare_reference("noisy", noisy_llr, max_candidates) != 0
            || p25_12_compare_reference("ties", zero_llr, max_candidates) != 0) {
            return 1;
        }
        for (uint32_t seed_idx = 0; seed_idx < 8U; seed_idx++) {
            p25_12_fill_seeded_llr(seeded_llr, 0xC001D00DU ^ (seed_idx * 0x9E3779B9U));
            if (p25_12_compare_reference("seeded", seeded_llr, max_candidates) != 0) {
                return 1;
            }
        }
    }
    return 0;
}

int
main(void) {
    const uint8_t payload[12] = {0x96, 0x2C, 0x11, 0x84, 0x7E, 0xA0, 0x39, 0x55, 0xC3, 0x08, 0xF1, 0x6B};

    int16_t bit_llr[196];
    dibits_to_llr(k_reference_dibits, bit_llr, 200);

    p25_12_candidate_t list[P25_12_MAX_CANDIDATES];
    p25_12_candidate_t guard_list[P25_12_MAX_CANDIDATES];
    p25_12_candidate_t guard_before[P25_12_MAX_CANDIDATES];
    DSD_MEMSET(guard_list, 0xA5, sizeof(guard_list));
    DSD_MEMCPY(guard_before, guard_list, sizeof(guard_before));
    if (p25_12_soft_llr_list(k_reference_dibits, NULL, guard_list, P25_12_MAX_CANDIDATES) != 0
        || p25_12_soft_llr_list(k_reference_dibits, bit_llr, NULL, P25_12_MAX_CANDIDATES) != 0
        || p25_12_soft_llr_list(k_reference_dibits, bit_llr, guard_list, 0) != 0
        || p25_12_soft_llr_list(k_reference_dibits, bit_llr, guard_list, -1) != 0
        || memcmp(guard_list, guard_before, sizeof(guard_list)) != 0) {
        DSD_FPRINTF(stderr, "P25 1/2 list guard failed\n");
        return 1;
    }

    int list_count = p25_12_soft_llr_list(k_reference_dibits, bit_llr, list, P25_12_MAX_CANDIDATES);
    if (list_count <= 0 || memcmp(list[0].bytes, payload, sizeof(payload)) != 0) {
        DSD_FPRINTF(stderr, "clean P25 1/2 list decode failed count=%d\n", list_count);
        return 1;
    }

    uint8_t best[12] = {0};
    int metric = p25_12_soft_llr(k_reference_dibits, bit_llr, best);
    if (metric != 0 || memcmp(best, payload, sizeof(payload)) != 0) {
        DSD_FPRINTF(stderr, "clean P25 1/2 single decode failed metric=%d\n", metric);
        return 1;
    }

    DSD_MEMSET(list, 0, sizeof(list));
    list_count = p25_12_soft_llr_list(k_reference_dibits, bit_llr, list, P25_12_MAX_CANDIDATES + 3);
    if (list_count != P25_12_MAX_CANDIDATES || memcmp(list[0].bytes, payload, sizeof(payload)) != 0) {
        DSD_FPRINTF(stderr, "clamped P25 1/2 list decode failed count=%d\n", list_count);
        return 1;
    }

    uint8_t noisy_dibits[98];
    DSD_MEMCPY(noisy_dibits, k_reference_dibits, sizeof(noisy_dibits));
    noisy_dibits[9] ^= 1U;
    dibits_to_llr(noisy_dibits, bit_llr, 200);
    set_dibit_llr_magnitude(noisy_dibits, bit_llr, 9, 10);
    int16_t noisy_llr[196];
    DSD_MEMCPY(noisy_llr, bit_llr, sizeof(noisy_llr));

    DSD_MEMSET(list, 0, sizeof(list));
    list_count = p25_12_soft_llr_list(noisy_dibits, bit_llr, list, P25_12_MAX_CANDIDATES);
    int found_original = 0;
    for (int i = 0; i < list_count; i++) {
        if (memcmp(list[i].bytes, payload, sizeof(payload)) == 0) {
            found_original = 1;
            break;
        }
    }
    if (!found_original) {
        DSD_FPRINTF(stderr, "noisy P25 1/2 list decode did not include original count=%d\n", list_count);
        return 2;
    }

    DSD_MEMSET(best, 0, sizeof(best));
    (void)p25_12_soft_llr(noisy_dibits, bit_llr, best);
    if (memcmp(best, list[0].bytes, sizeof(best)) != 0) {
        DSD_FPRINTF(stderr, "noisy P25 1/2 single/list best mismatch\n");
        return 2;
    }

    int16_t clean_llr[196];
    dibits_to_llr(k_reference_dibits, clean_llr, 200);
    if (p25_12_test_exact_equivalence(clean_llr, noisy_llr) != 0) {
        return 3;
    }

    DSD_FPRINTF(stderr, "P25 1/2-rate LLR list tests OK\n");
    return 0;
}
