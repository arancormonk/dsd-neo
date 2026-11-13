// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The P25 Phase 2 ISCH code: the 128 I-ISCH codewords of the (40,9) code and the S-ISCH (the sync word), and a
 * hard nearest-codeword read.
 *
 * Every word of the set lies at least 14 bits from every other, the S-ISCH included, so a word within 6 bits of one is
 * nearer it than any other. isch_lookup() and isch_lookup_soft() (ez.h) correct up to 7, past that radius.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_FEC_P25P2_ISCH_H_
#define DSD_NEO_INCLUDE_DSD_NEO_FEC_P25P2_ISCH_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** The S-ISCH: the 40-bit Phase 2 sync word, as the ISCH bits read it. */
#define DSD_P25P2_S_ISCH 0x575D57F7FFULL

enum {
    /** I-ISCH codewords: index i carries the channel in bits 5-6, the ISCH location in bits 3-4, a free bit and the
        2-bit UF count. */
    DSD_P25P2_I_ISCH_CODEWORDS = 128,
    /** What dsd_p25p2_isch_nearest() returns for the S-ISCH. */
    DSD_P25P2_ISCH_NEAREST_S_ISCH = DSD_P25P2_I_ISCH_CODEWORDS,
};

/** The I-ISCH codewords, by their 7 information bits. */
extern const uint64_t dsd_p25p2_i_isch_codewords[DSD_P25P2_I_ISCH_CODEWORDS];

/**
 * @brief The codeword nearest the 40 bits @p word (bit 39 first): an I-ISCH's information bits (0-127) or
 * DSD_P25P2_ISCH_NEAREST_S_ISCH, with the bits they differ by in @p out_bits (may be NULL).
 */
int dsd_p25p2_isch_nearest(uint64_t word, int* out_bits);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_FEC_P25P2_ISCH_H_ */
