// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * RCPC round trip through the production coding chain: type-2 bits are
 * encoded with the rate-1/4 mother code of EN 300 392-2 8.2.3.1.1, punctured
 * with tetra_rcpc_puncture_by_id(), restored with
 * tetra_rcpc_depuncture_by_id() and decoded with tetra_viterbi_decode_soft().
 * With noise-free costs every information bit must come back. Hard decisions
 * enter the decoder as confident costs (tetra_hard_bits_to_soft()).
 */

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/protocol/tetra/tetra_fec.h>
#include <stdint.h>
#include <stdio.h>

enum {
    MAX_TYPE2_BITS = 292,
    MAX_MOTHER_BITS = MAX_TYPE2_BITS * 4,
    NEUTRAL_COST = 0x7FFF,
};

/* G1 = 1 + D + D^4, G2 = 1 + D^2 + D^3 + D^4, G3 = 1 + D + D^2 + D^4, G4 = 1 + D + D^3 + D^4. */
static void
mother_encode(const uint8_t* bits, int len, uint8_t* out) {
    uint8_t d1 = 0, d2 = 0, d3 = 0, d4 = 0;
    for (int i = 0; i < len; i++) {
        const uint8_t b = bits[i];
        out[(i * 4) + 0] = (uint8_t)(b ^ d1 ^ d4);
        out[(i * 4) + 1] = (uint8_t)(b ^ d2 ^ d3 ^ d4);
        out[(i * 4) + 2] = (uint8_t)(b ^ d1 ^ d2 ^ d4);
        out[(i * 4) + 3] = (uint8_t)(b ^ d1 ^ d3 ^ d4);
        d4 = d3;
        d3 = d2;
        d2 = d1;
        d1 = b;
    }
}

/* A fixed pseudo-random bit pattern (xorshift32) so a failure reproduces. */
static void
fill_bits(uint8_t* bits, int len, uint32_t seed) {
    uint32_t x = seed;
    for (int i = 0; i < len; i++) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        bits[i] = (uint8_t)(x & 1U);
    }
}

static int
round_trip(int punct_id, int type2_len, int type3_len, uint32_t seed) {
    uint8_t info[MAX_TYPE2_BITS];
    uint8_t mother[MAX_MOTHER_BITS];
    uint8_t type3[MAX_MOTHER_BITS];
    uint16_t type3_costs[MAX_MOTHER_BITS];
    uint16_t restored[MAX_MOTHER_BITS];
    uint8_t decoded[MAX_TYPE2_BITS];
    const int mother_len = type2_len * 4;

    /* A type-2 block holds at least the four tail bits, and the buffers bound both lengths. */
    if (type2_len < 4 || type2_len > MAX_TYPE2_BITS || type3_len < 0 || type3_len > MAX_MOTHER_BITS) {
        DSD_FPRINTF(stderr, "punct %d: lengths %d/%d out of range\n", punct_id, type2_len, type3_len);
        return 1;
    }
    fill_bits(info, type2_len, seed);
    /* The last four type-2 bits are the tail that returns the encoder to state 0. */
    for (int i = type2_len - 4; i < type2_len; i++) {
        info[i] = 0;
    }
    mother_encode(info, type2_len, mother);

    if (tetra_rcpc_puncture_by_id(punct_id, mother, mother_len, type3, type3_len) != type3_len) {
        DSD_FPRINTF(stderr, "punct %d: puncture did not write %d bits\n", punct_id, type3_len);
        return 1;
    }
    for (int i = 0; i < type3_len; i++) {
        type3_costs[i] = type3[i] ? 0xFFFFU : 0x0000U;
    }
    for (int i = 0; i < mother_len; i++) {
        restored[i] = NEUTRAL_COST;
    }
    if (tetra_rcpc_depuncture_by_id(punct_id, type3_costs, type3_len, restored, mother_len) < 0) {
        DSD_FPRINTF(stderr, "punct %d: depuncture failed\n", punct_id);
        return 1;
    }
    for (int i = 0; i < mother_len; i++) {
        if (restored[i] != NEUTRAL_COST && restored[i] != (mother[i] ? 0xFFFFU : 0x0000U)) {
            DSD_FPRINTF(stderr, "punct %d: mother bit %d restored to the wrong value\n", punct_id, i);
            return 1;
        }
    }
    if (tetra_viterbi_decode_soft(restored, mother_len, decoded, type2_len) < 0) {
        DSD_FPRINTF(stderr, "punct %d: Viterbi refused %d costs\n", punct_id, mother_len);
        return 1;
    }
    for (int i = 0; i < type2_len; i++) {
        if (decoded[i] != info[i]) {
            DSD_FPRINTF(stderr, "punct %d seed %u: bit %d decoded %u, sent %u\n", punct_id, seed, i, decoded[i],
                        info[i]);
            return 1;
        }
    }
    return 0;
}

/* Hard decisions given to the Viterbi decoder as confident soft costs (tetra_hard_bits_to_soft()): a 0 costs nothing,
 * a 1 costs 0xFFFF, and only the low bit of an unpacked value counts. */
static int
hard_bits_keep_their_evidence(void) {
    static const uint8_t bits[] = {0, 1, 1, 0, 3, 2};
    uint16_t costs[6] = {0};
    tetra_hard_bits_to_soft(bits, costs, 6);
    static const uint16_t k_expected[6] = {0x0000u, 0xFFFFu, 0xFFFFu, 0x0000u, 0xFFFFu, 0x0000u};
    for (int i = 0; i < 6; i++) {
        if (costs[i] != k_expected[i]) {
            DSD_FPRINTF(stderr, "hard bit %d: cost 0x%04X, expected 0x%04X\n", i, costs[i], k_expected[i]);
            return 1;
        }
    }
    return 0;
}

int
main(void) {
    static const struct {
        int punct_id;
        int type2_len;
        int type3_len;
    } k_cases[] = {
        {TETRA_RCPC_PUNCT_2_3, 80, 120},      /* BSCH */
        {TETRA_RCPC_PUNCT_2_3, 144, 216},     /* SCH/HD, BNCH, STCH */
        {TETRA_RCPC_PUNCT_2_3, 288, 432},     /* SCH/F */
        {TETRA_RCPC_PUNCT_292_432, 292, 432}, /* TCH/7.2 */
        {TETRA_RCPC_PUNCT_148_432, 148, 432}, /* TCH/2.4 */
    };

    int rc = hard_bits_keep_their_evidence();
    for (size_t i = 0; i < sizeof k_cases / sizeof k_cases[0]; i++) {
        for (uint32_t seed = 1; seed <= 3; seed++) {
            rc |= round_trip(k_cases[i].punct_id, k_cases[i].type2_len, k_cases[i].type3_len, seed * 0x9E3779B9U);
        }
    }
    return rc;
}
