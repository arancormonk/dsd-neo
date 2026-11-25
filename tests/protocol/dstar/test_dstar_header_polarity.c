// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief One D-STAR RF header through both of its waveforms (issue #890, B18).
 *
 * The frame sync matches the header sync in both polarities (DSTAR_HD and INV_DSTAR_HD), and the hard slicer reads
 * each under its own: under +header a high symbol is 0, under -header 1 (digitize()). The header is decoded from soft
 * symbols, whose Viterbi costs (gmsk_soft_symbol_to_viterbi_cost()) used to read a high symbol as 1 under either: a
 * +header decoded from inverted bits and failed its CRC, as both headers of the capture behind DECODE_IQ_DSTAR
 * did. Here the same CRC-bearing coded header, sent as each polarity sends it, decodes through
 * dstar_header_decode_soft() with the real cost and the real slicer, and the hard slicer reads the same coded bits
 * from both. So does the end pattern, which the decoder matches on the slicer's bits (DSTAR_PROCESS).
 */

#include <assert.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/dibit.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/sync_patterns.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/protocol/dstar/dstar_header.h>
#include <dsd-neo/protocol/dstar/dstar_header_utils.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

/* The level a bit is sent at: under the positive polarity a 1 is low, as the frame sync's +patterns have it ('3' low,
 * '1' high). */
static float
bit_level(int bit, int positive) {
    const int high = positive ? !bit : bit;
    return high ? 1.0F : -1.0F;
}

/* The 24 symbols the hunt matches a header on: the last 9 bits of the 64-bit bit synchronization 1010...10 and the
 * 15-bit frame synchronization 111011001010000 (JARL STD 6.0 section 4.1.1 (a)-(b)). Each polarity's waveform of them is
 * the frame sync's pattern for it, so the waveforms below are the ones each sync names. */
static void
test_waveforms_are_the_ones_the_syncs_name(void) {
    static const char bits[] = "010101010111011001010000";
    for (int positive = 0; positive <= 1; positive++) {
        const char* pattern = positive ? DSTAR_HD : INV_DSTAR_HD;
        for (int i = 0; i < 24; i++) {
            const char want = bit_level(bits[i] == '1', positive) > 0.0F ? '1' : '3';
            assert(pattern[i] == want);
        }
    }
}

static void
convolution_encode(const int* bits, size_t bit_count, int* symbols) {
    int s0 = 0;
    int s1 = 0;
    for (size_t i = 0; i < bit_count; i++) {
        const int b = bits[i] & 0x1;
        symbols[2 * i] = b ^ s0 ^ s1;  // G1 = 111 (octal 7)
        symbols[(2 * i) + 1] = b ^ s1; // G2 = 101 (octal 5)
        s1 = s0;
        s0 = b;
    }
}

