// SPDX-License-Identifier: GPL-3.0-or-later
#include <dsd-neo/protocol/tetra/tetra_fec.h>
#include <string.h>
#include <stdio.h>
#include <dsd-neo/fec/viterbi.h>
#include <stdlib.h>

/* Minimal port of osmo-tetra puncturer tables and mapping functions
 * to implement exact RCPC depuncture ordering.
 * The arrays below are derived from osmo-tetra's `tetra_conv_enc.c`.
 */

/* Puncturer enums are declared in the header as macros to allow callers to
 * select puncturer ids without duplicating symbols. The detailed puncturer
 * table below uses the same ordering as those macros.
 */

/* Minimal no-puncture fallback pattern used by generic depuncturer when none supplied */
static const uint8_t punct_1_2_arr[] = { 1 };

struct puncturer {
    int type;
    const uint8_t *P;
    uint8_t t;
    uint8_t period;
    uint32_t (*i_func)(uint32_t j);
};

static uint32_t i_func_equals(uint32_t j) { return j; }
static uint32_t i_func_292(uint32_t j) { return (j + ((j-1)/65)); }
static uint32_t i_func_148(uint32_t j) { return (j + ((j-1)/35)); }

/* Puncturer tables from osmo-tetra */
static const uint8_t P_rate2_3[] = { 0, 1, 2, 5 };
static const uint8_t P_rate1_3[] = { 0, 1, 2, 3, 5, 6, 7 };
static const uint8_t P_rate8_12[] = { 0, 1, 2, 4 };
static const uint8_t P_rate8_18[] = { 0, 1, 2, 3, 4, 5, 7, 8, 10, 11 };
static const uint8_t P_rate8_17[] = { 0, 1, 2, 3, 4, 5, 7, 8, 10, 11, 13, 14, 16, 17, 19, 20, 22, 23 };

static const struct puncturer punct_2_3 = { .type = TETRA_RCPC_PUNCT_2_3, .P = P_rate2_3, .t = 3, .period = 8, .i_func = &i_func_equals };
static const struct puncturer punct_1_3 = { .type = TETRA_RCPC_PUNCT_1_3, .P = P_rate1_3, .t = 6, .period = 8, .i_func = &i_func_equals };
static const struct puncturer punct_292_432 = { .type = TETRA_RCPC_PUNCT_292_432, .P = P_rate2_3, .t = 3, .period = 8, .i_func = &i_func_292 };
static const struct puncturer punct_148_432 = { .type = TETRA_RCPC_PUNCT_148_432, .P = P_rate1_3, .t = 6, .period = 8, .i_func = &i_func_148 };
static const struct puncturer punct_112_168 = { .type = TETRA_RCPC_PUNCT_112_168, .P = P_rate8_12, .t = 3, .period = 6, .i_func = &i_func_equals };
static const struct puncturer punct_72_162 = { .type = TETRA_RCPC_PUNCT_72_162, .P = P_rate8_18, .t = 9, .period = 12, .i_func = &i_func_equals };
static const struct puncturer punct_38_80 = { .type = TETRA_RCPC_PUNCT_38_80, .P = P_rate8_17, .t = 17, .period = 24, .i_func = &i_func_equals };

static const struct puncturer *tetra_puncts[] = {
    &punct_2_3,
    &punct_1_3,
    &punct_292_432,
    &punct_148_432,
    &punct_112_168,
    &punct_72_162,
    &punct_38_80
};

