// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * P25P2 soft-decision RS erasure helpers.
 */

#include <dsd-neo/protocol/p25/p25p2_soft.h>
#include <dsd-neo/runtime/config.h>
#include <stdint.h>
#include <stdio.h>

/* Import LLR buffers from p25p2_frame.c */
extern int16_t p2llr[1400];
extern int16_t p2xllr[1400];

#define P25P2_SOFT_ERASURE_THRESHOLD 64

int
p25p2_soft_erasure_threshold(void) {
    const dsdneoRuntimeConfig* cfg = dsd_neo_get_config();
    if (!cfg) {
        dsd_neo_config_init();
        cfg = dsd_neo_get_config();
    }
    if (cfg) {
        if (cfg->p25p2_soft_erasure_threshold_is_set) {
            return cfg->p25p2_soft_erasure_threshold;
        }
        if (cfg->p25_soft_erasure_threshold_is_set) {
            return cfg->p25_soft_erasure_threshold;
        }
    }
    return P25P2_SOFT_ERASURE_THRESHOLD;
}

static uint8_t
p25p2_abs_llr_reliability(int16_t llr) {
    int v = llr < 0 ? -(int)llr : (int)llr;
    if (v > 255) {
        v = 255;
    }
    return (uint8_t)v;
}

typedef struct {
    uint8_t reliability;
    int position;
} P25P2ErasureCandidate;

static int
erasure_list_contains(const int* erasures, int count, int position) {
    for (int i = 0; i < count; i++) {
        if (erasures[i] == position) {
            return 1;
        }
    }
    return 0;
}

static void
sort_candidates(P25P2ErasureCandidate* candidates, int count) {
    for (int i = 0; i < count; i++) {
        for (int j = i + 1; j < count; j++) {
            if (candidates[j].reliability < candidates[i].reliability
                || (candidates[j].reliability == candidates[i].reliability
                    && candidates[j].position < candidates[i].position)) {
                P25P2ErasureCandidate tmp = candidates[i];
                candidates[i] = candidates[j];
                candidates[j] = tmp;
            }
        }
    }
}

static int
append_ranked_candidates(P25P2ErasureCandidate* candidates, int candidate_count, int* erasures, int total, int max_add,
                         int min_add) {
    sort_candidates(candidates, candidate_count);
    int threshold = p25p2_soft_erasure_threshold();
    int add_count = 0;
    for (int i = 0; i < candidate_count; i++) {
        if ((int)candidates[i].reliability < threshold) {
            add_count++;
        }
    }
    if (add_count < min_add) {
        add_count = min_add;
    }
    if (add_count > max_add) {
        add_count = max_add;
    }
    if (add_count > candidate_count) {
        add_count = candidate_count;
    }

    int added = 0;
    for (int i = 0; i < candidate_count && added < add_count; i++) {
        if (!erasure_list_contains(erasures, total, candidates[i].position)) {
            erasures[total++] = candidates[i].position;
            added++;
        }
    }
    return total;
}

uint8_t
p25p2_hexbit_llr_reliability(const uint16_t bit_offsets[6], int ts_counter, const int16_t* bit_llr) {
    if (bit_llr == NULL) {
        return 0;
    }

    uint8_t min_r = 255;
    for (int i = 0; i < 6; i++) {
        int abs_bit = (int)bit_offsets[i] + (ts_counter * 360);
        if (abs_bit < 0 || abs_bit >= 1400) {
            return 0;
        }
        uint8_t r = p25p2_abs_llr_reliability(bit_llr[abs_bit]);
        if (r < min_r) {
            min_r = r;
        }
    }
    return min_r;
}

/*
 * FACCH bit offset tables - see "Bit Layout Analysis" section for derivation.
 */
