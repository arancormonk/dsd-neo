// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Deterministic M17 Air Interface v2.0.4 vectors for helpers used by the
 * encoder/decoder paths.
 */

#include "m17_algorithms.h"

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/fec/block_codes.h>
#include <dsd-neo/protocol/m17/m17_parse.h>
#include <dsd-neo/protocol/m17/m17_tables.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

static int
expect_u64(const char* label, unsigned long long got, unsigned long long want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got 0x%llX want 0x%llX\n", label, got, want);
        return 1;
    }
    return 0;
}

static int
expect_u32(const char* label, uint32_t got, uint32_t want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got 0x%X want 0x%X\n", label, got, want);
        return 1;
    }
    return 0;
}

static int
expect_int(const char* label, int got, int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %d want %d\n", label, got, want);
        return 1;
    }
    return 0;
}

static int
expect_bytes(const char* label, const uint8_t* got, const uint8_t* want, size_t n) {
    if (memcmp(got, want, n) != 0) {
        DSD_FPRINTF(stderr, "%s: byte mismatch\n", label);
        return 1;
    }
    return 0;
}

static int
count_ones(const uint8_t* bits, size_t n) {
    int ones = 0;
    for (size_t i = 0; i < n; i++) {
        ones += bits[i] ? 1 : 0;
    }
    return ones;
}

static void
bytes_to_bits(const uint8_t* bytes, uint8_t* bits, size_t byte_count) {
    for (size_t byte = 0U; byte < byte_count; byte++) {
        for (size_t bit = 0U; bit < 8U; bit++) {
            bits[(byte * 8U) + bit] = (uint8_t)((bytes[byte] >> (7U - bit)) & 1U);
        }
    }
}

static void
bits_to_bytes(const uint8_t* bits, uint8_t* bytes, size_t byte_count) {
    for (size_t byte = 0U; byte < byte_count; byte++) {
        uint8_t value = 0U;
        for (size_t bit = 0U; bit < 8U; bit++) {
            value = (uint8_t)((value << 1U) | (bits[(byte * 8U) + bit] & 1U));
        }
        bytes[byte] = value;
    }
}

static uint32_t
bits_to_u32(const uint8_t* bits, size_t n) {
    uint32_t value = 0U;
    for (size_t i = 0; i < n; i++) {
        value = (uint32_t)((value << 1U) | (bits[i] & 1U));
    }
    return value;
}

static void
bits_from_u32(uint32_t value, uint8_t* bits, size_t n) {
    for (size_t i = 0; i < n; i++) {
        bits[i] = (uint8_t)((value >> (n - 1U - i)) & 1U);
    }
}

static int
test_physical_parameters(void) {
    int err = 0;
    err |= expect_int("symbol rate", M17_SYMBOL_RATE_SPS, 4800);
    err |= expect_int("bit rate", M17_BIT_RATE_BPS, 9600);
    err |= expect_int("deviation index numerator", M17_DEVIATION_INDEX_NUM, 1);
    err |= expect_int("deviation index denominator", M17_DEVIATION_INDEX_DEN, 3);
    err |= expect_int("deviation step", M17_DEVIATION_STEP_HZ, 800);
    err |= expect_int("max deviation", M17_DEVIATION_MAX_HZ, 2400);
    err |= expect_int("occupied bandwidth", M17_OCCUPIED_BANDWIDTH_HZ, 9000);
    err |= expect_int("minimum channel spacing", M17_MIN_CHANNEL_SPACING_HZ, 12500);
    err |= expect_int("frame bits", M17_FRAME_BITS, 384);
    err |= expect_int("frame symbols", M17_FRAME_SYMBOLS, 192);
    err |= expect_int("frame duration", M17_FRAME_DURATION_MS, 40);
    err |= expect_int("sync bits", M17_SYNC_BITS, 16);
    err |= expect_int("sync symbols", M17_SYNC_SYMBOLS, 8);
    err |= expect_int("payload bits", M17_PAYLOAD_BITS, 368);
    err |= expect_int("payload symbols", M17_PAYLOAD_SYMBOLS, 184);
    err |= expect_int("frame bit sum", M17_SYNC_BITS + M17_PAYLOAD_BITS, M17_FRAME_BITS);
    err |= expect_int("frame symbol sum", M17_SYNC_SYMBOLS + M17_PAYLOAD_SYMBOLS, M17_FRAME_SYMBOLS);
    err |= expect_int("frame duration from symbols", (M17_FRAME_SYMBOLS * 1000) / M17_SYMBOL_RATE_SPS,
                      M17_FRAME_DURATION_MS);
    err |= expect_int("LSF LSD bytes", M17_LSF_LSD_BYTES, 28);
    err |= expect_int("LSF LSD bits", M17_LSF_LSD_BITS, 224);
    err |= expect_int("LSF CRC bytes", M17_LSF_CRC_BYTES, 2);
    err |= expect_int("LSF CRC bits", M17_LSF_CRC_BITS, 16);
    err |= expect_int("LSF bytes", M17_LSF_BYTES, 30);
    err |= expect_int("LSF type1 bits", M17_LSF_TYPE1_BITS, 240);
    err |= expect_int("LSF flush bits", M17_LSF_FLUSH_BITS, 4);
    err |= expect_int("LSF type1+flush bits", M17_LSF_TYPE1_FLUSH_BITS, 244);
    err |= expect_int("LSF type2 bits", M17_LSF_TYPE2_BITS, 488);
    err |= expect_int("LSF type3 bits", M17_LSF_TYPE3_BITS, 368);
    err |= expect_int("LSF type4 bits", M17_LSF_TYPE4_BITS, 368);
    err |= expect_int("LSF byte bit sum", (M17_LSF_LSD_BYTES + M17_LSF_CRC_BYTES) * 8, M17_LSF_TYPE1_BITS);
    err |= expect_int("LICH content bits", M17_LICH_CONTENT_BITS, 48);
    err |= expect_int("LICH chunk bits", M17_LICH_CHUNK_BITS, 40);
    err |= expect_int("LICH counter bits", M17_LICH_COUNTER_BITS, 3);
    err |= expect_int("LICH reserved bits", M17_LICH_RESERVED_BITS, 5);
    err |= expect_int("LICH chunks", M17_LICH_CHUNKS, 6);
    err |= expect_int("LICH Golay blocks", M17_LICH_GOLAY_BLOCKS, 4);
    err |= expect_int("LICH Golay data bits", M17_LICH_GOLAY_DATA_BITS, 12);
    err |= expect_int("LICH Golay code bits", M17_LICH_GOLAY_CODE_BITS, 24);
    err |= expect_int("LICH bits", M17_LICH_BITS, 96);
    err |= expect_int("LICH content sum", M17_LICH_CHUNK_BITS + M17_LICH_COUNTER_BITS + M17_LICH_RESERVED_BITS,
                      M17_LICH_CONTENT_BITS);
    err |= expect_int("LICH Golay sum", M17_LICH_GOLAY_BLOCKS * M17_LICH_GOLAY_CODE_BITS, M17_LICH_BITS);
    err |= expect_int("stream FN bits", M17_STREAM_FN_BITS, 16);
    err |= expect_int("stream payload bits", M17_STREAM_PAYLOAD_BITS, 128);
    err |= expect_int("stream payload half bits", M17_STREAM_PAYLOAD_HALF_BITS, 64);
    err |= expect_int("stream content bits", M17_STREAM_CONTENT_BITS, 144);
    err |= expect_int("stream flush bits", M17_STREAM_FLUSH_BITS, 4);
    err |= expect_int("stream type1+flush bits", M17_STREAM_TYPE1_FLUSH_BITS, 148);
    err |= expect_int("stream type2 bits", M17_STREAM_TYPE2_BITS, 296);
    err |= expect_int("stream punctured bits", M17_STREAM_PUNCTURED_BITS, 272);
    err |= expect_int("stream payload with LICH bits", M17_LICH_BITS + M17_STREAM_PUNCTURED_BITS, M17_PAYLOAD_BITS);
    err |= expect_int("BERT payload bits", M17_BERT_PAYLOAD_BITS, 197);
    err |= expect_int("BERT flush bits", M17_BERT_FLUSH_BITS, 4);
    err |= expect_int("BERT type1+flush bits", M17_BERT_TYPE1_FLUSH_BITS, 201);
    err |= expect_int("BERT type 2 bits", M17_BERT_TYPE2_BITS, 402);
    err |= expect_int("packet chunk bytes", M17_PACKET_CHUNK_BYTES, 25);
    err |= expect_int("packet chunk bits", M17_PACKET_CHUNK_BITS, 200);
    err |= expect_int("packet metadata bits", M17_PACKET_METADATA_BITS, 6);
    err |= expect_int("packet content bits", M17_PACKET_CONTENT_BITS, 206);
    err |= expect_int("packet flush bits", M17_PACKET_FLUSH_BITS, 4);
    err |= expect_int("packet type1+flush bits", M17_PACKET_TYPE1_FLUSH_BITS, 210);
    err |= expect_int("packet type2 bits", M17_PACKET_TYPE2_BITS, 420);
    err |= expect_int("packet min frames", M17_PACKET_MIN_FRAMES, 1);
    err |= expect_int("packet max frames", M17_PACKET_MAX_FRAMES, 33);
    err |= expect_int("packet crc bytes", M17_PACKET_CRC_BYTES, 2);
    err |= expect_int("packet max application bytes", M17_PACKET_MAX_APPLICATION_BYTES, 823);
    err |= expect_int("packet max total bytes", M17_PACKET_MAX_TOTAL_BYTES, 825);
    err |= expect_int("preamble bits", M17_PREAMBLE_BITS, 384);
    err |= expect_int("preamble symbols", M17_PREAMBLE_SYMBOLS, 192);
    err |= expect_int("EoT bits", M17_EOT_BITS, 384);
    err |= expect_int("EoT symbols", M17_EOT_SYMBOLS, 192);
    err |= expect_int("recommended sample rate", M17_RECOMMENDED_SAMPLE_RATE_HZ, 48000);
    err |= expect_int("recommended upsample", M17_RECOMMENDED_UPSAMPLE_FACTOR, 10);
    err |= expect_int("RRC alpha numerator", M17_RRC_ALPHA_NUM, 1);
    err |= expect_int("RRC alpha denominator", M17_RRC_ALPHA_DEN, 2);
    err |= expect_int("RRC minimum span", M17_RRC_MIN_SPAN_SYMBOLS, 8);
    err |= expect_int("RRC recommended taps", M17_RRC_RECOMMENDED_TAPS, 81);
    err |= expect_int("CSMA default p numerator", M17_CSMA_DEFAULT_PROBABILITY_NUM, 1);
    err |= expect_int("CSMA default p denominator", M17_CSMA_DEFAULT_PROBABILITY_DEN, 4);
    err |= expect_int("CSMA slot ms", M17_CSMA_DEFAULT_SLOT_TIME_MS, 40);
    err |= expect_int("PRBS9 lock bits", M17_PRBS9_LOCK_BITS, 18);
    err |= expect_int("PRBS9 resync window", M17_PRBS9_RESYNC_WINDOW_BITS, 128);
    err |= expect_int("PRBS9 resync threshold", M17_PRBS9_RESYNC_ERROR_THRESHOLD, 18);
    return err;
}

