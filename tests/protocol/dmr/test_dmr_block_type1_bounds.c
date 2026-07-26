// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Regression tests for DMR type-1 block assembly bounds.
 *
 * data_byte_ctr only resets when a PDU completes (data_block_counter reaches
 * data_header_blocks). A burst sequence that resets the block counter without the
 * byte counter - the MBCH path does exactly that - lets the byte counter keep
 * climbing, and the append walked past dmr_pdu_sf[slot] while the CRC pass unpacked
 * the same oversized count into the stack-local dmr_pdu_sf_bits. These tests pin the
 * saturation behaviour.
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/protocol/dmr/dmr.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

static int
expect_eq_u32(const char* tag, uint32_t got, uint32_t want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %u want %u\n", tag, (unsigned)got, (unsigned)want);
        return 1;
    }
    return 0;
}

/* Seed a slot mid-PDU so the append path runs without the completion reset firing. */
static void
seed_incomplete_pdu(dsd_state* state, uint16_t byte_ctr) {
    state->currentslot = 0;
    state->data_byte_ctr[0] = byte_ctr;
    state->data_header_valid[0] = 1;
    state->data_block_counter[0] = 1;
    state->data_header_blocks[0] = 9; /* != data_block_counter, so the PDU stays open */
    state->data_conf_data[0] = 0;
    state->data_header_format[0] = 7;
    state->data_header_sap[0] = 0;
}

int
main(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    if (opts == NULL || state == NULL) {
        free(opts);
        free(state);
        DSD_FPRINTF(stderr, "allocation failed\n");
        return 1;
    }

    uint8_t block[24];
    for (int i = 0; i < 24; i++) {
        block[i] = (uint8_t)(0x40 + i);
    }

    const uint16_t cap = (uint16_t)sizeof(state->dmr_pdu_sf[0]);
    int rc = 0;

    /* Normal append must be untouched by the bound. */
    seed_incomplete_pdu(state, 0);
    dmr_block_assembler(opts, state, block, 12, 0x06, 1);
    rc |= expect_eq_u32("normal append", state->data_byte_ctr[0], 12U);

    /* An append that would straddle the end of the superframe stops at the boundary. */
    seed_incomplete_pdu(state, (uint16_t)(cap - 4U));
    dmr_block_assembler(opts, state, block, 12, 0x06, 1);
    rc |= expect_eq_u32("straddling append saturates", state->data_byte_ctr[0], cap);

    /* A counter already past the superframe must not append or advance further. */
    seed_incomplete_pdu(state, 60000U);
    dmr_block_assembler(opts, state, block, 12, 0x06, 1);
    rc |= expect_eq_u32("desynchronised counter saturates", state->data_byte_ctr[0], cap);

    /* Repeated bursts keep saturating rather than wrapping the uint16 counter. */
    for (int i = 0; i < 64; i++) {
        state->data_block_counter[0] = 1;
        state->data_header_blocks[0] = 9;
        state->data_header_valid[0] = 1;
        dmr_block_assembler(opts, state, block, 24, 0x06, 1);
        if (state->data_byte_ctr[0] > cap) {
            DSD_FPRINTF(stderr, "burst %d: byte ctr %u exceeds cap %u\n", i, (unsigned)state->data_byte_ctr[0],
                        (unsigned)cap);
            rc = 1;
            break;
        }
    }

    free(opts);
    free(state);

    if (rc != 0) {
        DSD_FPRINTF(stderr, "DMR_BLOCK_TYPE1_BOUNDS: FAIL\n");
        return 1;
    }
    DSD_FPRINTF(stderr, "DMR_BLOCK_TYPE1_BOUNDS: PASS\n");
    return 0;
}