static const uint16_t facch_payload_bit_offsets[26][6] = {
    /* 00 */ {2, 3, 4, 5, 6, 7},
    /* 01 */ {8, 9, 10, 11, 12, 13},
    /* 02 */ {14, 15, 16, 17, 18, 19},
    /* 03 */ {20, 21, 22, 23, 24, 25},
    /* 04 */ {26, 27, 28, 29, 30, 31},
    /* 05 */ {32, 33, 34, 35, 36, 37},
    /* 06 */ {38, 39, 40, 41, 42, 43},
    /* 07 */ {44, 45, 46, 47, 48, 49},
    /* 08 */ {50, 51, 52, 53, 54, 55},
    /* 09 */ {56, 57, 58, 59, 60, 61},
    /* 10 */ {62, 63, 64, 65, 66, 67},
    /* 11 */ {68, 69, 70, 71, 72, 73},
    /* 12 */ {76, 77, 78, 79, 80, 81},
    /* 13 */ {82, 83, 84, 85, 86, 87},
    /* 14 */ {88, 89, 90, 91, 92, 93},
    /* 15 */ {94, 95, 96, 97, 98, 99},
    /* 16 */ {100, 101, 102, 103, 104, 105},
    /* 17 */ {106, 107, 108, 109, 110, 111},
    /* 18 */ {112, 113, 114, 115, 116, 117},
    /* 19 */ {118, 119, 120, 121, 122, 123},
    /* 20 */ {124, 125, 126, 127, 128, 129},
    /* 21 */ {130, 131, 132, 133, 134, 135},
    /* 22 */ {136, 137, 180, 181, 182, 183}, /* cross-segment */
    /* 23 */ {184, 185, 186, 187, 188, 189},
    /* 24 */ {190, 191, 192, 193, 194, 195},
    /* 25 */ {196, 197, 198, 199, 200, 201},
};

static const uint16_t facch_parity_bit_offsets[19][6] = {
    /* 00 */ {202, 203, 204, 205, 206, 207},
    /* 01 */ {208, 209, 210, 211, 212, 213},
    /* 02 */ {214, 215, 216, 217, 218, 219},
    /* 03 */ {220, 221, 222, 223, 224, 225},
    /* 04 */ {226, 227, 228, 229, 230, 231},
    /* 05 */ {232, 233, 234, 235, 236, 237},
    /* 06 */ {238, 239, 240, 241, 242, 243},
    /* 07 */ {246, 247, 248, 249, 250, 251},
    /* 08 */ {252, 253, 254, 255, 256, 257},
    /* 09 */ {258, 259, 260, 261, 262, 263},
    /* 10 */ {264, 265, 266, 267, 268, 269},
    /* 11 */ {270, 271, 272, 273, 274, 275},
    /* 12 */ {276, 277, 278, 279, 280, 281},
    /* 13 */ {282, 283, 284, 285, 286, 287},
    /* 14 */ {288, 289, 290, 291, 292, 293},
    /* 15 */ {294, 295, 296, 297, 298, 299},
    /* 16 */ {300, 301, 302, 303, 304, 305},
    /* 17 */ {306, 307, 308, 309, 310, 311},
    /* 18 */ {312, 313, 314, 315, 316, 317},
};

/*
 * SACCH bit offset tables - see "Bit Layout Analysis" section for derivation.
 */
static const uint16_t sacch_payload_bit_offsets[30][6] = {
    /* 00 */ {2, 3, 4, 5, 6, 7},
    /* 01 */ {8, 9, 10, 11, 12, 13},
    /* 02 */ {14, 15, 16, 17, 18, 19},
    /* 03 */ {20, 21, 22, 23, 24, 25},
    /* 04 */ {26, 27, 28, 29, 30, 31},
    /* 05 */ {32, 33, 34, 35, 36, 37},
    /* 06 */ {38, 39, 40, 41, 42, 43},
    /* 07 */ {44, 45, 46, 47, 48, 49},
    /* 08 */ {50, 51, 52, 53, 54, 55},
    /* 09 */ {56, 57, 58, 59, 60, 61},
    /* 10 */ {62, 63, 64, 65, 66, 67},
    /* 11 */ {68, 69, 70, 71, 72, 73},
    /* 12 */ {76, 77, 78, 79, 80, 81},
    /* 13 */ {82, 83, 84, 85, 86, 87},
    /* 14 */ {88, 89, 90, 91, 92, 93},
    /* 15 */ {94, 95, 96, 97, 98, 99},
    /* 16 */ {100, 101, 102, 103, 104, 105},
    /* 17 */ {106, 107, 108, 109, 110, 111},
    /* 18 */ {112, 113, 114, 115, 116, 117},
    /* 19 */ {118, 119, 120, 121, 122, 123},
    /* 20 */ {124, 125, 126, 127, 128, 129},
    /* 21 */ {130, 131, 132, 133, 134, 135},
    /* 22 */ {136, 137, 138, 139, 140, 141},
    /* 23 */ {142, 143, 144, 145, 146, 147},
    /* 24 */ {148, 149, 150, 151, 152, 153},
    /* 25 */ {154, 155, 156, 157, 158, 159},
    /* 26 */ {160, 161, 162, 163, 164, 165},
    /* 27 */ {166, 167, 168, 169, 170, 171},
    /* 28 */ {172, 173, 174, 175, 176, 177},
    /* 29 */ {178, 179, 180, 181, 182, 183},
};

