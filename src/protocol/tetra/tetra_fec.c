// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * TETRA channel coding.
 *
 * Signalling channels (EN 300 392-2 clause 8.2): a 16-state rate-1/4 mother code punctured to the channel's rate,
 * block interleaving and scrambling. Speech (EN 300 395-2 clauses 5 and 6): class 0 sent uncoded, classes 1 and 2
 * through a 16-state rate-1/3 mother code punctured per class, the class-2 bits protected by a CRC.
 */

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/protocol/tetra/tetra_fec.h>
#include <stddef.h>
#include <stdint.h>

/* ---------------------------------------------------------------------------------------------------------------
 * RCPC puncturing (EN 300 392-2 clause 8.2.3.1.2, EN 300 395-2 clause 5.4.3.2): type-3 bit j carries mother bit
 *   k = Period * ((i - 1) div t) + P(i - t * ((i - 1) div t)),
 * where i = j except for the two 432-bit traffic schemes, which skip one position every 65 or 35.
 * ------------------------------------------------------------------------------------------------------------- */

typedef enum { PUNCT_I_EQUALS_J = 0, PUNCT_I_SKIP_65, PUNCT_I_SKIP_35 } tetra_punct_i;

typedef struct {
    const uint8_t* P; /* P(1)..P(t) */
    uint8_t t;
    uint8_t period;
    uint8_t i_rule;
} tetra_puncturer;

static const uint8_t k_p_2_3[] = {1, 2, 5};
static const uint8_t k_p_1_3[] = {1, 2, 3, 5, 6, 7};
static const uint8_t k_p_8_12[] = {1, 2, 4};
static const uint8_t k_p_8_18[] = {1, 2, 3, 4, 5, 7, 8, 10, 11};
static const uint8_t k_p_8_17[] = {1, 2, 3, 4, 5, 7, 8, 10, 11, 13, 14, 16, 17, 19, 20, 22, 23};

/* Indexed by TETRA_RCPC_PUNCT_*. */
static const tetra_puncturer k_puncturers[] = {
    {k_p_2_3, 3, 8, PUNCT_I_EQUALS_J},    {k_p_1_3, 6, 8, PUNCT_I_EQUALS_J},  {k_p_2_3, 3, 8, PUNCT_I_SKIP_65},
    {k_p_1_3, 6, 8, PUNCT_I_SKIP_35},     {k_p_8_12, 3, 6, PUNCT_I_EQUALS_J}, {k_p_8_18, 9, 12, PUNCT_I_EQUALS_J},
    {k_p_8_17, 17, 24, PUNCT_I_EQUALS_J},
};

static const tetra_puncturer*
tetra_puncturer_of(int punct_id) {
    if (punct_id < 0 || punct_id >= (int)(sizeof(k_puncturers) / sizeof(k_puncturers[0]))) {
        return NULL;
    }
    return &k_puncturers[punct_id];
}

/* Mother-code position (one-based) of type-3 bit @p j (one-based); 0 when there is none. */
static uint32_t
tetra_puncturer_k(const tetra_puncturer* p, uint32_t j) {
    if (j == 0) {
        return 0;
    }
    uint32_t i = j;
    if (p->i_rule == PUNCT_I_SKIP_65) {
        i = j + ((j - 1U) / 65U);
    } else if (p->i_rule == PUNCT_I_SKIP_35) {
        i = j + ((j - 1U) / 35U);
    }
    const uint32_t block = (i - 1U) / p->t;
    return (p->period * block) + p->P[(i - 1U) - (p->t * block)];
}

int
tetra_rcpc_map_j_to_k(int punct_id, uint32_t j) {
    const tetra_puncturer* p = tetra_puncturer_of(punct_id);
    if (!p || j == 0 || j > 0xFFFFFFu) {
        return -1;
    }
    return (int)tetra_puncturer_k(p, j);
}

