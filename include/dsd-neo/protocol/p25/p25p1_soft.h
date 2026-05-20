// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Soft-decision FEC decoders for P25 Phase 1 non-vocoder paths.
 *
 * These routines use per-bit reliability values (0-255) to improve decode
 * success at low SNR by implementing Chase-style soft decoding for Hamming
 * and Golay codes.
 */
#ifndef P25P1_SOFT_H_a3b5c7d9e1f24680
#define P25P1_SOFT_H_a3b5c7d9e1f24680

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Soft-decision Hamming(10,6,3) decoder using Chase-II style algorithm.
 *
 * @param bits      Input: 10 bits as char array [0..9], where [0..5]=data, [6..9]=parity.
 * @param reliab    Input: 10 reliability values [0..255] corresponding to bits.
 * @param out_bits  Output: corrected 10 bits (may be same pointer as bits).
 * @return 0=no error, 1=corrected, 2=uncorrectable.
 *
 * Algorithm:
 * 1. Try hard decode first. If syndrome=0, return success.
 * 2. If hard decode corrects (1 bit), return success.
 * 3. If hard decode fails (2+ errors detected):
 *    a. Find up to 3 candidate bit positions, preferring symbols below the
 *       configured erasure threshold and then the next-lowest reliabilities.
 *    b. Generate all 2^3=8 candidates by flipping combinations of these bits.
 *    c. For each candidate, compute syndrome. If valid (syndrome=0), compute penalty.
 *    d. Penalty = sum of reliab[i] for each bit that differs from original.
 *    e. Pick candidate with lowest penalty. Ties: prefer fewer flips.
 * 4. If no valid candidate found, return uncorrectable.
 */
int hamming_10_6_3_soft(const char* bits, const int* reliab, char* out_bits);

/**
 * Soft-decision Golay(24,6) decoder using small-list Chase algorithm.
 *
 * @param data      Input/Output: 6 data bits as char array.
 * @param parity    Input: 12 parity bits as char array.
 * @param reliab    Input: 18 reliability values [0..255], indices 0-5=data, 6-17=parity.
 * @param fixed     Output: number of bits corrected.
 * @return 0=success, 1=uncorrectable.
 *
 * Algorithm:
 * 1. Try hard decode. If success (<=3 errors), return.
 * 2. Prefer indices below the configured erasure threshold, then the next-lowest reliabilities.
 * 3. Take up to 5 candidate positions.
 * 4. Generate candidates: flip weight-1, weight-2, weight-3 combinations.
 *    Total candidates: C(5,1) + C(5,2) + C(5,3) = 5 + 10 + 10 = 25, plus original = 26 max.
 * 5. For each candidate, run hard decode. If valid:
 *    a. Compute penalty = sum of reliab[i] for differing bits.
 *    b. Track minimum penalty candidate.
 * 6. Return best candidate, or uncorrectable if none valid.
 */
int check_and_fix_golay_24_6_soft(char* data, char* parity, const int* reliab, int* fixed);

/**
 * Soft-decision Golay(24,12) decoder using small-list Chase algorithm.
 *
 * Same approach as Golay(24,6) but:
 * - 12 data bits + 12 parity bits
 * - Consider 6 least reliable bits
 * - Generate weight-1..4 flips: C(6,1)+C(6,2)+C(6,3)+C(6,4) = 6+15+20+15 = 56 candidates
 *
 * @param data      Input/Output: 12 data bits as char array.
 * @param parity    Input: 12 parity bits as char array.
 * @param reliab    Input: 24 reliability values [0..255], indices 0-11=data, 12-23=parity.
 * @param fixed     Output: number of bits corrected.
 * @return 0=success, 1=uncorrectable.
 */
int check_and_fix_golay_24_12_soft(char* data, char* parity, const int* reliab, int* fixed);

/**
 * Compute a symbol reliability from contiguous per-bit LLR values.
 *
 * @param llr       Signed bit LLR values. Positive values favor bit 1.
 * @param bit_count Number of bits in the symbol.
 * @return Minimum absolute bit reliability [0..255], or 0 for invalid input.
 */
uint8_t p25p1_llr_reliability(const int16_t* llr, int bit_count);

/**
 * Build a Reed-Solomon erasure list from P25P1 data/parity symbol reliabilities.
 *
 * Reed-Solomon wrappers use parity-first codeword positions, so returned erasures
 * are 0..parity_symbols-1 for parity and parity_symbols..parity_symbols+data_symbols-1 for data.
 *
 * @return Number of erasures written.
 */
int p25p1_build_rs_erasures(const uint8_t* data_reliab, int data_symbols, const uint8_t* parity_reliab,
                            int parity_symbols, int* erasures, int max_erasures);

/**
 * Get the P25P1 soft-decision erasure threshold.
 *
 * @return Threshold value 0-255. Symbols with reliability below this are marked as erasures.
 */
int p25p1_get_erasure_threshold(void);

#ifdef __cplusplus
}
#endif

#endif /* P25P1_SOFT_H_a3b5c7d9e1f24680 */