static const uint16_t sacch_parity_bit_offsets[22][6] = {
    /* 00 */ {184, 185, 186, 187, 188, 189},
    /* 01 */ {190, 191, 192, 193, 194, 195},
    /* 02 */ {196, 197, 198, 199, 200, 201},
    /* 03 */ {202, 203, 204, 205, 206, 207},
    /* 04 */ {208, 209, 210, 211, 212, 213},
    /* 05 */ {214, 215, 216, 217, 218, 219},
    /* 06 */ {220, 221, 222, 223, 224, 225},
    /* 07 */ {226, 227, 228, 229, 230, 231},
    /* 08 */ {232, 233, 234, 235, 236, 237},
    /* 09 */ {238, 239, 240, 241, 242, 243},
    /* 10 */ {246, 247, 248, 249, 250, 251},
    /* 11 */ {252, 253, 254, 255, 256, 257},
    /* 12 */ {258, 259, 260, 261, 262, 263},
    /* 13 */ {264, 265, 266, 267, 268, 269},
    /* 14 */ {270, 271, 272, 273, 274, 275},
    /* 15 */ {276, 277, 278, 279, 280, 281},
    /* 16 */ {282, 283, 284, 285, 286, 287},
    /* 17 */ {288, 289, 290, 291, 292, 293},
    /* 18 */ {294, 295, 296, 297, 298, 299},
    /* 19 */ {300, 301, 302, 303, 304, 305},
    /* 20 */ {306, 307, 308, 309, 310, 311},
    /* 21 */ {312, 313, 314, 315, 316, 317},
};

/**
 * Build dynamic erasure list for FACCH based on reliability.
 *
 * @param ts_counter     Current timeslot counter (0-3).
 * @param scrambled      1 if using descrambled LLRs, 0 for raw LLRs.
 * @param erasures       Output: array to append erasures (must have space for at least 28 entries).
 * @param n_fixed        Number of fixed erasures already in the array.
 * @param max_add        Maximum dynamic erasures to add (recommend <=10 for FACCH).
 * @return Total erasure count (fixed + dynamic).
 */
int
p25p2_facch_soft_erasures(int ts_counter, int scrambled, int* erasures, int n_fixed, int max_add) {
    const int16_t* bit_llr = scrambled ? p2xllr : p2llr;
    P25P2ErasureCandidate candidates[45];
    int candidate_count = 0;

    /* Check each of the 26 payload hexbits (RS positions 9-34) */
    for (int hb = 0; hb < 26 && candidate_count < 45; hb++) {
        const uint16_t* bits = facch_payload_bit_offsets[hb];
        uint8_t rel = p25p2_hexbit_llr_reliability(bits, ts_counter, bit_llr);
        int rs_pos = 9 + hb;
        if (!erasure_list_contains(erasures, n_fixed, rs_pos)) {
            candidates[candidate_count].reliability = rel;
            candidates[candidate_count].position = rs_pos;
            candidate_count++;
        }
    }

    /* Check parity hexbits (RS positions 35-53) */
    for (int hb = 0; hb < 19 && candidate_count < 45; hb++) {
        const uint16_t* bits = facch_parity_bit_offsets[hb];
        uint8_t rel = p25p2_hexbit_llr_reliability(bits, ts_counter, bit_llr);
        int rs_pos = 35 + hb;
        if (!erasure_list_contains(erasures, n_fixed, rs_pos)) {
            candidates[candidate_count].reliability = rel;
            candidates[candidate_count].position = rs_pos;
            candidate_count++;
        }
    }

    return append_ranked_candidates(candidates, candidate_count, erasures, n_fixed, max_add, 5);
}