int
tetra_rcpc_depuncture_by_id(int punct_id, const uint16_t* in_costs, int in_len, uint16_t* out_costs, int out_len) {
    const tetra_puncturer* p = tetra_puncturer_of(punct_id);
    if (!p || !in_costs || !out_costs || in_len < 0 || out_len < 0) {
        return -1;
    }
    for (int k = 0; k < out_len; k++) {
        out_costs[k] = TETRA_SOFT_NEUTRAL;
    }
    for (int j = 1; j <= in_len; j++) {
        const uint32_t k = tetra_puncturer_k(p, (uint32_t)j);
        if (k >= 1U && k <= (uint32_t)out_len) {
            out_costs[k - 1U] = in_costs[j - 1];
        }
    }
    return out_len;
}

int
tetra_rcpc_puncture_by_id(int punct_id, const uint8_t* mother, int mother_len, uint8_t* out, int out_len) {
    const tetra_puncturer* p = tetra_puncturer_of(punct_id);
    if (!p || !mother || !out || mother_len < 0 || out_len < 0) {
        return -1;
    }
    for (int j = 1; j <= out_len; j++) {
        const uint32_t k = tetra_puncturer_k(p, (uint32_t)j);
        out[j - 1] = (k >= 1U && k <= (uint32_t)mother_len) ? mother[k - 1U] : 0xFFu;
    }
    return out_len;
}

/* ---------------------------------------------------------------------------------------------------------------
 * Interleaving and scrambling (EN 300 392-2 clauses 8.2.4 and 8.2.5).
 * ------------------------------------------------------------------------------------------------------------- */

void
tetra_block_interleave(const uint8_t* in_bits, uint8_t* out_bits, int len, int a) {
    if (!in_bits || !out_bits || len <= 0 || a <= 0) {
        return;
    }
    const uint32_t K = (uint32_t)len;
    for (uint32_t i = 1; i <= K; i++) {
        out_bits[(((uint32_t)a * i) % K)] = in_bits[i - 1U];
    }
}

void
tetra_block_deinterleave_soft(const uint16_t* in_costs, uint16_t* out_costs, int len, int a) {
    if (!in_costs || !out_costs || len <= 0 || a <= 0) {
        return;
    }
    const uint32_t K = (uint32_t)len;
    for (uint32_t i = 1; i <= K; i++) {
        out_costs[i - 1U] = in_costs[(((uint32_t)a * i) % K)];
    }
}

void
tetra_speech_deinterleave_soft(const uint16_t* in_costs, uint16_t* out_costs) {
    if (!in_costs || !out_costs) {
        return;
    }
    /* C4(i * 24 + j) = C3(j * 18 + i): the received bits are the (24, 18) matrix transposed. */
    for (int column = 0; column < 18; column++) {
        for (int line = 0; line < 24; line++) {
            out_costs[(line * 18) + column] = in_costs[(column * 24) + line];
        }
    }
}

/* One step of the scrambling sequence generator of EN 300 392-2 clause 8.2.5.1: the register holds p(k-32)..p(k-1)
   with p(k-1) in bit 31; the output p(k) is the sum of the 14 tapped terms of equation 8.42. */
static uint8_t
tetra_lfsr32_next(uint32_t* lfsr) {
    const uint32_t s = *lfsr;
    const uint32_t bit = ((s >> 0) ^ (s >> 6) ^ (s >> 9) ^ (s >> 10) ^ (s >> 16) ^ (s >> 20) ^ (s >> 21) ^ (s >> 22)
                          ^ (s >> 24) ^ (s >> 25) ^ (s >> 27) ^ (s >> 28) ^ (s >> 30) ^ (s >> 31))
                         & 1u;
    *lfsr = (s >> 1) | (bit << 31);
    return (uint8_t)bit;
}

void
tetra_descramble_soft(uint16_t* costs, int len, uint32_t lfsr_init) {
    if (!costs || len <= 0) {
        return;
    }
    uint32_t state = lfsr_init ? lfsr_init : 3u; /* an all-zero register never leaves zero */
    for (int i = 0; i < len; i++) {
        if (tetra_lfsr32_next(&state) && costs[i] != TETRA_SOFT_NEUTRAL) {
            costs[i] = (uint16_t)(0xFFFFu ^ (uint32_t)costs[i]);
        }
    }
}

