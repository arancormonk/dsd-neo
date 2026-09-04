// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Focused checks for M17 parsing helpers.
 *
 * Builds a synthetic LSF bit buffer with known dst/src IDs, type
 * fields, and META bytes and verifies m17_parse_lsf() decodes them
 * into the expected m17_lsf_result.
 */

#include <dsd-neo/protocol/m17/m17_parse.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "dsd-neo/core/safe_api.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

// MSB-first test-vector writer.
static void
write_bits_from_u64(uint8_t* dst, uint64_t value, uint32_t nbits) {
    for (uint32_t i = 0; i < nbits; i++) {
        uint32_t shift = nbits - 1U - i;
        dst[i] = (uint8_t)((value >> shift) & 1U);
    }
}

static int
expect_eq_u64(const char* tag, uint64_t got, uint64_t want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %llu want %llu\n", tag, (unsigned long long)got, (unsigned long long)want);
        return 1;
    }
    return 0;
}

static int
expect_eq_u8(const char* tag, uint8_t got, uint8_t want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %u want %u\n", tag, (unsigned)got, (unsigned)want);
        return 1;
    }
    return 0;
}

static int
expect_close_double(const char* tag, double got, double want, double tolerance) {
    double diff = got - want;
    if (diff < 0.0) {
        diff = -diff;
    }
    if (diff > tolerance) {
        DSD_FPRINTF(stderr, "%s: got %.9f want %.9f\n", tag, got, want);
        return 1;
    }
    return 0;
}

static void
build_lsf_bits(uint8_t lsf_bits[240], uint64_t dst, uint64_t src, uint16_t lsf_type, const uint8_t meta[14]) {
    DSD_MEMSET(lsf_bits, 0, 240U);

    write_bits_from_u64(&lsf_bits[0], dst, 48U);
    write_bits_from_u64(&lsf_bits[48], src, 48U);
    write_bits_from_u64(&lsf_bits[96], (uint64_t)lsf_type, 16U);

    for (int i = 0; i < 14; i++) {
        write_bits_from_u64(&lsf_bits[112 + (i * 8)], meta[i], 8U);
    }
}

static int
test_parse_lsf_v2(void) {
    // Choose arbitrary but distinct dst/src values within the valid range.
    const uint64_t dst = 0x0000ABCDEF12ULL;
    const uint64_t src = 0x000012345678ULL;

    // Type word fields (packed into lsf_type as in m17_parse_lsf).
    const uint8_t dt = 2U;
    const uint8_t et = 1U;
    const uint8_t es = 2U;
    const uint8_t cn = 9U;
    const uint8_t rs = 1U;

    uint16_t lsf_type = 0;
    lsf_type |= 1U;
    lsf_type |= (uint16_t)((uint16_t)dt << 1);
    lsf_type |= (uint16_t)((uint16_t)et << 3);
    lsf_type |= (uint16_t)((uint16_t)es << 5);
    lsf_type |= (uint16_t)((uint16_t)cn << 7);
    lsf_type |= (uint16_t)((uint16_t)rs << 11);

    // META/IV bytes: make the first byte non-zero so has_meta=1.
    uint8_t meta[14];
    DSD_MEMSET(meta, 0, sizeof(meta));
    meta[0] = 0x42U;
    meta[1] = 0x99U;

    uint8_t lsf_bits[240];
    build_lsf_bits(lsf_bits, dst, src, lsf_type, meta);

    struct m17_lsf_result res;
    int rc = m17_parse_lsf(lsf_bits, sizeof(lsf_bits), &res);
    if (rc != 0) {
        DSD_FPRINTF(stderr, "m17_parse_lsf failed: rc=%d\n", rc);
        return 1;
    }

    int err = 0;
    err |= expect_eq_u64("dst", res.dst, dst);
    err |= expect_eq_u64("src", res.src, src);
    err |= expect_eq_u64("type_word", res.type_word, lsf_type);
    err |= expect_eq_u8("version", res.version, 2U);
    err |= expect_eq_u8("packet_stream", res.packet_stream, 1U);
    err |= expect_eq_u8("dt", res.dt, dt);
    err |= expect_eq_u8("et", res.et, et);
    err |= expect_eq_u8("es", res.es, es);
    err |= expect_eq_u8("cn", res.cn, cn);
    err |= expect_eq_u8("rs", res.rs, 0U);
    err |= expect_eq_u8("signature", res.signature, 1U);
    err |= expect_eq_u8("meta_is_iv", res.meta_is_iv, 0U);
    err |= expect_eq_u8("dst kind", res.dst_address_kind, M17_ADDRESS_STANDARD);
    err |= expect_eq_u8("src kind", res.src_address_kind, M17_ADDRESS_STANDARD);
    err |= expect_eq_u8("dst valid", res.dst_is_valid, 1U);
    err |= expect_eq_u8("src valid", res.src_is_valid, 1U);
    err |= expect_eq_u8("type reserved valid", res.type_reserved_valid, 1U);
    err |= expect_eq_u8("has_meta", res.has_meta, 1U);

    if (res.meta[0] != meta[0] || res.meta[1] != meta[1]) {
        DSD_FPRINTF(stderr, "meta[0..1]: got %02X %02X want %02X %02X\n", res.meta[0], res.meta[1], meta[0], meta[1]);
        err |= 1;
    }

    return err;
}

