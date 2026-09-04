// SPDX-License-Identifier: GPL-3.0-or-later
// Coverage fixtures intentionally use table-driven local encoders and strong
// stub symbols to exercise the standalone RC burst path in isolation.
// NOLINTBEGIN(misc-use-internal-linkage)
/*
 * Round-trip checks for the standalone DMR Reverse Channel burst
 * (ETSI TS 102 361-1 clause 6.4.1): RC PDU encode -> dmr_rc_decode_pdu,
 * burst bit assembly, and the dmrRC handler framing.
 */

#include <assert.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/dibit.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/sync_patterns.h>
#include <dsd-neo/fec/block_codes.h>
#include <dsd-neo/fec/bptc.h>
#include <dsd-neo/protocol/dmr/dmr.h>
#include <dsd-neo/protocol/dmr/dmr_utils_api.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

/* Copies of the parity-check matrices in src/fec/fec.c; every encoder built
 * from them is sanity-asserted against the in-tree decoder so a transcription
 * error fails loudly. */
static const uint8_t k_hamming_16_11_4_h[5][16] = {
    {1, 1, 1, 1, 0, 1, 0, 1, 1, 0, 0, 1, 0, 0, 0, 0}, {0, 1, 1, 1, 1, 0, 1, 0, 1, 1, 0, 0, 1, 0, 0, 0},
    {0, 0, 1, 1, 1, 1, 0, 1, 0, 1, 1, 0, 0, 1, 0, 0}, {1, 1, 1, 0, 1, 0, 1, 1, 0, 0, 1, 0, 0, 0, 1, 0},
    {1, 0, 1, 0, 0, 1, 1, 0, 1, 1, 1, 0, 0, 0, 0, 1},
};

static const uint8_t k_qr_16_7_6_h[9][16] = {
    {0, 1, 1, 1, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0}, {0, 0, 1, 1, 1, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0},
    {1, 0, 0, 1, 1, 1, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0}, {0, 0, 1, 1, 0, 1, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0},
    {0, 1, 1, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0}, {1, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0},
    {1, 1, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0}, {1, 1, 1, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0},
    {1, 0, 1, 0, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 1},
};

/* 4-bit RC command + masked CRC-7 -> 11 info bits, MSB first. */
static void
encode_rc_info_bits(uint8_t cmd, uint8_t crc_mask, uint8_t bits[11]) {
    for (int i = 0; i < 4; i++) {
        bits[i] = (uint8_t)((cmd >> (3 - i)) & 1);
    }
    const uint8_t crc = (uint8_t)(crc7(bits, 4) ^ crc_mask);
    for (int i = 0; i < 7; i++) {
        bits[4 + i] = (uint8_t)((crc >> (6 - i)) & 1);
    }
}

/* 11 info bits -> 32-bit RC single-burst BPTC matrix: Hamming(16,11,4) row
 * plus odd column-parity row. */
static void
encode_rc_matrix(const uint8_t info[11], uint8_t matrix[32]) {
    for (int i = 0; i < 11; i++) {
        matrix[i] = info[i] & 1U;
    }
    for (int row = 0; row < 5; row++) {
        uint8_t p = 0;
        for (int j = 0; j < 11; j++) {
            p ^= (uint8_t)(k_hamming_16_11_4_h[row][j] & matrix[j]);
        }
        matrix[11 + row] = p;
    }

    /* Sanity: the composed codeword must round-trip through the in-tree
     * decoder without correction. */
    uint8_t rx[16];
    uint8_t decoded[11];
    DSD_MEMCPY(rx, matrix, sizeof(rx));
    assert(Hamming_16_11_4_decode(rx, decoded, 1));
    for (int i = 0; i < 11; i++) {
        assert(decoded[i] == info[i]);
    }

    for (int i = 0; i < 16; i++) {
        matrix[16 + i] = matrix[i] ^ 1U;
    }
}

/* Invert the exact composed de-interleave mapping used by
 * BPTC_16x2_Extract_Data: DataMatrix[Placement[DeInt[i]]] = In[i]. */
static void
interleave_rc_matrix(const uint8_t matrix[32], uint8_t interleaved[32]) {
    for (int i = 0; i < 32; i++) {
        interleaved[i] = matrix[DeInterleaveReverseChannelBptcPlacement[DeInterleaveReverseChannelBptc[i]]];
    }
}