void
tetra_hard_bits_to_soft(const uint8_t* bits, uint16_t* costs, int len) {
    if (!bits || !costs || len <= 0) {
        return;
    }
    for (int i = 0; i < len; i++) {
        costs[i] = (bits[i] & 1u) ? 0xFFFFu : 0x0000u;
    }
}

uint16_t
tetra_crc16_ccitt_bits(const uint8_t* bits, int len) {
    if (!bits || len <= 0) {
        return 0xFFFFu;
    }
    uint16_t crc = 0xFFFFu;
    for (int i = 0; i < len; i++) {
        crc ^= (uint16_t)((bits[i] & 1u) << 15);
        crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u) : (uint16_t)(crc << 1);
    }
    return crc;
}

uint32_t
tetra_compute_scramb_seed(uint16_t mcc, uint16_t mnc, uint8_t colour) {
    /* e(1)..e(30) = MCC || MNC || colour code, preceded by p(-31) = p(-30) = 1. */
    const uint32_t s = (uint32_t)(colour & 0x3fu) | ((uint32_t)(mnc & 0x3fffu) << 6) | ((uint32_t)(mcc & 0x3ffu) << 20);
    return (s << 2) | 3u;
}

/* ---------------------------------------------------------------------------------------------------------------
 * Viterbi decoding of the 16-state convolutional codes.
 *
 * The register holds the four previous inputs. A code's taps select, out of (input << 4 | register), the terms of
 * each generator polynomial; `shift_up` says how the register takes the new input.
 * ------------------------------------------------------------------------------------------------------------- */

enum { TETRA_VITERBI_MAX_BITS = 512, TETRA_VITERBI_STATES = 16 };

typedef struct {
    int lanes;       /* coded bits per input bit */
    uint8_t taps[4]; /* generator polynomial terms */
    int shift_up;    /* 1: next = (register << 1 | input) & 15 (D1 in bit 0); 0: next = input << 3 | register >> 1 */
} tetra_conv_code;

/* EN 300 392-2 clause 8.2.3.1.1, register bit 0 = D: G1 = 1 + D + D^4, G2 = 1 + D^2 + D^3 + D^4,
   G3 = 1 + D + D^2 + D^4, G4 = 1 + D + D^3 + D^4. */
static const tetra_conv_code k_signalling_code = {4, {0x19, 0x1E, 0x1B, 0x1D}, 1};

/* EN 300 395-2 clause 5.4.3.1, register bit 3 = D: G1 = 1 + D + D^2 + D^3 + D^4, G2 = 1 + D + D^3 + D^4,
   G3 = 1 + D^2 + D^4. */
static const tetra_conv_code k_speech_code = {3, {0x1F, 0x1B, 0x15, 0x00}, 0};

static uint8_t
parity5(uint8_t value) {
    value ^= (uint8_t)(value >> 4);
    value ^= (uint8_t)(value >> 2);
    value ^= (uint8_t)(value >> 1);
    return value & 1u;
}

static int
conv_next_state(const tetra_conv_code* code, int state, int bit) {
    return code->shift_up ? (((state << 1) & 0xE) | bit) : ((bit << 3) | (state >> 1));
}

static int
conv_input_of(const tetra_conv_code* code, int next_state) {
    return code->shift_up ? (next_state & 1) : (next_state >> 3);
}

static uint64_t
conv_branch_cost(const tetra_conv_code* code, int state, int bit, const uint16_t* costs) {
    const uint8_t involved = (uint8_t)((bit << 4) | state);
    uint64_t branch = 0;
    for (int lane = 0; lane < code->lanes; lane++) {
        const uint16_t target = parity5((uint8_t)(involved & code->taps[lane])) ? 0xFFFFu : 0x0000u;
        const uint16_t got = costs[lane];
        branch += target > got ? (uint64_t)(target - got) : (uint64_t)(got - target);
    }
    return branch;
}

