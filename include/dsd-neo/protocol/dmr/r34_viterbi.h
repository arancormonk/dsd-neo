// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file
 * @brief Rate 3/4 Viterbi decoder helpers for DMR.
 */
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Normative DMR Rate 3/4 decoder (hard-decision Viterbi), compatible with
// existing dmr_34() packing (18-byte payload from the first 48 tribits).
//
// Input: 98 dibits (values 0..3).
// Output: 18 payload bytes.
// Returns 0 on success; non-zero on internal errors.
int dmr_r34_viterbi_decode(const uint8_t* dibits98, uint8_t out_bytes18[18]);

// Soft-decision variant using per-dibit reliability weights.
// reliab98: 98 entries, 0..255 (higher means more confident) for each dibit.
// Branch metric penalizes mismatches proportionally to reliability.
int dmr_r34_viterbi_decode_soft(const uint8_t* dibits98, const uint8_t* reliab98, uint8_t out_bytes18[18]);

// Simple R3/4 encoder helper for tests:
// Input: 18 payload bytes (packed as in dmr_34())
// Output: 98 dibits (0..3), interleaved per the standard.
int dmr_r34_encode(const uint8_t out_bytes18[18], uint8_t dibits98[98]);

#ifdef __cplusplus
}
#endif
