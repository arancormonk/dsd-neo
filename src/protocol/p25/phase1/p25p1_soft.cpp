// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Soft-decision FEC decoders for P25 Phase 1 non-vocoder paths.
 */

#include <cstring>
#include <dsd-neo/fec/Golay24.hpp>
#include <dsd-neo/fec/Hamming.hpp>
#include <dsd-neo/protocol/p25/p25p1_soft.h>
#include <stdint.h>

static constexpr int P25P1_SOFT_ERASURE_THRESHOLD = 64;

extern "C" int
p25p1_get_erasure_threshold(void) {
    return P25P1_SOFT_ERASURE_THRESHOLD;
}

static int
clamp_reliability(int reliab) {
    if (reliab < 0) {
        return 0;
    }
    if (reliab > 255) {
        return 255;
    }
    return reliab;
}

extern "C" uint8_t
p25p1_llr_reliability(const int16_t* llr, int bit_count) {
    if (llr == nullptr || bit_count <= 0) {
        return 0;
    }

    int min_r = 255;
    for (int i = 0; i < bit_count; i++) {
        int r = llr[i] < 0 ? -(int)llr[i] : (int)llr[i];
        r = clamp_reliability(r);
        if (r < min_r) {
            min_r = r;
        }
    }
    return (uint8_t)min_r;
}

typedef struct {
    uint8_t reliability;
    int position;
} P25P1RsErasureCandidate;

extern "C" int
p25p1_build_rs_erasures(const uint8_t* data_reliab, int data_symbols, const uint8_t* parity_reliab, int parity_symbols,
                        int* erasures, int max_erasures) {
    if (data_reliab == nullptr || parity_reliab == nullptr || erasures == nullptr || data_symbols < 0
        || parity_symbols < 0 || max_erasures <= 0) {
        return 0;
    }

    P25P1RsErasureCandidate candidates[64];
    int candidate_count = 0;
    int threshold = p25p1_get_erasure_threshold();

    for (int i = 0; i < parity_symbols && candidate_count < 64; i++) {
        uint8_t rel = parity_reliab[i];
        if (rel < threshold) {
            candidates[candidate_count].reliability = rel;
            candidates[candidate_count].position = i;
            candidate_count++;
        }
    }
    for (int i = 0; i < data_symbols && candidate_count < 64; i++) {
        uint8_t rel = data_reliab[i];
        if (rel < threshold) {
            candidates[candidate_count].reliability = rel;
            candidates[candidate_count].position = parity_symbols + i;
            candidate_count++;
        }
    }

    for (int i = 0; i < candidate_count; i++) {
        for (int j = i + 1; j < candidate_count; j++) {
            if (candidates[j].reliability < candidates[i].reliability
                || (candidates[j].reliability == candidates[i].reliability
                    && candidates[j].position < candidates[i].position)) {
                P25P1RsErasureCandidate tmp = candidates[i];
                candidates[i] = candidates[j];
                candidates[j] = tmp;
            }
        }
    }

    int out_count = candidate_count < max_erasures ? candidate_count : max_erasures;
    for (int i = 0; i < out_count; i++) {
        erasures[i] = candidates[i].position;
    }
    return out_count;
}

/* Helper: find indices of k smallest values in reliab[0..n-1].
 * Prefer configured erasure-threshold hits, then fill from the next weakest
 * symbols so the Chase list remains useful when only a few symbols are below
 * threshold.
 */
static void
find_k_least_reliable(const int* reliab, int n, int k, int* out_indices) {
    /* Simple selection: copy indices, partial sort by reliability */
    int indices[32]; /* max n we'll handle */
    for (int i = 0; i < n && i < 32; i++) {
        indices[i] = i;
    }

    /* Sort by reliability (good enough for small n). */
    for (int i = 0; i < n && i < 32; i++) {
        for (int j = i + 1; j < n; j++) {
            int rel_j = clamp_reliability(reliab[indices[j]]);
            int rel_i = clamp_reliability(reliab[indices[i]]);
            if (rel_j < rel_i || (rel_j == rel_i && indices[j] < indices[i])) {
                int tmp = indices[i];
                indices[i] = indices[j];
                indices[j] = tmp;
            }
        }
    }

    int threshold = p25p1_get_erasure_threshold();
    int selected[32] = {0};
    int out_count = 0;
    for (int i = 0; i < n && i < 32 && out_count < k; i++) {
        int idx = indices[i];
        if (clamp_reliability(reliab[idx]) < threshold) {
            out_indices[out_count++] = idx;
            selected[i] = 1;
        }
    }
    for (int i = 0; i < n && i < 32 && out_count < k; i++) {
        if (!selected[i]) {
            out_indices[out_count++] = indices[i];
        }
    }
}