/* Depuncture by puncturer id using soft costs. Mirrors tetra_rcpc_depunct logic. */
int tetra_rcpc_depuncture_by_id(int punct_id, const uint16_t* in_costs, int in_len, uint16_t* out_costs, int out_len) {
    if (punct_id < 0 || punct_id >= (int)(sizeof(tetra_puncts)/sizeof(tetra_puncts[0]))) return -1;
    if (!in_costs || !out_costs) return -1;
    const struct puncturer *punct = tetra_puncts[punct_id];
    const uint8_t *P = punct->P;
    uint8_t t = punct->t;
    uint8_t period = punct->period;
    uint32_t (*i_func)(uint32_t) = punct->i_func;

    /* initialize out with neutral costs */
    for (int i = 0; i < out_len; i++) out_costs[i] = 0x7FFF;

    /* map input coded symbols (in_costs) into mother-code positions per puncturer */
    for (uint32_t j = 1; j <= (uint32_t)in_len; j++) {
        uint32_t i = i_func(j);
        if (i == 0) continue;
        uint32_t block = (i - 1) / (uint32_t)t;
        uint32_t pos_in_block = (i - 1) % (uint32_t)t; /* 0-based */
        uint32_t p_index = pos_in_block + 1; /* preserve 1-based P layout used historically */
        uint32_t pval = (uint32_t)P[p_index];
        uint32_t k = (uint32_t)period * block + pval;
        if (k >= 1 && (int)(k-1) < out_len) {
            out_costs[k-1] = in_costs[j-1];
        }
    }

    return out_len;
}

/* Return the mother-code position k for given puncturer id and j index (1-based),
 * or -1 on error. This mirrors the mapping used in puncture/depuncture.
 */
int tetra_rcpc_map_j_to_k(int punct_id, uint32_t j) {
    if (punct_id < 0 || punct_id >= (int)(sizeof(tetra_puncts)/sizeof(tetra_puncts[0]))) return -1;
    const struct puncturer *punct = tetra_puncts[punct_id];
    const uint8_t *P = punct->P;
    uint8_t t = punct->t;
    uint8_t period = punct->period;
    uint32_t (*i_func)(uint32_t) = punct->i_func;

    /* Use a period-based lookup to compute k.
     * The original formula uses 1-based indexing into P; preserve that
     * semantics here but express it as a clear periodic lookup:
     *  - i = i_func(j)
     *  - block = (i-1) / t
     *  - pos_in_block = (i-1) % t  (0-based)
     *  - P is addressed as P[pos_in_block + 1] to match existing layout
     *  - k = period * block + P[pos_in_block + 1]
     */
    uint32_t i = i_func(j);
    if (i == 0) return -1;
    uint32_t block = (i - 1) / (uint32_t)t;
    uint32_t pos_in_block = (i - 1) % (uint32_t)t; /* 0-based */
    uint32_t p_index = pos_in_block + 1; /* preserve existing 1-based P layout */
    uint32_t pval = (uint32_t)P[p_index];
    uint32_t k = (uint32_t)period * block + pval;
    return (int)k;
}

/* Puncture mother-code by puncturer id (forward mapping). Writes up to out_len
 * type-3 symbols into out. Returns number written or -1 on error.
 */
int tetra_rcpc_puncture_by_id(int punct_id, const uint8_t* mother, int mother_len, uint8_t* out, int out_len) {
    if (punct_id < 0 || punct_id >= (int)(sizeof(tetra_puncts)/sizeof(tetra_puncts[0]))) return -1;
    if (!mother || !out) return -1;
    const struct puncturer *punct = tetra_puncts[punct_id];
    const uint8_t *P = punct->P;
    uint8_t t = punct->t;
    uint8_t period = punct->period;
    uint32_t (*i_func)(uint32_t) = punct->i_func;

    int written = 0;
    for (uint32_t j = 1; written < out_len; j++) {
        uint32_t i = i_func(j);
        if (i == 0) {
            out[written++] = 0xff;
            continue;
        }
        uint32_t block = (i - 1) / (uint32_t)t;
        uint32_t pos_in_block = (i - 1) % (uint32_t)t;
        uint32_t p_index = pos_in_block + 1;
        uint32_t pval = (uint32_t)P[p_index];
        uint32_t k = (uint32_t)period * block + pval;
        if (k >= 1 && (int)(k-1) < mother_len) {
            out[written++] = mother[k-1];
        } else {
            out[written++] = 0xff;
        }
    }

    return written;
}

