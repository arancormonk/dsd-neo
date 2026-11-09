// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * The downlink transmit chain of EN 300 392-2 clause 8 for whole TETRA bursts in tests: the type-1 bits of a
 * signalling block (tetra_test_pdu.h builds them) to the dibits a burst carries, and the AACH.
 *
 * - Signalling (clauses 8.2.3 to 8.2.5): CRC-16 and four tail bits, the rate-1/4 mother code punctured to 2/3, block
 *   interleaving and scrambling. The CRC, the mother code and the scrambling sequence are written out here from the
 *   standard, not taken from the decoder, so a decoder that agrees with them on a wrong seed or polynomial fails; the
 *   puncturing and interleaving tables are the library's, which test_air_chain_vector.c checks against a fixed vector.
 * - The AACH (clause 8.3.1.1): the (30,14) Reed-Muller codeword of the library's encoder, which test_tetra_aach.c
 *   checks against words received off air, scrambled here.
 * - Full-slot speech, TCH/S on an NTS1 burst (EN 300 395-2 clause 5.5): the class-2 CRC and parity bit, the rate-1/3
 *   speech mother code and the matrix interleaving are written out here from the standard; the 8/12 and 8/18
 *   puncturing tables are the library's.
 *
 * Scrambling seeds are in the decoder's register form (tetra_compute_scramb_seed()): p(-31) and p(-30) in bits 0 and
 * 1, the extended colour code e(30) to e(1) in bits 2 to 31. ttb_seed() makes one from the network and colour code.
 * Dibits carry two bits each, the first high, in the order the decoder reads them.
 */
#ifndef DSD_NEO_TESTS_PROTOCOL_TETRA_TETRA_TEST_BURST_H_
#define DSD_NEO_TESTS_PROTOCOL_TETRA_TETRA_TEST_BURST_H_

#include <assert.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/protocol/tetra/tetra_fec.h>
#include <stddef.h>
#include <stdint.h>

#include "tetra_test_pdu.h"

enum {
    TTB_BSCH_BITS = 60,     /* BSCH (and DMO SCH/S) type-1 bits */
    TTB_BSCH_DIBITS = 60,   /* its 120 coded bits */
    TTB_BLOCK_DIBITS = 108, /* a half-slot block: SB block 2, an NTS2 block, half an NTS1 block */
    TTB_AACH_DIBITS = 15,   /* the AACH: 7 in half 1 before the training sequence, 8 in half 2 after it */
    TTB_CODED_MAX = 432,
    TTB_TCH_FS_BITS = 274, /* full-slot speech type-1 bits: 102 class 0, 112 class 1, 60 class 2 (table 5 order) */
};

/* The scrambling seed of network @p mcc / @p mnc and colour code @p colour (clause 8.2.5.2): e(1) to e(30) are the MCC,
 * MNC and colour code, and colour code 0 is the predefined all-zero e(1) to e(30) (table 21.76 note 1), the BSCH's
 * fixed one. */
static inline uint32_t
ttb_seed(uint32_t mcc, uint32_t mnc, uint32_t colour) {
    if ((colour & 0x3Fu) == 0u) {
        return 3u;
    }
    const uint32_t e = ((mcc & 0x3FFu) << 20) | ((mnc & 0x3FFFu) << 6) | (colour & 0x3Fu);
    return (e << 2) | 3u;
}

#define TTB_BSCH_SEED 3u

/* p(1) to p(@p count) of the scrambling sequence of @p seed: equations 8.41 and 8.42 in direct form, from the history
 * p(-31) .. p(0), which the seed holds bit by bit. */
static inline void
ttb_scrambling_sequence(uint32_t seed, int count, uint8_t* pn) {
    static const uint8_t taps[] = {1, 2, 4, 5, 7, 8, 10, 11, 12, 16, 22, 23, 26, 32};
    uint8_t history[32 + TTB_CODED_MAX];
    assert(count >= 0 && count <= TTB_CODED_MAX);
    for (int j = 0; j < 32; j++) {
        history[j] = (uint8_t)((seed >> j) & 1u);
    }
    for (int k = 1; k <= count; k++) {
        uint8_t p = 0;
        for (size_t j = 0; j < sizeof taps; j++) {
            p ^= history[k + 31 - taps[j]];
        }
        history[k + 31] = p;
        pn[k - 1] = p;
    }
}

/* @p count bits, scrambled with @p seed, as @p count / 2 dibits. */
static inline void
ttb_scramble_to_dibits(const uint8_t* bits, int count, uint32_t seed, uint8_t* dibits) {
    uint8_t pn[TTB_CODED_MAX];
    assert(count % 2 == 0);
    ttb_scrambling_sequence(seed, count, pn);
    for (int i = 0; i < count / 2; i++) {
        const uint8_t b0 = (uint8_t)((bits[2 * i] ^ pn[2 * i]) & 1u);
        const uint8_t b1 = (uint8_t)((bits[(2 * i) + 1] ^ pn[(2 * i) + 1]) & 1u);
        dibits[i] = (uint8_t)((b0 << 1) | b1);
    }
}