/* One add-compare-select step over the trellis; @p pred records each state's surviving predecessor. */
static void
conv_acs_step(const tetra_conv_code* code, const uint64_t* prev, uint64_t* next, const uint16_t* costs, uint8_t* pred) {
    const uint64_t inf = UINT64_MAX / 4u;
    for (int state = 0; state < TETRA_VITERBI_STATES; state++) {
        next[state] = inf;
    }
    for (int state = 0; state < TETRA_VITERBI_STATES; state++) {
        if (prev[state] == inf) {
            continue;
        }
        for (int bit = 0; bit <= 1; bit++) {
            const int to = conv_next_state(code, state, bit);
            const uint64_t metric = prev[state] + conv_branch_cost(code, state, bit, costs);
            if (metric < next[to]) {
                next[to] = metric;
                pred[to] = (uint8_t)state;
            }
        }
    }
}

/* Decode @p nbits input bits from @p costs (code->lanes per bit). The encoders end on four zero tail bits, so the
   path ending in state 0 is the answer; the best state stands in only when state 0 is unreachable. */
static void
conv_viterbi(const tetra_conv_code* code, const uint16_t* costs, int nbits, uint8_t* out_bits) {
    const uint64_t inf = UINT64_MAX / 4u;
    uint64_t metric[2][TETRA_VITERBI_STATES];
    uint8_t pred[TETRA_VITERBI_MAX_BITS][TETRA_VITERBI_STATES];
    for (int state = 0; state < TETRA_VITERBI_STATES; state++) {
        metric[0][state] = state == 0 ? 0 : inf;
    }
    DSD_MEMSET(pred, 0, sizeof(pred[0]) * (size_t)nbits);
    for (int pos = 0; pos < nbits; pos++) {
        conv_acs_step(code, metric[pos & 1], metric[(pos + 1) & 1], costs + ((size_t)pos * (size_t)code->lanes),
                      pred[pos]);
    }
    const uint64_t* last = metric[nbits & 1];
    int state = 0;
    if (last[0] == inf) {
        for (int candidate = 1; candidate < TETRA_VITERBI_STATES; candidate++) {
            if (last[candidate] < last[state]) {
                state = candidate;
            }
        }
    }
    for (int pos = nbits - 1; pos >= 0; pos--) {
        out_bits[pos] = (uint8_t)conv_input_of(code, state);
        state = pred[pos][state];
    }
}

/* The coded bits of @p nbits input bits: code->lanes per input bit. */
static void
conv_encode(const tetra_conv_code* code, const uint8_t* bits, int nbits, uint8_t* coded) {
    int state = 0;
    for (int pos = 0; pos < nbits; pos++) {
        const int bit = bits[pos] & 1;
        const uint8_t involved = (uint8_t)((bit << 4) | state);
        for (int lane = 0; lane < code->lanes; lane++) {
            coded[((size_t)pos * (size_t)code->lanes) + (size_t)lane] = parity5((uint8_t)(involved & code->taps[lane]));
        }
        state = conv_next_state(code, state, bit);
    }
}

int
tetra_viterbi_decode_soft(const uint16_t* depunc, int depunc_len, uint8_t* out_bits, int out_bits_len) {
    if (!depunc || !out_bits || depunc_len <= 0 || (depunc_len & 3) != 0) {
        return -1;
    }
    const int nbits = depunc_len / 4;
    if (nbits > TETRA_VITERBI_MAX_BITS || out_bits_len < nbits) {
        return -1;
    }
    conv_viterbi(&k_signalling_code, depunc, nbits, out_bits);
    return nbits;
}

int
tetra_sch_hu_decode(const uint16_t* type4, uint8_t* out_bits) {
    /* 92 MAC bits + 16 CRC + 4 tail, rate 2/3 to 168 bits, interleaved with a = 13. */
    enum { K = 168, TYPE2 = 112, TYPE1 = 92, A = 13 };

    uint16_t deint[K];
    uint16_t depunc[TYPE2 * 4];
    uint8_t decoded[TYPE2];
    if (!type4 || !out_bits) {
        return 0;
    }
    tetra_block_deinterleave_soft(type4, deint, K, A);
    if (tetra_rcpc_depuncture_by_id(TETRA_RCPC_PUNCT_2_3, deint, K, depunc, TYPE2 * 4) < 0
        || tetra_viterbi_decode_soft(depunc, TYPE2 * 4, decoded, TYPE2) < TYPE2
        || tetra_crc16_ccitt_bits(decoded, TYPE1 + 16) != TETRA_CRC16_OK) {
        return 0;
    }
    DSD_MEMCPY(out_bits, decoded, TYPE1);
    return 1;
}