static int
test_parse_lsf_big_endian_layout(void) {
    const uint64_t dst = 0x010203040506ULL;
    const uint64_t src = 0x112233445566ULL;
    const uint16_t lsf_type = 0x05AAU;
    uint8_t meta[14];
    DSD_MEMSET(meta, 0, sizeof(meta));
    meta[0] = 0x80U;
    meta[1] = 0x01U;
    meta[13] = 0x7FU;

    uint8_t lsf_bits[240];
    build_lsf_bits(lsf_bits, dst, src, lsf_type, meta);

    struct m17_lsf_result res;
    const int rc = m17_parse_lsf(lsf_bits, sizeof(lsf_bits), &res);
    if (rc != 0) {
        DSD_FPRINTF(stderr, "m17_parse_lsf big-endian failed: rc=%d\n", rc);
        return 1;
    }

    int err = 0;
    err |= expect_eq_u64("big-endian dst", res.dst, dst);
    err |= expect_eq_u64("big-endian src", res.src, src);
    err |= expect_eq_u64("big-endian type", res.type_word, lsf_type);
    err |= expect_eq_u8("big-endian meta first", res.meta[0], meta[0]);
    err |= expect_eq_u8("big-endian meta second", res.meta[1], meta[1]);
    err |= expect_eq_u8("big-endian meta last", res.meta[13], meta[13]);
    return err;
}

static int
test_parse_lsf_spec_reserved_bits(void) {
    const uint64_t dst = 0x000000100001ULL;
    const uint64_t src = 0x000000200002ULL;
    const uint16_t lsf_type =
        (uint16_t)((0xBU << 12U) | (1U << 11U) | (0x7U << 7U) | (0x2U << 5U) | (0x1U << 3U) | (0x3U << 1U) | 1U);

    uint8_t meta[14];
    DSD_MEMSET(meta, 0, sizeof(meta));
    meta[0] = 0xABU;
    meta[13] = 0xCDU;

    uint8_t lsf_bits[240];
    build_lsf_bits(lsf_bits, dst, src, lsf_type, meta);

    struct m17_lsf_result res;
    const int rc = m17_parse_lsf(lsf_bits, sizeof(lsf_bits), &res);
    if (rc != 0) {
        DSD_FPRINTF(stderr, "m17_parse_lsf reserved bits failed: rc=%d\n", rc);
        return 1;
    }

    int err = 0;
    err |= expect_eq_u8("spec reserved version", res.version, 2U);
    err |= expect_eq_u8("spec reserved packet_stream", res.packet_stream, 1U);
    err |= expect_eq_u8("spec reserved dt", res.dt, 3U);
    err |= expect_eq_u8("spec reserved et", res.et, 1U);
    err |= expect_eq_u8("spec reserved es", res.es, 2U);
    err |= expect_eq_u8("spec reserved cn", res.cn, 7U);
    err |= expect_eq_u8("spec reserved signature", res.signature, 1U);
    err |= expect_eq_u8("spec reserved rs", res.rs, 0xBU);
    err |= expect_eq_u8("spec reserved meta_is_iv", res.meta_is_iv, 0U);
    err |= expect_eq_u8("spec reserved has_meta", res.has_meta, 1U);
    err |= expect_eq_u8("spec reserved type invalid", res.type_reserved_valid, 0U);
    err |= expect_eq_u8("spec reserved meta first", res.meta[0], meta[0]);
    err |= expect_eq_u8("spec reserved meta last", res.meta[13], meta[13]);
    return err;
}