static void
encode_rc_pdu(uint8_t cmd, uint8_t crc_mask, uint8_t interleaved[32]) {
    uint8_t info[11];
    uint8_t matrix[32];
    encode_rc_info_bits(cmd, crc_mask, info);
    encode_rc_matrix(info, matrix);
    interleave_rc_matrix(matrix, interleaved);
}

/* CC(4) + PI(1) + LCSS(2) -> QR(16,7,6) encoded EMB, sanity-checked against
 * the in-tree decoder. */
static void
encode_emb(uint8_t cc, uint8_t pi, uint8_t lcss, uint8_t bits[16]) {
    for (int i = 0; i < 4; i++) {
        bits[i] = (uint8_t)((cc >> (3 - i)) & 1);
    }
    bits[4] = pi & 1U;
    bits[5] = (uint8_t)((lcss >> 1) & 1);
    bits[6] = lcss & 1U;
    for (int row = 0; row < 9; row++) {
        uint8_t p = 0;
        for (int j = 0; j < 7; j++) {
            p ^= (uint8_t)(k_qr_16_7_6_h[row][j] & bits[j]);
        }
        bits[7 + row] = p;
    }

    uint8_t rx[16];
    DSD_MEMCPY(rx, bits, sizeof(rx));
    assert(QR_16_7_6_decode(rx));
    for (int i = 0; i < 7; i++) {
        assert(rx[i] == bits[i]);
    }
}

/* Map an interleaved-bit index back to its RC BPTC matrix position. */
static int
interleaved_index_for_matrix_position(int matrix_pos) {
    for (int i = 0; i < 32; i++) {
        if (DeInterleaveReverseChannelBptcPlacement[DeInterleaveReverseChannelBptc[i]] == matrix_pos) {
            return i;
        }
    }
    return -1;
}

static void
test_rc_pdu_round_trip_all_commands(void) {
    for (uint8_t cmd = 0; cmd < 16; cmd++) {
        uint8_t interleaved[32];
        uint8_t out_cmd = 0xFF;
        uint32_t out_hex = 0;
        encode_rc_pdu(cmd, 0x7A, interleaved);
        assert(dmr_rc_decode_pdu(interleaved, &out_cmd, &out_hex) == DMR_RC_DECODE_OK);
        assert(out_cmd == cmd);
        assert((out_hex >> 7) == cmd);
    }
}

static void
test_rc_command_names(void) {
    assert(strcmp(dmr_rc_command_name(0), "Increase Power By One Step") == 0);
    assert(strcmp(dmr_rc_command_name(3), "Set Power To Lowest") == 0);
    assert(strcmp(dmr_rc_command_name(4), "Cease Transmission Command") == 0);
    assert(strcmp(dmr_rc_command_name(5), "Cease Transmission Request") == 0);
    for (uint8_t cmd = 6; cmd < 16; cmd++) {
        assert(dmr_rc_command_name(cmd) == NULL);
    }
}

static void
test_rc_pdu_corrects_single_data_bit_error(void) {
    uint8_t interleaved[32];
    uint8_t out_cmd = 0xFF;
    encode_rc_pdu(5, 0x7A, interleaved);

    /* A flipped info bit (matrix positions 0..10) is corrected by the
     * Hamming(16,11,4) row and the odd-parity check still passes. */
    const int idx = interleaved_index_for_matrix_position(0);
    assert(idx >= 0);
    interleaved[idx] ^= 1U;
    assert(dmr_rc_decode_pdu(interleaved, &out_cmd, NULL) == DMR_RC_DECODE_OK);
    assert(out_cmd == 5);
}

static void
test_rc_pdu_rejects_parity_row_error(void) {
    uint8_t interleaved[32];
    encode_rc_pdu(4, 0x7A, interleaved);

    /* A flipped parity-row bit (matrix positions 16..31) is not correctable
     * and must be reported as an FEC failure. */
    const int idx = interleaved_index_for_matrix_position(20);
    assert(idx >= 0);
    interleaved[idx] ^= 1U;
    assert(dmr_rc_decode_pdu(interleaved, NULL, NULL) == DMR_RC_DECODE_FEC_ERR);
}

