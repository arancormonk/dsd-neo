// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* The absolute channel parameters of an MBC continuation (dmr_abs_params.h). */

#include "dmr_abs_params.h"

#include <dsd-neo/core/bit_packing.h>
#include <dsd-neo/core/safe_api.h>
#include <stddef.h>
#include <stdint.h>

/* Table C.7: an absolute frequency is 50..999 MHz plus 0..7999 steps of 125 Hz. */
enum { DMR_ABS_MHZ_MIN = 50, DMR_ABS_MHZ_MAX = 999, DMR_ABS_STEP_MAX = 7999 };

static long
dmr_abs_params_hz(uint16_t mhz, uint16_t step) {
    if (mhz < DMR_ABS_MHZ_MIN || mhz > DMR_ABS_MHZ_MAX || step > DMR_ABS_STEP_MAX) {
        return 0L;
    }
    return ((long)mhz * 1000000L) + ((long)step * 125L);
}

int
dmr_abs_params_shape_ok(const dmr_cspdu_shape* shape) {
    return shape != NULL && shape->is_mbc == 1U && shape->continuations == 1U && shape->cont_lb == 1U
           && shape->cont_csbko == shape->header_csbko;
}

int
dmr_abs_params_decode(const uint8_t* cs_pdu_bits, const dmr_cspdu_shape* shape, dmr_abs_params* out) {
    if (out == NULL) {
        return 0;
    }
    DSD_MEMSET(out, 0, sizeof(*out));
    if (cs_pdu_bits == NULL || !dmr_abs_params_shape_ok(shape)) {
        return 0;
    }

    /* The continuation (CSBK bits 96..191): LB, PF, CSBKO, 4 reserved, colour code (BC_AP: reserved), Cdeftype, 2
       reserved, then CdefParms: CHAN 12, TXMHz 10, TXKHz 13, RXMHz 10, RXKHz 13. */
    out->colour_code = (uint8_t)convert_bits_into_output(&cs_pdu_bits[108], 4);
    out->cdeftype = (uint8_t)convert_bits_into_output(&cs_pdu_bits[112], 4);
    out->chan = (uint16_t)convert_bits_into_output(&cs_pdu_bits[118], 12);
    if (out->cdeftype != 0U) {
        return 0;
    }
    const uint16_t tx_mhz = (uint16_t)convert_bits_into_output(&cs_pdu_bits[130], 10);
    const uint16_t tx_step = (uint16_t)convert_bits_into_output(&cs_pdu_bits[140], 13);
    const uint16_t rx_mhz = (uint16_t)convert_bits_into_output(&cs_pdu_bits[153], 10);
    const uint16_t rx_step = (uint16_t)convert_bits_into_output(&cs_pdu_bits[163], 13);
    out->rx_hz = dmr_abs_params_hz(rx_mhz, rx_step);
    if (out->rx_hz == 0L) {
        return 0;
    }
    out->tx_hz = dmr_abs_params_hz(tx_mhz, tx_step);
    return 1;
}
