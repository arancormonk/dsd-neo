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

/* 128-bit integer vectors for the Viterbi add-compare-select: SSE2 is the x86-64 baseline, NEON always present on
   AArch64. Other targets use the scalar reference. */
#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#define TETRA_VITERBI_SIMD_SSE2 1
#include <emmintrin.h>
#elif defined(__aarch64__) || defined(_M_ARM64)
#define TETRA_VITERBI_SIMD_NEON 1
#include <arm_neon.h>
#endif

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
    /* Coded word on the lower predecessor's edge into each destination, lane 0 in bit 0.
       The other predecessor flips every lane for both codes. */
    uint32_t incoming[16];
} tetra_conv_code;

/* EN 300 392-2 clause 8.2.3.1.1, register bit 0 = D: G1 = 1 + D + D^4, G2 = 1 + D^2 + D^3 + D^4,
   G3 = 1 + D + D^2 + D^4, G4 = 1 + D + D^3 + D^4. */
static const tetra_conv_code k_signalling_code = {
    4, {0x19, 0x1E, 0x1B, 0x1D}, 1, {0, 15, 13, 2, 6, 9, 11, 4, 10, 5, 7, 8, 12, 3, 1, 14}};

/* EN 300 395-2 clause 5.4.3.1, register bit 3 = D: G1 = 1 + D + D^2 + D^3 + D^4, G2 = 1 + D + D^3 + D^4,
   G3 = 1 + D^2 + D^4. */
static const tetra_conv_code k_speech_code = {
    3, {0x1F, 0x1B, 0x15, 0x00}, 0, {0, 3, 5, 6, 3, 0, 6, 5, 7, 4, 2, 1, 4, 7, 1, 2}};

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

/* Survivor metrics never exceed INF plus one branch: a reachable path costs at most 512 * 4 * 65535 < 2^27, so it
   stays below INF = 2^30 - 1, and the largest branch adds less than 2^18. Every sum therefore stays below 2^31, which
   lets the vector kernels compare metrics as signed 32-bit values. */
#define TETRA_VITERBI_INF (UINT32_MAX / 4u)

/* One add-compare-select step over the trellis; @p pred records each state's surviving predecessor. This is the
   reference the vector kernels below must match bit for bit. */
static void
conv_acs_step(const tetra_conv_code* code, const uint32_t* prev, uint32_t* next, const uint16_t* costs, uint8_t* pred) {
    /* INF plus a branch fits (see TETRA_VITERBI_INF), so unreachable paths can be clamped after selection. */
    const uint32_t inf = TETRA_VITERBI_INF;
    uint32_t branch[TETRA_VITERBI_STATES] = {0};
    for (int lane = 0; lane < code->lanes; lane++) {
        const uint32_t zero = costs[lane];
        const uint32_t one = 65535u - zero;
        for (int to = 0; to < TETRA_VITERBI_STATES; to++) {
            branch[to] += ((code->incoming[to] >> lane) & 1u) ? one : zero;
        }
    }
    const uint32_t complement = (uint32_t)code->lanes * 65535u;
    for (int to = 0; to < TETRA_VITERBI_STATES; to++) {
        const int low = code->shift_up ? (to >> 1) : ((to << 1) & 15);
        const int high = low + (code->shift_up ? 8 : 1);
        const uint32_t a = prev[low] + branch[to];
        const uint32_t b = prev[high] + (complement - branch[to]);
        /* Strict improvement by the higher predecessor preserves the original tie order. */
        const uint32_t best = b < a ? b : a;
        next[to] = best < inf ? best : inf;
        pred[to] = best < inf ? (uint8_t)(b < a ? high : low) : 0;
    }
}

/* @p nsteps trellis steps from the survivor metrics in @p metric, which receive the final ones; step i reads
   costs[i * lanes ...] and fills pred[i * 16 ...]. */
static void
conv_acs_run_scalar(const tetra_conv_code* code, uint32_t* metric, const uint16_t* costs, int nsteps, uint8_t* pred) {
    uint32_t spare[TETRA_VITERBI_STATES];
    uint32_t* bufs[2] = {metric, spare};
    for (int pos = 0; pos < nsteps; pos++) {
        conv_acs_step(code, bufs[pos & 1], bufs[(pos + 1) & 1], costs + ((size_t)pos * (size_t)code->lanes),
                      pred + ((size_t)pos * TETRA_VITERBI_STATES));
    }
    if (nsteps & 1) {
        DSD_MEMCPY(metric, spare, sizeof(spare));
    }
}