static void
test_rc_pdu_rejects_wrong_crc_mask(void) {
    uint8_t interleaved[32];
    encode_rc_pdu(4, 0x00, interleaved);
    assert(dmr_rc_decode_pdu(interleaved, NULL, NULL) == DMR_RC_DECODE_CRC_ERR);
    assert(dmr_rc_decode_pdu(NULL, NULL, NULL) == DMR_RC_DECODE_FEC_ERR);
}

/* ── dmrRC handler framing ─────────────────────────────────────────────── */

static int g_live_dibits[32];
static int g_live_count;
static int g_live_pos;

/* Strong spy for the event emitter: records the notice and keeps the real
 * core implementation (and its audio/history deps) out of the link. */
static unsigned g_notice_calls;
static uint8_t g_notice_last_slot;
static int g_notice_last_category;
static uint64_t g_notice_last_src;
static uint64_t g_notice_last_dst;
static int g_notice_alert_during;
static char g_notice_last_text[256];
static char g_notice_last_gps[64];

int
dsd_event_emit_data_notice_classified_with_gps(dsd_opts* opts, dsd_state* state, uint8_t slot,
                                               const dsd_call_observation* observation, dsd_event_category category,
                                               const char* notice, const char* gps) {
    (void)state;
    g_notice_calls++;
    g_notice_last_slot = slot;
    g_notice_last_category = (int)category;
    g_notice_last_src = observation->ota_source_id;
    g_notice_last_dst = observation->ota_target_id;
    g_notice_alert_during = opts->call_alert;
    DSD_SNPRINTF(g_notice_last_text, sizeof(g_notice_last_text), "%s", notice ? notice : "");
    DSD_SNPRINTF(g_notice_last_gps, sizeof(g_notice_last_gps), "%s", gps ? gps : "");
    return 0;
}

static void
reset_notice_spy(void) {
    g_notice_calls = 0;
    g_notice_last_slot = 0xFF;
    g_notice_last_category = -1;
    g_notice_last_src = 0;
    g_notice_last_dst = 0;
    g_notice_alert_during = -1;
    g_notice_last_text[0] = '\0';
    g_notice_last_gps[0] = '\0';
}

int
getDibitSoft(dsd_opts* opts, dsd_state* state, dsd_dibit_soft_t* out_soft) {
    (void)opts;
    (void)state;
    if (out_soft != NULL) {
        out_soft->reliability = 200U;
    }
    if (g_live_pos < g_live_count) {
        return g_live_dibits[g_live_pos++];
    }
    return 0;
}

/* Compose the full 48-dibit RC burst: RC_a(8) EMB_a(4) SYNC(24) EMB_b(4)
 * RC_b(8), splitting the 32 RC bits and 16 EMB bits around the sync. */
static void
build_rc_burst_dibits_mask(uint8_t cmd, uint8_t cc, uint8_t crc_mask, int dibits[48]) {
    uint8_t rc_bits[32];
    uint8_t emb_bits[16];
    encode_rc_pdu(cmd, crc_mask, rc_bits);
    encode_emb(cc, /*pi*/ 0, /*lcss single fragment*/ 0, emb_bits);

    for (size_t i = 0; i < 8U; i++) {
        dibits[i] = (rc_bits[i * 2U] << 1) | rc_bits[(i * 2U) + 1U];
        dibits[40U + i] = (rc_bits[16U + (i * 2U)] << 1) | rc_bits[16U + (i * 2U) + 1U];
    }
    for (size_t i = 0; i < 4U; i++) {
        dibits[8U + i] = (emb_bits[i * 2U] << 1) | emb_bits[(i * 2U) + 1U];
        dibits[36U + i] = (emb_bits[8U + (i * 2U)] << 1) | emb_bits[8U + (i * 2U) + 1U];
    }
    const char* sync = DMR_MS_RC_SYNC;
    for (int i = 0; i < 24; i++) {
        dibits[12 + i] = sync[i] - '0';
    }
}

static void
build_rc_burst_dibits(uint8_t cmd, uint8_t cc, int dibits[48]) {
    build_rc_burst_dibits_mask(cmd, cc, 0x7A, dibits);
}

