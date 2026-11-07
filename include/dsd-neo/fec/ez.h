// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief ezpwd-style Reed-Solomon helpers used by P25 Phase 2.
 *
 * Declares the RS(63,35) and ISCH lookup helpers implemented in `src/fec/ez.cpp`
 * so callers can use them directly.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_FEC_EZ_H_H
#define DSD_NEO_INCLUDE_DSD_NEO_FEC_EZ_H_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Decode the P25 Phase 2 ESS, a shortened RS(63,35) code: RS(44,16,29).
 *
 * Pass NULL and zero for a decoder without erasures. At most 28 erasure
 * positions are consumed.
 */
int ez_rs28_ess(int payload[96], int parity[168], const int* erasures, int n_erasures);

/**
 * Decode a P25 Phase 2 FACCH or SACCH, an RS(63,35) code shortened at the
 * front and punctured at the back:
 *
 * - FACCH: RS(45,26,20). Symbols 0..8 are shortened (known zero, never sent),
 *   payload 9..34, sent parity 35..53, punctured parity 54..62.
 * - SACCH: RS(52,30,23). Shortened 0..4, payload 5..34, sent parity 35..56,
 *   punctured parity 57..62.
 *
 * Bits are MSB-first per hexbit. Erasure positions are full RS(63,35)
 * indices; the punctured symbols must be among them. Shortened, out-of-range
 * and repeated positions are ignored, and at most 28 are used. A decode that
 * would correct a shortened symbol fails. Returns the number of sent symbols
 * (payload and parity) corrected, or < 0 on failure; the payload bits are
 * written only on success.
 */
int ez_rs28_facch(int payload[156], int parity[114], const int* erasures, int n_erasures);
int ez_rs28_sacch(int payload[180], int parity[132], const int* erasures, int n_erasures);

/**
 * The same decode with only the punctured symbols erased (FACCH 54..62,
 * SACCH 57..62), and without heap allocation.
 */
int ez_rs28_facch_fixed(int payload[156], int parity[114]);
int ez_rs28_sacch_fixed(int payload[180], int parity[132]);

/**
 * Encode a FACCH or SACCH payload: writes the sent parity (FACCH RS symbols
 * 35..53, SACCH 35..56) in the bit layout the decoders take.
 */
void ez_rs28_facch_encode(const int payload[156], int parity[114]);
void ez_rs28_sacch_encode(const int payload[180], int parity[132]);

int isch_lookup(uint64_t isch);
int isch_lookup_soft(uint64_t isch, const uint8_t reliab40[40]);

#ifdef __cplusplus
}
#endif
#endif /* DSD_NEO_INCLUDE_DSD_NEO_FEC_EZ_H_H */