/* Helper: compute syndrome for Hamming(10,6,3) */
static int
hamming_syndrome(const char* bits) {
    /* Syndrome bits from parity check matrix H:
     * h0 = 1110011000 -> bits 9,8,7,4,3
     * h1 = 1101010100 -> bits 9,8,6,4,2
     * h2 = 1011100010 -> bits 9,7,6,5,1
     * h3 = 0111100001 -> bits 8,7,6,5,0
     * bits[0]=bit9 (MSB), bits[9]=bit0 (LSB)
     */
    int s0 = bits[0] ^ bits[1] ^ bits[2] ^ bits[5] ^ bits[6];
    int s1 = bits[0] ^ bits[1] ^ bits[3] ^ bits[5] ^ bits[7];
    int s2 = bits[0] ^ bits[2] ^ bits[3] ^ bits[4] ^ bits[8];
    int s3 = bits[1] ^ bits[2] ^ bits[3] ^ bits[4] ^ bits[9];
    return (s0 << 3) | (s1 << 2) | (s2 << 1) | s3;
}

/* Compute penalty for flipping bits: confident bits are expensive to flip. */
static int
compute_penalty(const char* orig, const char* candidate, const int* reliab, int n) {
    int penalty = 0;
    for (int i = 0; i < n; i++) {
        if (orig[i] != candidate[i]) {
            penalty += clamp_reliability(reliab[i]);
        }
    }
    return penalty;
}

extern "C" int
hamming_10_6_3_soft(const char* bits, const int* reliab, char* out_bits) {
    char candidate[10];
    std::memcpy(candidate, bits, 10);

    /* Try hard decode first */
    int syndrome = hamming_syndrome(candidate);
    if (syndrome == 0) {
        /* No errors detected */
        std::memcpy(out_bits, bits, 10);
        return 0;
    }

    /* Use table implementation for hard decode */
    Hamming_10_6_3_TableImpl hamming;
    char hex[6], parity[4];
    std::memcpy(hex, bits, 6);
    std::memcpy(parity, bits + 6, 4);

    int hard_result = hamming.decode(hex, parity);
    if (hard_result == 1) {
        /* Single error corrected by hard decoder */
        std::memcpy(out_bits, hex, 6);
        std::memcpy(out_bits + 6, parity, 4);
        return 1;
    }

    if (hard_result == 0) {
        /* No error - shouldn't happen if syndrome != 0, but handle it */
        std::memcpy(out_bits, bits, 10);
        return 0;
    }

    /* Hard decode failed (2+ errors). Try soft decode with Chase-II. */
    /* Find 3 least reliable bit positions */
    int least_rel[3];
    find_k_least_reliable(reliab, 10, 3, least_rel);

    int best_penalty = 999999;
    char best_candidate[10];
    int found_valid = 0;
    int best_flips = 99;

    /* Try all 2^3 = 8 combinations of flipping these bits */
    for (int mask = 0; mask < 8; mask++) {
        std::memcpy(candidate, bits, 10);

        int num_flips = 0;
        for (int b = 0; b < 3; b++) {
            if (mask & (1 << b)) {
                candidate[least_rel[b]] ^= 1;
                num_flips++;
            }
        }

        /* Check if this candidate has valid syndrome */
        int syn = hamming_syndrome(candidate);
        if (syn == 0) {
            int penalty = compute_penalty(bits, candidate, reliab, 10);
            /* Prefer fewer flips if penalties are close */
            if (penalty < best_penalty || (penalty == best_penalty && num_flips < best_flips)) {
                best_penalty = penalty;
                best_flips = num_flips;
                std::memcpy(best_candidate, candidate, 10);
                found_valid = 1;
            }
        }
    }

    if (found_valid) {
        std::memcpy(out_bits, best_candidate, 10);
        return 1;
    }

    /* No valid candidate found */
    std::memcpy(out_bits, bits, 10);
    return 2;
}