/* ---------------------------------------------------------------------------------------------------------------
 * Speech channel decoding (EN 300 395-2 clauses 5 and 6).
 *
 * Type-2 bits are class 0 (uncoded), class 1, class 2, the class-2 parity bits and four zero tail bits. Classes 1
 * and 2 run through one rate-1/3 trellis, whose puncturing changes with the first class-2 bit and restarts there.
 * ------------------------------------------------------------------------------------------------------------- */

typedef struct {
    int coded;        /* type-3 bits */
    int class0;       /* uncoded type-2 bits */
    int class1;       /* type-2 bits coded at rate 8/12 */
    int class2;       /* class-2 bits */
    int parity;       /* CRC bits after class 2 */
    int class2_punct; /* puncturing scheme of class 2, its parity and the tail */
} tetra_speech_layout;

enum { TETRA_SPEECH_TAIL = 4, TETRA_SPEECH_MAX_CODED_INPUT = 112 + 60 + 8 + TETRA_SPEECH_TAIL };

/* Clause 5.5: 2 x 51 + 2 x 56 + 2 x 30 + 8 + 4 = 286 type-2 bits, 432 type-3 bits. */
static const tetra_speech_layout k_speech_full = {432, 102, 112, 60, 8, TETRA_RCPC_PUNCT_SPEECH_8_18};
/* Clause 5.6: 51 + 56 + 30 + 4 + 4 = 145 type-2 bits, 216 type-3 bits. */
static const tetra_speech_layout k_speech_half = {216, 51, 56, 30, 4, TETRA_RCPC_PUNCT_SPEECH_8_17};

static int
speech_coded_inputs(const tetra_speech_layout* lay) {
    return lay->class1 + lay->class2 + lay->parity + TETRA_SPEECH_TAIL;
}

static int
speech_class1_coded(const tetra_speech_layout* lay) {
    return (lay->class1 * 3) / 2; /* rate 8/12 */
}

/* Coded type-3 bits that disagree with a re-encode of the decoded class 1 and 2 bits. */
static int
speech_mismatches(const tetra_speech_layout* lay, const uint16_t* deint, const uint8_t* inputs) {
    uint8_t mother[TETRA_SPEECH_MAX_CODED_INPUT * 3];
    uint8_t coded[432];
    const int c1_mother = lay->class1 * 3;
    const int c1_coded = speech_class1_coded(lay);
    const int c2_mother = (speech_coded_inputs(lay) - lay->class1) * 3;
    const int c2_coded = lay->coded - lay->class0 - c1_coded;

    conv_encode(&k_speech_code, inputs, speech_coded_inputs(lay), mother);
    (void)tetra_rcpc_puncture_by_id(TETRA_RCPC_PUNCT_SPEECH_8_12, mother, c1_mother, coded, c1_coded);
    (void)tetra_rcpc_puncture_by_id(lay->class2_punct, mother + c1_mother, c2_mother, coded + c1_coded, c2_coded);
    int bad = 0;
    for (int i = 0; i < c1_coded + c2_coded; i++) {
        const uint8_t got = deint[lay->class0 + i] > TETRA_SOFT_NEUTRAL ? 1u : 0u;
        if (got != coded[i]) {
            bad++;
        }
    }
    return bad;
}

/* The CRC of EN 300 395-2 equations 43 to 45: F(X) = X^degree I(X) mod G(X), with I(X) = C(1) + C(2) X + ... and
   @p low the terms of G(X) below X^degree. Bit i of the result is f(i), sent as parity bit b(i + 1). */
