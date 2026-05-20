// SPDX-License-Identifier: ISC
/*-------------------------------------------------------------------------------
 * p25_12.c
 * P25p1 1/2 Rate Simple Trellis Decoder
 *
 * LWVMOBILE
 * 2023-10 DSD-FME Florida Man Edition
 *-----------------------------------------------------------------------------*/

#include <stdint.h>
#include <string.h>

#include <dsd-neo/protocol/p25/p25_12.h>

uint8_t p25_interleave[98] = {0,  1,  8,  9,  16, 17, 24, 25, 32, 33, 40, 41, 48, 49, 56, 57, 64, 65, 72, 73,
                              80, 81, 88, 89, 96, 97, 2,  3,  10, 11, 18, 19, 26, 27, 34, 35, 42, 43, 50, 51,
                              58, 59, 66, 67, 74, 75, 82, 83, 90, 91, 4,  5,  12, 13, 20, 21, 28, 29, 36, 37,
                              44, 45, 52, 53, 60, 61, 68, 69, 76, 77, 84, 85, 92, 93, 6,  7,  14, 15, 22, 23,
                              30, 31, 38, 39, 46, 47, 54, 55, 62, 63, 70, 71, 78, 79, 86, 87, 94, 95};

//this is a convertion table for converting the dibit pairs into constellation points
uint8_t p25_constellation_map[16] = {11, 12, 0, 7, 14, 9, 5, 2, 10, 13, 1, 6, 15, 8, 4, 3};

//digitized dibit to OTA symbol conversion for reference
//0 = +1; 1 = +3;
//2 = -1; 3 = -3;

//finite state machine values
uint8_t p25_fsm[16] = {0, 15, 12, 3, 4, 11, 8, 7, 13, 2, 1, 14, 9, 6, 5, 10};

//this is a dibit-pair to trellis dibit transition matrix (SDRTrunk and Ossmann)
//when evaluating hamming distance, we want to use this xor the dibit-pair nib,
//and not the fsm xor the constellation point
uint8_t p25_dtm[16] = {2, 12, 1, 15, 14, 0, 13, 3, 9, 7, 10, 4, 5, 11, 6, 8};

int
count_bits(uint8_t b, int slen) {
    int i = 0;
    int j = 0;
    for (j = 0; j < slen; j++) {
        if ((b & 1) == 1) {
            i++;
        }
        b = b >> 1;
    }
    return i;
}

uint8_t
find_min(uint8_t list[4], int len) {
    int min = list[0];
    uint8_t index = 0;
    int i;

    for (i = 1; i < len; i++) {
        if (list[i] < min) {
            min = list[i];
            index = (uint8_t)i;
        }

        //NOTE: Disqualifying result on uniqueness can impact decoding
        //let the CRC determine if the result is good or bad
        //its not uncommon for two values of the same min
        //distance to emerge, so its 50/50 each time
    }

    return index;
}

