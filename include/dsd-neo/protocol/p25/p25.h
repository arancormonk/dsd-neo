// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief P25 protocol decode entrypoints.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_P25_P25_H_
#define DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_P25_P25_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Dibits each P25 Phase 1 data unit occupies on air: the frame sync, the NID, the payload, the null bits that pad it to
   whole 70-bit segments and the status symbol after each segment (TIA-102.BAAA-A 8.2). */
enum {
    P25P1_HDU_DIBITS = 396,   /* 770 bits and 11 status symbols */
    P25P1_LDU_DIBITS = 864,   /* LDU1 and LDU2: 1680 bits and 24 status symbols */
    P25P1_TDU_DIBITS = 72,    /* 140 bits and 2 status symbols */
    P25P1_TDULC_DIBITS = 216, /* 420 bits and 6 status symbols */
};

/** Dibits a TSDU or a PDU of @p blocks 196-bit trellis blocks (a TSDU's TSBKs; a PDU's header and data blocks) occupies
    on air: the 112 bits of sync and NID and the blocks, padded to the next status symbol (TIA-102.BAAA-A 8.2.4). A TSDU
    of 1, 2 or 3 blocks takes 180, 288 or 360. 0 below one block. */
static inline uint32_t
p25p1_block_unit_dibits(int blocks) {
    if (blocks < 1) {
        return 0U;
    }
    const uint32_t bits = 112U + (196U * (uint32_t)blocks);
    return ((bits + 69U) / 70U) * 36U;
}

void processHDU(dsd_opts* opts, dsd_state* state);
void processLDU1(dsd_opts* opts, dsd_state* state);
void processLDU2(dsd_opts* opts, dsd_state* state);
void processTDU(dsd_opts* opts, dsd_state* state);
void processTDULC(dsd_opts* opts, dsd_state* state);
void processTSBK(dsd_opts* opts, dsd_state* state);
void processMPDU(dsd_opts* opts, dsd_state* state);
void processP2(dsd_opts* opts, dsd_state* state);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_P25_P25_H_ */