/* What @p count all-zero bits scrambled with @p seed send: the scrambling sequence, as dibits. */
static inline void
ttb_scrambled_zeros(uint32_t seed, int count, uint8_t* dibits) {
    const uint8_t zeros[TTB_CODED_MAX] = {0};
    ttb_scramble_to_dibits(zeros, count, seed, dibits);
}

/* The 16 check bits of @p n type-1 bits (clause 8.2.3.2): the CRC-CCITT register, initialised to all ones, over the
 * bits, then complemented, first bit high. */
static inline uint16_t
ttb_crc16(const uint8_t* bits, int n) {
    uint16_t crc = 0xFFFFu;
    for (int i = 0; i < n; i++) {
        const uint16_t in = (uint16_t)((bits[i] & 1u) << 15);
        crc = ((crc ^ in) & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u) : (uint16_t)(crc << 1);
    }
    return (uint16_t)~crc;
}

/* Signalling transmit chain for @p ntype1 type-1 bits: CRC-16 and four tail bits, the rate-1/4 mother code
 * (clause 8.2.3.1.1) punctured to 2/3 into @p ncoded bits, block interleaving with @p a and scrambling with @p seed, as
 * the dibits the decoder reads. BSCH (clause 8.3.1.3) is 60 bits to 120 with a = 11 and TTB_BSCH_SEED, SCH/HD
 * (clause 8.3.1.4.1) 124 bits to 216 with a = 101, SCH/HU (clause 8.3.1.4.3) 92 bits to 168 with a = 13, SCH/F
 * (clause 8.3.1.2) 268 bits to 432 with a = 103. */
static inline void
ttb_encode_signalling(const uint8_t* type1, int ntype1, int ncoded, int a, uint32_t seed, uint8_t* dibits) {
    enum { TYPE2_MAX = 288 };

    const int ntype2 = ntype1 + 16 + 4;
    assert(ntype2 <= TYPE2_MAX && ncoded <= TTB_CODED_MAX);
    uint8_t type2[TYPE2_MAX] = {0};
    uint8_t mother[TYPE2_MAX * 4] = {0};
    uint8_t type3[TTB_CODED_MAX];
    uint8_t type4[TTB_CODED_MAX];
    DSD_MEMCPY(type2, type1, (size_t)ntype1);
    const uint16_t fcs = ttb_crc16(type1, ntype1);
    for (int i = 0; i < 16; i++) {
        type2[ntype1 + i] = (uint8_t)((fcs >> (15 - i)) & 1u);
    }
    assert(tetra_crc16_ccitt_bits(type2, ntype1 + 16) == TETRA_CRC16_OK);
    /* G1 = 1 + D + D^4, G2 = 1 + D^2 + D^3 + D^4, G3 = 1 + D + D^2 + D^4, G4 = 1 + D + D^3 + D^4. */
    uint8_t d1 = 0;
    uint8_t d2 = 0;
    uint8_t d3 = 0;
    uint8_t d4 = 0;
    for (size_t i = 0; i < (size_t)ntype2; i++) {
        const uint8_t b = type2[i];
        mother[(i * 4U) + 0U] = (uint8_t)(b ^ d1 ^ d4);
        mother[(i * 4U) + 1U] = (uint8_t)(b ^ d2 ^ d3 ^ d4);
        mother[(i * 4U) + 2U] = (uint8_t)(b ^ d1 ^ d2 ^ d4);
        mother[(i * 4U) + 3U] = (uint8_t)(b ^ d1 ^ d3 ^ d4);
        d4 = d3;
        d3 = d2;
        d2 = d1;
        d1 = b;
    }
    const int punctured = tetra_rcpc_puncture_by_id(TETRA_RCPC_PUNCT_2_3, mother, ntype2 * 4, type3, ncoded);
    assert(punctured == ncoded);
    tetra_block_interleave(type3, type4, ncoded, a);
    ttb_scramble_to_dibits(type4, ncoded, seed, dibits);
}

/* An SCH/HD or BNCH block (124 type-1 bits, tetra_test_pdu.h TTB_SCH_HD_BITS) scrambled with @p seed: 108 dibits. */
static inline void
ttb_encode_sch_hd(const uint8_t* type1, uint32_t seed, uint8_t* dibits) {
    ttb_encode_signalling(type1, TTB_SCH_HD_BITS, 216, 101, seed, dibits);
}

/* A BSCH, or a direct mode SCH/S, of 60 type-1 bits (ttb_sync_pdu()): 60 dibits, scrambled with the fixed seed. */
static inline void
ttb_encode_bsch(const uint8_t* type1, uint8_t* dibits) {
    ttb_encode_signalling(type1, TTB_BSCH_BITS, 120, 11, TTB_BSCH_SEED, dibits);
}

/* The AACH carrying ACCESS-ASSIGN @p pdu: its (30,14) Reed-Muller codeword with the bits of @p error flipped, scrambled
 * with @p seed; 15 dibits, half 1's 7 then half 2's 8. */
