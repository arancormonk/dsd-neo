// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <assert.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/protocol/dstar/dstar.h>
#include <dsd-neo/protocol/dstar/dstar_header_utils.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

static const uint8_t k_slow_data_scrambler[24] = {
    0, 0, 0, 0, 1, 1, 1, 0, 1, 1, 1, 1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 0, 0, 1,
};

static void pack_slow_data_bytes(const uint8_t bytes[60], uint8_t bits[480]);
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

// Inverse of dstar_deinterleave_header_bits: map payload order -> on-air order.
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
test_scrambler_roundtrip(void) {
    int original[DSD_DSTAR_HEADER_CODED_BITS];
    int scrambled[DSD_DSTAR_HEADER_CODED_BITS];
    int recovered[DSD_DSTAR_HEADER_CODED_BITS];

    for (size_t i = 0; i < DSD_DSTAR_HEADER_CODED_BITS; i++) {
        original[i] = (int)((i * 3 + 1) & 0x1);
    }

    dstar_scramble_header_bits(original, scrambled, DSD_DSTAR_HEADER_CODED_BITS);
    dstar_scramble_header_bits(scrambled, recovered, DSD_DSTAR_HEADER_CODED_BITS);

    assert(memcmp(original, recovered, sizeof(original)) == 0);
}

static void
test_scrambler_reference_prefix(void) {
    static const int expected[32] = {
        0, 0, 0, 0, 1, 1, 1, 0, 1, 1, 1, 1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0, 1, 0,
    };
    int zeroes[DSD_DSTAR_HEADER_CODED_BITS];
    int scrambled[DSD_DSTAR_HEADER_CODED_BITS];

    DSD_MEMSET(zeroes, 0, sizeof(zeroes));
    dstar_scramble_header_bits(zeroes, scrambled, DSD_DSTAR_HEADER_CODED_BITS);

    assert(memcmp(expected, scrambled, sizeof(expected)) == 0);
}

static void
test_interleave_roundtrip(void) {
    int coded[DSD_DSTAR_HEADER_CODED_BITS];
    int on_air[DSD_DSTAR_HEADER_CODED_BITS];
    int recovered[DSD_DSTAR_HEADER_CODED_BITS];

    for (size_t i = 0; i < DSD_DSTAR_HEADER_CODED_BITS; i++) {
        coded[i] = (int)((i + 5) & 0x1);
    }

    dstar_interleave_header_bits(coded, on_air, DSD_DSTAR_HEADER_CODED_BITS);
    dstar_deinterleave_header_bits(on_air, recovered, DSD_DSTAR_HEADER_CODED_BITS);

    assert(memcmp(coded, recovered, sizeof(coded)) == 0);
}

static void
test_decode_pipeline(void) {
    int info_bits[DSD_DSTAR_HEADER_INFO_BITS];
    int coded[DSD_DSTAR_HEADER_CODED_BITS];
    int interleaved[DSD_DSTAR_HEADER_CODED_BITS];
    int scrambled[DSD_DSTAR_HEADER_CODED_BITS];
    int rx_buf[DSD_DSTAR_HEADER_CODED_BITS];
    int decoded[DSD_DSTAR_HEADER_INFO_BITS];

    for (size_t i = 0; i < DSD_DSTAR_HEADER_INFO_BITS; i++) {
        info_bits[i] = (int)((i * 7 + 3) & 0x1);
    }

    convolution_encode(info_bits, DSD_DSTAR_HEADER_INFO_BITS, coded);
    dstar_interleave_header_bits(coded, interleaved, DSD_DSTAR_HEADER_CODED_BITS);
    dstar_scramble_header_bits(interleaved, scrambled, DSD_DSTAR_HEADER_CODED_BITS);

    // Receiver path
    dstar_scramble_header_bits(scrambled, rx_buf, DSD_DSTAR_HEADER_CODED_BITS);
    dstar_deinterleave_header_bits(rx_buf, interleaved, DSD_DSTAR_HEADER_CODED_BITS);
    size_t out_len =
        dstar_header_viterbi_decode(interleaved, DSD_DSTAR_HEADER_CODED_BITS, decoded, DSD_DSTAR_HEADER_INFO_BITS);

    assert(out_len == DSD_DSTAR_HEADER_INFO_BITS);
    assert(memcmp(info_bits, decoded, sizeof(info_bits)) == 0);
}

