// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Regression tests for DMR type-2 (MBC/UDT) block CRC bounds.
 *
 * dmr_block_type2_update_crc() stages the assembled blocks into a stack buffer sized
 * for six blocks, then copied 12 * 8 * ctx->blocks elements into it. ctx->blocks comes
 * from data_header_blocks in a received data header and init_ctx only capped it at 127,
 * so a UDT header claiming more than six blocks wrote up to 12192 elements into 576 and
 * then ran the CRC over the same oversized span. Found by FUZZ_PROTOCOL_DMR_BLOCK; the
 * corpus seed udt_block_count_overflow.bin reproduces it under ASan without the clamp.
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

/* Stage a UDT PDU whose header claims `header_blocks` appended blocks. */
static void
seed_udt(dsd_state* state, uint8_t header_blocks) {
    state->currentslot = 0;
    state->data_header_valid[0] = 1;
    state->data_header_blocks[0] = header_blocks;
    state->data_block_counter[0] = header_blocks;
    state->data_byte_ctr[0] = 0;
    state->data_conf_data[0] = 0;
    state->data_header_format[0] = 0;
    state->data_header_sap[0] = 0;
    state->data_p_head[0] = 0;
    state->data_block_crc_valid[0][0] = 1;

    /* Non-zero payload so an out-of-bounds copy is visible rather than writing zeros. */
    for (size_t i = 0; i < sizeof(state->dmr_pdu_sf[0]); i++) {
        state->dmr_pdu_sf[0][i] = (uint8_t)(0x5AU + (i & 0x0FU));
    }
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
        block[i] = (uint8_t)(0xC0 + i);
    }

    /* A well-formed UDT (<= 6 blocks) must still run to completion. */
    seed_udt(state, 4);
    dmr_block_assembler(opts, state, block, 12, 0x06, 3);

    /*
     * A header claiming far more blocks than the staging buffer holds. Without the
     * clamp this writes ~12 KB into a 576-byte stack buffer; ASan fails the test.
     */
    seed_udt(state, 127);
    dmr_block_assembler(opts, state, block, 12, 0x06, 3);

    /* Exactly at the staging capacity, and one past it. */
    seed_udt(state, 6);
    dmr_block_assembler(opts, state, block, 12, 0x06, 3);
    seed_udt(state, 7);
    dmr_block_assembler(opts, state, block, 12, 0x06, 3);

    /*
     * The reserved-UAB path stages blockcounter * 96 elements into the same six-block
     * buffer. data_block_counter is a received uint8_t, so it reached 24480 elements
     * into 576 before the clamp. Also found by FUZZ_PROTOCOL_DMR_BLOCK.
     */
    for (uint8_t counter = 0; counter < 255U; counter = (uint8_t)(counter + 17U)) {
        seed_udt(state, 4);
        state->udt_uab_reserved[0] = 1;
        state->data_block_counter[0] = counter;
        dmr_block_assembler(opts, state, block, 12, 0x06, 2);
    }

    /*
     * The type-2 store offset is blockcounter * block_len, and blockcounter is captured
     * before handle_type2() clamps data_block_counter to 4. A received counter near the
     * uint8_t range therefore wrote past dmr_pdu_sf[slot] at the widest block length.
     * Also found by FUZZ_PROTOCOL_DMR_BLOCK.
     */
    for (uint8_t counter = 250U; counter != 0U; counter = (uint8_t)(counter + 1U)) {
        seed_udt(state, 4);
        state->udt_uab_reserved[0] = 0;
        state->data_block_counter[0] = counter;
        dmr_block_assembler(opts, state, block, 24, 0x06, 2);
    }

    free(opts);
    free(state);
    DSD_FPRINTF(stderr, "DMR_BLOCK_TYPE2_BOUNDS: PASS\n");
    return 0;
}