static int
test_lsf_address_and_meta_helpers(void) {
    int err = 0;
    err |= expect_eq_u8("address reserved", m17_address_classify(0ULL), M17_ADDRESS_RESERVED);
    err |= expect_eq_u8("address standard", m17_address_classify(0x9FDD51ULL), M17_ADDRESS_STANDARD);
    err |= expect_eq_u8("address extended", m17_address_classify(M17_ADDRESS_EXTENDED_MIN), M17_ADDRESS_EXTENDED);
    err |= expect_eq_u8("address broadcast", m17_address_classify(M17_ADDRESS_BROADCAST), M17_ADDRESS_BROADCAST_KIND);

    uint8_t meta[M17_META_BYTES] = {0};
    uint8_t lsf_bits[240];
    struct m17_lsf_result res;
    build_lsf_bits(lsf_bits, M17_ADDRESS_EXTENDED_MIN, 0x9FDD51ULL, 0U, meta);
    err |= expect_eq_u64("extended destination parse", (uint64_t)m17_parse_lsf(lsf_bits, sizeof(lsf_bits), &res), 0ULL);
    err |= expect_eq_u8("extended destination valid", res.dst_is_valid, 1U);
    err |= expect_eq_u8("standard source valid", res.src_is_valid, 1U);
    build_lsf_bits(lsf_bits, M17_ADDRESS_BROADCAST, M17_ADDRESS_EXTENDED_MIN, 0U, meta);
    err |=
        expect_eq_u64("broadcast destination parse", (uint64_t)m17_parse_lsf(lsf_bits, sizeof(lsf_bits), &res), 0ULL);
    err |= expect_eq_u8("broadcast destination valid", res.dst_is_valid, 1U);
    err |= expect_eq_u8("extended source invalid", res.src_is_valid, 0U);
    build_lsf_bits(lsf_bits, 0ULL, M17_ADDRESS_BROADCAST, 0U, meta);
    err |= expect_eq_u64("reserved destination parse", (uint64_t)m17_parse_lsf(lsf_bits, sizeof(lsf_bits), &res), 0ULL);
    err |= expect_eq_u8("reserved destination invalid", res.dst_is_valid, 0U);
    err |= expect_eq_u8("broadcast source invalid", res.src_is_valid, 0U);

    err |= expect_eq_u8("null meta text", m17_null_meta_protocol_for_subtype(0U), 0x80U);
    err |= expect_eq_u8("null meta gnss", m17_null_meta_protocol_for_subtype(1U), 0x81U);
    err |= expect_eq_u8("null meta ext", m17_null_meta_protocol_for_subtype(2U), 0x82U);
    err |= expect_eq_u8("null meta reserved", m17_null_meta_protocol_for_subtype(3U), 0U);
    err |= expect_eq_u8("CAN disabled allows", (uint8_t)m17_can_filter_allows(-1, 9U), 1U);
    err |= expect_eq_u8("CAN match allows", (uint8_t)m17_can_filter_allows(9, 9U), 1U);
    err |= expect_eq_u8("CAN mismatch rejects", (uint8_t)m17_can_filter_allows(9, 8U), 0U);
    err |= expect_eq_u8("CAN invalid configured rejects", (uint8_t)m17_can_filter_allows(16, 0U), 0U);

    char csd[10];
    DSD_MEMSET(csd, 0, sizeof(csd));
    if (m17_address_decode_csd(0x9FDD51ULL, csd) != 0 || strcmp(csd, "AB1CD") != 0) {
        DSD_FPRINTF(stderr, "address decode AB1CD: got '%s'\n", csd);
        err |= 1;
    }
    err |= expect_eq_u64("address decode extended rejected",
                         (uint64_t)m17_address_decode_csd(M17_ADDRESS_EXTENDED_MIN, csd), (uint64_t)-2);

    return err;
}

