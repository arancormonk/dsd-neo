// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file
 * @brief TETRA channel coding: the signalling channels of EN 300 392-2 clause 8 and the speech channel of
 * EN 300 395-2 clauses 5 and 6.
 *
 * Bits are one per byte (0 or 1). Soft costs are 16-bit: 0x0000 is a confident 0, 0xFFFF a confident 1 and 0x7FFF
 * no information (a punctured position).
 */
#ifndef DSD_NEO_PROTOCOL_TETRA_FEC_H
#define DSD_NEO_PROTOCOL_TETRA_FEC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* RCPC puncturing schemes. The first four puncture the rate-1/4 mother code of EN 300 392-2 clause 8.2.3.1; the last
 * three the rate-1/3 mother code of the speech channel, EN 300 395-2 clauses 5.5.2 and 5.6.2. */
#define TETRA_RCPC_PUNCT_2_3         0 /* EN 300 392-2 clause 8.2.3.1.3 */
#define TETRA_RCPC_PUNCT_1_3         1 /* EN 300 392-2 clause 8.2.3.1.4 */
#define TETRA_RCPC_PUNCT_292_432     2 /* EN 300 392-2 clause 8.2.3.1.5 */
#define TETRA_RCPC_PUNCT_148_432     3 /* EN 300 392-2 clause 8.2.3.1.6 */
#define TETRA_RCPC_PUNCT_SPEECH_8_12 4 /* speech class 1, EN 300 395-2 clause 5.5.2.1 */
#define TETRA_RCPC_PUNCT_SPEECH_8_18 5 /* full-slot speech class 2, EN 300 395-2 clause 5.5.2.2 */
#define TETRA_RCPC_PUNCT_SPEECH_8_17 6 /* half-slot speech class 2, EN 300 395-2 clause 5.6.2.1 */

/** Neutral soft cost: a punctured or unknown bit. */
#define TETRA_SOFT_NEUTRAL           ((uint16_t)0x7FFFu)

/** Remainder tetra_crc16_ccitt_bits() leaves over a block whose (K1 + 16, K1) check passes (EN 300 392-2 8.2.3.3). */
#define TETRA_CRC16_OK               0x1D0Fu

/** Descramble soft costs with the PN sequence of EN 300 392-2 clause 8.2.5 seeded with @p lfsr_init (0: the BSCH
 *  seed 3). A 1 in the sequence inverts the cost; a neutral cost stays neutral. */
void tetra_descramble_soft(uint16_t* costs, int len, uint32_t lfsr_init);

/** Hard bits as confident soft costs. */
void tetra_hard_bits_to_soft(const uint8_t* bits, uint16_t* costs, int len);

/** CRC-CCITT register over @p len bits, initialised to all ones; TETRA_CRC16_OK over data followed by its 16 check
 *  bits means the check passed. */
uint16_t tetra_crc16_ccitt_bits(const uint8_t* bits, int len);

/** Scrambling seed for a network identity (EN 300 392-2 clause 8.2.5.2): MCC (10 bits), MNC (14 bits) and colour
 *  code (6 bits). All three zero give the BSCH seed, 3. */
uint32_t tetra_compute_scramb_seed(uint16_t mcc, uint16_t mnc, uint8_t colour);

/** Viterbi decoder for the 16-state rate-1/4 mother code of EN 300 392-2 clause 8.2.3.1.1. @p depunc holds
 *  @p depunc_len soft costs (four per type-2 bit, at most 512 bits), @p out_bits receives depunc_len / 4 bits.
 *  Returns that count, or -1. */
int tetra_viterbi_decode_soft(const uint16_t* depunc, int depunc_len, uint8_t* out_bits, int out_bits_len);

/** Restore @p out_len mother-code soft costs from the @p in_len type-3 costs puncturing scheme @p punct_id kept;
 *  the rest are neutral. Returns out_len, or -1. */
int tetra_rcpc_depuncture_by_id(int punct_id, const uint16_t* in_costs, int in_len, uint16_t* out_costs, int out_len);

/** Puncture @p mother_len mother-code bits into @p out_len type-3 bits; a type-3 bit whose mother bit lies beyond
 *  @p mother_len is 0xFF. Returns out_len, or -1. */
