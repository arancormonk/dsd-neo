// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */
#ifndef DSD_NEO_CRYPTO_NXDN_KEYSTREAM_H
#define DSD_NEO_CRYPTO_NXDN_KEYSTREAM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Generate len_bits unpacked scrambler bits from a 15-bit key (NXDN TS 1-D §5.3.1).
 * The caller supplies room for len_bits bytes. Null output or nonpositive length is ignored.
 */
void nxdn_pdu_scrambler_keystream_creation(uint8_t* ks, int lfsr, int len_bits);

/** Expand a transmitted 64-bit MI into the 128-bit AES IV (NXDN TS 1-D §5.3.3.2).
 * Writes 16 bytes; bytes 8..15 also contain the next encryption session's 64-bit MI.
 * Null output is ignored. Does not advance decoder state.
 */
void nxdn_lfsr128_expand_iv_from_mi64(uint64_t mi, uint8_t out[16]);

#ifdef __cplusplus
}
#endif
#endif