static int
test_stream_signature_frame_numbers(void) {
    int err = 0;
    err |= expect_eq_u64("stream signature below",
                         (uint64_t)m17_stream_signature_frame_index(M17_STREAM_SIGNATURE_FN0 - 1U), (uint64_t)-1);
    err |= expect_eq_u64("stream signature index first",
                         (uint64_t)m17_stream_signature_frame_index(M17_STREAM_SIGNATURE_FN0), 0ULL);
    err |= expect_eq_u64("stream signature index second",
                         (uint64_t)m17_stream_signature_frame_index(M17_STREAM_SIGNATURE_FN1), 1ULL);
    err |= expect_eq_u64("stream signature index third",
                         (uint64_t)m17_stream_signature_frame_index(M17_STREAM_SIGNATURE_FN2), 2ULL);
    err |= expect_eq_u64("stream signature low 0x7fff",
                         (uint64_t)m17_stream_signature_frame_index(M17_STREAM_FRAME_COUNTER_MAX), (uint64_t)-1);
    err |= expect_eq_u64("stream signature eot non-signature",
                         (uint64_t)m17_stream_signature_frame_index(M17_STREAM_FRAME_END_MASK), (uint64_t)-1);
    err |= expect_eq_u64("stream signature index final",
                         (uint64_t)m17_stream_signature_frame_index(M17_STREAM_SIGNATURE_FN3), 3ULL);
    return err;
}

static int
test_signature_digest(void) {
    uint8_t digest[M17_SIGNATURE_DIGEST_BYTES] = {0};
    uint8_t payload[M17_SIGNATURE_DIGEST_BYTES];
    for (uint8_t i = 0U; i < M17_SIGNATURE_DIGEST_BYTES; i++) {
        payload[i] = (uint8_t)(i + 1U);
    }
    m17_signature_digest_update(digest, payload);

    int err = 0;
    for (uint8_t i = 0U; i < M17_SIGNATURE_DIGEST_BYTES - 1U; i++) {
        err |= expect_eq_u8("digest rotated first update", digest[i], (uint8_t)(i + 2U));
    }
    err |= expect_eq_u8("digest rotated first byte to tail", digest[M17_SIGNATURE_DIGEST_BYTES - 1U], 1U);

    DSD_MEMSET(payload, 0xFF, sizeof(payload));
    m17_signature_digest_update(digest, payload);
    err |= expect_eq_u8("digest second update head", digest[0], (uint8_t)(3U ^ 0xFFU));
    return err;
}

static int
test_signature_collector(void) {
    int err = 0;
    struct m17_signature_collector collector = {0};
    uint8_t payload[M17_SIGNATURE_DIGEST_BYTES];

    for (uint8_t frame = 0U; frame < 4U; frame++) {
        for (uint8_t i = 0U; i < M17_SIGNATURE_DIGEST_BYTES; i++) {
            payload[i] = (uint8_t)(frame * M17_SIGNATURE_DIGEST_BYTES + i);
        }
        const uint16_t fn = (frame == 0U)   ? M17_STREAM_SIGNATURE_FN0
                            : (frame == 1U) ? M17_STREAM_SIGNATURE_FN1
                            : (frame == 2U) ? M17_STREAM_SIGNATURE_FN2
                                            : M17_STREAM_SIGNATURE_FN3;
        const int rc = m17_signature_collector_push(&collector, fn, payload);
        err |= expect_eq_u64("signature collector rc", (uint64_t)rc, frame == 3U ? 1ULL : 0ULL);
    }

    err |= expect_eq_u8("signature collector mask", collector.received_mask, 0x0FU);
    err |= expect_eq_u8("signature collector complete", collector.complete, 1U);
    err |= expect_eq_u8("signature collector bad sequence", collector.bad_sequence, 0U);
    for (uint8_t i = 0U; i < M17_SIGNATURE_BYTES; i++) {
        err |= expect_eq_u8("signature byte", collector.signature[i], i);
    }

    DSD_MEMSET(&collector, 0, sizeof collector);
    DSD_MEMSET(payload, 0xA5, sizeof(payload));
    err |= expect_eq_u64("signature invalid fn",
                         (uint64_t)m17_signature_collector_push(&collector, M17_STREAM_FRAME_COUNTER_MAX, payload),
                         (uint64_t)-2);
    err |= expect_eq_u64("signature out of order",
                         (uint64_t)m17_signature_collector_push(&collector, M17_STREAM_SIGNATURE_FN1, payload),
                         (uint64_t)-3);
    err |= expect_eq_u8("signature out of order bad sequence", collector.bad_sequence, 1U);
    err |= expect_eq_u8("signature out of order incomplete", collector.complete, 0U);
    return err;
}