/* The vector kernels hold the 16 metrics as four 4-lane vectors (states 4k..4k+3). The branch metric is the sum of
   the lanes' costs plus, for each lane whose lower-edge coded bit is 1, the difference (65535 - 2 * cost), added
   modulo 2^32 through a lane mask. The predecessors are fixed shuffles: with shift_up, low = to >> 1 (states 0..7
   each duplicated) and high = low + 8; otherwise low = 2 * to mod 16 (the even states) and high = low + 1. */
#if defined(TETRA_VITERBI_SIMD_SSE2) || defined(TETRA_VITERBI_SIMD_NEON)
/* A bound under which every survivor stays below INF for @p remaining more steps, each adding at most one maximal
   branch (@p lanes * 65535); 0, which no metric is below, when there is none. */
static int
conv_acs_safe_limit(int remaining, int lanes) {
    const int64_t growth = (int64_t)remaining * (int64_t)lanes * 65535;
    return growth < (int64_t)TETRA_VITERBI_INF ? (int)((int64_t)TETRA_VITERBI_INF - growth) : 0;
}

#endif

#if defined(TETRA_VITERBI_SIMD_SSE2)
static __m128i
acs_select_sse2(__m128i mask, __m128i when_set, __m128i otherwise) {
    return _mm_or_si128(_mm_and_si128(mask, when_set), _mm_andnot_si128(mask, otherwise));
}

/* Even (@p odd 0) or odd 32-bit elements of a then b. */
static __m128i
acs_deinterleave_sse2(__m128i a, __m128i b, int odd) {
    const __m128 fa = _mm_castsi128_ps(a);
    const __m128 fb = _mm_castsi128_ps(b);
    return _mm_castps_si128(odd ? _mm_shuffle_ps(fa, fb, _MM_SHUFFLE(3, 1, 3, 1))
                                : _mm_shuffle_ps(fa, fb, _MM_SHUFFLE(2, 0, 2, 0)));
}

/* One vector of add-compare-select from the lower and higher predecessors' survivors and the lower edge's branch;
   @p took_high receives the selection mask. With @p clamp, @p reachable receives the mask of survivors below INF
   and the rest become INF; without it the caller has shown that every survivor stays below INF. */
static inline __m128i
acs_vector_sse2(__m128i low, __m128i high, __m128i branch, __m128i complement, __m128i* took_high, __m128i* reachable,
                const int clamp) {
    const __m128i inf = _mm_set1_epi32((int)TETRA_VITERBI_INF);
    const __m128i a = _mm_add_epi32(low, branch);
    const __m128i b = _mm_add_epi32(high, _mm_sub_epi32(complement, branch));
    /* Signed compares are exact: both sums are below 2^31 (see TETRA_VITERBI_INF). */
    *took_high = _mm_cmpgt_epi32(a, b);
    const __m128i best = acs_select_sse2(*took_high, b, a);
    if (!clamp) {
        return best;
    }
    *reachable = _mm_cmpgt_epi32(inf, best);
    return acs_select_sse2(*reachable, best, inf);
}

/* All-ones or zero 32-bit masks narrowed to bytes; signed saturation keeps both values. */
static inline __m128i
acs_narrow_masks_sse2(__m128i m0, __m128i m1, __m128i m2, __m128i m3) {
    return _mm_packs_epi16(_mm_packs_epi32(m0, m1), _mm_packs_epi32(m2, m3));
}

