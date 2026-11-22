// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The shape of a control signalling PDU (a single CSBK or an assembled MBC) and the one decoder of the absolute
 * channel parameters an MBC's continuation carries (TS 102 361-4 CG_AP Table 7.16, MV_AP Table 7.18, BC_AP Table 7.21;
 * CdefParms Table 7.103; Annex C Table C.7). Protocol-internal; decoder thread only.
 */

#ifndef DSD_NEO_SRC_PROTOCOL_DMR_DMR_ABS_PARAMS_H_
#define DSD_NEO_SRC_PROTOCOL_DMR_DMR_ABS_PARAMS_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* What the PDU being handled was assembled from. A single CSBK has is_mbc 0 and no continuation. An MBC has its
   header's CSBKO, the number of continuation blocks received after the header (1..3 are well formed, TS 102 361-1
   §7.4.0), and the first continuation's CSBKO and last-block bit (CSBK bits 98..103 and 96). proves_content is 1 when
   the bursts it came from prove content to a scan quiet check (a CSBK or MBC header by its own check, or a RAS
   signature -F accepts; an MBC also accepted as a whole): dmr_cspdu_ex() notes it only for a PDU it does not refuse. */
typedef struct {
    uint8_t is_mbc;
    uint8_t continuations;
    uint8_t header_csbko;
    uint8_t cont_csbko;
    uint8_t cont_lb;
    uint8_t proves_content;
} dmr_cspdu_shape;

/* Absolute channel parameters (Cdeftype 0): the LPCN they define (CHAN, which may be 0 or 4095: a frequency that names
   no channel), the receive and transmit frequencies in Hz (TX 0 when outside Table C.7), the destination colour code
   (CG_AP and MV_AP; reserved bits in BC_AP) and the Cdeftype. */
typedef struct {
    uint16_t chan;
    long rx_hz;
    long tx_hz;
    uint8_t colour_code;
    uint8_t cdeftype;
} dmr_abs_params;

/* 1 when @p shape is an MBC that can carry absolute parameters: a header followed by exactly one continuation, that
   continuation the last block (LB 1) and carrying its header's CSBKO. */
int dmr_abs_params_shape_ok(const dmr_cspdu_shape* shape);

/* Decode the absolute parameters of the PDU (@p cs_pdu_bits, its MBC continuation at bits 96..191). Returns 1 when
   they are usable: dmr_abs_params_shape_ok(), Cdeftype 0 and the receive frequency inside Table C.7 (50..999 MHz,
   0..7999 steps of 125 Hz). Otherwise returns 0 with rx_hz and tx_hz 0; @p out still holds the Cdeftype, CHAN and
   colour code read when the shape was right (all zero when it was not), for printing. */
int dmr_abs_params_decode(const uint8_t* cs_pdu_bits, const dmr_cspdu_shape* shape, dmr_abs_params* out);

/* dmr_cspdu() for a PDU of @p shape (NULL: a single CSBK). dmr_cspdu() is this with the single-CSBK shape. */
void dmr_cspdu_ex(dsd_opts* opts, dsd_state* state, uint8_t cs_pdu_bits[], uint8_t cs_pdu[], uint32_t crc_correct,
                  uint32_t irrecoverable, const dmr_cspdu_shape* shape);

/* The shape of the PDU dmr_cspdu_ex() is dispatching, for its handlers; the single-CSBK shape outside a dispatch. */
const dmr_cspdu_shape* dmr_cspdu_current_shape(void);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_SRC_PROTOCOL_DMR_DMR_ABS_PARAMS_H_ */
