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
 * positions (ESS symbol indices 0..43: ESS_B 0..15, ESS_A 16..43) are
 * consumed. Returns the number of symbols the decode changed (an erasure that
 * held the right value is not counted), or < 0 on failure.
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
 * would correct a shortened symbol fails, and so does one whose codeword (the
 * decoded payload, re-encoded) lies outside 2 x errors + erasures <= 28 of the
 * received word. Returns the number of sent symbols
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
 * 35..53, SACCH 35..56) in the bit layout the decoders take. If the codec
 * cannot be created (its allocation fails at first use), the parity is all
 * zeros; the decoders then fail every word.
 */
void ez_rs28_facch_encode(const int payload[156], int parity[114]);
void ez_rs28_sacch_encode(const int payload[180], int parity[132]);

/** Burst kinds of ez_p25p2_seed_tables, and its dimensions. */
enum {
    EZ_P25P2_SEED_FACCH = 0,
    EZ_P25P2_SEED_SACCH = 1,
    EZ_P25P2_SEED_KINDS = 2,
    EZ_P25P2_SEED_SLOTS = 12,       /* superframe slots T */
    EZ_P25P2_SEED_MAX_BITS = 312,   /* sent codeword bits: FACCH 270, SACCH 312 */
    EZ_P25P2_SEED_MAX_CHECKS = 132, /* check rows: FACCH 114, SACCH 132 */
    EZ_P25P2_SEED_WORDS = 5,        /* 64-bit words of a row over the codeword bits */
};

/**
 * A scrambled P25 Phase 2 FACCH or SACCH as linear equations over GF(2) in its 44-bit scrambling code
 * WACN(20) << 24 | SYSID(12) << 12 | NAC(12) (issue #804).
 *
 * A burst's sent codeword bits (payload bits, then sent parity bits, as the decoders above take them) sit at fixed
 * burst bits, each XORed with the superframe sequence at (20 + 360 * T + burst bit) for superframe slot T. Every check
 * row XORs to 0 over a codeword, and the sequence is linear in the code, so over the received bits r:
 *
 *     parity(check[kind][i] & r) == parity(seed[kind][T][i] & code)
 *
 * for each row i, with no bit error.
 */
typedef struct ez_p25p2_seed_tables {
    int payload_bits[EZ_P25P2_SEED_KINDS]; /* 156, 180 */
    int checks[EZ_P25P2_SEED_KINDS];       /* one row per sent parity bit: 114, 132 */
    /* Codeword bit k -> its burst bit (0..319, from the timeslot's start). */
    uint16_t burst_bit[EZ_P25P2_SEED_KINDS][EZ_P25P2_SEED_MAX_BITS];
    /* Row i over the codeword bits (bit k in word k / 64): parity bit i and the payload bits that set it. */
    uint64_t check[EZ_P25P2_SEED_KINDS][EZ_P25P2_SEED_MAX_CHECKS][EZ_P25P2_SEED_WORDS];
    /* Row i at superframe slot T over the code's bits (bit 0 = the NAC's least significant bit). */
    uint64_t seed[EZ_P25P2_SEED_KINDS][EZ_P25P2_SEED_SLOTS][EZ_P25P2_SEED_MAX_CHECKS];
} ez_p25p2_seed_tables;

/**
 * The tables, built at the first call (thread-safe) and kept; NULL if they could not be built (an allocation failed).
 */
const ez_p25p2_seed_tables* ez_p25p2_seed_tables_get(void);

int isch_lookup(uint64_t isch);
int isch_lookup_soft(uint64_t isch, const uint8_t reliab40[40]);

#ifdef __cplusplus
}
#endif
#endif /* DSD_NEO_INCLUDE_DSD_NEO_FEC_EZ_H_H */
