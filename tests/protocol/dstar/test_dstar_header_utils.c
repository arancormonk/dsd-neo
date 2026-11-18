// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <assert.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/position.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/protocol/dstar/dstar.h>
#include <dsd-neo/protocol/dstar/dstar_header.h>
#include <dsd-neo/protocol/dstar/dstar_header_utils.h>
#include <dsd-neo/runtime/signal_search_hooks.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "dsd-neo/core/dibit.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "dsd-neo/core/synctype_ids.h"
#include "test_support.h"

static const uint8_t k_slow_data_scrambler[24] = {
    0, 0, 0, 0, 1, 1, 1, 0, 1, 1, 1, 1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 0, 0, 1,
};

uint16_t
gmsk_soft_symbol_to_viterbi_cost(float symbol, const dsd_state* state) {
    (void)state;
    return symbol > 0.0F ? 0xF000U : 0x1000U;
}

static void pack_slow_data_bytes(const uint8_t bytes[60], uint8_t bits[480]);
static void build_encoded_header_fixture(float soft_rx[DSD_DSTAR_HEADER_CODED_BITS], uint8_t flags);
static void set_compacted_slow_data_bytes(uint8_t bytes[60], const uint8_t compact[51]);

static dsd_call_snapshot
get_dstar_call(const dsd_state* state) {
    dsd_call_snapshot call;
    DSD_MEMSET(&call, 0, sizeof(call));
    assert(dsd_call_state_get(state, 0U, &call) == 1);
    return call;
}

static void
convolution_encode(const int* bits, size_t bit_count, int* symbols) {
    int s0 = 0;
    int s1 = 0;
    for (size_t i = 0; i < bit_count; i++) {
        int b = bits[i] & 0x1;
        symbols[2 * i] = b ^ s0 ^ s1; // G1 = 111 (octal 7)
        symbols[2 * i + 1] = b ^ s1;  // G2 = 101 (octal 5)
        s1 = s0;
        s0 = b;
    }
}

// Map payload order to the on-air interleave order defined by the D-STAR specification.
static void
dstar_interleave_header_bits(const int* in, int* out, size_t bit_count) {
    size_t k = 0;
    for (size_t i = 0; i < bit_count; i++) {
        out[i] = in[k];
        k += 24;
        if (k >= 672) {
            k -= 671;
        } else if (k >= 660) {
            k -= 647;
        }
    }
}

static void
test_soft_decode_pipeline(void) {
    int info_bits[DSD_DSTAR_HEADER_INFO_BITS];
    int coded[DSD_DSTAR_HEADER_CODED_BITS];
    int interleaved[DSD_DSTAR_HEADER_CODED_BITS];
    uint16_t soft_interleaved[DSD_DSTAR_HEADER_CODED_BITS];
    uint16_t soft_rx[DSD_DSTAR_HEADER_CODED_BITS];
    uint16_t soft_descrambled[DSD_DSTAR_HEADER_CODED_BITS];
    uint16_t soft_deinterleaved[DSD_DSTAR_HEADER_CODED_BITS];
    int decoded[DSD_DSTAR_HEADER_INFO_BITS];

    for (size_t i = 0; i < DSD_DSTAR_HEADER_INFO_BITS; i++) {
        info_bits[i] = (int)((i * 5 + 2) & 0x1);
    }

    convolution_encode(info_bits, DSD_DSTAR_HEADER_INFO_BITS, coded);
    dstar_interleave_header_bits(coded, interleaved, DSD_DSTAR_HEADER_CODED_BITS);
    for (size_t i = 0; i < DSD_DSTAR_HEADER_CODED_BITS; i++) {
        soft_interleaved[i] = interleaved[i] ? 0xF000U : 0x1000U;
    }
    dstar_scramble_soft_costs(soft_interleaved, soft_rx, DSD_DSTAR_HEADER_CODED_BITS);

    dstar_scramble_soft_costs(soft_rx, soft_descrambled, DSD_DSTAR_HEADER_CODED_BITS);
    dstar_deinterleave_soft_costs(soft_descrambled, soft_deinterleaved, DSD_DSTAR_HEADER_CODED_BITS);
    size_t out_len = dstar_header_viterbi_decode_soft(soft_deinterleaved, DSD_DSTAR_HEADER_CODED_BITS, decoded,
                                                      DSD_DSTAR_HEADER_INFO_BITS);

    assert(out_len == DSD_DSTAR_HEADER_INFO_BITS);
    assert(memcmp(info_bits, decoded, sizeof(info_bits)) == 0);
}

