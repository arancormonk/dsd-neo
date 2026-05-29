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
#include <dsd-neo/platform/posix_compat.h>
#include <dsd-neo/protocol/p25/p25p1_soft.h>
#include <dsd-neo/runtime/config.h>
#include <stdint.h>
#include "dsd-neo/core/safe_api.h"

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

namespace {

struct P25P1RsErasureCandidate {
    uint8_t reliability;
    int position;
};

static void
p25p1_sort_erasure_candidates(P25P1RsErasureCandidate* candidates, int candidate_count) {
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
}

static int
p25p1_collect_rs_candidates(const uint8_t* data_reliab, int data_symbols, const uint8_t* parity_reliab,
                            int parity_symbols, int threshold, int include_all, P25P1RsErasureCandidate* candidates,
                            int max_candidates, int* threshold_hits) {
    int candidate_count = 0;
    int hits = 0;

    for (int i = 0; i < parity_symbols && candidate_count < max_candidates; i++) {
        uint8_t rel = parity_reliab[i];
        int below_threshold = (rel < threshold) ? 1 : 0;
        hits += below_threshold;
        if (include_all || below_threshold) {
            candidates[candidate_count].reliability = rel;
            candidates[candidate_count].position = i;
            candidate_count++;
        }
    }
    for (int i = 0; i < data_symbols && candidate_count < max_candidates; i++) {
        uint8_t rel = data_reliab[i];
        int below_threshold = (rel < threshold) ? 1 : 0;
        hits += below_threshold;
        if (include_all || below_threshold) {
            candidates[candidate_count].reliability = rel;
            candidates[candidate_count].position = parity_symbols + i;
            candidate_count++;
        }
    }

    if (threshold_hits != nullptr) {
        *threshold_hits = hits;
    }
    return candidate_count;
}

} // namespace

