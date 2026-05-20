// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief P25 Phase 1 1/2-rate trellis decoder interfaces.
 *
 * Declares the lightweight 1/2-rate trellis decoders implemented in
 * `src/protocol/p25/p25_12.c`.
 */

#pragma once

#include <stdint.h>

#define P25_12_MAX_CANDIDATES 8

typedef struct {
    uint8_t bytes[12];
    uint32_t metric;
} p25_12_candidate_t;

#ifdef __cplusplus
extern "C" {
#endif

int p25_12(uint8_t* input, uint8_t treturn[12]);
int p25_12_soft(uint8_t* input, const uint8_t* reliab98, uint8_t treturn[12]);
int p25_12_soft_llr(const uint8_t* input, const int16_t* bit_llr196, uint8_t treturn[12]);
int p25_12_soft_llr_list(const uint8_t* input, const int16_t* bit_llr196, p25_12_candidate_t* candidates,
                         int max_candidates);

#ifdef __cplusplus
}
#endif