static void
build_encoded_header_fixture_my(float soft_rx[DSD_DSTAR_HEADER_CODED_BITS], uint8_t flags, int corrupt_crc,
                                const char my[12]) {
    uint8_t header[41];
    int info_bits[DSD_DSTAR_HEADER_INFO_BITS];
    int coded[DSD_DSTAR_HEADER_CODED_BITS];
    int interleaved[DSD_DSTAR_HEADER_CODED_BITS];
    uint16_t soft_interleaved[DSD_DSTAR_HEADER_CODED_BITS];
    uint16_t soft_scrambled[DSD_DSTAR_HEADER_CODED_BITS];

    DSD_MEMSET(header, 0, sizeof header);
    header[0] = flags;
    DSD_MEMCPY(header + 3, "RPT2TST ", 8);
    DSD_MEMCPY(header + 11, "RPT1TST ", 8);
    DSD_MEMCPY(header + 19, "CQCQCQ  ", 8);
    DSD_MEMCPY(header + 27, my, 12);
    const uint16_t crc = (uint16_t)(dstar_crc16(header, 39U) ^ (corrupt_crc ? 0xFFFFU : 0x0000U));
    header[39] = (uint8_t)(crc >> 8U);
    header[40] = (uint8_t)crc;

    DSD_MEMSET(info_bits, 0, sizeof info_bits);
    for (int byte_idx = 0; byte_idx < 41; byte_idx++) {
        for (int bit = 0; bit < 8; bit++) {
            info_bits[(byte_idx * 8) + bit] = (header[byte_idx] >> bit) & 0x1;
        }
    }

    convolution_encode(info_bits, DSD_DSTAR_HEADER_INFO_BITS, coded);
    dstar_interleave_header_bits(coded, interleaved, DSD_DSTAR_HEADER_CODED_BITS);

    for (size_t i = 0; i < DSD_DSTAR_HEADER_CODED_BITS; i++) {
        soft_interleaved[i] = interleaved[i] ? 0xF000U : 0x1000U;
    }
    dstar_scramble_soft_costs(soft_interleaved, soft_scrambled, DSD_DSTAR_HEADER_CODED_BITS);
    for (size_t i = 0; i < DSD_DSTAR_HEADER_CODED_BITS; i++) {
        soft_rx[i] = soft_scrambled[i] > 0x7FFFU ? 1.0F : -1.0F;
    }
}

static void
build_encoded_header_fixture_ex(float soft_rx[DSD_DSTAR_HEADER_CODED_BITS], uint8_t flags, int corrupt_crc) {
    build_encoded_header_fixture_my(soft_rx, flags, corrupt_crc, "N0CALL  /TST");
}

static void
build_encoded_header_fixture(float soft_rx[DSD_DSTAR_HEADER_CODED_BITS], uint8_t flags) {
    build_encoded_header_fixture_ex(soft_rx, flags, 0);
}

static void
test_soft_header_decode_extracts_callsigns(void) {
    static dsd_state state;
    float soft_rx[DSD_DSTAR_HEADER_CODED_BITS];

    DSD_MEMSET(&state, 0, sizeof state);
    state.min = -1.0F;
    state.center = 0.0F;
    state.max = 1.0F;
    build_encoded_header_fixture(soft_rx, 0x78U);

    assert(dstar_header_decode_soft(&state, soft_rx) == 1);

    const dsd_call_snapshot call = get_dstar_call(&state);
    assert(call.kind == DSD_CALL_KIND_VOICE);
    assert(strcmp(call.route_text[1], "RPT2TST") == 0);
    assert(strcmp(call.route_text[0], "RPT1TST") == 0);
    assert(strcmp(call.target_text, "CQCQCQ") == 0);
    assert(strcmp(call.source_text, "N0CALL /TST") == 0);
}

static void
test_soft_data_header_preserves_callsigns(void) {
    static dsd_state state;
    float soft_rx[DSD_DSTAR_HEADER_CODED_BITS];

    DSD_MEMSET(&state, 0, sizeof state);
    state.min = -1.0F;
    state.center = 0.0F;
    state.max = 1.0F;
    build_encoded_header_fixture(soft_rx, 0x80U);

    assert(dstar_header_decode_soft(&state, soft_rx) == 1);

    const dsd_call_snapshot call = get_dstar_call(&state);
    assert(call.kind == DSD_CALL_KIND_DATA);
    assert(strcmp(call.route_text[1], "RPT2TST") == 0);
    assert(strcmp(call.route_text[0], "RPT1TST") == 0);
    assert(strcmp(call.target_text, "CQCQCQ") == 0);
    assert(strcmp(call.source_text, "N0CALL /TST") == 0);
}

/* Issue #800: only a header whose CRC passed and whose MY field names a callsign names the transmission's call
 * (dsd_state::dstar_tx_call_epoch), the only call its slow data reaches. processDSTAR_HD() leaves every RF header's
 * transmission naming none (DSTAR_PROCESS); the decode names it. A blank MY field names no one, though the call keeps
 * the callsign of the transmission before it. */
