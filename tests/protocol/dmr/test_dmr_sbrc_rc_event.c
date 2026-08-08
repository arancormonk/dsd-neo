// SPDX-License-Identifier: GPL-3.0-or-later
// Event-emission contract for the embedded SB/RC path: a validated RC command
// commits exactly one CONTROL row to the received slot's event history, with
// repeat suppression, and every invalid/gated variant stays stderr-only.
// Asserts against the real emitter (dsd_events.c is already in this link
// closure, so a strong spy would collide with it).
// NOLINTBEGIN(misc-use-internal-linkage)

#include <assert.h>
#include <dsd-neo/core/dibit.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/fec/block_codes.h>
#include <dsd-neo/fec/bptc.h>
#include <dsd-neo/protocol/dmr/dmr.h>
#include <dsd-neo/protocol/dmr/dmr_utils_api.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

/* Copy of the Hamming(16,11,4) parity-check matrix in src/fec/fec.c; the
 * encoder is sanity-asserted against the in-tree decoder. */
static const uint8_t k_hamming_16_11_4_h[5][16] = {
    {1, 1, 1, 1, 0, 1, 0, 1, 1, 0, 0, 1, 0, 0, 0, 0}, {0, 1, 1, 1, 1, 0, 1, 0, 1, 1, 0, 0, 1, 0, 0, 0},
    {0, 0, 1, 1, 1, 1, 0, 1, 0, 1, 1, 0, 0, 1, 0, 0}, {1, 1, 1, 0, 1, 0, 1, 1, 0, 0, 1, 0, 0, 0, 1, 0},
    {1, 0, 1, 0, 0, 1, 1, 0, 1, 1, 1, 0, 0, 0, 0, 1},
};

/* Load an 11-bit SB/RC value into the slot's embedded-signalling cache the way
 * dmr_sbrc_init_data reads it. odd_parity selects the RC single-burst BPTC
 * variant (second row = complement); pass 0 for the SB (even) variant. */
static void
load_sbrc_value(dsd_state* state, uint8_t slot, uint16_t value, int odd_parity) {
    uint8_t data_matrix[32];
    uint8_t interleaved[32];

    for (int i = 0; i < 11; i++) {
        data_matrix[i] = (uint8_t)((value >> (10 - i)) & 1U);
    }
    for (int row = 0; row < 5; row++) {
        uint8_t p = 0;
        for (int j = 0; j < 11; j++) {
            p ^= (uint8_t)(k_hamming_16_11_4_h[row][j] & data_matrix[j]);
        }
        data_matrix[11 + row] = p;
    }

    uint8_t rx[16];
    uint8_t decoded[11];
    DSD_MEMCPY(rx, data_matrix, sizeof(rx));
    assert(Hamming_16_11_4_decode(rx, decoded, 1));
    for (int i = 0; i < 11; i++) {
        assert(decoded[i] == ((value >> (10 - i)) & 1U));
    }

    for (int i = 0; i < 16; i++) {
        data_matrix[16 + i] = (uint8_t)(data_matrix[i] ^ (odd_parity ? 1U : 0U));
    }

    for (int i = 0; i < 32; i++) {
        interleaved[i] = data_matrix[DeInterleaveReverseChannelBptcPlacement[DeInterleaveReverseChannelBptc[i]]];
    }
    for (int i = 0; i < 32; i++) {
        state->dmr_embedded_signalling[slot][5][i + 8] = interleaved[i];
    }
}

/* 4-bit RC command + masked CRC-7 -> 11-bit SB/RC value. */
static uint16_t
build_rc_command_value(uint8_t rc_value) {
    uint8_t rc_bits[4];
    for (int i = 0; i < 4; i++) {
        rc_bits[i] = (uint8_t)((rc_value >> (3 - i)) & 1U);
    }
    const uint8_t masked_crc = (uint8_t)(crc7(rc_bits, 4U) ^ 0x7AU);
    return (uint16_t)(((uint16_t)(rc_value & 0xFU) << 7U) | masked_crc);
}

/* TXI opcode + delay + CRC-3 -> 11-bit SB value (even-parity variant). */
static uint16_t
build_txi_value(uint8_t opcode, uint8_t delay) {
    const uint8_t low8 = (uint8_t)(((delay & 0x1FU) << 3U) | (opcode & 0x7U));
    uint8_t low_bits[8];
    for (int i = 0; i < 8; i++) {
        low_bits[i] = (uint8_t)((low8 >> (7 - i)) & 1U);
    }
    return (uint16_t)(((uint16_t)crc3(low_bits, 8U) << 8U) | low8);
}

/* Strong stub: dmr_rc.o rides into this link for dmr_rc_notify_command, and
 * its standalone-burst reader must not drag the DSP/io symbol chain in. The
 * embedded path under test never reads dibits. */