static uint8_t
speech_crc(const uint8_t* bits, int len, int degree, uint8_t low) {
    const uint8_t top = (uint8_t)(1u << (degree - 1));
    const uint8_t mask = (uint8_t)((1u << degree) - 1u);
    uint8_t reg = 0;
    for (int i = len - 1; i >= 0; i--) {
        const int feedback = ((reg & top) != 0) != ((bits[i] & 1u) != 0);
        reg = (uint8_t)((reg << 1) & mask);
        if (feedback) {
            reg ^= low;
        }
    }
    return reg;
}

static int
speech_parity_matches(const uint8_t* sent, int count, uint8_t computed) {
    for (int i = 0; i < count; i++) {
        if ((sent[i] & 1u) != ((computed >> i) & 1u)) {
            return 0;
        }
    }
    return 1;
}

/* Clause 5.5.1: seven CRC bits from G(X) = 1 + X^3 + X^7, then b8, the sum of the class-2 and CRC bits. */
static int
speech_full_crc_ok(const uint8_t* class2) {
    if (!speech_parity_matches(class2 + 60, 7, speech_crc(class2, 60, 7, 0x09u))) {
        return 0;
    }
    uint8_t sum = 0;
    for (int i = 0; i < 67; i++) {
        sum ^= (uint8_t)(class2[i] & 1u);
    }
    return sum == (class2[67] & 1u);
}

/* Clause 5.6.1: four CRC bits from G(X) = 1 + X + X^4. */
static int
speech_half_crc_ok(const uint8_t* class2) {
    return speech_parity_matches(class2 + 30, 4, speech_crc(class2, 30, 4, 0x03u));
}

/* Decode one speech block into @p type2 (class 0, then the trellis inputs). */
static void
speech_decode(const tetra_speech_layout* lay, const uint16_t* deint, uint8_t* type2) {
    uint16_t mother[TETRA_SPEECH_MAX_CODED_INPUT * 3];
    const int c1_mother = lay->class1 * 3;
    const int c1_coded = speech_class1_coded(lay);
    const int c2_mother = (speech_coded_inputs(lay) - lay->class1) * 3;
    const int c2_coded = lay->coded - lay->class0 - c1_coded;

    for (int i = 0; i < lay->class0; i++) {
        type2[i] = deint[i] > TETRA_SOFT_NEUTRAL ? 1u : 0u;
    }
    (void)tetra_rcpc_depuncture_by_id(TETRA_RCPC_PUNCT_SPEECH_8_12, deint + lay->class0, c1_coded, mother, c1_mother);
    (void)tetra_rcpc_depuncture_by_id(lay->class2_punct, deint + lay->class0 + c1_coded, c2_coded, mother + c1_mother,
                                      c2_mother);
    conv_viterbi(&k_speech_code, mother, speech_coded_inputs(lay), type2 + lay->class0);
}

int
tetra_speech_full_decode(const uint16_t* deinterleaved, uint8_t* out_bits, int* crc_ok, int* mismatches) {
    uint8_t type2[102 + TETRA_SPEECH_MAX_CODED_INPUT];
    const tetra_speech_layout* lay = &k_speech_full;
    if (crc_ok) {
        *crc_ok = 0;
    }
    if (mismatches) {
        *mismatches = -1;
    }
    if (!deinterleaved || !out_bits) {
        return -1;
    }
    speech_decode(lay, deinterleaved, type2);
    if (crc_ok) {
        *crc_ok = speech_full_crc_ok(type2 + lay->class0 + lay->class1);
    }
    if (mismatches) {
        *mismatches = speech_mismatches(lay, deinterleaved, type2 + lay->class0);
    }
    DSD_MEMCPY(out_bits, type2, TETRA_SPEECH_FULL_BITS);
    return TETRA_SPEECH_FULL_BITS;
}