static void
test_soft_header_decode_names_the_dprs_source_only_with_a_callsign(void) {
    static dsd_state state;
    float soft_rx[DSD_DSTAR_HEADER_CODED_BITS];

    DSD_MEMSET(&state, 0, sizeof state);
    state.min = -1.0F;
    state.center = 0.0F;
    state.max = 1.0F;
    build_encoded_header_fixture(soft_rx, 0x00U);
    assert(dstar_header_decode_soft(&state, soft_rx) == 1);
    const uint64_t named = get_dstar_call(&state).epoch;
    assert(named != 0U && state.dstar_tx_call_epoch == named);

    state.dstar_tx_call_epoch = 0U;
    build_encoded_header_fixture_my(soft_rx, 0x00U, 0, "            ");
    assert(dstar_header_decode_soft(&state, soft_rx) == 1);
    assert(state.dstar_tx_call_epoch == 0U);
    assert(strcmp(get_dstar_call(&state).source_text, "N0CALL /TST") == 0);

    /* Only the suffix: no callsign either. */
    build_encoded_header_fixture_my(soft_rx, 0x00U, 0, "        /TST");
    assert(dstar_header_decode_soft(&state, soft_rx) == 1);
    assert(state.dstar_tx_call_epoch == 0U);

    build_encoded_header_fixture_ex(soft_rx, 0x00U, 1);
    assert(dstar_header_decode_soft(&state, soft_rx) == 0);
    assert(state.dstar_tx_call_epoch == 0U);
    dsd_state_ext_free_all(&state);
}

/* Issue #800: a suffix with a blank MY field is no caller. On a fresh call the RF header's suffix alone never becomes
 * the call's source: the call is anonymous, as its D-PRS records are. */
static void
test_soft_header_decode_suffix_alone_is_no_source(void) {
    static dsd_state state;
    float soft_rx[DSD_DSTAR_HEADER_CODED_BITS];

    DSD_MEMSET(&state, 0, sizeof state);
    state.min = -1.0F;
    state.center = 0.0F;
    state.max = 1.0F;
    build_encoded_header_fixture_my(soft_rx, 0x00U, 0, "        TEST");
    assert(dstar_header_decode_soft(&state, soft_rx) == 1);
    const dsd_call_snapshot call = get_dstar_call(&state);
    assert(call.phase == DSD_CALL_PHASE_ACTIVE);
    assert(call.source_text[0] == '\0');
    assert(strcmp(call.target_text, "CQCQCQ") == 0);
    assert(state.dstar_tx_call_epoch == 0U);
    dsd_state_ext_free_all(&state);
}

/* #391: the header CRC-16/X.25 is the D-STAR data path's only real check, and the SPS hunt
 * now spends it. Corrupt one soft bit's worth of the CRC field and the decode must say so
 * rather than publishing the callsigns it recovered anyway. */
static void
test_soft_header_decode_reports_a_failed_crc(void) {
    static dsd_state state;
    float soft_rx[DSD_DSTAR_HEADER_CODED_BITS];

    DSD_MEMSET(&state, 0, sizeof state);
    state.min = -1.0F;
    state.center = 0.0F;
    state.max = 1.0F;
    build_encoded_header_fixture_ex(soft_rx, 0x78U, 1);

    assert(dstar_header_decode_soft(&state, soft_rx) == 0);

    /* Nothing was published either: a header whose CRC failed is not a call observation. */
    dsd_call_snapshot call;
    DSD_MEMSET(&call, 0, sizeof(call));
    assert(dsd_call_state_get(&state, 0U, &call) != 1);
}

static void
test_crc16(void) {
    const uint8_t payload[] = "123456789";
    // D-STAR compares the byte-swapped CRC-16/X25 value against the extracted wire-order field.
    assert(dstar_crc16(payload, sizeof(payload) - 1) == 0x6e90);
}

static void
test_slow_data_header_accepts_wire_crc_order(void) {
    static dsd_opts opts;
    static dsd_state state;
    uint8_t bytes[60];
    uint8_t compact[51];
    uint8_t bits[480];

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMSET(bytes, 0x20, sizeof(bytes));
    DSD_MEMSET(compact, 0x20, sizeof(compact));

    bytes[0] = 0x55;
    compact[0] = 0x00;
    compact[1] = 0x00;
    compact[2] = 0x00;
    DSD_MEMCPY(compact + 3, "RPT2TST ", 8);
    DSD_MEMCPY(compact + 11, "RPT1TST ", 8);
    DSD_MEMCPY(compact + 19, "CQCQCQ  ", 8);
    DSD_MEMCPY(compact + 27, "N0CALL  /TST", 12);
    compact[39] = 0xb0;
    compact[40] = 0x43;

    set_compacted_slow_data_bytes(bytes, compact);
    pack_slow_data_bytes(bytes, bits);
    processDSTAR_SD(&opts, &state, bits);

    const dsd_call_snapshot call = get_dstar_call(&state);
    assert(strcmp(call.route_text[1], "RPT2TST") == 0);
    assert(strcmp(call.route_text[0], "RPT1TST") == 0);
    assert(strcmp(call.target_text, "CQCQCQ") == 0);
    assert(strcmp(call.source_text, "N0CALL /TST") == 0);
}

