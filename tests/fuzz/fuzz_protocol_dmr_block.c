// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Stateful fuzzer for DMR type-1 block assembly.
 *
 * The overflow this targets is temporal, not a single bad length: data_byte_ctr
 * only clears when a PDU completes (data_block_counter reaches data_header_blocks),
 * so a burst sequence that restarts the block counter without clearing the byte
 * counter - the MBCH path does exactly that - leaves the two desynchronised and the
 * byte counter climbing across subsequent bursts. Nothing about any individual burst
 * looks wrong, which is why static analysis does not find it.
 *
 * State is therefore retained across records in the fuzz input, and the superframe
 * invariant is asserted after every operation rather than at the end.
 *
 * The data header SAP is seeded and mutable too: it selects which decoder a completed
 * PDU is handed to (IP, compressed UDP/IPv4, short data, MNIS, unknown), so without it
 * none of those parsers is reachable from this target (#450).
 */

#include <dsd-neo/core/events.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/protocol/dmr/dmr.h>
#include <stdint.h>
#include <stdlib.h>

#include "fuzz_support.h"

/* Bytes of fuzz input consumed per simulated burst. */
#define DSD_DMR_FUZZ_OP_BYTES 8U

/*
 * The invariant is that a burst never *advances* the byte counter past the superframe
 * row, not that the assembler normalises one. A counter already above the row is a
 * legitimate input condition - reproducing that desync is the point of this target -
 * and paths other than the type-1 append leave it alone by design.
 */
static void
check_superframe_invariant(const dsd_state* state, uint8_t slot, uint16_t before) {
    const uint16_t cap = (uint16_t)sizeof(state->dmr_pdu_sf[slot]);
    const uint16_t after = state->data_byte_ctr[slot];
    if (after > cap && after > before) {
        /* Abort so libFuzzer records the input; ASan would already have fired on a write. */
        abort();
    }
}

int
LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (data == NULL || size < DSD_DMR_FUZZ_OP_BYTES) {
        return 0;
    }
    size = dsd_fuzz_bounded_size(size);

    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    if (opts == NULL || state == NULL) {
        free(opts);
        free(state);
        return 0;
    }

    /* The UDT text paths write into event history, which the app allocates at init. */
    state->event_history_s = (Event_History_I*)calloc(2, sizeof(Event_History_I));
    if (state->event_history_s == NULL) {
        free(opts);
        free(state);
        return 0;
    }
    for (int i = 0; i < 2; i++) {
        init_event_history(&state->event_history_s[i], 0, 255);
    }

    /* The first record seeds the starting counters; the rest drive bursts. */
    const uint8_t slot = (uint8_t)(data[0] & 0x01U);
    state->currentslot = slot;
    state->data_byte_ctr[slot] = (uint16_t)(((uint16_t)data[1] << 8) | data[2]);
    state->data_header_valid[slot] = (uint8_t)(data[3] & 0x01U);
    state->data_header_sap[slot] = (uint8_t)((data[3] >> 1) & 0x0FU);
    state->data_header_blocks[slot] = (uint8_t)(data[4] % 128U);
    state->data_block_counter[slot] = (uint8_t)(data[5] % 128U);
    state->data_conf_data[slot] = (uint8_t)(data[6] & 0x01U);
    state->data_p_head[slot] = (uint8_t)(data[7] & 0x01U);

    uint8_t block[24];
    for (size_t off = DSD_DMR_FUZZ_OP_BYTES; off + DSD_DMR_FUZZ_OP_BYTES <= size; off += DSD_DMR_FUZZ_OP_BYTES) {
        const uint8_t* op = &data[off];

        /* A block counter reset without a byte counter reset is the desync that matters. */
        if ((op[0] & 0x80U) != 0U) {
            state->data_block_counter[slot] = (uint8_t)(op[1] % 128U);
        }
        if ((op[0] & 0x40U) != 0U) {
            state->data_header_blocks[slot] = (uint8_t)(op[2] % 128U);
        }
        if ((op[0] & 0x20U) != 0U) {
            state->data_header_valid[slot] = (uint8_t)(op[3] & 0x01U);
        }
        if ((op[0] & 0x10U) != 0U) {
            state->data_header_format[slot] = (uint8_t)(op[4] % 16U);
        }
        if ((op[0] & 0x08U) != 0U) {
            state->data_header_sap[slot] = (uint8_t)((op[3] >> 1) & 0x0FU);
        }

        const uint8_t block_len = (uint8_t)(op[5] % 25U);
        const uint8_t databurst = (uint8_t)(op[6] % 17U);
        const uint8_t type = (uint8_t)(op[7] % 4U);

        for (size_t i = 0; i < sizeof(block); i++) {
            block[i] = (uint8_t)(op[i % DSD_DMR_FUZZ_OP_BYTES] + (uint8_t)i);
        }

        const uint16_t before = state->data_byte_ctr[slot];
        dmr_block_assembler(opts, state, block, block_len, databurst, type);
        check_superframe_invariant(state, slot, before);
    }

    /* Decoding lazily allocates call-state extensions; release them per record. */
    dsd_state_ext_free_all(state);
    free(state->event_history_s);
    free(opts);
    free(state);
    return 0;
}
