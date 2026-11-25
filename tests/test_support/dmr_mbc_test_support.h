// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Hands dmr_cspdu_ex() a PDU as the block assembler hands over a two-block MBC, for the tests that build absolute
   channel parameters (CG_AP, MV_AP, BC_AP) without the burst path. The test needs the DMR module's private include
   directory, for dmr_abs_params.h. */
#ifndef DSD_NEO_TEST_DMR_MBC_TEST_SUPPORT_H
#define DSD_NEO_TEST_DMR_MBC_TEST_SUPPORT_H

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <stdint.h>
#include "dmr_abs_params.h"

/* The PDU in @p bits / @p bytes as the assembler hands over a two-block MBC: header LB 0, one continuation carrying
   LB 1 and the header's CSBKO (TS 102 361-4 CG_AP/MV_AP/BC_AP), CRC checked. */
static inline void
dsd_test_dmr_cspdu_mbc(dsd_opts* opts, dsd_state* state, uint8_t* bits, uint8_t* bytes) {
    const uint8_t csbko = (uint8_t)(bytes[0] & 0x3FU);
    bytes[0] = csbko;
    bytes[12] = (uint8_t)(0x80U | csbko);
    bits[0] = 0U;
    bits[96] = 1U;
    for (int i = 0; i < 6; i++) {
        bits[98 + i] = (uint8_t)((csbko >> (5 - i)) & 1U);
    }
    const dmr_cspdu_shape shape = {1U, 1U, csbko, csbko, 1U, 0U};
    dmr_cspdu_ex(opts, state, bits, bytes, 1U, 0U, &shape);
}

/* The PDU in @p bits / @p bytes as the block assembler hands over an MBC with @p continuations continuation blocks after
   its header (LB 0), CRC checked. The first continuation's CSBKO stays what the test wrote at bits 98..103 (a block of
   another message keeps its own); with one continuation it is the last block (LB 1), with more the first one is not. */
static inline void
dsd_test_dmr_cspdu_mbc_blocks(dsd_opts* opts, dsd_state* state, uint8_t* bits, uint8_t* bytes, uint8_t continuations) {
    const uint8_t csbko = (uint8_t)(bytes[0] & 0x3FU);
    uint8_t cont_csbko = 0U;
    for (int i = 0; i < 6; i++) {
        cont_csbko = (uint8_t)((cont_csbko << 1U) | (bits[98 + i] & 1U));
    }
    const uint8_t cont_lb = (uint8_t)(continuations == 1U ? 1U : 0U);
    bytes[0] = csbko;
    bytes[12] = (uint8_t)((cont_lb << 7U) | cont_csbko);
    bits[0] = 0U;
    bits[96] = cont_lb;
    const dmr_cspdu_shape shape = {1U, continuations, csbko, cont_csbko, cont_lb, 0U};
    dmr_cspdu_ex(opts, state, bits, bytes, 1U, 0U, &shape);
}

#endif