/* What the signal search's confirmation can read when the evidence hook fires: the app-control observer copies the
 * active call's callsigns inside the hook, from state, and only on that confirmation. */
static struct {
    int calls;
    int call_active;
    int call_protocol_matches;
    char source_text[DSD_CALL_IDENTITY_TEXT_SIZE];
    char target_text[DSD_CALL_IDENTITY_TEXT_SIZE];
} g_search_evidence;

static void
capture_search_evidence(const dsd_state* state, const char* protocol, int sync, int slot, uint32_t target,
                        uint32_t source, const char* access) {
    (void)sync;
    (void)slot;
    (void)target;
    (void)source;
    (void)access;
    assert(protocol != NULL && strcmp(protocol, "D-STAR") == 0);
    dsd_call_snapshot call;
    DSD_MEMSET(&call, 0, sizeof(call));
    g_search_evidence.calls++;
    const int active = dsd_call_state_get(state, 0U, &call) > 0 && call.phase == DSD_CALL_PHASE_ACTIVE;
    g_search_evidence.call_active = active;
    g_search_evidence.call_protocol_matches = active && call.protocol == state->synctype;
    DSD_SNPRINTF(g_search_evidence.source_text, sizeof(g_search_evidence.source_text), "%s",
                 active ? call.source_text : "");
    DSD_SNPRINTF(g_search_evidence.target_text, sizeof(g_search_evidence.target_text), "%s",
                 active ? call.target_text : "");
}

/* Call notes a signal search hears, and the source the slot-0 call record named when each arrived. */
static int g_call_notes;
static char g_call_note_source[64];

static void
capture_call_note(const dsd_state* state, uint8_t slot) {
    dsd_call_snapshot call;
    g_call_notes++;
    g_call_note_source[0] = '\0';
    if (slot == 0U && dsd_call_state_get(state, 0U, &call) > 0 && call.phase == DSD_CALL_PHASE_ACTIVE) {
        DSD_SNPRINTF(g_call_note_source, sizeof(g_call_note_source), "%s", call.source_text);
    }
}

/* A slow-data header whose CRC passes is proof on its own; the signal search hears that proof only once the header's
 * callsigns are on the call, so a header followed by nothing decodable still names the station. */
static void
test_slow_data_header_search_evidence_follows_the_published_call(void) {
    static dsd_opts opts;
    static dsd_state state;
    uint8_t bytes[60];
    uint8_t compact[51];
    uint8_t bits[480];

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMSET(&g_search_evidence, 0, sizeof(g_search_evidence));
    DSD_MEMSET(bytes, 0x20, sizeof(bytes));
    DSD_MEMSET(compact, 0x20, sizeof(compact));
    state.synctype = DSD_SYNC_DSTAR_VOICE_POS;

    bytes[0] = 0x55;
    compact[0] = 0x00;
    compact[1] = 0x00;
    compact[2] = 0x00;
    DSD_MEMCPY(compact + 3, "RPT2TST ", 8);
    DSD_MEMCPY(compact + 11, "RPT1TST ", 8);
    DSD_MEMCPY(compact + 19, "CQCQCQ  ", 8);
    DSD_MEMCPY(compact + 27, "N0CALL  /TST", 12);
    compact[39] = 0xb0;

    /* A damaged header tells the search nothing. */
    compact[40] = 0x44;
    set_compacted_slow_data_bytes(bytes, compact);
    dsd_signal_search_evidence_set(capture_search_evidence);
    dsd_signal_search_call_set(capture_call_note);
    g_call_notes = 0;
    pack_slow_data_bytes(bytes, bits);
    processDSTAR_SD(&opts, &state, bits);
    assert(g_search_evidence.calls == 0 && g_call_notes == 0);

    compact[40] = 0x43;
    set_compacted_slow_data_bytes(bytes, compact);
    pack_slow_data_bytes(bytes, bits);
    processDSTAR_SD(&opts, &state, bits);
    dsd_signal_search_evidence_set(NULL);
    dsd_signal_search_call_set(NULL);

    assert(g_search_evidence.calls == 1);
    // The header's call is noted with its evidence, once its callsigns are on it.
    assert(g_call_notes == 1 && strcmp(g_call_note_source, "N0CALL /TST") == 0);
    assert(g_search_evidence.call_active == 1);
    assert(g_search_evidence.call_protocol_matches == 1);
    assert(strcmp(g_search_evidence.target_text, "CQCQCQ") == 0);
    assert(strcmp(g_search_evidence.source_text, "N0CALL /TST") == 0);
    dsd_state_ext_free_all(&state);
}