/* Debug: print puncturer parameters and P array */
void tetra_rcpc_print_puncturer(int punct_id) {
    if (punct_id < 0 || punct_id >= (int)(sizeof(tetra_puncts)/sizeof(tetra_puncts[0]))) {
        fprintf(stderr, "[TETRA] puncturer id %d out of range\n", punct_id);
        return;
    }
    const struct puncturer *p = tetra_puncts[punct_id];
    fprintf(stderr, "[TETRA] puncturer id=%d t=%u period=%u P=[", punct_id, p->t, p->period);
    /* print first few entries of P for brevity */
    for (int i = 0; i < 16 && p->P[i] != 0 && i < 64; i++) {
        if (i) fprintf(stderr, ",");
        fprintf(stderr, "%u", p->P[i]);
    }
    fprintf(stderr, "]\n");
}

int tetra_rcpc_get_puncturer_params(int punct_id, const uint8_t **outP, int *out_t, int *out_period) {
    if (punct_id < 0 || punct_id >= (int)(sizeof(tetra_puncts)/sizeof(tetra_puncts[0]))) return -1;
    const struct puncturer *p = tetra_puncts[punct_id];
    if (outP) *outP = p->P;
    if (out_t) *out_t = (int)p->t;
    if (out_period) *out_period = (int)p->period;
    return 0;
}

void tetra_block_deinterleave(uint8_t* in, uint8_t* out, int len, int a) {
    if (!in || !out || len <= 0 || a <= 0) return;
    /* ETSI EN 300 392-2 §8.2.4.1 block deinterleave:
     * π(i) = 1 + (a*i % K) ;  out[i-1] = in[π(i)-1]  for i = 1..K
     * gcd(a, K) must equal 1 for a valid permutation.
     */
    const uint32_t K = (uint32_t)len;
    const uint32_t A = (uint32_t)a;
    for (uint32_t i = 1; i <= K; i++) {
        uint32_t k = 1u + (A * i % K);
        out[i - 1] = in[k - 1];
    }
}

/* TETRA scrambling PN generator — 32-bit right-shifting Fibonacci LFSR.
 * Polynomial: x^32+x^26+x^23+x^22+x^16+x^12+x^11+x^10+x^8+x^7+x^5+x^4+x^2+x+1
 * (ETSI EN 300 392-2 §8.2.5 / osmo-tetra tetra_scramb.c)
 *
 * Tap macro: ST(x,y) = (x) >> (32-y), so ST(x,32) = x (bit 0 from LSB).
 * The feedback bit (= output PN bit) is XOR of 14 tap positions, then
 * injected at the MSB after a right-shift of the register.
 *
 * Seed formula (§8.2.5.2):
 *   seed = ((colour & 0x3f) | ((mnc & 0x3fff) << 6) | ((mcc & 0x3ff) << 20)) << 2) | 3
 * When MCC=MNC=colour=0: seed = 3.  This is the BSCH scrambling seed.
 */
static uint8_t tetra_lfsr32_next(uint32_t *lfsr)
{
    uint32_t s = *lfsr;
    /* taps at bit positions 0,6,9,10,16,20,21,22,24,25,27,28,30,31 (0=LSB) */
    uint32_t bit = (
        (s >>  0) ^ (s >>  6) ^ (s >>  9) ^ (s >> 10) ^
        (s >> 16) ^ (s >> 20) ^ (s >> 21) ^ (s >> 22) ^
        (s >> 24) ^ (s >> 25) ^ (s >> 27) ^ (s >> 28) ^
        (s >> 30) ^ (s >> 31)
    ) & 1u;
    *lfsr = (s >> 1) | (bit << 31);
    return (uint8_t)bit;
}

void tetra_descramble(uint8_t* in, int len, uint32_t lfsr_init) {
    if (!in || len <= 0) return;
    uint32_t state = lfsr_init ? lfsr_init : 3u; /* seed=0 is illegal; default to BSCH seed */
    for (int i = 0; i < len; i++)
        in[i] ^= tetra_lfsr32_next(&state);
}

/* Descramble soft-costs: invert polarity where PN bit == 1.
 * Neutral cost 0x7FFF stays neutral (completely uncertain ↔ no information).
 * All other values are bitwise-complemented: v → (0xFFFF ^ v).
 */