int tetra_rcpc_puncture_by_id(int punct_id, const uint8_t* mother, int mother_len, uint8_t* out, int out_len);

/** The one-based mother-code position k that type-3 bit @p j (one-based) of scheme @p punct_id carries, or -1. */
int tetra_rcpc_map_j_to_k(int punct_id, uint32_t j);

/** Block interleaving of EN 300 392-2 clause 8.2.4.1 over @p len bits with parameter @p a: bit i goes to position
 *  1 + (a * i mod len). */
void tetra_block_interleave(const uint8_t* in_bits, uint8_t* out_bits, int len, int a);

/** The inverse of tetra_block_interleave() on soft costs. */
void tetra_block_deinterleave_soft(const uint16_t* in_costs, uint16_t* out_costs, int len, int a);

/** Speech matrix deinterleaving of EN 300 395-2 clause 5.5.3: 432 type-4 costs to 432 type-3 costs. */
void tetra_speech_deinterleave_soft(const uint16_t* in_costs, uint16_t* out_costs);

/** Type-1 speech bits in one full-slot burst: two 137-bit frames (EN 300 395-2 clause 5.5). */
#define TETRA_SPEECH_FULL_BITS 274
/** Type-1 speech bits in one half-slot burst: one frame (EN 300 395-2 clause 5.6). */
#define TETRA_SPEECH_HALF_BITS 137

/**
 * Full-slot speech channel decoder (EN 300 395-2 clauses 5.5 and 6). @p deinterleaved is the 432 type-3 soft costs
 * from tetra_speech_deinterleave_soft(); @p out_bits receives the 274 type-1 bits in table 5 order: 102 class 0,
 * 112 class 1, 60 class 2. @p crc_ok is set to 1 when the seven CRC bits and the overall parity bit match the
 * class-2 bits (clause 5.5.1), else 0. @p mismatches is set to the number of the 330 coded class 1 and 2 bits that
 * disagree with a re-encode of the decoded bits. Either output may be NULL. Returns 274, or -1.
 */
int tetra_speech_full_decode(const uint16_t* deinterleaved, uint8_t* out_bits, int* crc_ok, int* mismatches);

/**
 * Half-slot speech channel decoder (EN 300 395-2 clauses 5.6 and 6). @p deinterleaved is the 216 type-3 soft costs
 * after the (216, 101) block deinterleaver; @p out_bits receives the 137 type-1 bits in table 6 order: 51 class 0,
 * 56 class 1, 30 class 2. @p crc_ok is set to 1 when the four CRC bits match (clause 5.6.1), else 0. @p mismatches
 * counts the 165 coded class 1 and 2 bits that disagree with a re-encode. Returns 137, or -1.
 */
int tetra_speech_half_decode(const uint16_t* deinterleaved, uint8_t* out_bits, int* crc_ok, int* mismatches);

/** SCH/HU (EN 300 392-2 clause 8.3.1.4.3): 168 descrambled type-4 soft costs to the 92 MAC bits in @p out_bits.
 *  Returns 1 when the CRC passes. */
int tetra_sch_hu_decode(const uint16_t* type4, uint8_t* out_bits);

/** The shortened (30,14) Reed-Muller code of the AACH (EN 300 392-2 clause 8.2.3.2): the 14 type-1 bits of @p info,
 *  b1(1) the most significant, to 30 type-2 bits, b2(1) bit 29 of the result. The code is systematic: b2(1) to b2(14)
 *  are the type-1 bits. */
uint32_t tetra_rm3014_encode(uint16_t info);

/** Decode 30 received type-2 bits laid out as tetra_rm3014_encode() returns them. With a codeword within 3 bits (the
 *  code's minimum distance is 8, so there is at most one), stores its 14 type-1 bits in @p info (may be NULL) and
 *  returns the number of bits corrected, 0 to 3; otherwise, or with a bit above bit 29 set, returns -1 and leaves
 *  @p info alone. */
int tetra_rm3014_decode(uint32_t received, uint16_t* info);

#ifdef __cplusplus
}
#endif

#endif // DSD_NEO_PROTOCOL_TETRA_FEC_H
