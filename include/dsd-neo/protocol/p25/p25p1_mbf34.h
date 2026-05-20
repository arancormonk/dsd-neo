// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file
 * @brief Matched-bit filter coefficients for P25 Phase 1 (MBF3/4).
 */
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_PROTOCOL_P25_P25P1_MBF34_H
#define DSD_NEO_PROTOCOL_P25_P25P1_MBF34_H

#include <stdint.h>

#define P25_MBF34_MAX_CANDIDATES 8

typedef struct {
    uint8_t bytes[18];
    uint32_t metric;
} p25_mbf34_candidate_t;

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Decode a P25 Phase 1 Confirmed Data MBF (3/4) block.
 *
 * @param dibits 98 input dibits (already deinterleaved at the symbol boundary).
 * @param out [out] Decoded 18-byte payload.
 * @return 0 on success; negative on failure.
 */
int p25_mbf34_decode(const uint8_t dibits[98], uint8_t out[18]);
int p25_mbf34_decode_soft(const uint8_t dibits[98], const int16_t bit_llr[196], uint8_t out[18]);
int p25_mbf34_decode_soft_list(const uint8_t dibits[98], const int16_t bit_llr[196], p25_mbf34_candidate_t* candidates,
                               int max_candidates);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_PROTOCOL_P25_P25P1_MBF34_H */