static void
test_assemble_bits_round_trip(void) {
    int dibits[48];
    build_rc_burst_dibits(/*cmd*/ 5, /*cc*/ 7, dibits);

    uint8_t emb_bits[16];
    uint8_t rc_bits[32];
    dmr_rc_assemble_bits(dibits, emb_bits, rc_bits);

    uint8_t expected_rc[32];
    uint8_t expected_emb[16];
    encode_rc_pdu(5, 0x7A, expected_rc);
    encode_emb(7, 0, 0, expected_emb);
    for (int i = 0; i < 32; i++) {
        assert(rc_bits[i] == expected_rc[i]);
    }
    for (int i = 0; i < 16; i++) {
        assert(emb_bits[i] == expected_emb[i]);
    }

    uint8_t out_cmd = 0xFF;
    assert(dmr_rc_decode_pdu(rc_bits, &out_cmd, NULL) == DMR_RC_DECODE_OK);
    assert(out_cmd == 5);
}

static void
run_handler_case(int inverted, uint8_t debug_burst) {
    static dsd_opts opts;
    static dsd_state state;
    static int payload_buf[64];
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMSET(payload_buf, 0, sizeof(payload_buf));

    int dibits[48];
    build_rc_burst_dibits(/*cmd*/ 4, /*cc*/ 1, dibits);
    if (inverted) {
        for (int i = 0; i < 48; i++) {
            dibits[i] = (dibits[i] ^ 2) & 3;
        }
    }

    for (int i = 0; i < 36; i++) {
        payload_buf[i] = dibits[i];
    }
    g_live_count = 12;
    g_live_pos = 0;
    for (int i = 0; i < 12; i++) {
        g_live_dibits[i] = dibits[36 + i];
    }

    opts.inverted_dmr = inverted;
    opts.dmr_debug_burst = debug_burst;
    state.dmr_payload_buf = payload_buf;
    state.dmr_payload_p = payload_buf + 36;

    dmrRC(&opts, &state);
    assert(g_live_pos == 12);
    dsd_state_ext_free_all(&state);
}

static void
test_handler_consumes_trailing_dibits(void) {
    run_handler_case(/*inverted*/ 0, /*debug_burst*/ 0);
    run_handler_case(/*inverted*/ 0, /*debug_burst*/ 1);
    run_handler_case(/*inverted*/ 1, /*debug_burst*/ 0);
}

/* Drive a full burst with the given command/CC/CRC-mask through dmrRC. */
static void
drive_rc_burst(dsd_opts* opts, dsd_state* state, int payload_buf[64], uint8_t cmd, uint8_t cc, uint8_t crc_mask) {
    int dibits[48];
    build_rc_burst_dibits_mask(cmd, cc, crc_mask, dibits);
    for (int i = 0; i < 36; i++) {
        payload_buf[i] = dibits[i];
    }
    g_live_count = 12;
    g_live_pos = 0;
    for (int i = 0; i < 12; i++) {
        g_live_dibits[i] = dibits[36 + i];
    }
    state->dmr_payload_buf = payload_buf;
    state->dmr_payload_p = payload_buf + 36;
    dmrRC(opts, state);
}

static void
test_handler_emits_control_event(void) {
    static dsd_opts opts;
    static dsd_state state;
    static int payload_buf[64];
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMSET(payload_buf, 0, sizeof(payload_buf));
    reset_notice_spy();

    /* A validated command surfaces exactly one CONTROL notice on slot 0 with
     * sentinel IDs and the trusted EMB color code, with the beeper stashed
     * off for the duration of the emit. */
    opts.call_alert = 1;
    drive_rc_burst(&opts, &state, payload_buf, /*cmd*/ 4, /*cc*/ 1, 0x7A);
    assert(g_notice_calls == 1U);
    assert(g_notice_last_slot == 0U);
    assert(g_notice_last_category == (int)DSD_EVENT_CATEGORY_CONTROL);
    assert(g_notice_last_src == 0xFFFFFFU);
    assert(g_notice_last_dst == 0xFFFFFFU);
    assert(strcmp(g_notice_last_text, "DMR RC: Cease Transmission Command; CC: 01;") == 0);
    assert(strcmp(g_notice_last_gps, "") == 0);
    assert(g_notice_alert_during == 0);
    assert(opts.call_alert == 1);

    /* The same burst repeated back-to-back is a repeat train, not a new
     * operator event: still one row. */
    drive_rc_burst(&opts, &state, payload_buf, /*cmd*/ 4, /*cc*/ 1, 0x7A);
    assert(g_notice_calls == 1U);

    dsd_state_ext_free_all(&state);
}