/* One trellis step on the survivors in @p p (four vectors), writing the step's 16 predecessors to @p pred_row. */
static inline void
acs_step_sse2(__m128i* p, const __m128i (*lane_mask)[4], const uint16_t* step, uint8_t* pred_row, const int lanes,
              const int shift_up, const int clamp) {
    const __m128i complement = _mm_set1_epi32(lanes * 65535);
    const __m128i delta = _mm_set1_epi8((char)(shift_up ? 8 : 1));
    const __m128i low_index = shift_up ? _mm_setr_epi8(0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7)
                                       : _mm_setr_epi8(0, 2, 4, 6, 8, 10, 12, 14, 0, 2, 4, 6, 8, 10, 12, 14);
    int base = 0;
    for (int lane = 0; lane < lanes; lane++) {
        base += step[lane];
    }
    __m128i b0 = _mm_set1_epi32(base);
    __m128i b1 = b0, b2 = b0, b3 = b0;
    for (int lane = 0; lane < lanes; lane++) {
        const __m128i diff = _mm_set1_epi32(65535 - (2 * (int)step[lane]));
        b0 = _mm_add_epi32(b0, _mm_and_si128(lane_mask[lane][0], diff));
        b1 = _mm_add_epi32(b1, _mm_and_si128(lane_mask[lane][1], diff));
        b2 = _mm_add_epi32(b2, _mm_and_si128(lane_mask[lane][2], diff));
        b3 = _mm_add_epi32(b3, _mm_and_si128(lane_mask[lane][3], diff));
    }
    __m128i l0, l1, l2, l3, h0, h1, h2, h3;
    if (shift_up) {
        l0 = _mm_unpacklo_epi32(p[0], p[0]);
        l1 = _mm_unpackhi_epi32(p[0], p[0]);
        l2 = _mm_unpacklo_epi32(p[1], p[1]);
        l3 = _mm_unpackhi_epi32(p[1], p[1]);
        h0 = _mm_unpacklo_epi32(p[2], p[2]);
        h1 = _mm_unpackhi_epi32(p[2], p[2]);
        h2 = _mm_unpacklo_epi32(p[3], p[3]);
        h3 = _mm_unpackhi_epi32(p[3], p[3]);
    } else {
        l0 = l2 = acs_deinterleave_sse2(p[0], p[1], 0);
        l1 = l3 = acs_deinterleave_sse2(p[2], p[3], 0);
        h0 = h2 = acs_deinterleave_sse2(p[0], p[1], 1);
        h1 = h3 = acs_deinterleave_sse2(p[2], p[3], 1);
    }
    __m128i t0, t1, t2, t3;
    __m128i r0 = _mm_setzero_si128(), r1 = r0, r2 = r0, r3 = r0;
    p[0] = acs_vector_sse2(l0, h0, b0, complement, &t0, &r0, clamp);
    p[1] = acs_vector_sse2(l1, h1, b1, complement, &t1, &r1, clamp);
    p[2] = acs_vector_sse2(l2, h2, b2, complement, &t2, &r2, clamp);
    p[3] = acs_vector_sse2(l3, h3, b3, complement, &t3, &r3, clamp);
    __m128i chosen = _mm_add_epi8(low_index, _mm_and_si128(acs_narrow_masks_sse2(t0, t1, t2, t3), delta));
    if (clamp) {
        chosen = _mm_and_si128(acs_narrow_masks_sse2(r0, r1, r2, r3), chosen);
    }
    _mm_storeu_si128((__m128i*)(void*)pred_row, chosen);
}

/* Whether every survivor is below @p limit. */
static inline int
acs_all_below_sse2(const __m128i* p, int limit) {
    const __m128i bound = _mm_set1_epi32(limit);
    const __m128i below = _mm_and_si128(_mm_and_si128(_mm_cmpgt_epi32(bound, p[0]), _mm_cmpgt_epi32(bound, p[1])),
                                        _mm_and_si128(_mm_cmpgt_epi32(bound, p[2]), _mm_cmpgt_epi32(bound, p[3])));
    return _mm_movemask_epi8(below) == 0xFFFF;
}

static inline void
conv_acs_run_simd(const tetra_conv_code* code, uint32_t* metric, const uint16_t* costs, int nsteps, uint8_t* pred,
                  const int lanes, const int shift_up) {
    __m128i lane_mask[4][4];
    for (int lane = 0; lane < lanes; lane++) {
        const __m128i bit = _mm_set1_epi32(1 << lane);
        for (int k = 0; k < 4; k++) {
            const __m128i incoming = _mm_loadu_si128((const __m128i*)(const void*)(code->incoming + (4 * k)));
            lane_mask[lane][k] = _mm_cmpeq_epi32(_mm_and_si128(incoming, bit), bit);
        }
    }
    __m128i p[4];
    for (int k = 0; k < 4; k++) {
        p[k] = _mm_loadu_si128((const __m128i*)(const void*)(metric + (4 * k)));
    }
    int pos = 0;
    /* Clamp until every survivor sits below INF by more than the largest growth the remaining steps allow (one
       maximal branch each), from which point no survivor can reach INF. From the decoder's start state that takes
       four steps. */
    for (; pos < nsteps; pos++) {
        if (acs_all_below_sse2(p, conv_acs_safe_limit(nsteps - pos, lanes))) {
            break;
        }
        acs_step_sse2(p, (const __m128i(*)[4])lane_mask, costs + ((size_t)pos * (size_t)lanes),
                      pred + ((size_t)pos * TETRA_VITERBI_STATES), lanes, shift_up, 1);
    }
    for (; pos < nsteps; pos++) {
        acs_step_sse2(p, (const __m128i(*)[4])lane_mask, costs + ((size_t)pos * (size_t)lanes),
                      pred + ((size_t)pos * TETRA_VITERBI_STATES), lanes, shift_up, 0);
    }
    for (int k = 0; k < 4; k++) {
        _mm_storeu_si128((__m128i*)(void*)(metric + (4 * k)), p[k]);
    }
}