static int
test_stream_1600_arbitrary_assemble(void) {
    uint8_t accumulator[48];
    uint8_t out_packet[49];
    DSD_MEMSET(accumulator, 0, sizeof(accumulator));
    DSD_MEMSET(out_packet, 0, sizeof(out_packet));

    int err = 0;
    for (uint16_t frame_number = 0; frame_number < 6U; frame_number++) {
        uint8_t chunk[8];
        for (uint8_t i = 0; i < 8U; i++) {
            chunk[i] = (uint8_t)((frame_number * 8U) + i + 1U);
        }

        const int rc = m17_stream_1600_arbitrary_assemble(accumulator, frame_number, chunk, out_packet);
        err |= expect_eq_u64("stream arbitrary assemble rc", (uint64_t)rc, (frame_number == 5U) ? 1ULL : 0ULL);
    }

    err |= expect_eq_u8("stream arbitrary protocol", out_packet[0], 0x99U);
    for (uint8_t i = 0; i < 48U; i++) {
        err |= expect_eq_u8("stream arbitrary payload", out_packet[i + 1U], (uint8_t)(i + 1U));
        err |= expect_eq_u8("stream arbitrary accumulator reset", accumulator[i], 0U);
    }

    uint8_t chunk[8] = {0xAAU, 0xBBU, 0xCCU, 0xDDU, 0xEEU, 0xF0U, 0x12U, 0x34U};
    int rc = m17_stream_1600_arbitrary_assemble(accumulator, 12U, chunk, out_packet);
    err |= expect_eq_u64("stream arbitrary wrapped rc", (uint64_t)rc, 0ULL);
    for (uint8_t i = 0; i < 8U; i++) {
        err |= expect_eq_u8("stream arbitrary wrapped chunk", accumulator[i], chunk[i]);
    }

    rc = m17_stream_1600_arbitrary_assemble(NULL, 0U, chunk, out_packet);
    if (rc != -1) {
        DSD_FPRINTF(stderr, "stream arbitrary null accumulator: got %d want -1\n", rc);
        err |= 1;
    }

    return err;
}