void tetra_descramble_soft(uint16_t* costs, int len, uint32_t lfsr_init) {
    if (!costs || len <= 0) return;
    uint32_t state = lfsr_init ? lfsr_init : 3u;
    for (int i = 0; i < len; i++) {
        uint8_t pn = tetra_lfsr32_next(&state);
        if (pn && costs[i] != 0x7FFFu)
            costs[i] = (uint16_t)(0xFFFFu ^ (uint32_t)costs[i]);
    }
}

void tetra_hard_bits_to_soft(const uint8_t* bits, uint16_t* costs, int len) {
    if (!bits || !costs || len <= 0) return;
    for (int i = 0; i < len; i++) costs[i] = (bits[i] & 1u) ? 0xFFFFu : 0x0000u;
}

uint16_t tetra_crc16_ccitt_bits(const uint8_t* bits, int len) {
    if (!bits || len <= 0) return 0xFFFFu;
    uint16_t crc = 0xFFFFu;
    for (int i = 0; i < len; i++) {
        crc ^= (uint16_t)((bits[i] & 1u) << 15);
        crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u) : (uint16_t)(crc << 1);
    }
    return crc;
}

/* Compute the TETRA scrambling seed from network identity parameters.
 * Formula from ETSI EN 300 392-2 §8.2.5.2 and osmo-tetra tetra_scramb.c:
 *   seed = ((colour & 0x3f) | ((mnc & 0x3fff) << 6) | ((mcc & 0x3ff) << 20)) << 2) | 3
 * The constant 3 (SCRAMB_INIT) represents the pre-fill value p(-31)=1, p(-30)=1.
 *
 * Special case: mcc=0, mnc=0, colour=0 → seed = 3 (BSCH scrambling seed).
 */
uint32_t tetra_compute_scramb_seed(uint16_t mcc, uint16_t mnc, uint8_t colour)
{
    uint32_t s = (uint32_t)(colour  & 0x3fu)
               | ((uint32_t)(mnc    & 0x3fffu) <<  6)
               | ((uint32_t)(mcc    & 0x3ffu)  << 20);
    return (s << 2) | 3u;   /* 3 = SCRAMB_INIT per §8.2.5.2 */
}

void tetra_viterbi_decode(uint8_t* in, uint8_t* out, int len) {
    // Placeholder: just copy input to output
    memcpy(out, in, len);
    fprintf(stderr, "[TETRA] viterbi_decode called (len=%d)\n", len);
}

int tetra_viterbi_decode_soft(const uint16_t* depunc, int depunc_len, uint8_t* out_bits, int out_bits_len) {
    if (!depunc || !out_bits) return -1;
    if (depunc_len <= 0 || (depunc_len & 3) != 0) return -1;
    const int symbols = depunc_len / 4;
    if (symbols > 512 || out_bits_len < symbols) return -1;

    /* 16-state, constraint-length-5 TETRA rate-1/4 mother code. State bits
     * [0..3] are D,D2,D3,D4, matching tetra_conv_enc.c. */
    uint64_t prev_metric[16], next_metric[16];
    uint8_t predecessor[512][16];
    uint8_t decision[512][16];
    const uint64_t inf = UINT64_MAX / 4u;
    for (int state = 0; state < 16; state++) prev_metric[state] = state == 0 ? 0 : inf;

    for (int pos = 0; pos < symbols; pos++) {
        for (int state = 0; state < 16; state++) next_metric[state] = inf;
        for (int state = 0; state < 16; state++) {
            if (prev_metric[state] == inf) continue;
            const int d1 = (state >> 0) & 1;
            const int d2 = (state >> 1) & 1;
            const int d3 = (state >> 2) & 1;
            const int d4 = (state >> 3) & 1;
            for (int bit = 0; bit <= 1; bit++) {
                const int expected[4] = {bit ^ d1 ^ d4, bit ^ d2 ^ d3 ^ d4,
                                         bit ^ d1 ^ d2 ^ d4, bit ^ d1 ^ d3 ^ d4};
                uint64_t branch = 0;
                for (int lane = 0; lane < 4; lane++) {
                    const uint16_t target = expected[lane] ? 0xFFFFu : 0x0000u;
                    const uint16_t got = depunc[pos * 4 + lane];
                    branch += target > got ? target - got : got - target;
                }
                const int next = ((state << 1) & 0xEu) | bit;
                const uint64_t metric = prev_metric[state] + branch;
                if (metric < next_metric[next]) {
                    next_metric[next] = metric;
                    predecessor[pos][next] = (uint8_t)state;
                    decision[pos][next] = (uint8_t)bit;
                }
            }
        }
        memcpy(prev_metric, next_metric, sizeof(prev_metric));
    }

    /* Signalling encoders append four zero tail bits, so state zero is the
     * normative endpoint. Falling back to the best state keeps the helper
     * useful for legacy unterminated diagnostic vectors. */
    int state = 0;
    if (prev_metric[state] == inf) {
        for (int candidate = 1; candidate < 16; candidate++)
            if (prev_metric[candidate] < prev_metric[state]) state = candidate;
    }
    for (int pos = symbols - 1; pos >= 0; pos--) {
        out_bits[pos] = decision[pos][state];
        state = predecessor[pos][state];
    }
    return symbols;
}