/**
 * Build dynamic erasure list for SACCH based on reliability.
 *
 * @param ts_counter     Current timeslot counter (0-3).
 * @param scrambled      1 if using descrambled buffers, 0 otherwise.
 * @param erasures       Output: array to append erasures (must have space for at least 28 entries).
 * @param n_fixed        Number of fixed erasures already in the array.
 * @param max_add        Maximum dynamic erasures to add (recommend <=16 for SACCH).
 * @return Total erasure count (fixed + dynamic).
 */
int
p25p2_sacch_soft_erasures(int ts_counter, int scrambled, int* erasures, int n_fixed, int max_add) {
    const int16_t* bit_llr = scrambled ? p2xllr : p2llr;
    P25P2ErasureCandidate candidates[52];
    int candidate_count = 0;

    /* Check each of the 30 payload hexbits (RS positions 5-34) */
    for (int hb = 0; hb < 30 && candidate_count < 52; hb++) {
        const uint16_t* bits = sacch_payload_bit_offsets[hb];
        uint8_t rel = p25p2_hexbit_llr_reliability(bits, ts_counter, bit_llr);
        int rs_pos = 5 + hb;
        if (!erasure_list_contains(erasures, n_fixed, rs_pos)) {
            candidates[candidate_count].reliability = rel;
            candidates[candidate_count].position = rs_pos;
            candidate_count++;
        }
    }

    /* Check parity hexbits (RS positions 35-56) */
    for (int hb = 0; hb < 22 && candidate_count < 52; hb++) {
        const uint16_t* bits = sacch_parity_bit_offsets[hb];
        uint8_t rel = p25p2_hexbit_llr_reliability(bits, ts_counter, bit_llr);
        int rs_pos = 35 + hb;
        if (!erasure_list_contains(erasures, n_fixed, rs_pos)) {
            candidates[candidate_count].reliability = rel;
            candidates[candidate_count].position = rs_pos;
            candidate_count++;
        }
    }

    return append_ranked_candidates(candidates, candidate_count, erasures, n_fixed, max_add, 8);
}

static uint8_t
contiguous_hexbit_reliability(const int16_t* llr, int bit_offset) {
    uint8_t min_r = 255;
    for (int i = 0; i < 6; i++) {
        uint8_t r = p25p2_abs_llr_reliability(llr[bit_offset + i]);
        if (r < min_r) {
            min_r = r;
        }
    }
    return min_r;
}

int
p25p2_ess_soft_erasures_ranked(const int16_t payload_llr[96], const int16_t parity_llr[168], int* erasures,
                               int max_add) {
    if (!payload_llr || !parity_llr || !erasures || max_add <= 0) {
        return 0;
    }

    P25P2ErasureCandidate candidates[44];
    int candidate_count = 0;
    for (int hb = 0; hb < 16; hb++) {
        candidates[candidate_count].reliability = contiguous_hexbit_reliability(payload_llr, hb * 6);
        candidates[candidate_count].position = hb;
        candidate_count++;
    }
    for (int hb = 0; hb < 28; hb++) {
        candidates[candidate_count].reliability = contiguous_hexbit_reliability(parity_llr, hb * 6);
        candidates[candidate_count].position = 16 + hb;
        candidate_count++;
    }
    sort_candidates(candidates, candidate_count);

    int threshold = p25p2_soft_erasure_threshold();
    int out_count = 0;
    for (int i = 0; i < candidate_count; i++) {
        if ((int)candidates[i].reliability < threshold) {
            out_count++;
        }
    }
    if (out_count < 14) {
        out_count = 14;
    }
    if (out_count > max_add) {
        out_count = max_add;
    }
    if (out_count > candidate_count) {
        out_count = candidate_count;
    }
    for (int i = 0; i < out_count; i++) {
        erasures[i] = candidates[i].position;
    }
    return out_count;
}