static int
test_parse_gnss_v2(void) {
    const uint16_t bearing = 300U;
    const uint32_t latitude_raw = 0x400000U;
    const uint32_t longitude_raw = 0xC00000U;
    const uint16_t altitude = 3000U;
    const uint16_t speed = 0x123U;
    const uint16_t reserved = 0x000U;

    uint8_t input[15];
    DSD_MEMSET(input, 0, sizeof(input));
    input[0] = 0x81U;
    input[1] = 0x12U;
    input[2] = (uint8_t)((0xFU << 4U) | (3U << 1U) | ((bearing >> 8U) & 0x1U));
    input[3] = (uint8_t)(bearing & 0xFFU);
    input[4] = (uint8_t)(latitude_raw >> 16U);
    input[5] = (uint8_t)(latitude_raw >> 8U);
    input[6] = (uint8_t)latitude_raw;
    input[7] = (uint8_t)(longitude_raw >> 16U);
    input[8] = (uint8_t)(longitude_raw >> 8U);
    input[9] = (uint8_t)longitude_raw;
    input[10] = (uint8_t)(altitude >> 8U);
    input[11] = (uint8_t)altitude;
    input[12] = (uint8_t)(speed >> 4U);
    input[13] = (uint8_t)(((speed & 0xFU) << 4U) | (reserved >> 8U));
    input[14] = (uint8_t)reserved;

    struct m17_gnss_result res;
    int rc = m17_parse_gnss_v2(input, sizeof(input), &res);
    if (rc != 0) {
        DSD_FPRINTF(stderr, "m17_parse_gnss_v2 failed: rc=%d\n", rc);
        return 1;
    }

    int err = 0;
    err |= expect_eq_u8("gnss data_source", res.data_source, 1U);
    err |= expect_eq_u8("gnss station_type", res.station_type, 2U);
    err |= expect_eq_u8("gnss validity", res.validity, 0xFU);
    err |= expect_eq_u8("gnss radius_exponent", res.radius_exponent, 3U);
    err |= expect_eq_u64("gnss bearing", res.bearing_deg, bearing);
    err |= expect_close_double("gnss latitude", res.latitude_deg, ((double)(int32_t)latitude_raw * 90.0) / 8388607.0,
                               0.000001);
    err |= expect_close_double("gnss longitude", res.longitude_deg, ((double)-4194304 * 180.0) / 8388607.0, 0.000001);
    err |= expect_close_double("gnss radius", res.radius_m, 8.0, 0.000001);
    err |= expect_close_double("gnss speed", res.speed_kmh, 145.5, 0.000001);
    err |= expect_close_double("gnss altitude", res.altitude_m, 1000.0, 0.000001);
    err |= expect_eq_u64("gnss reserved", res.reserved, reserved);
    err |= expect_eq_u8("gnss invalid-zero mask", res.invalid_zero_fields, 0U);

    input[0] = 0x91U;
    rc = m17_parse_gnss_v2(input, sizeof(input), &res);
    if (rc != 0) {
        DSD_FPRINTF(stderr, "m17_parse_gnss_v2 0x91 failed: rc=%d\n", rc);
        err |= 1;
    }

    input[0] = 0x05U;
    rc = m17_parse_gnss_v2(input, sizeof(input), &res);
    if (rc != -3) {
        DSD_FPRINTF(stderr, "m17_parse_gnss_v2 invalid protocol: got rc=%d want -3\n", rc);
        err |= 1;
    }

    rc = m17_parse_gnss_v2(input, 14U, &res);
    if (rc != -2) {
        DSD_FPRINTF(stderr, "m17_parse_gnss_v2 short input: got rc=%d want -2\n", rc);
        err |= 1;
    }

    input[0] = 0x81U;
    input[13] |= 0x01U;
    rc = m17_parse_gnss_v2(input, sizeof(input), &res);
    if (rc != -4) {
        DSD_FPRINTF(stderr, "m17_parse_gnss_v2 reserved bits: got rc=%d want -4\n", rc);
        err |= 1;
    }
    input[13] &= 0xF0U;

    input[2] = (uint8_t)((0x2U << 4U) | 0x01U);
    input[3] = 0x68U; // 360 degrees is invalid when velocity is marked valid.
    rc = m17_parse_gnss_v2(input, sizeof(input), &res);
    if (rc != -5) {
        DSD_FPRINTF(stderr, "m17_parse_gnss_v2 invalid bearing: got rc=%d want -5\n", rc);
        err |= 1;
    }

    input[2] = (uint8_t)(3U << 1U);
    input[3] = 0x01U;
    rc = m17_parse_gnss_v2(input, sizeof(input), &res);
    if (rc != 0 || res.invalid_zero_fields != 0xFU || res.latitude_deg != 0.0 || res.longitude_deg != 0.0
        || res.altitude_m != 0.0f || res.speed_kmh != 0.0f || res.bearing_deg != 0U || res.radius_m != 0.0f) {
        DSD_FPRINTF(stderr, "m17_parse_gnss_v2 invalid-field zeroing failed: rc=%d mask=%02X\n", rc,
                    res.invalid_zero_fields);
        err |= 1;
    }

    return err;
}

static void
write_be48(uint8_t* out, uint64_t value) {
    for (int i = 0; i < 6; i++) {
        out[i] = (uint8_t)(value >> (40U - ((uint32_t)i * 8U)));
    }
}

