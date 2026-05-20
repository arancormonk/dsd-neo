// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * P25P2 soft-decision RS erasure marking API.
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Compute reliability for a hexbit from per-bit signed LLR metrics.
 *
 * @param bit_offsets Six p2bit/p2xbit indices (relative to TS start).
 * @param ts_counter  Current timeslot counter (0-3).
 * @param bit_llr     Per-bit signed reliability array (p2llr or p2xllr).
 * @return Minimum absolute reliability of constituent bits [0..255]; 0 on OOB.
 */
uint8_t p25p2_hexbit_llr_reliability(const uint16_t bit_offsets[6], int ts_counter, const int16_t* bit_llr);

/**
 * Return the P25P2 soft-decision erasure threshold.
 *
 * Reliability values below this threshold expand the ranked erasure prefix
 * used by P25P2 soft-decision helpers. Helpers also retain a conservative
 * minimum weakest-symbol prefix so soft recovery remains useful when all
 * symbols are above the threshold.
 */
int p25p2_soft_erasure_threshold(void);

/**
 * Build soft-decision erasure list for FACCH.
 *
 * @param ts_counter  Current timeslot counter (0-3).
 * @param scrambled   1 for p2xllr, 0 for p2llr.
 * @param erasures    Erasure array (in/out). Must have space for n_fixed + max_add.
 * @param n_fixed     Number of fixed erasures already present (typically 18 for FACCH).
 * @param max_add     Maximum dynamic erasures to add (recommend <=10 for FACCH).
 * @return Total erasure count.
 */
int p25p2_facch_soft_erasures(int ts_counter, int scrambled, int* erasures, int n_fixed, int max_add);

/**
 * Build soft-decision erasure list for SACCH.
 *
 * @param ts_counter  Current timeslot counter (0-3).
 * @param scrambled   1 for p2xllr, 0 for p2llr.
 * @param erasures    Erasure array (in/out). Must have space for n_fixed + max_add.
 * @param n_fixed     Number of fixed erasures already present (typically 11 for SACCH).
 * @param max_add     Maximum dynamic erasures to add (recommend <=16 for SACCH).
 * @return Total erasure count.
 */
int p25p2_sacch_soft_erasures(int ts_counter, int scrambled, int* erasures, int n_fixed, int max_add);

/**
 * Build soft-decision ESS erasures from collected contiguous LLR buffers.
 */
int p25p2_ess_soft_erasures_from_llr(const int16_t payload_llr[96], const int16_t parity_llr[168], int* erasures,
                                     int max_payload_add, int max_parity_add);

/**
 * Build a globally ranked ESS erasure list across all 44 RS symbols.
 */
int p25p2_ess_soft_erasures_ranked(const int16_t payload_llr[96], const int16_t parity_llr[168], int* erasures,
                                   int max_add);

#ifdef __cplusplus
}
#endif