int
tetra_speech_half_decode(const uint16_t* deinterleaved, uint8_t* out_bits, int* crc_ok, int* mismatches) {
    uint8_t type2[51 + TETRA_SPEECH_MAX_CODED_INPUT];
    const tetra_speech_layout* lay = &k_speech_half;
    if (crc_ok) {
        *crc_ok = 0;
    }
    if (mismatches) {
        *mismatches = -1;
    }
    if (!deinterleaved || !out_bits) {
        return -1;
    }
    speech_decode(lay, deinterleaved, type2);
    if (crc_ok) {
        *crc_ok = speech_half_crc_ok(type2 + lay->class0 + lay->class1);
    }
    if (mismatches) {
        *mismatches = speech_mismatches(lay, deinterleaved, type2 + lay->class0);
    }
    DSD_MEMCPY(out_bits, type2, TETRA_SPEECH_HALF_BITS);
    return TETRA_SPEECH_HALF_BITS;
}

/* ---------------------------------------------------------------------------------------------------------------
 * Shortened (30,14) Reed-Muller code (EN 300 392-2 clause 8.2.3.2)
 * ------------------------------------------------------------------------------------------------------------- */

enum { RM3014_BITS = 30, RM3014_INFO_BITS = 14 };

/* G = [I14 | P]: row i of P is what type-1 bit b1(i + 1) adds to b2(15) .. b2(30), b2(15) the most significant. */
static const uint16_t k_rm3014_parity[RM3014_INFO_BITS] = {
    0x9B60u, 0x2DE0u, 0xFC20u, 0xE03Cu, 0x983Au, 0x5436u, 0x2C2Eu,
    0xFFDFu, 0x8339u, 0x42B5u, 0x21ADu, 0x1273u, 0x096Bu, 0x04E7u,
};

static uint16_t
rm3014_parity(uint16_t info) {
    uint16_t parity = 0;
    for (int i = 0; i < RM3014_INFO_BITS; i++) {
        if ((info >> (RM3014_INFO_BITS - 1 - i)) & 1u) {
            parity ^= k_rm3014_parity[i];
        }
    }
    return parity;
}

/* The syndrome an error in bit @p k of a word (0 = b2(30)) leaves: its parity row for a type-1 bit, itself for a
   parity bit. */
static uint16_t
rm3014_bit_syndrome(int k) {
    return k >= 16 ? k_rm3014_parity[RM3014_BITS - 1 - k] : (uint16_t)(1u << k);
}

uint32_t
tetra_rm3014_encode(uint16_t info) {
    info &= (uint16_t)((1u << RM3014_INFO_BITS) - 1u);
    return ((uint32_t)info << 16) | rm3014_parity(info);
}

/* The error of at most 3 bits that leaves syndrome @p s, into @p error; its weight, or -1 for none. With minimum
   distance 8 two such errors differ by a codeword of weight 6 or less, so there is at most one and the search order
   does not matter. */
static int
rm3014_error_for(uint16_t s, uint32_t* error) {
    for (int a = 0; a < RM3014_BITS; a++) {
        const uint16_t sa = rm3014_bit_syndrome(a);
        if (sa == s) {
            *error = 1u << a;
            return 1;
        }
        for (int b = a + 1; b < RM3014_BITS; b++) {
            const uint16_t sab = (uint16_t)(sa ^ rm3014_bit_syndrome(b));
            if (sab == s) {
                *error = (1u << a) | (1u << b);
                return 2;
            }
            for (int c = b + 1; c < RM3014_BITS; c++) {
                if ((uint16_t)(sab ^ rm3014_bit_syndrome(c)) == s) {
                    *error = (1u << a) | (1u << b) | (1u << c);
                    return 3;
                }
            }
        }
    }
    return -1;
}

int
tetra_rm3014_decode(uint32_t received, uint16_t* info) {
    if ((received >> RM3014_BITS) != 0u) {
        return -1;
    }
    const uint16_t s = (uint16_t)(rm3014_parity((uint16_t)(received >> 16)) ^ (received & 0xFFFFu));
    uint32_t error = 0;
    const int corrected = s == 0u ? 0 : rm3014_error_for(s, &error);
    if (corrected < 0) {
        return -1;
    }
    if (info) {
        *info = (uint16_t)((received ^ error) >> 16);
    }
    return corrected;
}