static int
test_text_meta_blocks(void) {
    uint8_t meta[M17_META_BYTES];
    struct m17_meta_text_block block;
    struct m17_meta_text_assembler assembler = {0};
    char text[M17_TEXT_MAX_BYTES + 1U];
    uint8_t text_len = 0U;
    int err = 0;

    DSD_MEMSET(meta, ' ', sizeof(meta));
    meta[0] = 0x00U;
    err |= expect_eq_u64("text no-data parse", (uint64_t)m17_meta_text_parse_block(meta, &block), 0ULL);
    err |= expect_eq_u8("text no-data flag", block.has_text, 0U);

    DSD_MEMSET(meta, ' ', sizeof(meta));
    meta[0] = 0x31U;
    DSD_MEMCPY(meta + 1, "ABCDEFGHIJKLM", M17_TEXT_BLOCK_BYTES);
    err |= expect_eq_u64("text block1 parse", (uint64_t)m17_meta_text_parse_block(meta, &block), 0ULL);
    err |= expect_eq_u8("text block1 total", block.total_blocks, 2U);
    err |= expect_eq_u8("text block1 index", block.block_index, 0U);
    err |= expect_eq_u64("text block1 incomplete",
                         (uint64_t)m17_meta_text_assembler_push(&assembler, &block, text, &text_len), 0ULL);

    DSD_MEMSET(meta, ' ', sizeof(meta));
    meta[0] = 0x32U;
    DSD_MEMCPY(meta + 1, "NO", 2U);
    err |= expect_eq_u64("text block2 parse", (uint64_t)m17_meta_text_parse_block(meta, &block), 0ULL);
    err |= expect_eq_u64("text block2 complete",
                         (uint64_t)m17_meta_text_assembler_push(&assembler, &block, text, &text_len), 1ULL);
    err |= expect_eq_u8("text complete len", text_len, 15U);
    if (strcmp(text, "ABCDEFGHIJKLMNO") != 0) {
        DSD_FPRINTF(stderr, "text complete got '%s'\n", text);
        err |= 1;
    }

    DSD_MEMSET(meta, ' ', sizeof(meta));
    meta[0] = 0x12U;
    err |= expect_eq_u64("text invalid segment", (uint64_t)m17_meta_text_parse_block(meta, &block), (uint64_t)-2);

    return err;
}

static int
test_extended_callsign_meta(void) {
    uint8_t input[15];
    DSD_MEMSET(input, 0, sizeof(input));
    input[0] = 0x82U;
    write_be48(input + 1, 0x9FDD51ULL);
    write_be48(input + 7, 0x9FDD51ULL);

    struct m17_extended_callsign_result ext;
    int rc = m17_parse_extended_callsign_meta(input, sizeof(input), &ext);
    int err = 0;
    if (rc != 0 || strcmp(ext.field1_csd, "AB1CD") != 0 || strcmp(ext.field2_csd, "AB1CD") != 0
        || ext.has_field2 != 1U) {
        DSD_FPRINTF(stderr, "extended callsign parse failed: rc=%d field1='%s' field2='%s'\n", rc, ext.field1_csd,
                    ext.field2_csd);
        err |= 1;
    }

    input[14] = 0x01U;
    rc = m17_parse_extended_callsign_meta(input, sizeof(input), &ext);
    if (rc != -4) {
        DSD_FPRINTF(stderr, "extended callsign reserved tail: got rc=%d want -4\n", rc);
        err |= 1;
    }
    input[14] = 0x00U;

    write_be48(input + 1, M17_ADDRESS_BROADCAST);
    rc = m17_parse_extended_callsign_meta(input, sizeof(input), &ext);
    if (rc != -5) {
        DSD_FPRINTF(stderr, "extended callsign invalid field1: got rc=%d want -5\n", rc);
        err |= 1;
    }
    return err;
}

int
main(void) {
    int err = 0;
    err |= test_parse_lsf_v2();
    err |= test_parse_lsf_big_endian_layout();
    err |= test_parse_lsf_spec_reserved_bits();
    err |= test_lsf_address_and_meta_helpers();
    err |= test_stream_signature_frame_numbers();
    err |= test_signature_digest();
    err |= test_signature_collector();
    err |= test_stream_1600_arbitrary_assemble();
    err |= test_parse_gnss_v2();
    err |= test_text_meta_blocks();
    err |= test_extended_callsign_meta();

    if (err == 0) {
        printf("M17_LSF_PARSE: OK\n");
    }
    return err;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