static void
pack_slow_data_bytes(const uint8_t bytes[60], uint8_t bits[480]) {
    DSD_MEMSET(bits, 0, 480);
    for (int byte_idx = 0; byte_idx < 60; byte_idx++) {
        uint8_t byte = bytes[59 - byte_idx];
        for (int bit = 0; bit < 8; bit++) {
            int packed_pos = byte_idx * 8 + bit;
            int input_pos = 479 - packed_pos;
            uint8_t payload_bit = (uint8_t)((byte >> (7 - bit)) & 1U);
            bits[input_pos] = payload_bit ^ k_slow_data_scrambler[input_pos % 24];
        }
    }
}

static void
set_compacted_slow_data_bytes(uint8_t bytes[60], const uint8_t compact[51]) {
    int compact_idx = 0;

    for (int byte_idx = 1; byte_idx < 60 && compact_idx < 51; byte_idx++) {
        if (byte_idx % 6 == 0) {
            continue;
        }
        bytes[byte_idx] = compact[compact_idx++];
    }
}

// Open a D-STAR voice call on the slot, as a decoded header (dstar_header_observe()) followed by a voice frame's sync
// leaves it: slow-data detail reaches the history row only through the call its transmission's header named (issue
// #575 follow-up, issue #800).
static void
open_dstar_call(dsd_opts* opts, dsd_state* state) {
    dstar_header_observe(state, DSD_SYNC_DSTAR_VOICE_POS, 0, "RPT2TST", "RPT1TST", "CQCQCQ", "N0CALL      ");
    assert(state->dstar_tx_call_epoch != 0U);
    dsd_event_sync_slot(opts, state, 0U);
}

/* One superframe's slow data: each block's mini-header and its five data bytes, the rest spaces. */
static void
slow_data_blocks(uint8_t bytes[60], const uint8_t* minis, const char* const* data, int blocks) {
    DSD_MEMSET(bytes, 0x20, 60);
    for (int b = 0; b < blocks; b++) {
        uint8_t* block = bytes + ((size_t)b * 6U);
        block[0] = minis[b];
        for (int i = 0; i < 5 && data[b][i] != '\0'; i++) {
            block[1 + i] = (uint8_t)data[b][i];
        }
    }
}

/* Each block by its own mini-header (issue #800): a text block's five characters, the one right after its mini-header
 * included, go to its place in the 20-character message (block 0x40 + n is characters 5n..5n+4); a GPS block's bytes
 * never do, before or after the text. */
static void
test_slow_data_text_by_mini_header(void) {
    static dsd_opts opts;
    static dsd_state state;
    Event_History_I* history;
    uint8_t bytes[60];
    uint8_t bits[480];

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    history = (Event_History_I*)calloc(2u, sizeof(*history));
    assert(history != NULL);
    state.event_history_s = history;

    static const uint8_t text_minis[2] = {0x40, 0x41};
    static const char* const text_data[2] = {"ABCDE", "G"};
    slow_data_blocks(bytes, text_minis, text_data, 2);
    pack_slow_data_bytes(bytes, bits);
    uint64_t revision = history[0].revision;
    processDSTAR_SD(&opts, &state, bits);

    assert(strcmp(state.dstar_txt, "ABCDEG") == 0);
    /* No call is open: the display text is kept, the history row is left alone. */
    assert(state.event_history_s[0].Event_History_Items[0].text_message[0] == '\0');
    assert(history[0].revision == revision);

    /* The decoder descrambles the block in place, so the repeat is packed afresh. The transmission is confirmed (its
       header's CRC passed), so its text reaches the call's row. */
    open_dstar_call(&opts, &state);
    state.dstar_confirmed = 1;
    pack_slow_data_bytes(bytes, bits);
    revision = history[0].revision;
    processDSTAR_SD(&opts, &state, bits);
    assert(strcmp(state.event_history_s[0].Event_History_Items[0].text_message, "ABCDEG") == 0);
    assert(history[0].revision == revision + 1U);

    /* A text block 0 then GPS blocks with coordinates: only the text block's characters are text. */
    static const uint8_t mixed_minis[4] = {0x40, 0x35, 0x35, 0x35};
    static const char* const mixed_data[4] = {"HELLO", "2230.", "00N/0", "4500."};
    slow_data_blocks(bytes, mixed_minis, mixed_data, 4);
    pack_slow_data_bytes(bytes, bits);
    processDSTAR_SD(&opts, &state, bits);
    assert(strcmp(state.dstar_txt, "HELLO") == 0);
    assert(strcmp(state.event_history_s[0].Event_History_Items[0].text_message, "HELLO") == 0);

    /* A GPS continuation first, then the rest of the message: the text blocks are kept in their places. */
    static const uint8_t late_minis[3] = {0x35, 0x41, 0x42};
    static const char* const late_data[3] = {"0.00E", " WORL", "D"};
    slow_data_blocks(bytes, late_minis, late_data, 3);
    pack_slow_data_bytes(bytes, bits);
    processDSTAR_SD(&opts, &state, bits);
    assert(strcmp(state.dstar_txt, "HELLO WORLD") == 0);
    assert(strcmp(state.event_history_s[0].Event_History_Items[0].text_message, "HELLO WORLD") == 0);
    assert(strstr(state.event_history_s[0].Event_History_Items[0].text_message, "0.00E") == NULL);
    dsd_state_ext_free_all(&state);
    free(history);
}

