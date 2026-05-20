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
#include <dsd-neo/runtime/config.h>
#include <stdint.h>

static constexpr int P25P1_SOFT_ERASURE_THRESHOLD = 64;
static constexpr int P25P1_SOFT_HARD_OVERRIDE_MARGIN = 8;

extern "C" int
p25p1_get_erasure_threshold(void) {
    const dsdneoRuntimeConfig* cfg = dsd_neo_get_config();
    if (!cfg) {
        dsd_neo_config_init(nullptr);
        cfg = dsd_neo_get_config();
    }
    if (cfg) {
        if (cfg->p25p1_soft_erasure_threshold_is_set) {
            return cfg->p25p1_soft_erasure_threshold;
        }
        if (cfg->p25_soft_erasure_threshold_is_set) {
            return cfg->p25_soft_erasure_threshold;
        }
    }
    return P25P1_SOFT_ERASURE_THRESHOLD;
}

extern "C" int
p25_soft_hard_override_enabled(void) {
    const dsdneoRuntimeConfig* cfg = dsd_neo_get_config();
    if (!cfg) {
        dsd_neo_config_init(nullptr);
        cfg = dsd_neo_get_config();
    }
    if (cfg && cfg->p25_soft_hard_override_is_set) {
        return cfg->p25_soft_hard_override_enable != 0;
    }
    return 1;
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

extern "C" int
p25p1_build_rs_ranked_erasures(const uint8_t* data_reliab, int data_symbols, const uint8_t* parity_reliab,
                               int parity_symbols, int min_erasures, int* erasures, int max_erasures) {
    if (data_reliab == nullptr || parity_reliab == nullptr || erasures == nullptr || data_symbols < 0
        || parity_symbols < 0 || min_erasures < 0 || max_erasures <= 0) {
        return 0;
    }

    P25P1RsErasureCandidate candidates[64];
    int candidate_count = 0;
    int threshold_hits = 0;
    int threshold = p25p1_get_erasure_threshold();

    for (int i = 0; i < parity_symbols && candidate_count < 64; i++) {
        uint8_t rel = parity_reliab[i];
        if (rel < threshold) {
            threshold_hits++;
        }
        candidates[candidate_count].reliability = rel;
        candidates[candidate_count].position = i;
        candidate_count++;
    }
    for (int i = 0; i < data_symbols && candidate_count < 64; i++) {
        uint8_t rel = data_reliab[i];
        if (rel < threshold) {
            threshold_hits++;
        }
        candidates[candidate_count].reliability = rel;
        candidates[candidate_count].position = parity_symbols + i;
        candidate_count++;
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

    int out_count = threshold_hits > min_erasures ? threshold_hits : min_erasures;
    if (out_count > candidate_count) {
        out_count = candidate_count;
    }
    if (out_count > max_erasures) {
        out_count = max_erasures;
    }
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

static int
count_differences(const char* a, const char* b, int n) {
    int count = 0;
    for (int i = 0; i < n; i++) {
        if (a[i] != b[i]) {
            count++;
        }
    }
    return count;
}

extern "C" int
hamming_10_6_3_soft(const char* bits, const int* reliab, char* out_bits) {
    char orig[10];
    std::memcpy(orig, bits, 10);

    int best_penalty = 999999;
    char best_candidate[10];
    int found_valid = 0;
    int best_flips = 99;
    char hard_candidate[10];
    int hard_valid = 0;
    int hard_corrected = 0;
    int hard_penalty = 999999;

    Hamming_10_6_3_TableImpl hamming;
    char hex[6], parity[4];
    std::memcpy(hex, bits, 6);
    std::memcpy(parity, bits + 6, 4);

    int hard_result = hamming.decode(hex, parity);
    if (hard_result == 0 || hard_result == 1) {
        char hard_parity[4];
        std::memcpy(hard_candidate, hex, 6);
        hamming.encode(hex, hard_parity);
        std::memcpy(hard_candidate + 6, hard_parity, 4);
        hard_valid = 1;
        hard_corrected = (hard_result == 1);
        hard_penalty = compute_penalty(orig, hard_candidate, reliab, 10);
        best_penalty = hard_penalty;
        best_flips = count_differences(orig, hard_candidate, 10);
        std::memcpy(best_candidate, hard_candidate, 10);
        found_valid = 1;
    }

    int least_rel[5];
    find_k_least_reliable(reliab, 10, 5, least_rel);

    for (int mask = 0; mask < 32; mask++) {
        char candidate[10];
        std::memcpy(candidate, bits, 10);

        int num_flips = 0;
        for (int b = 0; b < 5; b++) {
            if (mask & (1 << b)) {
                candidate[least_rel[b]] ^= 1;
                num_flips++;
            }
        }
        if (num_flips > 2) {
            continue;
        }

        int syn = hamming_syndrome(candidate);
        if (syn == 0) {
            int penalty = compute_penalty(bits, candidate, reliab, 10);
            if (penalty < best_penalty || (penalty == best_penalty && num_flips < best_flips)) {
                best_penalty = penalty;
                best_flips = num_flips;
                std::memcpy(best_candidate, candidate, 10);
                found_valid = 1;
            }
        }
    }

    if (found_valid) {
        if (hard_valid && hard_corrected && std::memcmp(best_candidate, hard_candidate, 10) != 0) {
            if (!p25_soft_hard_override_enabled() || best_penalty + P25P1_SOFT_HARD_OVERRIDE_MARGIN >= hard_penalty) {
                std::memcpy(out_bits, hard_candidate, 10);
                return 1;
            }
        }
        std::memcpy(out_bits, best_candidate, 10);
        return count_differences(orig, best_candidate, 10) == 0 ? 0 : 1;
    }

    std::memcpy(out_bits, bits, 10);
    return 2;
}

extern "C" int
check_and_fix_golay_24_6_soft(char* data, char* parity, const int* reliab, int* fixed) {
    *fixed = 0;

    char orig[18];
    std::memcpy(orig, data, 6);
    std::memcpy(orig + 6, parity, 12);

    char hex_copy[6], parity_copy[12];
    std::memcpy(hex_copy, data, 6);
    std::memcpy(parity_copy, parity, 12);

    DSDGolay24 golay;
    int hard_fixed = 0;
    int hard_result = golay.decode_6(hex_copy, parity_copy, &hard_fixed);

    int best_penalty = 999999;
    char best_data[6];
    int best_fixed = 0;
    int found_valid = 0;
    char hard_data[6];
    int hard_valid = 0;
    int hard_corrected = 0;
    int hard_penalty = 999999;

    if (hard_result == 0) {
        char decoded[18];
        std::memcpy(decoded, hex_copy, 6);
        char enc_parity[12];
        golay.encode_6(hex_copy, enc_parity);
        std::memcpy(decoded + 6, enc_parity, 12);
        hard_valid = 1;
        hard_corrected = (hard_fixed > 0);
        hard_penalty = compute_penalty(orig, decoded, reliab, 18);
        best_penalty = hard_penalty;
        best_fixed = count_differences(orig, decoded, 18);
        std::memcpy(hard_data, hex_copy, 6);
        std::memcpy(best_data, hex_copy, 6);
        found_valid = 1;
    }

    int least_rel[8];
    find_k_least_reliable(reliab, 18, 8, least_rel);

    for (int mask = 0; mask < 256; mask++) {
        int weight = 0;
        for (int b = 0; b < 8; b++) {
            if (mask & (1 << b)) {
                weight++;
            }
        }
        if (weight > 4) {
            continue;
        }

        char candidate[18];
        std::memcpy(candidate, orig, 18);
        for (int b = 0; b < 8; b++) {
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
            int fixed_count = count_differences(orig, decoded, 18);
            if (penalty < best_penalty || (penalty == best_penalty && fixed_count < best_fixed)) {
                best_penalty = penalty;
                std::memcpy(best_data, cand_data, 6);
                best_fixed = fixed_count;
                found_valid = 1;
            }
        }
    }

    if (found_valid) {
        if (hard_valid && hard_corrected && std::memcmp(best_data, hard_data, 6) != 0) {
            if (!p25_soft_hard_override_enabled() || best_penalty + P25P1_SOFT_HARD_OVERRIDE_MARGIN >= hard_penalty) {
                std::memcpy(data, hard_data, 6);
                *fixed = hard_fixed;
                return 0;
            }
        }
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

    char orig[24];
    std::memcpy(orig, data, 12);
    std::memcpy(orig + 12, parity, 12);

    char dodeca_copy[12], parity_copy[12];
    std::memcpy(dodeca_copy, data, 12);
    std::memcpy(parity_copy, parity, 12);

    DSDGolay24 golay;
    int hard_fixed = 0;
    int hard_result = golay.decode_12(dodeca_copy, parity_copy, &hard_fixed);

    int best_penalty = 999999;
    char best_data[12];
    int best_fixed = 0;
    int found_valid = 0;
    char hard_data[12];
    int hard_valid = 0;
    int hard_corrected = 0;
    int hard_penalty = 999999;

    if (hard_result == 0) {
        char decoded[24];
        std::memcpy(decoded, dodeca_copy, 12);
        char enc_parity[12];
        golay.encode_12(dodeca_copy, enc_parity);
        std::memcpy(decoded + 12, enc_parity, 12);
        hard_valid = 1;
        hard_corrected = (hard_fixed > 0);
        hard_penalty = compute_penalty(orig, decoded, reliab, 24);
        best_penalty = hard_penalty;
        best_fixed = count_differences(orig, decoded, 24);
        std::memcpy(hard_data, dodeca_copy, 12);
        std::memcpy(best_data, dodeca_copy, 12);
        found_valid = 1;
    }

    int least_rel[8];
    find_k_least_reliable(reliab, 24, 8, least_rel);

    for (int mask = 0; mask < 256; mask++) {
        int weight = 0;
        for (int b = 0; b < 8; b++) {
            if (mask & (1 << b)) {
                weight++;
            }
        }
        if (weight > 4) {
            continue;
        }

        char candidate[24];
        std::memcpy(candidate, orig, 24);
        for (int b = 0; b < 8; b++) {
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
            int fixed_count = count_differences(orig, decoded, 24);
            if (penalty < best_penalty || (penalty == best_penalty && fixed_count < best_fixed)) {
                best_penalty = penalty;
                std::memcpy(best_data, cand_data, 12);
                best_fixed = fixed_count;
                found_valid = 1;
            }
        }
    }

    if (found_valid) {
        if (hard_valid && hard_corrected && std::memcmp(best_data, hard_data, 12) != 0) {
            if (!p25_soft_hard_override_enabled() || best_penalty + P25P1_SOFT_HARD_OVERRIDE_MARGIN >= hard_penalty) {
                std::memcpy(data, hard_data, 12);
                *fixed = hard_fixed;
                return 0;
            }
        }
        std::memcpy(data, best_data, 12);
        *fixed = best_fixed;
        return 0;
    }

    /* No valid candidate found */
    return 1;
}