extern "C" int
check_and_fix_golay_24_6_soft(char* data, char* parity, const int* reliab, int* fixed) {
    *fixed = 0;

    /* Try hard decode first */
    char hex_copy[6], parity_copy[12];
    std::memcpy(hex_copy, data, 6);
    std::memcpy(parity_copy, parity, 12);

    DSDGolay24 golay;
    int hard_fixed = 0;
    int hard_result = golay.decode_6(hex_copy, parity_copy, &hard_fixed);

    if (hard_result == 0) {
        /* Hard decode succeeded */
        std::memcpy(data, hex_copy, 6);
        *fixed = hard_fixed;
        return 0;
    }

    /* Hard decode failed. Try soft decode with Chase-style algorithm. */
    /* Find 5 least reliable bit positions across all 18 bits (6 data + 12 parity) */
    int least_rel[5];
    find_k_least_reliable(reliab, 18, 5, least_rel);

    int best_penalty = 999999;
    char best_data[6];
    int best_fixed = 0;
    int found_valid = 0;

    /* Combine data and parity for candidate generation */
    char orig[18], candidate[18];
    std::memcpy(orig, data, 6);
    std::memcpy(orig + 6, parity, 12);

    /* Try all weight-1, weight-2, weight-3 combinations: C(5,1)+C(5,2)+C(5,3) = 25 candidates */
    /* Plus original = 26 total */
    for (int mask = 0; mask < 32; mask++) {
        int weight = 0;
        for (int b = 0; b < 5; b++) {
            if (mask & (1 << b)) {
                weight++;
            }
        }
        if (weight > 3) {
            continue; /* Skip weight-4 and weight-5 */
        }

        std::memcpy(candidate, orig, 18);
        for (int b = 0; b < 5; b++) {
            if (mask & (1 << b)) {
                candidate[least_rel[b]] ^= 1;
            }
        }

        /* Try hard decode on this candidate */
        char cand_data[6], cand_parity[12];
        std::memcpy(cand_data, candidate, 6);
        std::memcpy(cand_parity, candidate + 6, 12);

        int cand_fixed = 0;
        int cand_result = golay.decode_6(cand_data, cand_parity, &cand_fixed);

        if (cand_result == 0) {
            /* Valid decode. Compute penalty based on original bits. */
            char decoded[18];
            std::memcpy(decoded, cand_data, 6);
            /* Re-encode to get corrected parity for penalty calc */
            char enc_parity[12];
            golay.encode_6(cand_data, enc_parity);
            std::memcpy(decoded + 6, enc_parity, 12);

            int penalty = compute_penalty(orig, decoded, reliab, 18);
            if (penalty < best_penalty) {
                best_penalty = penalty;
                std::memcpy(best_data, cand_data, 6);
                best_fixed = cand_fixed + weight;
                found_valid = 1;
            }
        }
    }

    if (found_valid) {
        std::memcpy(data, best_data, 6);
        *fixed = best_fixed;
        return 0;
    }

    /* No valid candidate found */
    return 1;
}

extern "C" int
check_and_fix_golay_24_12_soft(char* data, char* parity, const int* reliab, int* fixed) {
    *fixed = 0;

    /* Try hard decode first */
    char dodeca_copy[12], parity_copy[12];
    std::memcpy(dodeca_copy, data, 12);
    std::memcpy(parity_copy, parity, 12);

    DSDGolay24 golay;
    int hard_fixed = 0;
    int hard_result = golay.decode_12(dodeca_copy, parity_copy, &hard_fixed);

    if (hard_result == 0) {
        /* Hard decode succeeded */
        std::memcpy(data, dodeca_copy, 12);
        *fixed = hard_fixed;
        return 0;
    }

    /* Hard decode failed. Try soft decode with Chase-style algorithm. */
    /* Find 6 least reliable bit positions across all 24 bits */
    int least_rel[6];
    find_k_least_reliable(reliab, 24, 6, least_rel);

    int best_penalty = 999999;
    char best_data[12];
    int best_fixed = 0;
    int found_valid = 0;

    /* Combine data and parity for candidate generation */
    char orig[24], candidate[24];
    std::memcpy(orig, data, 12);
    std::memcpy(orig + 12, parity, 12);

    /* Try all weight-1..4 combinations: C(6,1)+C(6,2)+C(6,3)+C(6,4) = 6+15+20+15 = 56 candidates */
    for (int mask = 0; mask < 64; mask++) {
        int weight = 0;
        for (int b = 0; b < 6; b++) {
            if (mask & (1 << b)) {
                weight++;
            }
        }
        if (weight > 4) {
            continue; /* Skip weight-5 and weight-6 */
        }

        std::memcpy(candidate, orig, 24);
        for (int b = 0; b < 6; b++) {
            if (mask & (1 << b)) {
                candidate[least_rel[b]] ^= 1;
            }
        }

        /* Try hard decode on this candidate */
        char cand_data[12], cand_parity[12];
        std::memcpy(cand_data, candidate, 12);
        std::memcpy(cand_parity, candidate + 12, 12);

        int cand_fixed = 0;
        int cand_result = golay.decode_12(cand_data, cand_parity, &cand_fixed);

        if (cand_result == 0) {
            /* Valid decode. Compute penalty based on original bits. */
            char decoded[24];
            std::memcpy(decoded, cand_data, 12);
            /* Re-encode to get corrected parity for penalty calc */
            char enc_parity[12];
            golay.encode_12(cand_data, enc_parity);
            std::memcpy(decoded + 12, enc_parity, 12);

            int penalty = compute_penalty(orig, decoded, reliab, 24);
            if (penalty < best_penalty) {
                best_penalty = penalty;
                std::memcpy(best_data, cand_data, 12);
                best_fixed = cand_fixed + weight;
                found_valid = 1;
            }
        }
    }

    if (found_valid) {
        std::memcpy(data, best_data, 12);
        *fixed = best_fixed;
        return 0;
    }

    /* No valid candidate found */
    return 1;
}