/* Slow data decodes after a single unconfirmed sync, so its text reaches a history row only from a confirmed D-STAR
 * transmission and only through the D-STAR call its header named (issue #800). A DMR call still open on slot 0 takes
 * none of it, confirmed or not: a filler block then a text block 0 leaves the DMR row as it was. The named D-STAR call
 * takes it only once the transmission is confirmed. */
static void
test_slow_data_text_needs_a_confirmed_dstar_call(void) {
    static dsd_opts opts;
    static dsd_state state;
    uint8_t bytes[60];
    uint8_t bits[480];
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    Event_History_I* history = (Event_History_I*)calloc(2u, sizeof(*history));
    assert(history != NULL);
    state.event_history_s = history;

    dsd_call_observation dmr = dsd_call_observation_data(DSD_SYNC_DMR_BS_VOICE_POS, 0U, 1001U, 2002U);
    dmr.kind = DSD_CALL_KIND_GROUP_VOICE;
    assert(dsd_call_state_observe(&state, &dmr, DSD_CALL_BOUNDARY_BEGIN) == 1);
    dsd_event_sync_slot(&opts, &state, 0U);
    static const uint8_t minis[2] = {0x66, 0x40};
    static const char* const data[2] = {"fffff", "HELLO"};
    for (int confirmed = 0; confirmed < 2; confirmed++) {
        state.dstar_confirmed = confirmed;
        state.dstar_txt[0] = '\0';
        slow_data_blocks(bytes, minis, data, 2);
        pack_slow_data_bytes(bytes, bits);
        const uint64_t revision = history[0].revision;
        processDSTAR_SD(&opts, &state, bits);
        assert(strstr(state.dstar_txt, "HELLO") != NULL); /* the display keeps it */
        assert(state.event_history_s[0].Event_History_Items[0].text_message[0] == '\0');
        assert(history[0].revision == revision);
    }
    dsd_call_snapshot call;
    assert(dsd_call_state_get(&state, 0U, &call) == 1 && DSD_SYNC_IS_DMR(call.protocol));
    dsd_state_ext_free_all(&state);
    free(history);

    DSD_MEMSET(&state, 0, sizeof(state));
    Event_History_I* dstar_history = (Event_History_I*)calloc(2u, sizeof(*dstar_history));
    assert(dstar_history != NULL);
    state.event_history_s = dstar_history;
    state.synctype = DSD_SYNC_DSTAR_VOICE_POS;
    open_dstar_call(&opts, &state); /* the call is open, but this transmission has proved nothing yet */
    slow_data_blocks(bytes, minis, data, 2);
    pack_slow_data_bytes(bytes, bits);
    processDSTAR_SD(&opts, &state, bits);
    assert(state.event_history_s[0].Event_History_Items[0].text_message[0] == '\0');
    state.dstar_confirmed = 1;
    pack_slow_data_bytes(bytes, bits);
    processDSTAR_SD(&opts, &state, bits);
    assert(strstr(state.event_history_s[0].Event_History_Items[0].text_message, "HELLO") != NULL);
    dsd_state_ext_free_all(&state);
    free(dstar_history);
}

/* Whether @p text holds a byte below 0x20 other than a line break, or DEL. */
static int
has_control_byte(const char* text) {
    for (; *text != '\0'; text++) {
        const unsigned char c = (unsigned char)*text;
        if ((c < 0x20U && c != '\n') || c == 0x7FU) {
            return 1;
        }
    }
    return 0;
}

/* Run @p fn with stderr captured into @p out. */
static void
capture_stderr_of(void (*fn)(void* arg), void* arg, char* out, size_t out_size) {
    dsd_test_capture_stderr cap;
    out[0] = '\0';
    assert(dsd_test_capture_stderr_begin(&cap, "dstar_ctl") == 0);
    fn(arg);
    assert(dsd_test_capture_stderr_end(&cap) == 0);
    assert(dsd_test_capture_stderr_read(&cap, out, out_size) == 0);
}

typedef struct {
    dsd_opts* opts;
    dsd_state* state;
    uint8_t* bits;
} sd_run;

static void
run_slow_data(void* arg) {
    sd_run* r = (sd_run*)arg;
    processDSTAR_SD(r->opts, r->state, r->bits);
}