static void
test_handler_no_event_on_invalid_or_reserved(void) {
    static dsd_opts opts;
    static dsd_state state;
    static int payload_buf[64];
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMSET(payload_buf, 0, sizeof(payload_buf));
    reset_notice_spy();

    /* CRC failure (wrong mask): no event row. */
    drive_rc_burst(&opts, &state, payload_buf, /*cmd*/ 4, /*cc*/ 1, /*mask*/ 0x00);
    assert(g_notice_calls == 0U);

    /* Reserved command (6..15): decodes fine but stays stderr-only. */
    drive_rc_burst(&opts, &state, payload_buf, /*cmd*/ 6, /*cc*/ 1, 0x7A);
    assert(g_notice_calls == 0U);

    dsd_state_ext_free_all(&state);
}

static void
test_notify_dedup_window(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    reset_notice_spy();

    /* Identical command inside the 5 s sliding window is suppressed and
     * refreshes the window. */
    dmr_rc_notify_command(&opts, &state, 0U, 2U, 4U, /*have_cc*/ 1, /*cc*/ 1, (time_t)1000);
    assert(g_notice_calls == 1U);
    dmr_rc_notify_command(&opts, &state, 0U, 2U, 4U, 1, 1, (time_t)1004);
    assert(g_notice_calls == 1U);
    /* 8 s after the first emit but only 4 s after the refresh: still
     * suppressed (proves the window slides). */
    dmr_rc_notify_command(&opts, &state, 0U, 2U, 4U, 1, 1, (time_t)1008);
    assert(g_notice_calls == 1U);
    /* 6 s of RC silence: same command is a new event. */
    dmr_rc_notify_command(&opts, &state, 0U, 2U, 4U, 1, 1, (time_t)1014);
    assert(g_notice_calls == 2U);

    /* A different command always emits immediately; without a trusted color
     * code the CC clause is omitted. */
    dmr_rc_notify_command(&opts, &state, 0U, 2U, 5U, /*have_cc*/ 0, 0U, (time_t)1014);
    assert(g_notice_calls == 3U);
    assert(strcmp(g_notice_last_text, "DMR RC: Cease Transmission Request;") == 0);

    /* Dedup keys are independent: the same command on an embedded-slot key
     * is not suppressed by the standalone key. */
    dmr_rc_notify_command(&opts, &state, 1U, 0U, 5U, 0, 0U, (time_t)1014);
    assert(g_notice_calls == 4U);
    assert(g_notice_last_slot == 1U);

    /* Reserved commands never emit. */
    dmr_rc_notify_command(&opts, &state, 0U, 2U, 6U, 0, 0U, (time_t)2000);
    assert(g_notice_calls == 4U);

    dsd_state_ext_free_all(&state);
}

static void
test_handler_bails_out_on_short_history(void) {
    static dsd_opts opts;
    static dsd_state state;
    static int payload_buf[64];
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMSET(payload_buf, 0, sizeof(payload_buf));

    g_live_count = 12;
    g_live_pos = 0;

    /* Fewer than 36 cached dibits behind the write pointer: no live reads. */
    state.dmr_payload_buf = payload_buf;
    state.dmr_payload_p = payload_buf + 10;
    dmrRC(&opts, &state);
    assert(g_live_pos == 0);

    /* No payload buffer at all. */
    state.dmr_payload_buf = NULL;
    state.dmr_payload_p = NULL;
    dmrRC(&opts, &state);
    assert(g_live_pos == 0);
}

int
main(void) {
    Hamming_16_11_4_init();
    QR_16_7_6_init();

    test_rc_pdu_round_trip_all_commands();
    test_rc_command_names();
    test_rc_pdu_corrects_single_data_bit_error();
    test_rc_pdu_rejects_parity_row_error();
    test_rc_pdu_rejects_wrong_crc_mask();
    test_assemble_bits_round_trip();
    test_handler_consumes_trailing_dibits();
    test_handler_emits_control_event();
    test_handler_no_event_on_invalid_or_reserved();
    test_notify_dedup_window();
    test_handler_bails_out_on_short_history();

    printf("DMR_RC_BURST: OK\n");
    return 0;
}

// NOLINTEND(misc-use-internal-linkage)