int tetra_rcpc_depuncture_soft(const uint16_t* in_costs, int info_bits_len, const uint8_t* punct, int p_len,
                               uint16_t* out_costs, int out_len) {
    if (!in_costs || !out_costs) return -1;
    if (!punct || p_len <= 0) {
        /* fallback to no-puncture */
        punct = punct_1_2_arr;
        p_len = 1;
    }

    int in_pos = 0;
    int out_pos = 0;

    /* Walk the puncture pattern repeating until we consume the requested output length.
     * If punct[i % p_len] == 1 then a coded symbol is present for this information bit;
     * otherwise it's punctured and we insert a neutral cost (0x7FFF).
     * This generic depuncturer maps each info bit to punctured coded symbols in sequence.
     */
    while (out_pos < out_len && in_pos < info_bits_len) {
        for (int pi = 0; pi < p_len && out_pos < out_len; pi++) {
            if (punct[pi]) {
                /* use the next info soft cost */
                out_costs[out_pos++] = in_costs[in_pos++];
            } else {
                /* punctured: insert neutral / uncertain cost */
                out_costs[out_pos++] = 0x7FFF;
            }
        }
    }

    /* If we exhausted info bits but still need to fill output, pad with neutral costs */
    while (out_pos < out_len) {
        out_costs[out_pos++] = 0x7FFF;
    }

    return out_pos;
}

void tetra_block_interleave(const uint8_t* in_bits, uint8_t* out_bits, int len, int rows, int cols) {
    if (!in_bits || !out_bits || rows <= 0 || len <= 0) return;
    /* ETSI EN 300 392-2 §8.2.4.1 block interleave:
     * π(i) = 1 + (a*i % K) ;  out[π(i)-1] = in[i-1]  for i = 1..K
     * `rows` is the interleaver parameter a; `cols` is unused (kept for ABI compat).
     */
    const uint32_t K = (uint32_t)len;
    const uint32_t A = (uint32_t)rows;
    (void)cols;
    for (uint32_t i = 1; i <= K; i++) {
        uint32_t k = 1u + (A * i % K);
        out_bits[k - 1] = in_bits[i - 1];
    }
}

void tetra_block_deinterleave_bits(const uint8_t* in_bits, uint8_t* out_bits, int len, int rows, int cols) {
    if (!in_bits || !out_bits || rows <= 0 || len <= 0) return;
    /* ETSI deinterleave: out[i-1] = in[π(i)-1] where π(i) = 1 + (a*i % K).
     * `rows` is the interleaver parameter a; `cols` unused. */
    const uint32_t K = (uint32_t)len;
    const uint32_t A = (uint32_t)rows;
    (void)cols;
    for (uint32_t i = 1; i <= K; i++) {
        uint32_t k = 1u + (A * i % K);
        out_bits[i - 1] = in_bits[k - 1];
    }
}