typedef struct {
    dsd_state* state;
    const float* soft;
    int result;
} hd_run;

static void
run_header(void* arg) {
    hd_run* r = (hd_run*)arg;
    r->result = dstar_header_decode_soft(r->state, r->soft);
}

/* Radio text is shown and kept as printable ASCII only (issue #800): control bytes (ESC, 0x1A-0x1F among them) in a
 * text block, in GPS data, or in a header's callsigns (on RF or in the slow data, CRC passing over the raw bytes)
 * become spaces, on the console and on the call. */
static void
test_control_bytes_never_reach_display_text(void) {
    static dsd_opts opts;
    static dsd_state state;
    Event_History_I* history;
    uint8_t bytes[60];
    uint8_t bits[480];
    char out[8192];

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    history = (Event_History_I*)calloc(2u, sizeof(*history));
    assert(history != NULL);
    state.event_history_s = history;
    open_dstar_call(&opts, &state);
    state.dstar_confirmed = 1; /* its header's CRC passed */
    sd_run run = {&opts, &state, bits};

    static const uint8_t text_minis[2] = {0x40, 0x41};
    static const char* const text_data[2] = {"AB\x1b"
                                             "CD",
                                             "E\x1a\x1f"
                                             "F"};
    slow_data_blocks(bytes, text_minis, text_data, 2);
    pack_slow_data_bytes(bytes, bits);
    capture_stderr_of(run_slow_data, &run, out, sizeof(out));
    assert(strcmp(state.dstar_txt, "AB CDE  F") == 0);
    assert(strcmp(state.event_history_s[0].Event_History_Items[0].text_message, "AB CDE  F") == 0);
    assert(strstr(out, "TEXT: AB CDE  F") != NULL);
    assert(!has_control_byte(out));

    static const uint8_t gps_minis[2] = {0x35, 0x35};
    static const char* const gps_data[2] = {"$$CR\x1b", "C\x1e"
                                                        "12"};
    slow_data_blocks(bytes, gps_minis, gps_data, 2);
    pack_slow_data_bytes(bytes, bits);
    capture_stderr_of(run_slow_data, &run, out, sizeof(out));
    /* Issue #800 (g62-r6): the GPS blocks are a raw dump of a record not yet checked or admitted: only with -Z. */
    assert(strstr(out, "GPS DATA") == NULL);
    assert(strstr(out, "$$CR") == NULL);
    opts.payload = 1;
    pack_slow_data_bytes(bytes, bits);
    capture_stderr_of(run_slow_data, &run, out, sizeof(out));
    opts.payload = 0;
    assert(strstr(out, "GPS DATA: ") != NULL);
    assert(!has_control_byte(out));

    /* A slow-data header whose CRC passes over a callsign holding ESC. */
    uint8_t compact[51];
    DSD_MEMSET(bytes, 0x20, sizeof(bytes));
    DSD_MEMSET(compact, 0x20, sizeof(compact));
    bytes[0] = 0x55;
    compact[0] = 0x00;
    compact[1] = 0x00;
    compact[2] = 0x00;
    DSD_MEMCPY(compact + 3, "RPT2TST ", 8);
    DSD_MEMCPY(compact + 11, "RPT1TST ", 8);
    DSD_MEMCPY(compact + 19, "CQCQCQ  ", 8);
    DSD_MEMCPY(compact + 27,
               "N1\x1b"
               "CALL /TST",
               12);
    const uint16_t crc = dstar_crc16(compact, 39U);
    compact[39] = (uint8_t)(crc >> 8);
    compact[40] = (uint8_t)(crc & 0xFFU);
    set_compacted_slow_data_bytes(bytes, compact);
    pack_slow_data_bytes(bytes, bits);
    capture_stderr_of(run_slow_data, &run, out, sizeof(out));
    assert(strstr(out, "SRC: N1 CALL /TST") != NULL);
    assert(!has_control_byte(out));
    assert(strcmp(get_dstar_call(&state).source_text, "N1 CALL /TST") == 0);
    dsd_state_ext_free_all(&state);
    free(history);

    /* The RF header. */
    float soft_rx[DSD_DSTAR_HEADER_CODED_BITS];
    DSD_MEMSET(&state, 0, sizeof state);
    state.min = -1.0F;
    state.center = 0.0F;
    state.max = 1.0F;
    build_encoded_header_fixture_my(soft_rx, 0x00U, 0,
                                    "N2\x1b"
                                    "CALL /TST");
    hd_run hd = {&state, soft_rx, 0};
    capture_stderr_of(run_header, &hd, out, sizeof(out));
    assert(hd.result == 1);
    assert(strstr(out, "SRC: N2 CALL /TST") != NULL);
    assert(!has_control_byte(out));
    assert(strcmp(get_dstar_call(&state).source_text, "N2 CALL /TST") == 0);
    dsd_state_ext_free_all(&state);
}