int
getDibitSoft(dsd_opts* opts, dsd_state* state, dsd_dibit_soft_t* out_soft) {
    (void)opts;
    (void)state;
    if (out_soft != NULL) {
        out_soft->reliability = 0U;
    }
    return 0;
}

static Event_History_I g_event_history[2];

static void
reset_fixture(dsd_opts* opts, dsd_state* state) {
    dsd_state_ext_free_all(state);
    DSD_MEMSET(opts, 0, sizeof *opts);
    DSD_MEMSET(state, 0, sizeof *state);
    DSD_MEMSET(g_event_history, 0, sizeof g_event_history);
    state->event_history_s = g_event_history;
    init_event_history(&state->event_history_s[0], 0, 255);
    init_event_history(&state->event_history_s[1], 0, 255);
    opts->dmr_le = 1;
    opts->call_alert = 1;
}

static void
test_valid_rc_command_emits_on_received_slot(void) {
    static dsd_opts opts;
    static dsd_state state;
    reset_fixture(&opts, &state);

    state.currentslot = 1;
    load_sbrc_value(&state, 1, build_rc_command_value(5U), /*odd_parity*/ 1);
    dmr_sbrc(&opts, &state, /*power*/ 1);

    const Event_History* committed = &g_event_history[1].Event_History_Items[1];
    assert(committed->category == DSD_EVENT_CATEGORY_CONTROL);
    assert(committed->severity == DSD_EVENT_SEVERITY_INFO);
    assert(committed->source_id == 0xFFFFFFU);
    assert(committed->target_id == 0xFFFFFFU);
    assert(strstr(committed->event_string, "DMR RC: Cease Transmission Request;") != NULL);
    assert(committed->gps_s[0] == '\0');
    assert(g_event_history[0].Event_History_Items[1].event_string[0] == '\0');
    assert(opts.call_alert == 1);

    /* The same command repeated within the window is one repeat train: the
     * ring must not move again. */
    const uint64_t revision_after_first = g_event_history[1].revision;
    dmr_sbrc(&opts, &state, 1);
    assert(g_event_history[1].revision == revision_after_first);

    /* Each slot has its own dedup key: the same command decoded on the other
     * slot is still a fresh event, committed to that slot's ring. */
    state.currentslot = 0;
    load_sbrc_value(&state, 0, build_rc_command_value(5U), 1);
    dmr_sbrc(&opts, &state, 1);
    assert(strstr(g_event_history[0].Event_History_Items[1].event_string, "DMR RC: Cease Transmission Request;")
           != NULL);

    dsd_state_ext_free_all(&state);
}

static void
expect_no_commit(dsd_opts* opts, dsd_state* state, uint16_t value, int odd_parity, uint8_t power) {
    reset_fixture(opts, state);
    state->currentslot = 0;
    const uint64_t revision_before = g_event_history[0].revision;
    load_sbrc_value(state, 0, value, odd_parity);
    dmr_sbrc(opts, state, power);
    assert(g_event_history[0].revision == revision_before);
    assert(g_event_history[0].Event_History_Items[1].event_string[0] == '\0');
    dsd_state_ext_free_all(state);
}

static void
test_invalid_or_gated_variants_do_not_emit(void) {
    static dsd_opts opts;
    static dsd_state state;

    /* Reserved command (6..15): decodes with a valid CRC but stays
     * stderr-only. */
    expect_no_commit(&opts, &state, build_rc_command_value(6U), 1, 1);

    /* Corrupted CRC-7. */
    expect_no_commit(&opts, &state, (uint16_t)(build_rc_command_value(5U) ^ 0x1U), 1, 1);

    /* FEC failure: even-parity rows presented as the RC (odd) variant. */
    expect_no_commit(&opts, &state, build_rc_command_value(5U), 0, 1);

    /* SB (power=0) TXI payloads are not RC commands. */
    expect_no_commit(&opts, &state, build_txi_value(3U, 4U), 0, 0);

    /* Late-entry gate off: the standard SB/RC branch never runs. */
    reset_fixture(&opts, &state);
    opts.dmr_le = 0;
    state.currentslot = 0;
    const uint64_t revision_before = g_event_history[0].revision;
    load_sbrc_value(&state, 0, build_rc_command_value(5U), 1);
    dmr_sbrc(&opts, &state, 1);
    assert(g_event_history[0].revision == revision_before);
    dsd_state_ext_free_all(&state);
}

int
main(void) {
    InitAllFecFunction();

    test_valid_rc_command_emits_on_received_slot();
    test_invalid_or_gated_variants_do_not_emit();

    printf("DMR_SBRC_RC_EVENT: OK\n");
    return 0;
}

// NOLINTEND(misc-use-internal-linkage)