void tetra_block_deinterleave_soft(const uint16_t* in_costs, uint16_t* out_costs, int len, int rows, int cols) {
    if (!in_costs || !out_costs || rows <= 0 || len <= 0) return;
    /* ETSI deinterleave for soft costs: out[i-1] = in[π(i)-1] where π(i) = 1 + (a*i % K).
     * `rows` is the interleaver parameter a; `cols` unused. */
    const uint32_t K = (uint32_t)len;
    const uint32_t A = (uint32_t)rows;
    (void)cols;
    for (uint32_t i = 1; i <= K; i++) {
        uint32_t k = 1u + (A * i % K);
        out_costs[i - 1] = in_costs[k - 1];
    }
}

void tetra_speech_deinterleave_soft(const uint16_t* in_costs, uint16_t* out_costs) {
    if (!in_costs || !out_costs) return;
    for (int column = 0; column < 18; column++)
        for (int line = 0; line < 24; line++)
            out_costs[line * 18 + column] = in_costs[column * 24 + line];
}

static uint8_t parity5(uint8_t value) {
    value ^= (uint8_t)(value >> 4);
    value ^= (uint8_t)(value >> 2);
    value ^= (uint8_t)(value >> 1);
    return value & 1u;
}

int tetra_speech_channel_decode(const uint16_t* deinterleaved, uint8_t* out_bits, int out_len) {
    if (!deinterleaved || !out_bits || out_len < 274) return -1;
    for (int i = 0; i < 102; i++)
        out_bits[i] = deinterleaved[i] > 0x7fffu;

    static const uint8_t a1[3][8] = {
        {1,1,1,1,1,1,1,1}, {1,0,1,0,1,0,1,0}, {0,0,0,0,0,0,0,0}
    };
    static const uint8_t a2[3][8] = {
        {1,1,1,1,1,1,1,1}, {1,1,1,1,1,1,1,1}, {1,0,0,0,1,0,0,0}
    };
    uint16_t symbols[184][3];
    int input = 102;
    for (int pos = 0; pos < 184; pos++) {
        const uint8_t (*pattern)[8] = pos < 112 ? a1 : a2;
        const int phase = pos < 112 ? pos : pos - 112;
        for (int lane = 0; lane < 3; lane++)
            symbols[pos][lane] = pattern[lane][phase & 7]
                                     ? deinterleaved[input++] : 0x7fffu;
    }
    if (input != 432) return -1;

    uint64_t prev[16], next[16];
    uint8_t predecessor[184][16], decision[184][16];
    const uint64_t inf = UINT64_MAX / 4u;
    for (int state = 0; state < 16; state++) prev[state] = state == 0 ? 0 : inf;
    for (int pos = 0; pos < 184; pos++) {
        for (int state = 0; state < 16; state++) next[state] = inf;
        for (int state = 0; state < 16; state++) {
            if (prev[state] == inf) continue;
            for (int bit = 0; bit <= 1; bit++) {
                const uint8_t involved = (uint8_t)((bit << 4) | state);
                const uint8_t expected[3] = {
                    parity5(involved & 0x1fu),
                    parity5(involved & 0x1bu),
                    parity5(involved & 0x15u)
                };
                uint64_t branch = 0;
                for (int lane = 0; lane < 3; lane++) {
                    const uint16_t target = expected[lane] ? 0xffffu : 0u;
                    const uint16_t got = symbols[pos][lane];
                    branch += target > got ? target - got : got - target;
                }
                const int next_state = (bit << 3) | (state >> 1);
                const uint64_t metric = prev[state] + branch;
                if (metric < next[next_state]) {
                    next[next_state] = metric;
                    predecessor[pos][next_state] = (uint8_t)state;
                    decision[pos][next_state] = (uint8_t)bit;
                }
            }
        }
        memcpy(prev, next, sizeof(prev));
    }

    uint8_t decoded[184];
    int state = 0;
    for (int pos = 183; pos >= 0; pos--) {
        decoded[pos] = decision[pos][state];
        state = predecessor[pos][state];
    }
    memcpy(out_bits + 102, decoded, 172);
    return 274;
}