#elif defined(TETRA_VITERBI_SIMD_NEON)

/* The SSE2 kernel's structure, with unsigned compares and minimum. */
static inline uint32x4_t
acs_vector_neon(uint32x4_t low, uint32x4_t high, uint32x4_t branch, uint32x4_t complement, uint32x4_t* took_high,
                uint32x4_t* reachable, const int clamp) {
    const uint32x4_t inf = vdupq_n_u32(TETRA_VITERBI_INF);
    const uint32x4_t a = vaddq_u32(low, branch);
    const uint32x4_t b = vaddq_u32(high, vsubq_u32(complement, branch));
    *took_high = vcltq_u32(b, a);
    const uint32x4_t best = vminq_u32(a, b);
    if (!clamp) {
        return best;
    }
    *reachable = vcltq_u32(best, inf);
    return vminq_u32(best, inf);
}

static inline uint8x16_t
acs_narrow_masks_neon(uint32x4_t m0, uint32x4_t m1, uint32x4_t m2, uint32x4_t m3) {
    return vcombine_u8(vmovn_u16(vcombine_u16(vmovn_u32(m0), vmovn_u32(m1))),
                       vmovn_u16(vcombine_u16(vmovn_u32(m2), vmovn_u32(m3))));
}

static inline void
acs_step_neon(uint32x4_t* p, const uint32x4_t (*lane_mask)[4], const uint16_t* step, uint8_t* pred_row, const int lanes,
              const int shift_up, const int clamp) {
    static const uint8_t k_low_up[16] = {0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7};
    static const uint8_t k_low_down[16] = {0, 2, 4, 6, 8, 10, 12, 14, 0, 2, 4, 6, 8, 10, 12, 14};
    const uint32x4_t complement = vdupq_n_u32((uint32_t)lanes * 65535u);
    const uint8x16_t delta = vdupq_n_u8(shift_up ? 8u : 1u);
    const uint8x16_t low_index = vld1q_u8(shift_up ? k_low_up : k_low_down);
    uint32_t base = 0;
    for (int lane = 0; lane < lanes; lane++) {
        base += step[lane];
    }
    uint32x4_t b0 = vdupq_n_u32(base);
    uint32x4_t b1 = b0, b2 = b0, b3 = b0;
    for (int lane = 0; lane < lanes; lane++) {
        const uint32x4_t diff = vdupq_n_u32(65535u - (2u * (uint32_t)step[lane]));
        b0 = vaddq_u32(b0, vandq_u32(lane_mask[lane][0], diff));
        b1 = vaddq_u32(b1, vandq_u32(lane_mask[lane][1], diff));
        b2 = vaddq_u32(b2, vandq_u32(lane_mask[lane][2], diff));
        b3 = vaddq_u32(b3, vandq_u32(lane_mask[lane][3], diff));
    }
    uint32x4_t l0, l1, l2, l3, h0, h1, h2, h3;
    if (shift_up) {
        const uint32x4x2_t l01 = vzipq_u32(p[0], p[0]);
        const uint32x4x2_t l23 = vzipq_u32(p[1], p[1]);
        const uint32x4x2_t h01 = vzipq_u32(p[2], p[2]);
        const uint32x4x2_t h23 = vzipq_u32(p[3], p[3]);
        l0 = l01.val[0];
        l1 = l01.val[1];
        l2 = l23.val[0];
        l3 = l23.val[1];
        h0 = h01.val[0];
        h1 = h01.val[1];
        h2 = h23.val[0];
        h3 = h23.val[1];
    } else {
        const uint32x4x2_t s01 = vuzpq_u32(p[0], p[1]);
        const uint32x4x2_t s23 = vuzpq_u32(p[2], p[3]);
        l0 = l2 = s01.val[0];
        l1 = l3 = s23.val[0];
        h0 = h2 = s01.val[1];
        h1 = h3 = s23.val[1];
    }
    uint32x4_t t0, t1, t2, t3;
    uint32x4_t r0 = vdupq_n_u32(0), r1 = r0, r2 = r0, r3 = r0;
    p[0] = acs_vector_neon(l0, h0, b0, complement, &t0, &r0, clamp);
    p[1] = acs_vector_neon(l1, h1, b1, complement, &t1, &r1, clamp);
    p[2] = acs_vector_neon(l2, h2, b2, complement, &t2, &r2, clamp);
    p[3] = acs_vector_neon(l3, h3, b3, complement, &t3, &r3, clamp);
    uint8x16_t chosen = vaddq_u8(low_index, vandq_u8(acs_narrow_masks_neon(t0, t1, t2, t3), delta));
    if (clamp) {
        chosen = vandq_u8(acs_narrow_masks_neon(r0, r1, r2, r3), chosen);
    }
    vst1q_u8(pred_row, chosen);
}

