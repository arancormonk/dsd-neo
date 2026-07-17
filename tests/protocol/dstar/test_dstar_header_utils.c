// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <assert.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/protocol/dstar/dstar.h>
#include <dsd-neo/protocol/dstar/dstar_header.h>
#include <dsd-neo/protocol/dstar/dstar_header_utils.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "dsd-neo/core/dibit.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

static const uint8_t k_slow_data_scrambler[24] = {
    0, 0, 0, 0, 1, 1, 1, 0, 1, 1, 1, 1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 0, 0, 1,
};

uint16_t
gmsk_soft_symbol_to_viterbi_cost(float symbol, const dsd_state* state) {
    (void)state;
    return symbol > 0.0F ? 0xF000U : 0x1000U;
}

static void pack_slow_data_bytes(const uint8_t bytes[60], uint8_t bits[480]);
static void build_encoded_header_fixture(float soft_rx[DSD_DSTAR_HEADER_CODED_BITS]);
static void set_compacted_slow_data_bytes(uint8_t bytes[60], const uint8_t compact[51]);

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
build_encoded_header_fixture(float soft_rx[DSD_DSTAR_HEADER_CODED_BITS]) {
    uint8_t header[41];
    int info_bits[DSD_DSTAR_HEADER_INFO_BITS];
    int coded[DSD_DSTAR_HEADER_CODED_BITS];
    int interleaved[DSD_DSTAR_HEADER_CODED_BITS];
    uint16_t soft_interleaved[DSD_DSTAR_HEADER_CODED_BITS];
    uint16_t soft_scrambled[DSD_DSTAR_HEADER_CODED_BITS];

    DSD_MEMSET(header, 0, sizeof header);
    header[0] = 0xF8U;
    DSD_MEMCPY(header + 3, "RPT2TST ", 8);
    DSD_MEMCPY(header + 11, "RPT1TST ", 8);
    DSD_MEMCPY(header + 19, "CQCQCQ  ", 8);
    DSD_MEMCPY(header + 27, "N0CALL  /TST", 12);

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
test_soft_header_decode_extracts_callsigns(void) {
    static dsd_state state;
    float soft_rx[DSD_DSTAR_HEADER_CODED_BITS];

    DSD_MEMSET(&state, 0, sizeof state);
    state.min = -1.0F;
    state.center = 0.0F;
    state.max = 1.0F;
    build_encoded_header_fixture(soft_rx);

    dstar_header_decode_soft(&state, soft_rx);

    assert(strcmp(state.dstar_rpt2, "RPT2TST ") == 0);
    assert(strcmp(state.dstar_rpt1, "RPT1TST ") == 0);
    assert(strcmp(state.dstar_dst, "CQCQCQ  ") == 0);
    assert(strcmp(state.dstar_src, "N0CALL  /TST") == 0);
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

    assert(strcmp(state.dstar_rpt2, "RPT2TST ") == 0);
    assert(strcmp(state.dstar_rpt1, "RPT1TST ") == 0);
    assert(strcmp(state.dstar_dst, "CQCQCQ  ") == 0);
    assert(strcmp(state.dstar_src, "N0CALL  /TST") == 0);
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

static void
test_slow_data_text_keeps_byte_after_marker(void) {
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
    DSD_MEMSET(bytes, 0x20, sizeof(bytes));

    bytes[0] = 0x40;
    bytes[1] = 'A';
    bytes[2] = 'B';
    bytes[3] = 'C';
    bytes[4] = 'D';
    bytes[5] = 'E';
    bytes[6] = 'x';
    bytes[7] = 'G';

    pack_slow_data_bytes(bytes, bits);
    const uint64_t revision = history[0].revision;
    processDSTAR_SD(&opts, &state, bits);

    assert(state.dstar_txt[5] == 'E');
    assert(state.dstar_txt[6] == ' ');
    assert(state.dstar_txt[7] == 'G');
    assert(history[0].revision == revision + 1U);
    free(history);
}

static void
test_slow_data_aprs_latitude_uses_compacted_direction(void) {
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
    compact[0] = '$';
    compact[1] = '$';
    compact[2] = 'C';
    compact[3] = 'R';
    compact[4] = 'C';
    compact[30] = '!';
    compact[31] = '4';
    compact[32] = '1';
    compact[33] = '3';
    compact[34] = '0';
    compact[35] = '.';
    compact[36] = '5';
    compact[37] = '9';
    compact[38] = 'N';
    compact[39] = '/';
    compact[40] = '0';
    compact[41] = '8';
    compact[42] = '7';
    compact[43] = '3';
    compact[44] = '0';
    compact[45] = '.';
    compact[46] = '1';
    compact[47] = '5';
    compact[48] = 'W';
    set_compacted_slow_data_bytes(bytes, compact);

    pack_slow_data_bytes(bytes, bits);
    const uint64_t revision = history[0].revision;
    processDSTAR_SD(&opts, &state, bits);

    assert(strstr(state.dstar_gps, "Lat: 41d 30m 59s N ") != NULL);
    assert(strstr(state.dstar_gps, "Lon: 087d 30m 15s W ") != NULL);
    assert(strcmp(state.event_history_s[0].Event_History_Items[0].gps_s, state.dstar_gps) == 0);
    assert(history[0].revision == revision + 1U);
    free(history);
}

int
main(void) {
    test_soft_decode_pipeline();
    test_soft_header_decode_extracts_callsigns();
    test_crc16();
    test_slow_data_header_accepts_wire_crc_order();
    test_slow_data_text_keeps_byte_after_marker();
    test_slow_data_aprs_latitude_uses_compacted_direction();
    return 0;
}