/* Issue #800 2.10: a superframe that opens with "$$CRC" and an APRS position, but is no whole record whose CRC
 * matches, is shown as data and is neither a position nor evidence (DSTAR_DPRS covers the checked record). */
static void
test_slow_data_unchecked_aprs_is_no_position(void) {
    static dsd_opts opts;
    static dsd_state state;
    Event_History_I* history;
    uint8_t bytes[60];
    uint8_t compact[51];
    uint8_t bits[480];

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    history = (Event_History_I*)calloc(2u, sizeof(*history));
    assert(history != NULL);
    state.event_history_s = history;
    DSD_MEMSET(bytes, 0x20, sizeof(bytes));
    DSD_MEMSET(compact, 0x20, sizeof(compact));

    bytes[0] = 0x35;
    DSD_MEMCPY(compact, "$$CRC", 5);
    DSD_MEMCPY(compact + 30, "!2230.00N/04500.00E", 19);
    set_compacted_slow_data_bytes(bytes, compact);

    open_dstar_call(&opts, &state);
    pack_slow_data_bytes(bytes, bits);
    uint64_t revision = history[0].revision;
    const uint64_t rev = dsd_position_last_rev();
    processDSTAR_SD(&opts, &state, bits);

    assert(strstr(state.dstar_gps, "Lat") == NULL);
    assert(state.dstar_confirmed == 0);
    assert(dsd_position_last_rev() == rev);
    assert(state.event_history_s[0].Event_History_Items[0].gps_s[0] == '\0');
    assert(state.event_history_s[0].Event_History_Items[0].text_message[0] == '\0');
    assert(history[0].revision == revision);
    dsd_state_ext_free_all(&state);
    free(history);
}

/* Issue #800 (g62-r7): every slow-data block is classified by its own mini-header, wherever it sits in the superframe.
 * GPS bytes reach the output only through the -Z "GPS DATA:" dump: a GPS block after a block of no known kind (0x51)
 * is never part of the "_UNK:" dump, -Z or not; a GPS block after a text block is dumped with -Z like one heading the
 * superframe. */
static void
test_gps_blocks_classified_individually(void) {
    static dsd_opts opts;
    static dsd_state state;
    uint8_t bytes[60];
    uint8_t bits[480];
    char out[8192];
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    Event_History_I* history = (Event_History_I*)calloc(2u, sizeof(*history));
    assert(history != NULL);
    state.event_history_s = history;
    open_dstar_call(&opts, &state);
    sd_run run = {&opts, &state, bits};

    static const uint8_t firsts[2] = {0x51, 0x40};
    static const char* const first_data[2] = {"ABCDE", "HELLO"};
    for (int i = 0; i < 2; i++) {
        for (int payload = 0; payload <= 1; payload++) {
            DSD_MEMSET(bytes, 0x66, sizeof(bytes));
            bytes[0] = firsts[i];
            DSD_MEMCPY(bytes + 1, first_data[i], 5U);
            bytes[6] = 0x35;
            DSD_MEMCPY(bytes + 7, "2230.", 5U);
            opts.payload = payload;
            pack_slow_data_bytes(bytes, bits);
            capture_stderr_of(run_slow_data, &run, out, sizeof(out));
            const char* gps = strstr(out, "GPS DATA: 2230.");
            const char* coordinate = strstr(out, "2230.");
            if (payload) {
                /* Once, in the GPS dump. */
                assert(gps != NULL && coordinate == gps + strlen("GPS DATA: "));
                assert(strstr(coordinate + 1, "2230.") == NULL);
            } else {
                assert(gps == NULL && coordinate == NULL && strstr(out, "GPS DATA") == NULL);
            }
            if (i == 0) {
                assert(strstr(out, "_UNK: QABCDE") != NULL); /* the unknown block alone */
            } else {
                assert(strstr(out, "_UNK") == NULL && strstr(out, "TEXT: HELLO") != NULL);
            }
        }
    }
    opts.payload = 0;
    dsd_state_ext_free_all(&state);
    free(history);
}

int
main(void) {
    test_soft_decode_pipeline();
    test_soft_header_decode_extracts_callsigns();
    test_soft_data_header_preserves_callsigns();
    test_soft_header_decode_reports_a_failed_crc();
    test_soft_header_decode_names_the_dprs_source_only_with_a_callsign();
    test_soft_header_decode_suffix_alone_is_no_source();
    test_crc16();
    test_slow_data_header_accepts_wire_crc_order();
    test_slow_data_header_search_evidence_follows_the_published_call();
    test_slow_data_text_by_mini_header();
    test_slow_data_text_needs_a_confirmed_dstar_call();
    test_control_bytes_never_reach_display_text();
    test_gps_blocks_classified_individually();
    test_slow_data_unchecked_aprs_is_no_position();
    return 0;
}
