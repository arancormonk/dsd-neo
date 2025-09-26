// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Temporary API for a normative Rate 3/4 decoder.
// For now this is a thin wrapper over the existing dmr_34() implementation.
// A future patch will replace the internals with a Viterbi-based decoder per ETSI.
//
// Input: 98 dibits (values 0..3 compressed into 0/1 per bit position internally).
// Output: 18 payload bytes.
// Returns 0 on success; non-zero on internal errors.
int dmr_r34_viterbi_decode(const uint8_t* dibits98, uint8_t out_bytes18[18]);

#ifdef __cplusplus
}
#endif