extern "C" int
p25p1_build_rs_erasures(const uint8_t* data_reliab, int data_symbols, const uint8_t* parity_reliab, int parity_symbols,
                        int* erasures, int max_erasures) {
    if (data_reliab == nullptr || parity_reliab == nullptr || erasures == nullptr || data_symbols < 0
        || parity_symbols < 0 || max_erasures <= 0) {
        return 0;
    }

    P25P1RsErasureCandidate candidates[64];
    int threshold = p25p1_get_erasure_threshold();
    int candidate_count =
        p25p1_collect_rs_candidates(data_reliab, data_symbols, parity_reliab, parity_symbols, threshold,
                                    /*include_all*/ 0, candidates, 64, nullptr);
    p25p1_sort_erasure_candidates(candidates, candidate_count);

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
    int threshold = p25p1_get_erasure_threshold();
    int threshold_hits = 0;
    int candidate_count =
        p25p1_collect_rs_candidates(data_reliab, data_symbols, parity_reliab, parity_symbols, threshold,
                                    /*include_all*/ 1, candidates, 64, &threshold_hits);
    p25p1_sort_erasure_candidates(candidates, candidate_count);

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
    if (reliab == nullptr || out_indices == nullptr || n <= 0 || k <= 0) {
        return;
    }
    for (int i = 0; i < k; i++) {
        out_indices[i] = 0;
    }

    int count = (n < 32) ? n : 32;
    if (k > count) {
        k = count;
    }
    P25P1RsErasureCandidate candidates[32];
    for (int i = 0; i < count; i++) {
        candidates[i].reliability = (uint8_t)clamp_reliability(reliab[i]);
        candidates[i].position = i;
    }
    p25p1_sort_erasure_candidates(candidates, count);

    int threshold = p25p1_get_erasure_threshold();
    int out_count = 0;
    for (int i = 0; i < count && out_count < k; i++) {
        if (candidates[i].reliability < threshold) {
            out_indices[out_count++] = candidates[i].position;
        }
    }
    for (int i = 0; i < count && out_count < k; i++) {
        if (candidates[i].reliability >= threshold) {
            out_indices[out_count++] = candidates[i].position;
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

namespace {

struct P25P1SoftHammingState {
    int best_penalty;
    int best_flips;
    int found_valid;
    char best_candidate[10];
    int hard_valid;
    int hard_corrected;
    int hard_penalty;
    char hard_candidate[10];
};

} // namespace

static void
p25p1_hamming_seed_from_hard(const char* bits, const int* reliab, Hamming_10_6_3_TableImpl* hamming,
                             P25P1SoftHammingState* st) {
    char hex[6], parity[4];
    DSD_MEMCPY(hex, bits, 6);
    DSD_MEMCPY(parity, bits + 6, 4);

    int hard_result = hamming->decode(hex, parity);
    if (hard_result != 0 && hard_result != 1) {
        return;
    }

    char hard_parity[4];
    DSD_MEMCPY(st->hard_candidate, hex, 6);
    hamming->encode(hex, hard_parity);
    DSD_MEMCPY(st->hard_candidate + 6, hard_parity, 4);

    st->hard_valid = 1;
    st->hard_corrected = (hard_result == 1);
    st->hard_penalty = compute_penalty(bits, st->hard_candidate, reliab, 10);
    st->best_penalty = st->hard_penalty;
    st->best_flips = count_differences(bits, st->hard_candidate, 10);
    DSD_MEMCPY(st->best_candidate, st->hard_candidate, 10);
    st->found_valid = 1;
}

static void
p25p1_hamming_search_masked_candidates(const char* bits, const int* reliab, const int* least_rel,
                                       P25P1SoftHammingState* st) {
    for (int mask = 0; mask < 32; mask++) {
        char candidate[10];
        DSD_MEMCPY(candidate, bits, 10);

        int num_flips = 0;
        for (int b = 0; b < 5; b++) {
            if (mask & (1 << b)) {
                candidate[least_rel[b]] ^= 1;
                num_flips++;
            }
        }
        if (num_flips > 2 || hamming_syndrome(candidate) != 0) {
            continue;
        }

        int penalty = compute_penalty(bits, candidate, reliab, 10);
        if (penalty < st->best_penalty || (penalty == st->best_penalty && num_flips < st->best_flips)) {
            st->best_penalty = penalty;
            st->best_flips = num_flips;
            DSD_MEMCPY(st->best_candidate, candidate, 10);
            st->found_valid = 1;
        }
    }
}

namespace {

struct p25p1_golay_seed_result {
    int* best_penalty;
    int* best_fixed;
    char* best_data;
    int* found_valid;
    char* hard_data;
    int* hard_valid;
    int* hard_corrected;
    int* hard_penalty;
};

} // namespace

static void
p25p1_golay6_seed_hard(const char* orig, const int* reliab, const DSDGolay24* golay, const char* hex_copy,
                       int hard_fixed, const p25p1_golay_seed_result* out) {
    char decoded[18];
    DSD_MEMCPY(decoded, hex_copy, 6);
    char enc_parity[12];
    golay->encode_6(hex_copy, enc_parity);
    DSD_MEMCPY(decoded + 6, enc_parity, 12);

    *out->hard_valid = 1;
    *out->hard_corrected = (hard_fixed > 0);
    *out->hard_penalty = compute_penalty(orig, decoded, reliab, 18);
    *out->best_penalty = *out->hard_penalty;
    *out->best_fixed = count_differences(orig, decoded, 18);
    DSD_MEMCPY(out->hard_data, hex_copy, 6);
    DSD_MEMCPY(out->best_data, hex_copy, 6);
    *out->found_valid = 1;
}

static void
p25p1_golay6_search(const char* orig, const int* reliab, const int* least_rel, DSDGolay24* golay, int* best_penalty,
                    int* best_fixed, char best_data[6], int* found_valid) {
    for (int mask = 0; mask < 256; mask++) {
        if (dsd_popcount64((uint64_t)mask) > 4) {
            continue;
        }

        char candidate[18];
        DSD_MEMCPY(candidate, orig, 18);
        for (int b = 0; b < 8; b++) {
            if (mask & (1 << b)) {
                candidate[least_rel[b]] ^= 1;
            }
        }

        char cand_data[6], cand_parity[12];
        DSD_MEMCPY(cand_data, candidate, 6);
        DSD_MEMCPY(cand_parity, candidate + 6, 12);

        int cand_fixed = 0;
        if (golay->decode_6(cand_data, cand_parity, &cand_fixed) != 0) {
            continue;
        }

        char decoded[18];
        DSD_MEMCPY(decoded, cand_data, 6);
        char enc_parity[12];
        golay->encode_6(cand_data, enc_parity);
        DSD_MEMCPY(decoded + 6, enc_parity, 12);

        int penalty = compute_penalty(orig, decoded, reliab, 18);
        int fixed_count = count_differences(orig, decoded, 18);
        if (penalty < *best_penalty || (penalty == *best_penalty && fixed_count < *best_fixed)) {
            *best_penalty = penalty;
            DSD_MEMCPY(best_data, cand_data, 6);
            *best_fixed = fixed_count;
            *found_valid = 1;
        }
    }
}

static void
p25p1_golay12_seed_hard(const char* orig, const int* reliab, const DSDGolay24* golay, const char* dodeca_copy,
                        int hard_fixed, const p25p1_golay_seed_result* out) {
    char decoded[24];
    DSD_MEMCPY(decoded, dodeca_copy, 12);
    char enc_parity[12];
    golay->encode_12(dodeca_copy, enc_parity);
    DSD_MEMCPY(decoded + 12, enc_parity, 12);

    *out->hard_valid = 1;
    *out->hard_corrected = (hard_fixed > 0);
    *out->hard_penalty = compute_penalty(orig, decoded, reliab, 24);
    *out->best_penalty = *out->hard_penalty;
    *out->best_fixed = count_differences(orig, decoded, 24);
    DSD_MEMCPY(out->hard_data, dodeca_copy, 12);
    DSD_MEMCPY(out->best_data, dodeca_copy, 12);
    *out->found_valid = 1;
}

static void
p25p1_golay12_search(const char* orig, const int* reliab, const int* least_rel, DSDGolay24* golay, int* best_penalty,
                     int* best_fixed, char best_data[12], int* found_valid) {
    for (int mask = 0; mask < 256; mask++) {
        if (dsd_popcount64((uint64_t)mask) > 4) {
            continue;
        }

        char candidate[24];
        DSD_MEMCPY(candidate, orig, 24);
        for (int b = 0; b < 8; b++) {
            if (mask & (1 << b)) {
                candidate[least_rel[b]] ^= 1;
            }
        }

        char cand_data[12], cand_parity[12];
        DSD_MEMCPY(cand_data, candidate, 12);
        DSD_MEMCPY(cand_parity, candidate + 12, 12);

        int cand_fixed = 0;
        if (golay->decode_12(cand_data, cand_parity, &cand_fixed) != 0) {
            continue;
        }

        char decoded[24];
        DSD_MEMCPY(decoded, cand_data, 12);
        char enc_parity[12];
        golay->encode_12(cand_data, enc_parity);
        DSD_MEMCPY(decoded + 12, enc_parity, 12);

        int penalty = compute_penalty(orig, decoded, reliab, 24);
        int fixed_count = count_differences(orig, decoded, 24);
        if (penalty < *best_penalty || (penalty == *best_penalty && fixed_count < *best_fixed)) {
            *best_penalty = penalty;
            DSD_MEMCPY(best_data, cand_data, 12);
            *best_fixed = fixed_count;
            *found_valid = 1;
        }
    }
}

extern "C" int
hamming_10_6_3_soft(const char* bits, const int* reliab, char* out_bits) {
    if (bits == nullptr || reliab == nullptr || out_bits == nullptr) {
        return 2;
    }

    char orig[10];
    DSD_MEMCPY(orig, bits, 10);

    P25P1SoftHammingState st = {999999, 99, 0, {0}, 0, 0, 999999, {0}};

    Hamming_10_6_3_TableImpl hamming;
    p25p1_hamming_seed_from_hard(bits, reliab, &hamming, &st);

    int least_rel[5];
    find_k_least_reliable(reliab, 10, 5, least_rel);
    p25p1_hamming_search_masked_candidates(bits, reliab, least_rel, &st);

    if (st.found_valid) {
        if (st.hard_valid && st.hard_corrected && std::memcmp(st.best_candidate, st.hard_candidate, 10) != 0) {
            if (!p25_soft_hard_override_enabled()
                || st.best_penalty + P25P1_SOFT_HARD_OVERRIDE_MARGIN >= st.hard_penalty) {
                DSD_MEMCPY(out_bits, st.hard_candidate, 10);
                return 1;
            }
        }
        DSD_MEMCPY(out_bits, st.best_candidate, 10);
        return count_differences(orig, st.best_candidate, 10) == 0 ? 0 : 1;
    }

    DSD_MEMCPY(out_bits, bits, 10);
    return 2;
}

extern "C" int
check_and_fix_golay_24_6_soft(char* data, const char* parity, const int* reliab, int* fixed) {
    if (fixed == nullptr) {
        return 1;
    }
    *fixed = 0;
    if (data == nullptr || parity == nullptr || reliab == nullptr) {
        return 1;
    }

    char orig[18];
    DSD_MEMCPY(orig, data, 6);
    DSD_MEMCPY(orig + 6, parity, 12);

    char hex_copy[6], parity_copy[12];
    DSD_MEMCPY(hex_copy, data, 6);
    DSD_MEMCPY(parity_copy, parity, 12);

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
        p25p1_golay_seed_result seed = {&best_penalty, &best_fixed, best_data,       &found_valid,
                                        hard_data,     &hard_valid, &hard_corrected, &hard_penalty};
        p25p1_golay6_seed_hard(orig, reliab, &golay, hex_copy, hard_fixed, &seed);
    }

    int least_rel[8];
    find_k_least_reliable(reliab, 18, 8, least_rel);

    p25p1_golay6_search(orig, reliab, least_rel, &golay, &best_penalty, &best_fixed, best_data, &found_valid);

    if (found_valid) {
        if (hard_valid && hard_corrected && std::memcmp(best_data, hard_data, 6) != 0) {
            if (!p25_soft_hard_override_enabled() || best_penalty + P25P1_SOFT_HARD_OVERRIDE_MARGIN >= hard_penalty) {
                DSD_MEMCPY(data, hard_data, 6);
                *fixed = hard_fixed;
                return 0;
            }
        }
        DSD_MEMCPY(data, best_data, 6);
        *fixed = best_fixed;
        return 0;
    }

    /* No valid candidate found */
    return 1;
}

extern "C" int
check_and_fix_golay_24_12_soft(char* data, const char* parity, const int* reliab, int* fixed) {
    if (fixed == nullptr) {
        return 1;
    }
    *fixed = 0;
    if (data == nullptr || parity == nullptr || reliab == nullptr) {
        return 1;
    }

    char orig[24];
    DSD_MEMCPY(orig, data, 12);
    DSD_MEMCPY(orig + 12, parity, 12);

    char dodeca_copy[12], parity_copy[12];
    DSD_MEMCPY(dodeca_copy, data, 12);
    DSD_MEMCPY(parity_copy, parity, 12);

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
        p25p1_golay_seed_result seed = {&best_penalty, &best_fixed, best_data,       &found_valid,
                                        hard_data,     &hard_valid, &hard_corrected, &hard_penalty};
        p25p1_golay12_seed_hard(orig, reliab, &golay, dodeca_copy, hard_fixed, &seed);
    }

    int least_rel[8];
    find_k_least_reliable(reliab, 24, 8, least_rel);

    p25p1_golay12_search(orig, reliab, least_rel, &golay, &best_penalty, &best_fixed, best_data, &found_valid);

    if (found_valid) {
        if (hard_valid && hard_corrected && std::memcmp(best_data, hard_data, 12) != 0) {
            if (!p25_soft_hard_override_enabled() || best_penalty + P25P1_SOFT_HARD_OVERRIDE_MARGIN >= hard_penalty) {
                DSD_MEMCPY(data, hard_data, 12);
                *fixed = hard_fixed;
                return 0;
            }
        }
        DSD_MEMCPY(data, best_data, 12);
        *fixed = best_fixed;
        return 0;
    }

    /* No valid candidate found */
    return 1;
}