int
p25_12(uint8_t* input, uint8_t treturn[12]) {
    /*
     * Soft-decision (semi-soft) 4-state Viterbi over dibit-pair nibbles.
     * Branch metric = Hamming distance between observed nibble and expected
     * transition nibble from p25_dtm[(prev_state<<2)|next_state].
     * This replaces the greedy FSM walk to improve robustness on marginal SNR.
     */
    enum { N_SYMS = 49, N_ST = 4 };

    int i, j;

    /* Deinterleave input dibits (98) into symbol-ordered dibits */
    uint8_t deinterleaved_dibits[98];
    memset(deinterleaved_dibits, 0, sizeof(deinterleaved_dibits));
    for (i = 0; i < 98; i++) {
        deinterleaved_dibits[p25_interleave[i]] = input[i];
    }

    /* Pack dibit pairs into 4-bit nibbles (observations per trellis step) */
    uint8_t nibs[N_SYMS];
    memset(nibs, 0, sizeof(nibs));
    for (i = 0; i < N_SYMS; i++) {
        nibs[i] = (uint8_t)((deinterleaved_dibits[(i * 2) + 0] << 2) | (deinterleaved_dibits[(i * 2) + 1]));
    }

    /* Viterbi: path metrics and backpointers */
    /* Use uint16_t; worst-case metric <= 49*4 = 196 */
    uint16_t prev_metric[N_ST];
    uint16_t curr_metric[N_ST];
    uint8_t backptr[N_SYMS][N_ST];

    /* Initialize metrics: bias start at state 0 slightly */
    for (j = 0; j < N_ST; j++) {
        prev_metric[j] = (uint16_t)((j == 0) ? 0 : 1);
    }

    for (i = 0; i < N_SYMS; i++) {
        /* For each candidate next state, pick best predecessor */
        for (int st_next = 0; st_next < N_ST; st_next++) {
            uint16_t best = (uint16_t)0xFFFF;
            uint8_t best_prev = 0;
            for (int st_prev = 0; st_prev < N_ST; st_prev++) {
                /* Expected transition nibble from (st_prev -> st_next) */
                uint8_t expect = p25_dtm[(st_prev << 2) | st_next] & 0xF;
                /* Hamming distance on 4-bit nibble */
                uint16_t bm = (uint16_t)count_bits((uint8_t)((nibs[i] ^ expect) & 0xF), 4);
                uint16_t cost = (uint16_t)(prev_metric[st_prev] + bm);
                if (cost < best) {
                    best = cost;
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
    uint16_t best_final = curr_metric[0];
    int st = 0;
    for (j = 1; j < N_ST; j++) {
        if (curr_metric[j] < best_final) {
            best_final = curr_metric[j];
            st = j;
        }
    }

    /* Traceback to recover tdibits (next states) */
    uint8_t tdibits[N_SYMS];
    for (i = N_SYMS - 1; i >= 0; i--) {
        tdibits[i] = (uint8_t)st;
        st = backptr[i][st];
        if (i == 0) {
            break; /* avoid i underflow for unsigned loop */
        }
    }

    /* Pack first 48 tdibits into 12 bytes (MSB-first as before) */
    for (i = 0; i < 12; i++) {
        treturn[i] = (uint8_t)((tdibits[(i * 4) + 0] << 6) | (tdibits[(i * 4) + 1] << 4) | (tdibits[(i * 4) + 2] << 2)
                               | (tdibits[(i * 4) + 3]));
    }

    /* Return aggregate metric as a rough error indicator (compat: previous returned count) */
    return (int)best_final;
}

static uint32_t
p25_llr_bit_cost(int16_t llr, int expected_bit) {
    if (expected_bit) {
        return (llr < 0) ? (uint32_t)(-llr) : 0U;
    }
    return (llr > 0) ? (uint32_t)llr : 0U;
}

typedef struct {
    uint8_t valid;
    uint32_t metric;
    uint8_t states[49];
} p25_12_path_t;

static void
p25_12_insert_path(p25_12_path_t paths[P25_12_MAX_CANDIDATES], const p25_12_path_t* candidate) {
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

static void
p25_12_pack_path_bytes(const uint8_t states[49], uint8_t out[12]) {
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
    memcpy(candidates[insert_at].bytes, bytes, 12);
    candidates[insert_at].metric = metric;
}

int
p25_12_soft_llr_list(const uint8_t* input, const int16_t* bit_llr196, p25_12_candidate_t* candidates,
                     int max_candidates) {
    enum { N_SYMS = 49, N_ST = 4 };

    (void)input;
    if (bit_llr196 == NULL || candidates == NULL || max_candidates <= 0) {
        return 0;
    }
    if (max_candidates > P25_12_MAX_CANDIDATES) {
        max_candidates = P25_12_MAX_CANDIDATES;
    }

    int16_t llr_dei[196];
    memset(llr_dei, 0, sizeof(llr_dei));
    for (int i = 0; i < 98; i++) {
        int p = p25_interleave[i];
        llr_dei[(p * 2) + 0] = bit_llr196[(i * 2) + 0];
        llr_dei[(p * 2) + 1] = bit_llr196[(i * 2) + 1];
    }

    p25_12_path_t prev[N_ST][P25_12_MAX_CANDIDATES];
    p25_12_path_t curr[N_ST][P25_12_MAX_CANDIDATES];
    memset(prev, 0, sizeof(prev));
    for (int st = 0; st < N_ST; st++) {
        prev[st][0].valid = 1;
        prev[st][0].metric = (st == 0) ? 0U : 256U;
    }

    for (int i = 0; i < N_SYMS; i++) {
        memset(curr, 0, sizeof(curr));
        int base = i * 4;
        for (int st_prev = 0; st_prev < N_ST; st_prev++) {
            for (int rank = 0; rank < P25_12_MAX_CANDIDATES; rank++) {
                if (!prev[st_prev][rank].valid) {
                    continue;
                }
                for (int st_next = 0; st_next < N_ST; st_next++) {
                    uint8_t expect = p25_dtm[(st_prev << 2) | st_next] & 0xF;
                    uint32_t cost = p25_llr_bit_cost(llr_dei[base + 0], (expect >> 3) & 1)
                                    + p25_llr_bit_cost(llr_dei[base + 1], (expect >> 2) & 1)
                                    + p25_llr_bit_cost(llr_dei[base + 2], (expect >> 1) & 1)
                                    + p25_llr_bit_cost(llr_dei[base + 3], expect & 1);
                    p25_12_path_t candidate = prev[st_prev][rank];
                    candidate.metric += cost;
                    candidate.states[i] = (uint8_t)st_next;
                    p25_12_insert_path(curr[st_next], &candidate);
                }
            }
        }
        memcpy(prev, curr, sizeof(prev));
    }

    int out_count = 0;
    for (int st = 0; st < N_ST; st++) {
        for (int rank = 0; rank < P25_12_MAX_CANDIDATES; rank++) {
            if (!prev[st][rank].valid) {
                continue;
            }
            uint8_t bytes[12];
            p25_12_pack_path_bytes(prev[st][rank].states, bytes);
            p25_12_insert_candidate(candidates, &out_count, max_candidates, bytes, prev[st][rank].metric);
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
    memset(llr_dei, 0, sizeof(llr_dei));
    for (i = 0; i < 98; i++) {
        int p = p25_interleave[i];
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
    for (i = N_SYMS - 1; i >= 0; i--) {
        tdibits[i] = (uint8_t)st;
        st = backptr[i][st];
        if (i == 0) {
            break;
        }
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
int
p25_12_soft(uint8_t* input, const uint8_t* reliab98, uint8_t treturn[12]) {
    int16_t llr[196];
    for (int i = 0; i < 98; i++) {
        int r = reliab98 ? reliab98[i] : 255;
        llr[(i * 2) + 0] = (int16_t)(((input[i] >> 1) & 1) ? r : -r);
        llr[(i * 2) + 1] = (int16_t)((input[i] & 1) ? r : -r);
    }
    return p25_12_soft_llr(input, llr, treturn);
}