static void
test_soft_decode_pipeline(void) {
    int info_bits[DSD_DSTAR_HEADER_INFO_BITS];
    int coded[DSD_DSTAR_HEADER_CODED_BITS];
    int interleaved[DSD_DSTAR_HEADER_CODED_BITS];
    int scrambled[DSD_DSTAR_HEADER_CODED_BITS];
    uint16_t soft_rx[DSD_DSTAR_HEADER_CODED_BITS];
    uint16_t soft_descrambled[DSD_DSTAR_HEADER_CODED_BITS];
    uint16_t soft_deinterleaved[DSD_DSTAR_HEADER_CODED_BITS];
    int decoded[DSD_DSTAR_HEADER_INFO_BITS];

    for (size_t i = 0; i < DSD_DSTAR_HEADER_INFO_BITS; i++) {
        info_bits[i] = (int)((i * 5 + 2) & 0x1);
    }

    convolution_encode(info_bits, DSD_DSTAR_HEADER_INFO_BITS, coded);
    dstar_interleave_header_bits(coded, interleaved, DSD_DSTAR_HEADER_CODED_BITS);
    dstar_scramble_header_bits(interleaved, scrambled, DSD_DSTAR_HEADER_CODED_BITS);

    for (size_t i = 0; i < DSD_DSTAR_HEADER_CODED_BITS; i++) {
        soft_rx[i] = scrambled[i] ? 0xF000U : 0x1000U;
    }

    dstar_scramble_soft_costs(soft_rx, soft_descrambled, DSD_DSTAR_HEADER_CODED_BITS);
    dstar_deinterleave_soft_costs(soft_descrambled, soft_deinterleaved, DSD_DSTAR_HEADER_CODED_BITS);
    size_t out_len = dstar_header_viterbi_decode_soft(soft_deinterleaved, DSD_DSTAR_HEADER_CODED_BITS, decoded,
                                                      DSD_DSTAR_HEADER_INFO_BITS);

    assert(out_len == DSD_DSTAR_HEADER_INFO_BITS);
    assert(memcmp(info_bits, decoded, sizeof(info_bits)) == 0);
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
    processDSTAR_SD(&opts, &state, bits);

    assert(state.dstar_txt[5] == 'E');
    assert(state.dstar_txt[6] == ' ');
    assert(state.dstar_txt[7] == 'G');
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
    compact[41] = '0';
    compact[42] = '8';
    compact[43] = '7';
    compact[44] = '3';
    compact[45] = '0';
    compact[46] = '.';
    compact[47] = '1';
    compact[48] = '5';
    compact[49] = 'W';
    set_compacted_slow_data_bytes(bytes, compact);

    pack_slow_data_bytes(bytes, bits);
    processDSTAR_SD(&opts, &state, bits);

    assert(strstr(state.dstar_gps, "Lat: 41d 30m 59s N ") != NULL);
    assert(strstr(state.dstar_gps, "Lon: 087d 30m 15s W ") != NULL);
    assert(strcmp(state.event_history_s[0].Event_History_Items[0].gps_s, state.dstar_gps) == 0);
    free(history);
}

int
main(void) {
    test_scrambler_roundtrip();
    test_scrambler_reference_prefix();
    test_interleave_roundtrip();
    test_decode_pipeline();
    test_soft_decode_pipeline();
    test_crc16();
    test_slow_data_header_accepts_wire_crc_order();
    test_slow_data_text_keeps_byte_after_marker();
    test_slow_data_aprs_latitude_uses_compacted_direction();
    return 0;
}