static inline void
ttb_encode_aach(uint16_t pdu, uint32_t error, uint32_t seed, uint8_t* dibits) {
    const uint32_t word = tetra_rm3014_encode(pdu) ^ error;
    uint8_t bits[30];
    for (int i = 0; i < 30; i++) {
        bits[i] = (uint8_t)((word >> (29 - i)) & 1u);
    }
    ttb_scramble_to_dibits(bits, 30, seed, dibits);
}

/* The seven CRC bits of full-slot speech class 2 (EN 300 395-2 clause 5.5.1, equations 43 to 45): F(X) = X^7 I(X)
 * mod G(X) with G(X) = 1 + X^3 + X^7 and I(X) = C(1) + C(2) X + ... + C(60) X^59, f(i) the coefficient of X^i, sent
 * as b(i + 1). */
static inline void
ttb_speech_full_crc(const uint8_t* class2, uint8_t* crc) {
    uint8_t rem[60 + 7] = {0}; /* X^7 I(X): coefficient k of X^k */
    for (int k = 0; k < 60; k++) {
        rem[k + 7] = (uint8_t)(class2[k] & 1u);
    }
    for (int k = 60 + 7 - 1; k >= 7; k--) {
        if (rem[k]) { /* subtract X^(k-7) G(X) */
            rem[k] ^= 1u;
            rem[k - 7 + 3] ^= 1u;
            rem[k - 7] ^= 1u;
        }
    }
    DSD_MEMCPY(crc, rem, 7);
}

/* Full-slot speech (EN 300 395-2 clause 5.5): @p speech's 274 type-1 bits (TTB_TCH_FS_BITS, table 5 order), the class-2
 * CRC and parity bit and four tail bits, class 1 coded at rate 8/12 and the rest at 8/18 from one rate-1/3 trellis,
 * class 0 sent uncoded before them, matrix interleaved and scrambled with @p seed: the 216 dibits of an NTS1 burst's
 * two blocks. */
static inline void
ttb_encode_tch_fs(const uint8_t* speech, uint32_t seed, uint8_t* dibits) {
    enum { CLASS0 = 102, CLASS1 = 112, CLASS2 = 60, CODED_IN = CLASS1 + CLASS2 + 8 + 4, C1_CODED = 168 };

    uint8_t in[CODED_IN] = {0};
    DSD_MEMCPY(in, speech + CLASS0, CLASS1 + CLASS2);
    uint8_t* const parity = in + CLASS1 + CLASS2;
    ttb_speech_full_crc(speech + CLASS0 + CLASS1, parity);
    uint8_t sum = 0;
    for (int i = 0; i < CLASS2; i++) {
        sum ^= (uint8_t)(speech[CLASS0 + CLASS1 + i] & 1u);
    }
    for (int i = 0; i < 7; i++) {
        sum ^= parity[i];
    }
    parity[7] = sum; /* b8: class 2 and the CRC bits sum to 0 with it */
    /* Clause 5.4.3.1: G1 = 1 + D + D^2 + D^3 + D^4, G2 = 1 + D + D^3 + D^4, G3 = 1 + D^2 + D^4. */
    uint8_t mother[CODED_IN * 3];
    uint8_t d1 = 0;
    uint8_t d2 = 0;
    uint8_t d3 = 0;
    uint8_t d4 = 0;
    for (size_t i = 0; i < (size_t)CODED_IN; i++) {
        const uint8_t b = (uint8_t)(in[i] & 1u);
        mother[(i * 3U) + 0U] = (uint8_t)(b ^ d1 ^ d2 ^ d3 ^ d4);
        mother[(i * 3U) + 1U] = (uint8_t)(b ^ d1 ^ d3 ^ d4);
        mother[(i * 3U) + 2U] = (uint8_t)(b ^ d2 ^ d4);
        d4 = d3;
        d3 = d2;
        d2 = d1;
        d1 = b;
    }
    uint8_t type3[TTB_CODED_MAX];
    for (int i = 0; i < CLASS0; i++) {
        type3[i] = (uint8_t)(speech[i] & 1u);
    }
    int n = tetra_rcpc_puncture_by_id(TETRA_RCPC_PUNCT_SPEECH_8_12, mother, CLASS1 * 3, type3 + CLASS0, C1_CODED);
    assert(n == C1_CODED);
    n = tetra_rcpc_puncture_by_id(TETRA_RCPC_PUNCT_SPEECH_8_18, mother + (CLASS1 * 3), (CODED_IN - CLASS1) * 3,
                                  type3 + CLASS0 + C1_CODED, TTB_CODED_MAX - CLASS0 - C1_CODED);
    assert(n == TTB_CODED_MAX - CLASS0 - C1_CODED);
    /* Clause 5.5.3: the 432 bits written into a matrix of 24 lines and 18 columns line by line, read column by
     * column: C4(i * 24 + j) = C3(j * 18 + i). */
    uint8_t type4[TTB_CODED_MAX];
    for (int i = 0; i < 18; i++) {
        for (int j = 0; j < 24; j++) {
            type4[(i * 24) + j] = type3[(j * 18) + i];
        }
    }
    ttb_scramble_to_dibits(type4, TTB_CODED_MAX, seed, dibits);
}

#endif /* DSD_NEO_TESTS_PROTOCOL_TETRA_TETRA_TEST_BURST_H_ */