static inline int
acs_all_below_neon(const uint32x4_t* p, int limit) {
    const uint32x4_t bound = vdupq_n_u32((uint32_t)limit);
    const uint32x4_t below = vandq_u32(vandq_u32(vcltq_u32(p[0], bound), vcltq_u32(p[1], bound)),
                                       vandq_u32(vcltq_u32(p[2], bound), vcltq_u32(p[3], bound)));
    const uint32x2_t folded = vand_u32(vget_low_u32(below), vget_high_u32(below));
    return (vget_lane_u32(folded, 0) & vget_lane_u32(folded, 1)) == UINT32_MAX;
}

static inline void
conv_acs_run_simd(const tetra_conv_code* code, uint32_t* metric, const uint16_t* costs, int nsteps, uint8_t* pred,
                  const int lanes, const int shift_up) {
    uint32x4_t lane_mask[4][4];
    for (int lane = 0; lane < lanes; lane++) {
        const uint32x4_t bit = vdupq_n_u32(1u << lane);
        for (int k = 0; k < 4; k++) {
            lane_mask[lane][k] = vtstq_u32(vld1q_u32(code->incoming + (4 * k)), bit);
        }
    }
    uint32x4_t p[4];
    for (int k = 0; k < 4; k++) {
        p[k] = vld1q_u32(metric + (4 * k));
    }
    int pos = 0;
    /* See the SSE2 kernel. */
    for (; pos < nsteps; pos++) {
        if (acs_all_below_neon(p, conv_acs_safe_limit(nsteps - pos, lanes))) {
            break;
        }
        acs_step_neon(p, (const uint32x4_t(*)[4])lane_mask, costs + ((size_t)pos * (size_t)lanes),
                      pred + ((size_t)pos * TETRA_VITERBI_STATES), lanes, shift_up, 1);
    }
    for (; pos < nsteps; pos++) {
        acs_step_neon(p, (const uint32x4_t(*)[4])lane_mask, costs + ((size_t)pos * (size_t)lanes),
                      pred + ((size_t)pos * TETRA_VITERBI_STATES), lanes, shift_up, 0);
    }
    for (int k = 0; k < 4; k++) {
        vst1q_u32(metric + (4 * k), p[k]);
    }
}

#endif

static void
conv_acs_run(const tetra_conv_code* code, uint32_t* metric, const uint16_t* costs, int nsteps, uint8_t* pred) {
#if defined(TETRA_VITERBI_SIMD_SSE2) || defined(TETRA_VITERBI_SIMD_NEON)
    /* One copy per code shape, so the per-lane and per-vector loops unroll into registers. */
    if (code->lanes == 4 && code->shift_up) {
        conv_acs_run_simd(code, metric, costs, nsteps, pred, 4, 1);
        return;
    }
    if (code->lanes == 3 && !code->shift_up) {
        conv_acs_run_simd(code, metric, costs, nsteps, pred, 3, 0);
        return;
    }
#endif
    conv_acs_run_scalar(code, metric, costs, nsteps, pred);
}

/* Decode @p nbits input bits from @p costs (code->lanes per bit). The encoders end on four zero tail bits, so the
   path ending in state 0 is the answer; the best state stands in only when state 0 is unreachable. */
static void
conv_viterbi(const tetra_conv_code* code, const uint16_t* costs, int nbits, uint8_t* out_bits) {
    const uint32_t inf = TETRA_VITERBI_INF;
    uint32_t metric[TETRA_VITERBI_STATES];
    /* Every step writes all 16 predecessors of its row, so the rows need no clearing. */
    uint8_t pred[TETRA_VITERBI_MAX_BITS * TETRA_VITERBI_STATES];
    for (int state = 0; state < TETRA_VITERBI_STATES; state++) {
        metric[state] = state == 0 ? 0 : inf;
    }
    conv_acs_run(code, metric, costs, nbits, pred);
    int state = 0;
    if (metric[0] == inf) {
        for (int candidate = 1; candidate < TETRA_VITERBI_STATES; candidate++) {
            if (metric[candidate] < metric[state]) {
                state = candidate;
            }
        }
    }
    for (int pos = nbits - 1; pos >= 0; pos--) {
        out_bits[pos] = (uint8_t)conv_input_of(code, state);
        state = pred[((size_t)pos * TETRA_VITERBI_STATES) + (size_t)state];
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
