// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef DSD_NEO_PROTOCOL_TETRA_FEC_H
#define DSD_NEO_PROTOCOL_TETRA_FEC_H

#include <stdint.h>

/* RCPC puncturer id constants (match internal puncturer table in tetra_fec.c) */
#define TETRA_RCPC_PUNCT_2_3      0
#define TETRA_RCPC_PUNCT_1_3      1
#define TETRA_RCPC_PUNCT_292_432  2
#define TETRA_RCPC_PUNCT_148_432  3
#define TETRA_RCPC_PUNCT_112_168  4
#define TETRA_RCPC_PUNCT_72_162   5
#define TETRA_RCPC_PUNCT_38_80    6

// Placeholder for TETRA FEC, deinterleave, and descramble routines
void tetra_block_deinterleave(uint8_t* in, uint8_t* out, int len, int a);
void tetra_descramble(uint8_t* in, int len, uint32_t lfsr_init);
/* Descramble soft-costs: inverts soft cost polarity where scrambler bit == 1
 * `costs` length is number of bits; `lfsr_init` seeds the PN generator.
 */
void tetra_descramble_soft(uint16_t* costs, int len, uint32_t lfsr_init);

/* Convert unpacked hard bits to the soft-cost convention used by the Viterbi
 * backend: zero is a confident 0 and 0xffff is a confident 1. */
void tetra_hard_bits_to_soft(const uint8_t* bits, uint16_t* costs, int len);
uint16_t tetra_crc16_ccitt_bits(const uint8_t* bits, int len);

/* Compute TETRA scrambling seed from network identity (ETSI EN 300 392-2 §8.2.5.2).
 * mcc: 10-bit Mobile Country Code, mnc: 14-bit Mobile Network Code,
 * colour: 6-bit full colour code (from BSCH).
 * Returns seed=3 when all inputs are zero (BSCH / all-zero network identity).
 */
uint32_t tetra_compute_scramb_seed(uint16_t mcc, uint16_t mnc, uint8_t colour);
void tetra_viterbi_decode(uint8_t* in, uint8_t* out, int len);
/* Soft-cost depuncture + Viterbi helper: depunc array length is depunc_len (coded symbols),
 * out_bits receives unpacked bits (caller must allocate sufficient space).
 */
int tetra_viterbi_decode_soft(const uint16_t* depunc, int depunc_len, uint8_t* out_bits, int out_bits_len);

/* RCPC depuncture helper: expand soft costs according to a puncture pattern.
 * - `punct` is an array of 0/1 values repeating over the coded stream.
 * - `p_len` is its length.
 * - `in_costs` contains soft costs for the information bits (len = info_bits_len).
 * - `out_costs` must be preallocated for `out_len` coded-symbols.
 * Returns number of coded symbols written, or -1 on error.
 */
int tetra_rcpc_depuncture_soft(const uint16_t* in_costs, int info_bits_len, const uint8_t* punct, int p_len,
							   uint16_t* out_costs, int out_len);

/* Depuncture by known puncturer id (matches internal osmo-tetra puncturer enums).
 * Returns number of coded symbols written or -1 on error.
 */
int tetra_rcpc_depuncture_by_id(int punct_id, const uint16_t* in_costs, int in_len, uint16_t* out_costs,
								int out_len);

/* Forward RCPC puncturing, primarily used by conformance vectors and synthetic
 * off-air fixtures. Returns the number of output bits written or -1. */
int tetra_rcpc_puncture_by_id(int punct_id, const uint8_t* mother, int mother_len, uint8_t* out, int out_len);

/* Accessor: retrieve puncturer parameters for tests/validation.
 * Returns 0 on success, -1 on invalid punct_id.
 * `outP` points to the internal P array (callers must not modify), `out_t` is t,
 * and `out_period` is the period value.
 */
int tetra_rcpc_get_puncturer_params(int punct_id, const uint8_t **outP, int *out_t, int *out_period);

/* Map a one-based transmitted-bit index to its one-based mother-code index.
 * Exposed for conformance tests and diagnostic tools. Returns -1 for an
 * invalid puncturer or index.
 */
int tetra_rcpc_map_j_to_k(int punct_id, uint32_t j);



/* Block interleaver/deinterleaver helpers. These implement a generic rectangular
 * interleaver; real TETRA interleaver parameters should be applied by caller.
 */
void tetra_block_interleave(const uint8_t* in_bits, uint8_t* out_bits, int len, int rows, int cols);
void tetra_block_deinterleave_bits(const uint8_t* in_bits, uint8_t* out_bits, int len, int rows, int cols);
void tetra_block_deinterleave_soft(const uint16_t* in_costs, uint16_t* out_costs, int len, int rows, int cols);

/* TCH/FS speech deinterleaver from EN 300 395-2: transpose the received
 * 18-column by 24-line representation into the 432-bit channel-code order. */
void tetra_speech_deinterleave_soft(const uint16_t* in_costs, uint16_t* out_costs);

/* Decode one 432-bit TCH/FS channel frame into the 274 sensitivity-ordered
 * ACELP bits for two 30 ms speech frames. Returns 274 or -1. */
int tetra_speech_channel_decode(const uint16_t* deinterleaved, uint8_t* out_bits, int out_len);

/* Helper: select interleaver dimensions for a given puncturer/frame length.
 * Returns 0 on success, -1 on unknown mapping.
 */
int tetra_get_interleaver_dims(int punct_id, int bits_len, int *out_rows, int *out_cols);

#endif // DSD_NEO_PROTOCOL_TETRA_FEC_H
