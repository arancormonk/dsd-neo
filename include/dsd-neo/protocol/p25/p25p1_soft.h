// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Soft-decision FEC decoders for P25 Phase 1 non-vocoder paths.
 *
 * These routines use per-bit reliability values (0-255) and signed LLRs to
 * improve decode success at low SNR with Chase-style Hamming/Golay searches
 * and ranked Reed-Solomon erasures.
 */
#ifndef P25P1_SOFT_H_a3b5c7d9e1f24680
#define P25P1_SOFT_H_a3b5c7d9e1f24680

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int reliab;
    int16_t llr[2];
} P25P1SoftDibit;

/**
 * Soft-decision Hamming(10,6,3) decoder using Chase-II style algorithm.
 *
 * @param bits      Input: 10 bits as char array [0..9], where [0..5]=data, [6..9]=parity.
 * @param reliab    Input: 10 reliability values [0..255] corresponding to bits.
 * @param out_bits  Output: corrected 10 bits (may be same pointer as bits).
 * @return 0=no error, 1=corrected, 2=uncorrectable.
 *
 * The decoder seeds the search from the hard-decision result, then explores
 * low-weight flips over the weakest received bits. The surviving valid codeword
 * with the lowest reliability penalty wins.
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
 * The decoder considers the hard-decision codeword and a bounded low-weight
 * flip list over the weakest received bits. Valid candidates are scored by
 * accumulated reliability penalty.
 */
int check_and_fix_golay_24_6_soft(char* data, const char* parity, const int* reliab, int* fixed);

/**
 * Soft-decision Golay(24,12) decoder using small-list Chase algorithm.
 *
 * Same approach as Golay(24,6), with a larger bounded list for the wider
 * 12-data-bit codeword.
 *
 * @param data      Input/Output: 12 data bits as char array.
 * @param parity    Input: 12 parity bits as char array.
 * @param reliab    Input: 24 reliability values [0..255], indices 0-11=data, 12-23=parity.
 * @param fixed     Output: number of bits corrected.
 * @return 0=success, 1=uncorrectable.
 */
int check_and_fix_golay_24_12_soft(char* data, const char* parity, const int* reliab, int* fixed);

/**
 * Compute a symbol reliability from contiguous per-bit LLR values.
 *
 * @param llr       Signed bit LLR values. Positive values favor bit 1.
 * @param bit_count Number of bits in the symbol.
 * @return Minimum absolute bit reliability [0..255], or 0 for invalid input.
 */
uint8_t p25p1_llr_reliability(const int16_t* llr, int bit_count);

/**
 * Build a weakest-first Reed-Solomon erasure list from P25P1 data/parity reliabilities.
 *
 * The P25 Reed-Solomon decoders use parity-first codeword positions, so returned erasures
 * are 0..parity_symbols-1 for parity and parity_symbols..parity_symbols+data_symbols-1 for data.
 * The list includes every symbol below the configured erasure threshold, then fills to
 * min_erasures from the globally weakest remaining symbols. The caller chooses how many
 * prefixes of the returned list to try.
 *
 * @param min_erasures Minimum number of weakest symbols to return when available.
 * @return Number of ranked erasures written, capped by max_erasures.
 */
int p25p1_build_rs_ranked_erasures(const uint8_t* data_reliab, int data_symbols, const uint8_t* parity_reliab,
                                   int parity_symbols, int min_erasures, int* erasures, int max_erasures);

int p25p1_rs_36_20_17_soft_reliability(char* data, const char* parity, const uint8_t* data_reliab,
                                       const uint8_t* parity_reliab);
int p25p1_rs_24_12_13_soft_reliability(char* data, const char* parity, const uint8_t* data_reliab,
                                       const uint8_t* parity_reliab);
int p25p1_rs_24_16_9_soft_reliability(char* data, const char* parity, const uint8_t* data_reliab,
                                      const uint8_t* parity_reliab);

/**
 * Get the P25P1 soft-decision erasure threshold.
 *
 * @return Threshold value 0-255. Symbols with reliability below this are marked as erasures.
 */
int p25p1_get_erasure_threshold(void);

/**
 * Return whether conservative soft candidates may override hard-corrected P25 FEC output.
 *
 * Defaults to enabled; callers and decoders must still apply strict reliability gates.
 */
int p25_soft_hard_override_enabled(void);

#ifdef __cplusplus
}
#endif

#endif /* P25P1_SOFT_H_a3b5c7d9e1f24680 */