int tetra_get_interleaver_dims(int punct_id, int bits_len, int *out_rows, int *out_cols) {
    if (!out_rows || !out_cols) return -1;
    /* Static interleaver dimensions table (authoritative values).
     * Only known canonical block lengths are accepted — do not fall back
     * to a heuristic. Unknown lengths return -1 so callers can decide.
     * Source: osmo-tetra `tetra_blk_param` mapping (ported entries).
     */
    struct inter_tab { int bits; int rows; } table[] = {
        {30,  1},   /* BBK (no interleave) - represent as rows=1 (identity) */
        {80, 11},   /* BSCH-ish small block (conservative) */
        {120, 11},  /* SB1 */
        {162, 11},  /* speech class-ish (conservative) */
        {168, 13},  /* SCH/HU */
        {216, 101}, /* SB2 / NDB */
        {432, 103}, /* SCH/F and some TCH modes */
    };
    const int n = sizeof(table) / sizeof(table[0]);
    int found = 0;
    for (int i = 0; i < n; i++) {
        if (table[i].bits == bits_len) {
            *out_rows = table[i].rows;
            found = 1;
            break;
        }
    }
    if (!found) return -1;
    if (*out_rows <= 0) return -1;
    *out_cols = (bits_len + *out_rows - 1) / *out_rows;
    return 0;
}

#ifdef TETRA_FEC_TEST_MAIN
#include <assert.h>
#include <stdint.h>

/* Simple test harness: build with -DTETRA_FEC_TEST_MAIN to run.
 * Example (MSVC): cl /Iinclude /D TETRA_FEC_TEST_MAIN src\protocol\tetra\tetra_fec.c /Fe:depunct_test.exe
 */
int main(void) {
    fprintf(stderr, "[TETRA TEST] Starting depuncture mapping test...\n");

    /* We'll perform a round-trip test: puncture a synthetic mother buffer,
     * then depuncture and verify that reconstructed positions match the
     * original mother buffer values.
     */
    const int mother_len = 512;
    uint8_t *mother = (uint8_t*)malloc(mother_len);
    for (int i = 0; i < mother_len; i++) mother[i] = (uint8_t)(i & 0xFF);

    /* Choose a common puncturer and a realistic punctured length */
    int punct_id = 2; /* punct_292_432 */
    const int puncted_len = 432;
    uint8_t *puncted = (uint8_t*)malloc(puncted_len);
    int wrote = tetra_rcpc_puncture_by_id(punct_id, mother, mother_len, puncted, puncted_len);
    if (wrote != puncted_len) {
        fprintf(stderr, "[TETRA TEST] unexpected puncture length %d (wanted %d)\n", wrote, puncted_len);
        return 3;
    }

    /* Convert punctured bytes into soft costs (simple mapping) */
    uint16_t *in_costs = (uint16_t*)malloc(sizeof(uint16_t) * puncted_len);
    for (int i = 0; i < puncted_len; i++) in_costs[i] = (uint16_t)puncted[i];

    uint16_t *out_costs = (uint16_t*)malloc(sizeof(uint16_t) * mother_len);
    for (int i = 0; i < mother_len; i++) out_costs[i] = 0x7FFF;

    int ret = tetra_rcpc_depuncture_by_id(punct_id, in_costs, puncted_len, out_costs, mother_len);
    if (ret < 0) {
        fprintf(stderr, "[TETRA TEST] depuncture returned error\n");
        return 4;
    }

    /* Verify reconstructed positions equal original mother values */
    int mismatches = 0;
    int mapped = 0;
    for (int i = 0; i < mother_len; i++) {
        if (out_costs[i] != 0x7FFF) {
            mapped++;
            if ((uint8_t)out_costs[i] != mother[i]) {
                mismatches++;
                if (mismatches < 8) {
                    fprintf(stderr, "[TETRA TEST] mismatch at pos %d: got %u expected %u\n", i, out_costs[i], mother[i]);
                }
            }
        }
    }

    fprintf(stderr, "[TETRA TEST] puncted_len=%d mapped_positions=%d mismatches=%d\n", puncted_len, mapped, mismatches);
    if (mismatches == 0 && mapped > 0) {
        fprintf(stderr, "[TETRA TEST] Puncture->depuncture roundtrip OK\n");
    } else {
        fprintf(stderr, "[TETRA TEST] Roundtrip FAILED\n");
        return 5;
    }

    free(mother);
    free(puncted);
    free(in_costs);
    free(out_costs);
    return 0;
}
#endif