/* Payload order to the on-air interleave order (JARL STD 6.0 section 4.1.1). */
static void
interleave_header_bits(const int* in, int* out, size_t bit_count) {
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

/* The 660 coded bits of a header naming N0CALL, as sent: convolutionally coded, interleaved and scrambled. */
static void
build_coded_header(int coded_air[DSD_DSTAR_HEADER_CODED_BITS]) {
    uint8_t header[41];
    int info_bits[DSD_DSTAR_HEADER_INFO_BITS];
    int coded[DSD_DSTAR_HEADER_CODED_BITS];
    int interleaved[DSD_DSTAR_HEADER_CODED_BITS];
    uint16_t costs[DSD_DSTAR_HEADER_CODED_BITS];
    uint16_t scrambled[DSD_DSTAR_HEADER_CODED_BITS];

    DSD_MEMSET(header, 0, sizeof(header));
    DSD_MEMCPY(header + 3, "RPT2TST ", 8);
    DSD_MEMCPY(header + 11, "RPT1TST ", 8);
    DSD_MEMCPY(header + 19, "CQCQCQ  ", 8);
    DSD_MEMCPY(header + 27, "N0CALL  /TST", 12);
    const uint16_t crc = dstar_crc16(header, 39U);
    header[39] = (uint8_t)(crc >> 8U);
    header[40] = (uint8_t)crc;

    DSD_MEMSET(info_bits, 0, sizeof(info_bits));
    for (int byte_idx = 0; byte_idx < 41; byte_idx++) {
        for (int bit = 0; bit < 8; bit++) {
            info_bits[(byte_idx * 8) + bit] = (header[byte_idx] >> bit) & 0x1;
        }
    }
    convolution_encode(info_bits, DSD_DSTAR_HEADER_INFO_BITS, coded);
    interleave_header_bits(coded, interleaved, DSD_DSTAR_HEADER_CODED_BITS);
    /* The scrambler is its own inverse: scrambling the bits' sure costs scrambles the bits. */
    for (size_t i = 0; i < DSD_DSTAR_HEADER_CODED_BITS; i++) {
        costs[i] = interleaved[i] ? 0xFFFFU : 0x0000U;
    }
    dstar_scramble_soft_costs(costs, scrambled, DSD_DSTAR_HEADER_CODED_BITS);
    for (size_t i = 0; i < DSD_DSTAR_HEADER_CODED_BITS; i++) {
        coded_air[i] = scrambled[i] > 0x7FFFU ? 1 : 0;
    }
}

/* A two-level session's slicer: levels at -1 and +1 about 0, the dibit history the slicer writes. */
static void
init_two_level(dsd_state* state, int* dibits, int synctype) {
    DSD_MEMSET(state, 0, sizeof(*state));
    state->min = -1.0F;
    state->center = 0.0F;
    state->max = 1.0F;
    state->dibit_buf = dibits;
    state->dibit_buf_p = dibits;
    state->synctype = synctype;
}

static void
test_one_header_decodes_through_both_waveforms(void) {
    static dsd_opts opts;
    static dsd_state state;
    static int dibits[DSD_DSTAR_HEADER_CODED_BITS + 64];
    int coded[DSD_DSTAR_HEADER_CODED_BITS];
    float wave[DSD_DSTAR_HEADER_CODED_BITS];
    build_coded_header(coded);
    DSD_MEMSET(&opts, 0, sizeof(opts));

    for (int positive = 0; positive <= 1; positive++) {
        init_two_level(&state, dibits, positive ? DSD_SYNC_DSTAR_HD_POS : DSD_SYNC_DSTAR_HD_NEG);
        for (int i = 0; i < DSD_DSTAR_HEADER_CODED_BITS; i++) {
            wave[i] = bit_level(coded[i], positive);
            /* The hard slicer reads the coded bit from either waveform. */
            assert(digitize(&opts, &state, wave[i]) == coded[i]);
        }

        const int ok = dstar_header_decode_soft(&state, wave);
        if (!ok) {
            DSD_FPRINTF(stderr, "FAIL: the %s header failed its CRC\n", positive ? "+" : "-");
        }
        assert(ok == 1);
        dsd_call_snapshot call;
        DSD_MEMSET(&call, 0, sizeof(call));
        assert(dsd_call_state_get(&state, 0U, &call) == 1);
        assert(call.protocol == state.synctype);
        assert(strcmp(call.source_text, "N0CALL /TST") == 0);
        assert(strcmp(call.target_text, "CQCQCQ") == 0);
        dsd_state_ext_free_all(&state);
    }
}

/* The soft costs are those of the slicer's bit: a sure high symbol costs a sure 0 under +header, a sure 1 under
 * -header (and with no sync type, as before). */
static void
test_soft_cost_follows_the_slicer(void) {
    static dsd_state state;
    static int dibits[8];
    init_two_level(&state, dibits, DSD_SYNC_DSTAR_HD_POS);
    assert(gmsk_soft_symbol_to_viterbi_cost(1.0F, &state) < 2000U);
    assert(gmsk_soft_symbol_to_viterbi_cost(-1.0F, &state) > 63500U);
    init_two_level(&state, dibits, DSD_SYNC_DSTAR_HD_NEG);
    assert(gmsk_soft_symbol_to_viterbi_cost(1.0F, &state) > 63500U);
    assert(gmsk_soft_symbol_to_viterbi_cost(-1.0F, &state) < 2000U);
    init_two_level(&state, dibits, DSD_SYNC_NONE);
    assert(gmsk_soft_symbol_to_viterbi_cost(1.0F, &state) > 63500U);
}

/* The end pattern (JARL STD 6.0 section 4.1.2 (h)), sent in either polarity, is the same 48 bits to the slicer under
 * that polarity's voice sync: the decoder matches it on those bits. */
static void
test_end_pattern_reads_the_same_in_both_polarities(void) {
    static dsd_opts opts;
    static dsd_state state;
    static int dibits[64];
    const uint64_t pattern = UINT64_C(0xAAAAAAAA135E);
    DSD_MEMSET(&opts, 0, sizeof(opts));
    for (int positive = 0; positive <= 1; positive++) {
        init_two_level(&state, dibits, positive ? DSD_SYNC_DSTAR_VOICE_POS : DSD_SYNC_DSTAR_VOICE_NEG);
        uint64_t read = 0U;
        for (int i = 47; i >= 0; i--) {
            const int bit = (int)((pattern >> (unsigned)i) & 1U);
            read = (read << 1) | (uint64_t)(digitize(&opts, &state, bit_level(bit, positive)) & 1);
        }
        assert(read == pattern);
    }
}

int
main(void) {
    test_waveforms_are_the_ones_the_syncs_name();
    test_soft_cost_follows_the_slicer();
    test_one_header_decodes_through_both_waveforms();
    test_end_pattern_reads_the_same_in_both_polarities();
    printf("DSTAR_HEADER_POLARITY: OK\n");
    return 0;
}