static int
test_crc16_vectors(void) {
    int err = 0;

    const uint8_t empty[1] = {0U};
    err |= expect_u32("crc empty", m17_crc16(empty, 0U), 0xFFFFU);

    const uint8_t a[] = {'A'};
    err |= expect_u32("crc A", m17_crc16(a, (uint16_t)sizeof(a)), 0x206EU);

    const uint8_t digits[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    err |= expect_u32("crc 123456789", m17_crc16(digits, (uint16_t)sizeof(digits)), 0x772BU);

    uint8_t range[256];
    for (uint16_t i = 0; i < 256U; i++) {
        range[i] = (uint8_t)i;
    }
    err |= expect_u32("crc 00..ff", m17_crc16(range, (uint16_t)sizeof(range)), 0x1C31U);

    uint8_t lsf[M17_LSF_BYTES];
    for (uint8_t i = 0U; i < M17_LSF_LSD_BYTES; i++) {
        lsf[i] = (uint8_t)(i + 1U);
    }
    const uint16_t lsf_crc = m17_crc16(lsf, M17_LSF_LSD_BYTES);
    lsf[M17_LSF_LSD_BYTES] = (uint8_t)(lsf_crc >> 8U);
    lsf[M17_LSF_LSD_BYTES + 1U] = (uint8_t)(lsf_crc & 0xFFU);
    err |= expect_u32("crc LSF big-endian residue", m17_crc16(lsf, M17_LSF_BYTES), 0U);

    lsf[M17_LSF_LSD_BYTES] = (uint8_t)(lsf_crc & 0xFFU);
    lsf[M17_LSF_LSD_BYTES + 1U] = (uint8_t)(lsf_crc >> 8U);
    err |= expect_int("crc LSF little-endian residue nonzero", m17_crc16(lsf, M17_LSF_BYTES) != 0U, 1);

    return err;
}

static int
test_dibit_symbol_mapping(void) {
    int err = 0;
    err |= expect_int("dibit 00", m17_symbol_from_dibit(0U), +1);
    err |= expect_int("dibit 01", m17_symbol_from_dibit(1U), +3);
    err |= expect_int("dibit 10", m17_symbol_from_dibit(2U), -1);
    err |= expect_int("dibit 11", m17_symbol_from_dibit(3U), -3);

    const uint8_t byte = 0xB4U;
    const int want_symbols[4] = {-1, -3, +3, +1};
    for (int i = 0; i < 4; i++) {
        const uint8_t dibit = (uint8_t)((byte >> (6 - (2 * i))) & 0x3U);
        err |= expect_int("0xB4 symbol", m17_symbol_from_dibit(dibit), want_symbols[i]);
    }
    return err;
}

static int
test_physical_words(void) {
    int err = 0;

    uint8_t dibits[M17_FRAME_SYMBOLS];
    m17_fill_repeating_16bit_dibits(M17_PREAMBLE_LSF_WORD, dibits);
    err |= expect_int("LSF preamble first", m17_symbol_from_dibit(dibits[0]), +3);
    err |= expect_int("LSF preamble second", m17_symbol_from_dibit(dibits[1]), -3);
    err |= expect_int("LSF preamble last", m17_symbol_from_dibit(dibits[M17_FRAME_SYMBOLS - 1]), -3);

    m17_fill_repeating_16bit_dibits(M17_PREAMBLE_BERT_WORD, dibits);
    err |= expect_int("BERT preamble first", m17_symbol_from_dibit(dibits[0]), -3);
    err |= expect_int("BERT preamble second", m17_symbol_from_dibit(dibits[1]), +3);
    err |= expect_int("BERT preamble last", m17_symbol_from_dibit(dibits[M17_FRAME_SYMBOLS - 1]), +3);

    m17_fill_repeating_16bit_dibits(M17_EOT_MARKER_WORD, dibits);
    const int want_eot[8] = {+3, +3, +3, +3, +3, +3, -3, +3};
    for (int i = 0; i < 8; i++) {
        err |= expect_int("EoT symbol", m17_symbol_from_dibit(dibits[i]), want_eot[i]);
    }

    uint8_t sync[M17_SYNC_SYMBOLS];
    m17_fill_sync_dibits_from_word(M17_SYNC_LSF_WORD, sync);
    const int want_lsf[8] = {+3, +3, +3, +3, -3, -3, +3, -3};
    for (int i = 0; i < M17_SYNC_SYMBOLS; i++) {
        err |= expect_int("LSF sync symbol", m17_symbol_from_dibit(sync[i]), want_lsf[i]);
    }

    m17_fill_sync_dibits_from_word(M17_SYNC_BERT_WORD, sync);
    const int want_bert[8] = {-3, +3, -3, -3, +3, +3, +3, +3};
    for (int i = 0; i < M17_SYNC_SYMBOLS; i++) {
        err |= expect_int("BERT sync symbol", m17_symbol_from_dibit(sync[i]), want_bert[i]);
    }

    m17_fill_sync_dibits_from_word(M17_SYNC_STREAM_WORD, sync);
    const int want_stream[8] = {-3, -3, -3, -3, +3, +3, -3, +3};
    for (int i = 0; i < M17_SYNC_SYMBOLS; i++) {
        err |= expect_int("stream sync symbol", m17_symbol_from_dibit(sync[i]), want_stream[i]);
    }

    m17_fill_sync_dibits_from_word(M17_SYNC_PACKET_WORD, sync);
    const int want_packet[8] = {+3, -3, +3, +3, -3, -3, -3, -3};
    for (int i = 0; i < M17_SYNC_SYMBOLS; i++) {
        err |= expect_int("packet sync symbol", m17_symbol_from_dibit(sync[i]), want_packet[i]);
    }

    return err;
}

static int
test_packet_layout_helpers(void) {
    int err = 0;
    uint8_t last = 0U;
    uint16_t app = 0U;
    uint8_t metadata = 0U;
    uint8_t eof = 0U;
    uint8_t value = 0U;

    err |= expect_int("packet empty app frames", m17_packet_frame_count_for_app_bytes(0U, &last), 1);
    err |= expect_int("packet empty app last bytes", last, 2);
    err |= expect_int("packet one frame max app frames", m17_packet_frame_count_for_app_bytes(23U, &last), 1);
    err |= expect_int("packet one frame max app last bytes", last, 25);
    err |= expect_int("packet two frame boundary frames", m17_packet_frame_count_for_app_bytes(24U, &last), 2);
    err |= expect_int("packet two frame boundary last bytes", last, 1);
    err |= expect_int("packet max app frames", m17_packet_frame_count_for_app_bytes(823U, &last), 33);
    err |= expect_int("packet max app last bytes", last, 25);
    err |= expect_int("packet oversize rejected", m17_packet_frame_count_for_app_bytes(824U, &last), -1);

    err |= expect_int("packet eof app len", m17_packet_app_bytes_from_eof(0U, 2U, &app), 0);
    err |= expect_int("packet eof empty app", app, 0);
    err |= expect_int("packet eof app len max", m17_packet_app_bytes_from_eof(32U, 25U, &app), 0);
    err |= expect_int("packet eof max app", app, 823);
    err |= expect_int("packet eof last zero rejected", m17_packet_app_bytes_from_eof(0U, 0U, &app), -1);
    err |= expect_int("packet eof frame overflow rejected", m17_packet_app_bytes_from_eof(33U, 1U, &app), -1);

    err |= expect_int("packet metadata eof0", m17_packet_metadata_byte(0U, 31U, &metadata), 0);
    err |= expect_int("packet metadata eof0 byte", metadata, 0x7CU);
    err |= expect_int("packet parse eof0", m17_packet_parse_metadata_byte(metadata, &eof, &value), 0);
    err |= expect_int("packet parse eof0 flag", eof, 0);
    err |= expect_int("packet parse eof0 value", value, 31);
    err |= expect_int("packet metadata eof1", m17_packet_metadata_byte(1U, 25U, &metadata), 0);
    err |= expect_int("packet metadata eof1 byte", metadata, 0xE4U);
    err |= expect_int("packet parse eof1", m17_packet_parse_metadata_byte(metadata, &eof, &value), 0);
    err |= expect_int("packet parse eof1 flag", eof, 1);
    err |= expect_int("packet parse eof1 value", value, 25);
    err |= expect_int("packet metadata eof1 zero rejected", m17_packet_metadata_byte(1U, 0U, &metadata), -1);
    err |= expect_int("packet metadata eof1 high rejected", m17_packet_metadata_byte(1U, 26U, &metadata), -1);
    err |= expect_int("packet parse flush bits rejected", m17_packet_parse_metadata_byte(0xE5U, &eof, &value), -1);
    return err;
}

static int
test_base40_encode(void) {
    int err = 0;
    err |= expect_u64("AB1CD", m17_encode_b40_callsign(0ULL, "AB1CD"), 0x9FDD51ULL);
    err |= expect_u64("AB1CD trailing spaces", m17_encode_b40_callsign(0ULL, "AB1CD    "), 0x9FDD51ULL);
    err |= expect_u64("lowercase", m17_encode_b40_callsign(0ULL, "ab1cd"), 0x9FDD51ULL);
    err |= expect_u64("invalid maps to space", m17_encode_b40_callsign(0ULL, "A?B"), 0x0C81ULL);
    err |= expect_u64("broadcast passthrough", m17_encode_b40_callsign(0xFFFFFFFFFFFFULL, "N0CALL"), 0xFFFFFFFFFFFFULL);

    for (uint8_t i = 0U; i < 40U; i++) {
        char csd[2] = {m17_base40_alphabet[i], '\0'};
        err |= expect_u64("single-character alphabet value", m17_encode_b40_callsign(0ULL, csd), i);
    }

    unsigned long long expected_abcdefghi = 0ULL;
    for (int n = 8; n >= 0; n--) {
        expected_abcdefghi = (expected_abcdefghi * 40ULL) + (unsigned long long)(n + 1);
    }
    err |= expect_u64("max nine characters", m17_encode_b40_callsign(0ULL, "ABCDEFGHI"), expected_abcdefghi);
    err |= expect_u64("ignore after nine characters", m17_encode_b40_callsign(0ULL, "ABCDEFGHIJ"), expected_abcdefghi);
    err |= expect_u64("standard range maximum", m17_encode_b40_callsign(0ULL, "........."), M17_ADDRESS_STANDARD_MAX);
    return err;
}

static int
test_randomizer_and_puncturing_tables(void) {
    int err = 0;
    const uint8_t want_randomizer[46] = {
        0xD6U, 0xB5U, 0xE2U, 0x30U, 0x82U, 0xFFU, 0x84U, 0x62U, 0xBAU, 0x4EU, 0x96U, 0x90U, 0xD8U, 0x98U, 0xDDU, 0x5DU,
        0x0CU, 0xC8U, 0x52U, 0x43U, 0x91U, 0x1DU, 0xF8U, 0x6EU, 0x68U, 0x2FU, 0x35U, 0xDAU, 0x14U, 0xEAU, 0xCDU, 0x76U,
        0x19U, 0x8DU, 0xD5U, 0x80U, 0xD1U, 0x33U, 0x87U, 0x13U, 0x57U, 0x18U, 0x2DU, 0x29U, 0x78U, 0xC3U};
    uint8_t bytes[46];
    for (int b = 0; b < 46; b++) {
        uint8_t value = 0U;
        for (int bit = 0; bit < 8; bit++) {
            value = (uint8_t)((value << 1U) | (m17_scramble[(b * 8) + bit] & 1U));
        }
        bytes[b] = value;
        err |= expect_u32("randomizer byte", bytes[b], want_randomizer[b]);
    }

    err |= expect_u32("randomizer first", bytes[0], 0xD6U);
    err |= expect_u32("randomizer second", bytes[1], 0xB5U);
    err |= expect_u32("randomizer last", bytes[45], 0xC3U);
    err |= expect_int("P1 length ones", count_ones(m17_puncture_pattern_1, M17_PUNCTURE_P1_LEN), 46);
    err |= expect_int("P2 length ones", count_ones(m17_puncture_pattern_2, M17_PUNCTURE_P2_LEN), 11);
    err |= expect_int("P3 length ones", count_ones(m17_puncture_pattern_3, M17_PUNCTURE_P3_LEN), 7);
    err |= expect_int("P1 last", m17_puncture_pattern_1[M17_PUNCTURE_P1_LEN - 1U], 1);
    err |= expect_int("P2 last", m17_puncture_pattern_2[M17_PUNCTURE_P2_LEN - 1U], 0);
    err |= expect_int("P3 last", m17_puncture_pattern_3[M17_PUNCTURE_P3_LEN - 1U], 0);
    return err;
}

static int
test_fec_stage_helpers(void) {
    int err = 0;
    const uint8_t impulse[5] = {1U, 0U, 0U, 0U, 0U};
    const uint8_t want_encoded[10] = {1U, 1U, 0U, 1U, 0U, 1U, 1U, 0U, 1U, 1U};
    uint8_t encoded_impulse[10];
    DSD_MEMSET(encoded_impulse, 0, sizeof(encoded_impulse));
    m17_convolution_encode_bits(impulse, encoded_impulse, 5U);
    for (int i = 0; i < 10; i++) {
        err |= expect_u32("conv impulse", encoded_impulse[i], want_encoded[i]);
    }

    uint8_t lsf_type1[M17_LSF_TYPE1_FLUSH_BITS];
    uint8_t lsf_type2[M17_LSF_TYPE2_BITS];
    uint8_t lsf_type3[M17_LSF_TYPE3_BITS];
    DSD_MEMSET(lsf_type1, 0, sizeof(lsf_type1));
    DSD_MEMSET(lsf_type2, 0, sizeof(lsf_type2));
    DSD_MEMSET(lsf_type3, 0, sizeof(lsf_type3));
    lsf_type1[0] = 1U;

    m17_convolution_encode_bits(lsf_type1, lsf_type2, M17_LSF_TYPE1_FLUSH_BITS);
    uint16_t consumed = 0U;
    uint16_t out = m17_puncture_bits(lsf_type2, M17_LSF_TYPE2_BITS, m17_puncture_pattern_1, M17_PUNCTURE_P1_LEN,
                                     lsf_type3, M17_LSF_TYPE3_BITS, &consumed);
    err |= expect_int("P1 output bits", out, M17_LSF_TYPE3_BITS);
    err |= expect_int("P1 consumed bits", consumed, M17_LSF_TYPE2_BITS);

    uint8_t bert_type2[M17_BERT_TYPE2_BITS];
    uint8_t bert_type3[M17_PAYLOAD_BITS];
    DSD_MEMSET(bert_type2, 0x01, sizeof(bert_type2));
    out = m17_puncture_bits(bert_type2, M17_BERT_TYPE2_BITS, m17_puncture_pattern_2, M17_PUNCTURE_P2_LEN, bert_type3,
                            M17_PAYLOAD_BITS, &consumed);
    err |= expect_int("P2 BERT output bits", out, M17_PAYLOAD_BITS);
    err |= expect_int("P2 BERT consumed bits", consumed, M17_BERT_TYPE2_BITS);

    uint8_t stream_type2[M17_STREAM_TYPE2_BITS];
    uint8_t stream_type3[M17_STREAM_PUNCTURED_BITS];
    DSD_MEMSET(stream_type2, 0x01, sizeof(stream_type2));
    out = m17_puncture_bits(stream_type2, M17_STREAM_TYPE2_BITS, m17_puncture_pattern_2, M17_PUNCTURE_P2_LEN,
                            stream_type3, M17_STREAM_PUNCTURED_BITS, &consumed);
    err |= expect_int("P2 stream output bits", out, M17_STREAM_PUNCTURED_BITS);
    err |= expect_int("P2 stream consumed bits", consumed, M17_STREAM_TYPE2_BITS);

    uint8_t packet_type2[M17_PACKET_TYPE2_BITS];
    uint8_t packet_type3[M17_PAYLOAD_BITS];
    DSD_MEMSET(packet_type2, 0x01, sizeof(packet_type2));
    out = m17_puncture_bits(packet_type2, M17_PACKET_TYPE2_BITS, m17_puncture_pattern_3, M17_PUNCTURE_P3_LEN,
                            packet_type3, M17_PAYLOAD_BITS, &consumed);
    err |= expect_int("P3 packet output bits", out, M17_PAYLOAD_BITS);
    err |= expect_int("P3 packet consumed bits", consumed, M17_PACKET_TYPE2_BITS);

    uint8_t interleaved[M17_PAYLOAD_BITS];
    uint8_t deinterleaved[M17_PAYLOAD_BITS];
    uint8_t randomized[M17_PAYLOAD_BITS];
    uint8_t derandomized[M17_PAYLOAD_BITS];
    for (int i = 0; i < M17_PAYLOAD_BITS; i++) {
        lsf_type3[i] = (uint8_t)((i * 5 + 1) & 1U);
    }
    DSD_MEMSET(interleaved, 0, sizeof(interleaved));
    DSD_MEMSET(deinterleaved, 0, sizeof(deinterleaved));
    DSD_MEMSET(randomized, 0, sizeof(randomized));
    DSD_MEMSET(derandomized, 0, sizeof(derandomized));
    m17_interleave_368_bits(lsf_type3, interleaved);
    m17_deinterleave_368_bits(interleaved, deinterleaved);
    m17_randomize_368_bits(interleaved, randomized);
    m17_randomize_368_bits(randomized, derandomized);
    for (int i = 0; i < M17_PAYLOAD_BITS; i++) {
        err |= expect_u32("interleave roundtrip", deinterleaved[i], lsf_type3[i]);
        err |= expect_u32("randomizer roundtrip", derandomized[i], interleaved[i]);
    }

    m17_payload_encode_bits(lsf_type3, randomized);
    m17_payload_decode_bits(randomized, deinterleaved);
    for (int i = 0; i < M17_PAYLOAD_BITS; i++) {
        err |= expect_u32("payload codec roundtrip", deinterleaved[i], lsf_type3[i]);
    }

    uint8_t seen[M17_PAYLOAD_BITS];
    DSD_MEMSET(seen, 0, sizeof(seen));
    for (int i = 0; i < M17_PAYLOAD_BITS; i++) {
        DSD_MEMSET(lsf_type3, 0, sizeof(lsf_type3));
        DSD_MEMSET(interleaved, 0, sizeof(interleaved));
        lsf_type3[i] = 1U;
        m17_interleave_368_bits(lsf_type3, interleaved);
        int found = -1;
        for (int j = 0; j < M17_PAYLOAD_BITS; j++) {
            if (interleaved[j] != 0U) {
                found = j;
                seen[j]++;
            }
        }
        const int expected = ((45 * i) + (92 * i * i)) % M17_PAYLOAD_BITS;
        err |= expect_int("interleave one-hot position", found, expected);
    }
    for (int i = 0; i < M17_PAYLOAD_BITS; i++) {
        err |= expect_u32("interleave bijection", seen[i], 1U);
    }

    return err;
}

static int
test_golay_helpers(void) {
    int err = 0;

    Golay_24_12_init();

    uint8_t data[M17_LICH_GOLAY_DATA_BITS];
    uint8_t encoded[M17_LICH_GOLAY_CODE_BITS];
    uint8_t decoded[M17_LICH_GOLAY_DATA_BITS];
    DSD_MEMSET(data, 0, sizeof(data));
    DSD_MEMSET(encoded, 0, sizeof(encoded));
    DSD_MEMSET(decoded, 0, sizeof(decoded));

    data[0] = 1U;
    m17_golay24_encode12_bits(data, encoded);
    err |= expect_u32("Golay first generator row", bits_to_u32(encoded, M17_LICH_GOLAY_CODE_BITS), 0x800C75U);

    encoded[5] ^= 1U;
    encoded[18] ^= 1U;
    err |= expect_int("Golay two-bit correction", m17_golay24_decode24_bits(encoded, decoded), 0);
    for (int i = 0; i < M17_LICH_GOLAY_DATA_BITS; i++) {
        err |= expect_u32("Golay corrected data", decoded[i], data[i]);
    }

    bits_from_u32(0xACEU, data, M17_LICH_GOLAY_DATA_BITS);
    m17_golay24_encode12_bits(data, encoded);
    for (int i = 0; i < M17_LICH_GOLAY_DATA_BITS; i++) {
        err |= expect_u32("Golay systematic data bit", encoded[i], data[i]);
    }

    return err;
}

static int
test_lich_helpers(void) {
    int err = 0;

    Golay_24_12_init();

    uint8_t lsf[M17_LSF_TYPE1_BITS];
    uint8_t lsf_type1[M17_LSF_TYPE1_FLUSH_BITS];
    uint8_t lich[M17_LICH_CONTENT_BITS];
    uint8_t encoded_lich[M17_LICH_BITS];
    uint8_t decoded_lich[M17_LICH_CONTENT_BITS];
    for (int i = 0; i < M17_LSF_TYPE1_BITS; i++) {
        lsf[i] = (uint8_t)((i * 3 + (i / 5)) & 1U);
    }
    DSD_MEMSET(lsf_type1, 0, sizeof(lsf_type1));
    for (int i = 0; i < M17_LSF_TYPE1_BITS; i++) {
        lsf_type1[i] = lsf[i];
    }

    uint8_t lsf_frame[M17_PAYLOAD_BITS];
    uint8_t lsf_decoded[M17_PAYLOAD_BITS];
    uint8_t lsf_encoded[M17_LSF_TYPE2_BITS];
    uint8_t lsf_punctured[M17_PAYLOAD_BITS];
    uint16_t consumed = 0U;
    DSD_MEMSET(lsf_frame, 0, sizeof(lsf_frame));
    DSD_MEMSET(lsf_decoded, 0, sizeof(lsf_decoded));
    DSD_MEMSET(lsf_encoded, 0, sizeof(lsf_encoded));
    DSD_MEMSET(lsf_punctured, 0, sizeof(lsf_punctured));
    err |= expect_int("LSF frame output bits", m17_lsf_encode_type1_bits(lsf_type1, lsf_frame, &consumed),
                      M17_PAYLOAD_BITS);
    err |= expect_int("LSF frame consumed bits", consumed, M17_LSF_TYPE2_BITS);
    m17_payload_decode_bits(lsf_frame, lsf_decoded);
    m17_convolution_encode_bits(lsf_type1, lsf_encoded, M17_LSF_TYPE1_FLUSH_BITS);
    (void)m17_puncture_bits(lsf_encoded, M17_LSF_TYPE2_BITS, m17_puncture_pattern_1, M17_PUNCTURE_P1_LEN, lsf_punctured,
                            M17_PAYLOAD_BITS, NULL);
    for (int i = 0; i < M17_PAYLOAD_BITS; i++) {
        err |= expect_u32("LSF frame inverse", lsf_decoded[i], lsf_punctured[i]);
    }

    for (uint8_t cnt = 0U; cnt < M17_LICH_CHUNKS; cnt++) {
        DSD_MEMSET(lich, 0, sizeof(lich));
        DSD_MEMSET(encoded_lich, 0, sizeof(encoded_lich));
        DSD_MEMSET(decoded_lich, 0, sizeof(decoded_lich));

        err |= expect_int("LICH build content", m17_lich_build_content(lsf, cnt, lich), 0);
        for (int i = 0; i < M17_LICH_CHUNK_BITS; i++) {
            err |= expect_u32("LICH chunk bit", lich[i], lsf[((size_t)cnt * M17_LICH_CHUNK_BITS) + i]);
        }
        err |= expect_u32("LICH counter high", lich[40], (cnt >> 2U) & 1U);
        err |= expect_u32("LICH counter mid", lich[41], (cnt >> 1U) & 1U);
        err |= expect_u32("LICH counter low", lich[42], cnt & 1U);
        err |= expect_int("LICH reserved ones", count_ones(&lich[43], M17_LICH_RESERVED_BITS), 0);

        m17_lich_encode_bits(lich, encoded_lich);
        err |= expect_u32("LICH first block data", bits_to_u32(encoded_lich, M17_LICH_GOLAY_DATA_BITS),
                          bits_to_u32(lich, M17_LICH_GOLAY_DATA_BITS));
        err |= expect_int("LICH decode bits", m17_lich_decode_bits(encoded_lich, decoded_lich), 0);
        for (int i = 0; i < M17_LICH_CONTENT_BITS; i++) {
            err |= expect_u32("LICH decoded content", decoded_lich[i], lich[i]);
        }

        uint8_t parsed_cnt = 0xFFU;
        uint8_t parsed_reserved = 0xFFU;
        err |= expect_int("LICH parse content", m17_lich_parse_content(decoded_lich, &parsed_cnt, &parsed_reserved), 0);
        err |= expect_u32("LICH parsed counter", parsed_cnt, cnt);
        err |= expect_u32("LICH parsed reserved", parsed_reserved, 0U);
    }

    DSD_MEMSET(lich, 0, sizeof(lich));
    err |= expect_int("LICH build invalid cnt", m17_lich_build_content(lsf, M17_LICH_CHUNKS, lich), -1);
    lich[40] = 1U;
    lich[41] = 1U;
    lich[42] = 0U;
    err |= expect_int("LICH parse invalid cnt 6", m17_lich_parse_content(lich, NULL, NULL), -1);

    DSD_MEMSET(lich, 0, sizeof(lich));
    DSD_MEMSET(encoded_lich, 0, sizeof(encoded_lich));
    lich[0] = 1U;
    m17_lich_encode_bits(lich, encoded_lich);
    err |=
        expect_u32("LICH Golay expansion first block", bits_to_u32(encoded_lich, M17_LICH_GOLAY_CODE_BITS), 0x800C75U);
    err |= expect_u32("LICH Golay expansion second block",
                      bits_to_u32(&encoded_lich[M17_LICH_GOLAY_CODE_BITS], M17_LICH_GOLAY_CODE_BITS), 0U);

    return err;
}

static int
test_stream_content_helpers(void) {
    int err = 0;

    uint8_t payload[M17_STREAM_PAYLOAD_BITS];
    uint8_t first_half[M17_STREAM_PAYLOAD_HALF_BITS];
    uint8_t second_half[M17_STREAM_PAYLOAD_HALF_BITS];
    uint8_t type1[M17_STREAM_TYPE1_FLUSH_BITS];
    uint8_t parsed_payload[M17_STREAM_PAYLOAD_BITS];
    for (int i = 0; i < M17_STREAM_PAYLOAD_HALF_BITS; i++) {
        first_half[i] = (uint8_t)((i * 11 + 3) & 1U);
        second_half[i] = (uint8_t)((i * 7 + 1) & 1U);
    }
    m17_stream_pack_payload_halves(first_half, second_half, payload);
    for (int i = 0; i < M17_STREAM_PAYLOAD_HALF_BITS; i++) {
        err |= expect_u32("stream first half", payload[i], first_half[i]);
        err |= expect_u32("stream second half", payload[M17_STREAM_PAYLOAD_HALF_BITS + i], second_half[i]);
    }

    DSD_MEMSET(type1, 0x01, sizeof(type1));
    m17_stream_build_type1_bits(0x1234U, payload, type1);
    err |= expect_u32("stream FN field", bits_to_u32(type1, M17_STREAM_FN_BITS), 0x1234U);
    for (int i = 0; i < M17_STREAM_PAYLOAD_BITS; i++) {
        err |= expect_u32("stream payload bit", type1[M17_STREAM_FN_BITS + i], payload[i]);
    }
    err |= expect_int("stream flush bits", count_ones(&type1[M17_STREAM_CONTENT_BITS], M17_STREAM_FLUSH_BITS), 0);

    uint16_t parsed_fn = 0U;
    DSD_MEMSET(parsed_payload, 0, sizeof(parsed_payload));
    err |= expect_int("stream parse type1", m17_stream_parse_type1_bits(type1, &parsed_fn, parsed_payload), 0);
    err |= expect_u32("stream parsed FN", parsed_fn, 0x1234U);
    for (int i = 0; i < M17_STREAM_PAYLOAD_BITS; i++) {
        err |= expect_u32("stream parsed payload", parsed_payload[i], payload[i]);
    }

    m17_stream_build_type1_bits((uint16_t)(M17_STREAM_FRAME_END_MASK | 0x0123U), payload, type1);
    err |= expect_u32("stream EoT bit", type1[0], 1U);
    err |= expect_u32("stream EoT counter", bits_to_u32(&type1[1], 15), 0x0123U);
    err |= expect_u32("stream next 0", m17_stream_next_frame_counter(0U), 1U);
    err |= expect_u32("stream next max wraps", m17_stream_next_frame_counter(M17_STREAM_FRAME_COUNTER_MAX), 0U);
    err |= expect_u32("stream next masks EoT", m17_stream_next_frame_counter(M17_STREAM_FRAME_END_MASK), 1U);

    uint8_t lich[M17_LICH_CONTENT_BITS];
    uint8_t lich_encoded[M17_LICH_BITS];
    uint8_t stream_punctured[M17_STREAM_PUNCTURED_BITS];
    uint8_t combined[M17_PAYLOAD_BITS];
    DSD_MEMSET(lich, 0, sizeof(lich));
    DSD_MEMSET(lich_encoded, 0, sizeof(lich_encoded));
    DSD_MEMSET(stream_punctured, 0, sizeof(stream_punctured));
    DSD_MEMSET(combined, 0, sizeof(combined));

    lich[0] = 1U;
    m17_lich_encode_bits(lich, lich_encoded);
    m17_stream_encode_type1_bits(type1, stream_punctured);
    m17_stream_combine_frame_bits(lich_encoded, stream_punctured, combined);
    for (int i = 0; i < M17_LICH_BITS; i++) {
        err |= expect_u32("stream combiner LICH", combined[i], lich_encoded[i]);
    }
    for (int i = 0; i < M17_STREAM_PUNCTURED_BITS; i++) {
        err |= expect_u32("stream combiner payload", combined[M17_LICH_BITS + i], stream_punctured[i]);
    }

    return err;
}

static int
test_packet_and_bert_frame_helpers(void) {
    int err = 0;

    uint8_t bert_payload[M17_BERT_PAYLOAD_BITS];
    uint8_t bert_type1[M17_BERT_TYPE1_FLUSH_BITS];
    uint8_t bert_frame[M17_PAYLOAD_BITS];
    uint8_t bert_interleaved[M17_PAYLOAD_BITS];
    uint8_t bert_type3[M17_PAYLOAD_BITS];
    uint8_t bert_type2[M17_BERT_TYPE2_BITS];
    uint8_t bert_expected_type3[M17_PAYLOAD_BITS];
    for (int i = 0; i < M17_BERT_PAYLOAD_BITS; i++) {
        bert_payload[i] = (uint8_t)((i * 5 + 2) & 1U);
    }
    DSD_MEMSET(bert_type1, 0, sizeof(bert_type1));
    DSD_MEMSET(bert_frame, 0, sizeof(bert_frame));
    DSD_MEMSET(bert_interleaved, 0, sizeof(bert_interleaved));
    DSD_MEMSET(bert_type3, 0, sizeof(bert_type3));
    DSD_MEMSET(bert_type2, 0, sizeof(bert_type2));
    DSD_MEMSET(bert_expected_type3, 0, sizeof(bert_expected_type3));
    DSD_MEMCPY(bert_type1, bert_payload, sizeof(bert_payload));

    uint16_t consumed = 0U;
    uint16_t out = m17_bert_encode_type1_bits(bert_type1, bert_frame, &consumed);
    err |= expect_int("BERT frame output bits", out, M17_PAYLOAD_BITS);
    err |= expect_int("BERT frame consumed bits", consumed, M17_BERT_TYPE2_BITS);
    m17_randomize_368_bits(bert_frame, bert_interleaved);
    m17_deinterleave_368_bits(bert_interleaved, bert_type3);
    m17_convolution_encode_bits(bert_type1, bert_type2, M17_BERT_TYPE1_FLUSH_BITS);
    (void)m17_puncture_bits(bert_type2, M17_BERT_TYPE2_BITS, m17_puncture_pattern_2, M17_PUNCTURE_P2_LEN,
                            bert_expected_type3, M17_PAYLOAD_BITS, NULL);
    for (int i = 0; i < M17_PAYLOAD_BITS; i++) {
        err |= expect_u32("BERT frame inverse", bert_type3[i], bert_expected_type3[i]);
    }

    uint8_t packet_chunk[M17_PACKET_CHUNK_BITS];
    uint8_t packet_type1[M17_PACKET_TYPE1_FLUSH_BITS];
    uint8_t packet_frame[M17_PAYLOAD_BITS];
    uint8_t packet_interleaved[M17_PAYLOAD_BITS];
    uint8_t packet_type3[M17_PAYLOAD_BITS];
    uint8_t packet_type2[M17_PACKET_TYPE2_BITS];
    uint8_t packet_expected_type3[M17_PAYLOAD_BITS];
    for (int i = 0; i < M17_PACKET_CHUNK_BITS; i++) {
        packet_chunk[i] = (uint8_t)((i * 3 + 1) & 1U);
    }
    DSD_MEMSET(packet_type1, 0, sizeof(packet_type1));
    DSD_MEMSET(packet_frame, 0, sizeof(packet_frame));
    DSD_MEMSET(packet_interleaved, 0, sizeof(packet_interleaved));
    DSD_MEMSET(packet_type3, 0, sizeof(packet_type3));
    DSD_MEMSET(packet_type2, 0, sizeof(packet_type2));
    DSD_MEMSET(packet_expected_type3, 0, sizeof(packet_expected_type3));

    const uint8_t metadata = 0xE4U;
    m17_packet_build_type1_bits(packet_chunk, metadata, packet_type1);
    for (int i = 0; i < M17_PACKET_CHUNK_BITS; i++) {
        err |= expect_u32("packet type1 chunk", packet_type1[i], packet_chunk[i]);
    }
    for (int i = 0; i < M17_PACKET_METADATA_BITS; i++) {
        err |= expect_u32("packet metadata bit", packet_type1[M17_PACKET_CHUNK_BITS + i], (metadata >> (7 - i)) & 1U);
    }
    err |=
        expect_int("packet flush bits", count_ones(&packet_type1[M17_PACKET_CONTENT_BITS], M17_PACKET_FLUSH_BITS), 0);

    consumed = 0U;
    out = m17_packet_encode_type1_bits(packet_type1, packet_frame, &consumed);
    err |= expect_int("packet frame output bits", out, M17_PAYLOAD_BITS);
    err |= expect_int("packet frame consumed bits", consumed, M17_PACKET_TYPE2_BITS);
    m17_randomize_368_bits(packet_frame, packet_interleaved);
    m17_deinterleave_368_bits(packet_interleaved, packet_type3);
    m17_convolution_encode_bits(packet_type1, packet_type2, M17_PACKET_TYPE1_FLUSH_BITS);
    (void)m17_puncture_bits(packet_type2, M17_PACKET_TYPE2_BITS, m17_puncture_pattern_3, M17_PUNCTURE_P3_LEN,
                            packet_expected_type3, M17_PAYLOAD_BITS, NULL);
    for (int i = 0; i < M17_PAYLOAD_BITS; i++) {
        err |= expect_u32("packet frame inverse", packet_type3[i], packet_expected_type3[i]);
    }

    return err;
}

static int
test_prbs9_sequence(void) {
    int err = 0;
    uint16_t lfsr = 1U;
    const uint8_t want[32] = {0, 0, 0, 0, 1, 0, 0, 0, 1, 1, 0, 0, 0, 0, 1, 0,
                              0, 1, 1, 1, 0, 0, 1, 0, 1, 0, 1, 0, 1, 1, 0, 0};
    for (int i = 0; i < 32; i++) {
        err |= expect_u32("PRBS9 bit", m17_prbs9_next_bit(&lfsr), want[i]);
    }
    err |= expect_u32("PRBS9 state", lfsr, 0x0ACU);
    return err;
}

static int
test_prbs9_fill_bits(void) {
    int err = 0;
    uint16_t fill_lfsr = 1U;
    uint16_t step_lfsr = 1U;
    uint8_t bits[M17_BERT_PAYLOAD_BITS * 2U];
    DSD_MEMSET(bits, 0, sizeof(bits));

    m17_prbs9_fill_bits(&fill_lfsr, bits, M17_BERT_PAYLOAD_BITS);
    m17_prbs9_fill_bits(&fill_lfsr, &bits[M17_BERT_PAYLOAD_BITS], M17_BERT_PAYLOAD_BITS);

    for (int i = 0; i < (M17_BERT_PAYLOAD_BITS * 2); i++) {
        err |= expect_u32("PRBS9 filled bit", bits[i], m17_prbs9_next_bit(&step_lfsr));
    }
    err |= expect_u32("PRBS9 filled state", fill_lfsr, step_lfsr);

    uint16_t zero_lfsr = 0U;
    err |= expect_u32("PRBS9 zero state first bit", m17_prbs9_next_bit(&zero_lfsr), 0U);
    err |= expect_u32("PRBS9 zero state advances from one", zero_lfsr, 2U);
    return err;
}

static int
test_prbs9_receiver(void) {
    int err = 0;
    uint16_t tx = 1U;
    m17_prbs9_rx_state rx;
    m17_prbs9_rx_init(&rx, 1U);

    for (int i = 0; i < M17_PRBS9_LOCK_BITS; i++) {
        const uint8_t bit = (uint8_t)m17_prbs9_next_bit(&tx);
        err |= expect_int("PRBS9 rx sync bit not counted", m17_prbs9_rx_push_bit(&rx, bit), 0);
    }
    err |= expect_int("PRBS9 rx locked", rx.locked, 1);
    err |= expect_int("PRBS9 rx total bits at lock", (int)rx.total_bits, 0);

    for (int i = 0; i < 16; i++) {
        const uint8_t bit = (uint8_t)m17_prbs9_next_bit(&tx);
        err |= expect_int("PRBS9 rx valid bit", m17_prbs9_rx_push_bit(&rx, bit), 1);
    }
    err |= expect_int("PRBS9 rx counted bits", (int)rx.total_bits, 16);
    err |= expect_int("PRBS9 rx errors", (int)rx.total_errors, 0);

    for (int i = 0; i < M17_PRBS9_RESYNC_WINDOW_BITS; i++) {
        const uint8_t bit = (uint8_t)(m17_prbs9_next_bit(&tx) ^ (i < 19 ? 1U : 0U));
        (void)m17_prbs9_rx_push_bit(&rx, bit);
    }
    err |= expect_int("PRBS9 rx resync count", (int)rx.resync_count, 1);
    err |= expect_int("PRBS9 rx unlocked after threshold", rx.locked, 0);
    err |= expect_int("PRBS9 rx threshold errors", (int)rx.total_errors, 19);
    return err;
}

static int
test_scrambler_helpers(void) {
    int err = 0;

    err |= expect_u32("scrambler subtype 0 mask", m17_scrambler_mask_for_subtype(0U), M17_SCRAMBLER_8BIT_MASK);
    err |= expect_u32("scrambler subtype 1 mask", m17_scrambler_mask_for_subtype(1U), M17_SCRAMBLER_16BIT_MASK);
    err |= expect_u32("scrambler subtype 2 mask", m17_scrambler_mask_for_subtype(2U), M17_SCRAMBLER_24BIT_MASK);
    err |= expect_u32("scrambler reserved mask", m17_scrambler_mask_for_subtype(3U), 0U);

    static const uint8_t want0[M17_SIGNATURE_DIGEST_BYTES] = {0x4EU, 0xECU, 0xF7U, 0xE9U, 0x9AU, 0x8CU, 0x1DU, 0x57U,
                                                              0xCAU, 0x13U, 0xFCU, 0x2FU, 0x1AU, 0x02U, 0x38U, 0x97U};
    static const uint8_t want1[M17_SIGNATURE_DIGEST_BYTES] = {0xC8U, 0xC2U, 0x3CU, 0x9EU, 0x45U, 0x00U, 0xE9U, 0x9FU,
                                                              0x8EU, 0xB7U, 0x9FU, 0x9DU, 0x84U, 0x0AU, 0x0CU, 0x86U};
    static const uint8_t want2[M17_SIGNATURE_DIGEST_BYTES] = {0x3DU, 0x08U, 0x94U, 0x37U, 0x71U, 0xF7U, 0x3CU, 0xAEU,
                                                              0x5BU, 0xE0U, 0x66U, 0x73U, 0x92U, 0x0AU, 0x93U, 0xFBU};
    static const uint8_t want2_fn3[M17_SIGNATURE_DIGEST_BYTES] = {
        0x4BU, 0x33U, 0xA5U, 0x68U, 0x48U, 0xCEU, 0x3DU, 0x9DU, 0x74U, 0x7EU, 0x68U, 0x72U, 0x4FU, 0x20U, 0x78U, 0x7DU};

    uint8_t zero_bits[M17_STREAM_PAYLOAD_BITS];
    uint8_t scrambled_bits[M17_STREAM_PAYLOAD_BITS];
    uint8_t descrambled_bits[M17_STREAM_PAYLOAD_BITS];
    uint8_t bytes[M17_SIGNATURE_DIGEST_BYTES];
    DSD_MEMSET(zero_bits, 0, sizeof(zero_bits));
    DSD_MEMSET(scrambled_bits, 0, sizeof(scrambled_bits));
    DSD_MEMSET(descrambled_bits, 0, sizeof(descrambled_bits));
    DSD_MEMSET(bytes, 0, sizeof(bytes));

    err |= expect_int("scrambler subtype 0 apply",
                      m17_scrambler_apply_bits(0U, 0xA5U, 0U, zero_bits, scrambled_bits, M17_STREAM_PAYLOAD_BITS), 0);
    bits_to_bytes(scrambled_bits, bytes, sizeof(bytes));
    err |= expect_bytes("scrambler subtype 0 vector", bytes, want0, sizeof(bytes));

    err |= expect_int("scrambler subtype 1 apply",
                      m17_scrambler_apply_bits(1U, 0xBEEFU, 0U, zero_bits, scrambled_bits, M17_STREAM_PAYLOAD_BITS), 0);
    bits_to_bytes(scrambled_bits, bytes, sizeof(bytes));
    err |= expect_bytes("scrambler subtype 1 vector", bytes, want1, sizeof(bytes));

    err |=
        expect_int("scrambler subtype 2 apply",
                   m17_scrambler_apply_bits(2U, 0xC0FFEEU, 0U, zero_bits, scrambled_bits, M17_STREAM_PAYLOAD_BITS), 0);
    bits_to_bytes(scrambled_bits, bytes, sizeof(bytes));
    err |= expect_bytes("scrambler subtype 2 vector", bytes, want2, sizeof(bytes));

    err |=
        expect_int("scrambler subtype 2 frame advance",
                   m17_scrambler_apply_bits(2U, 0xC0FFEEU, 3U, zero_bits, scrambled_bits, M17_STREAM_PAYLOAD_BITS), 0);
    bits_to_bytes(scrambled_bits, bytes, sizeof(bytes));
    err |= expect_bytes("scrambler subtype 2 fn3 vector", bytes, want2_fn3, sizeof(bytes));

    err |= expect_int(
        "scrambler self inverse",
        m17_scrambler_apply_bits(2U, 0xC0FFEEU, 3U, scrambled_bits, descrambled_bits, M17_STREAM_PAYLOAD_BITS), 0);
    for (int i = 0; i < M17_STREAM_PAYLOAD_BITS; i++) {
        err |= expect_u32("scrambler recovered bit", descrambled_bits[i], 0U);
    }
    err |= expect_int("scrambler zero seed rejected",
                      m17_scrambler_apply_bits(2U, 0U, 0U, zero_bits, scrambled_bits, M17_STREAM_PAYLOAD_BITS), -1);
    err |= expect_int("scrambler reserved subtype rejected",
                      m17_scrambler_apply_bits(3U, 1U, 0U, zero_bits, scrambled_bits, M17_STREAM_PAYLOAD_BITS), -1);

    uint8_t input_bytes[M17_SIGNATURE_DIGEST_BYTES];
    uint8_t input_bits[M17_STREAM_PAYLOAD_BITS];
    for (size_t i = 0U; i < M17_SIGNATURE_DIGEST_BYTES; i++) {
        input_bytes[i] = (uint8_t)(0xA0U + (uint8_t)i);
    }
    bytes_to_bits(input_bytes, input_bits, sizeof(input_bytes));
    err |= expect_int("scrambler arbitrary input",
                      m17_scrambler_apply_bits(0U, 0xA5U, 0U, input_bits, scrambled_bits, M17_STREAM_PAYLOAD_BITS), 0);
    err |= expect_int(
        "scrambler arbitrary recover",
        m17_scrambler_apply_bits(0U, 0xA5U, 0U, scrambled_bits, descrambled_bits, M17_STREAM_PAYLOAD_BITS), 0);
    for (int i = 0; i < M17_STREAM_PAYLOAD_BITS; i++) {
        err |= expect_u32("scrambler arbitrary recovered bit", descrambled_bits[i], input_bits[i]);
    }

    return err;
}

static int
test_aes_helpers(void) {
    int err = 0;

    err |= expect_int("AES subtype 0 key bytes", m17_aes_key_bytes_for_subtype(0U), M17_AES_128_KEY_BYTES);
    err |= expect_int("AES subtype 1 key bytes", m17_aes_key_bytes_for_subtype(1U), M17_AES_192_KEY_BYTES);
    err |= expect_int("AES subtype 2 key bytes", m17_aes_key_bytes_for_subtype(2U), M17_AES_256_KEY_BYTES);
    err |= expect_int("AES reserved key bytes", m17_aes_key_bytes_for_subtype(3U), 0);

    uint8_t nonce[M17_AES_NONCE_BYTES];
    uint8_t counter[M17_AES_COUNTER_BYTES];
    for (int i = 0; i < M17_AES_NONCE_BYTES; i++) {
        nonce[i] = (uint8_t)i;
    }
    DSD_MEMSET(counter, 0, sizeof(counter));
    m17_aes_build_counter(nonce, (uint16_t)(0x8000U | 0x0123U), counter);
    for (int i = 0; i < M17_AES_NONCE_BYTES; i++) {
        err |= expect_u32("AES nonce byte", counter[i], nonce[i]);
    }
    err |= expect_u32("AES counter high masks EoT", counter[14], 0x01U);
    err |= expect_u32("AES counter low", counter[15], 0x23U);

    m17_aes_build_counter(nonce, 0xFFFFU, counter);
    err |= expect_u32("AES final FN counter high masks EoT", counter[14], 0x7FU);
    err |= expect_u32("AES final FN counter low", counter[15], 0xFFU);
    return err;
}

int
main(void) {
    int err = 0;
    err |= test_physical_parameters();
    err |= test_crc16_vectors();
    err |= test_dibit_symbol_mapping();
    err |= test_physical_words();
    err |= test_packet_layout_helpers();
    err |= test_base40_encode();
    err |= test_randomizer_and_puncturing_tables();
    err |= test_fec_stage_helpers();
    err |= test_golay_helpers();
    err |= test_lich_helpers();
    err |= test_stream_content_helpers();
    err |= test_packet_and_bert_frame_helpers();
    err |= test_prbs9_sequence();
    err |= test_prbs9_fill_bits();
    err |= test_prbs9_receiver();
    err |= test_scrambler_helpers();
    err |= test_aes_helpers();

    if (err == 0) {
        printf("M17_ALGORITHMS: OK\n");
    }
    return err;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
