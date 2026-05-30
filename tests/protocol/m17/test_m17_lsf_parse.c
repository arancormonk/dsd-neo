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
#include "dsd-neo/core/safe_api.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

// Minimal MSB-first bit-to-integer helper matching the DMR utils API.
uint64_t
ConvertBitIntoBytes(const uint8_t* bits, uint32_t n) { // NOLINT(misc-use-internal-linkage)
    uint64_t v = 0ULL;
    for (uint32_t i = 0; i < n; i++) {
        v = (v << 1) | (uint64_t)(bits[i] & 1U);
    }
    return v;
}

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
    const uint8_t es = 3U;
    const uint8_t cn = 9U;
    const uint8_t rs = 1U;

    uint16_t lsf_type = 0;
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
    err |= expect_eq_u8("dt", res.dt, dt);
    err |= expect_eq_u8("et", res.et, et);
    err |= expect_eq_u8("es", res.es, es);
    err |= expect_eq_u8("cn", res.cn, cn);
    err |= expect_eq_u8("rs", res.rs, rs);
    err |= expect_eq_u8("payload_contents", res.payload_contents, dt);
    err |= expect_eq_u8("encryption_type", res.encryption_type, et);
    err |= expect_eq_u8("signature", res.signature, 1U);
    err |= expect_eq_u8("meta_contents", res.meta_contents, es);
    err |= expect_eq_u8("meta_is_iv", res.meta_is_iv, 0U);
    err |= expect_eq_u8("has_meta", res.has_meta, 1U);

    if (res.meta[0] != meta[0] || res.meta[1] != meta[1]) {
        DSD_FPRINTF(stderr, "meta[0..1]: got %02X %02X want %02X %02X\n", res.meta[0], res.meta[1], meta[0], meta[1]);
        err |= 1;
    }

    return err;
}

static int
test_parse_lsf_v3_meta(void) {
    // Choose arbitrary but distinct dst/src values within the valid range.
    const uint64_t dst = 0x0000ABCDEF12ULL;
    const uint64_t src = 0x000012345678ULL;

    const uint8_t payload_contents = 0xFU;
    const uint8_t encryption_type = 0x5U;
    const uint8_t signature = 1U;
    const uint8_t meta_contents = 0x3U;
    const uint8_t can = 0xAU;

    uint16_t lsf_type = 0;
    lsf_type |= (uint16_t)((uint16_t)payload_contents << 12);
    lsf_type |= (uint16_t)((uint16_t)encryption_type << 9);
    lsf_type |= (uint16_t)((uint16_t)signature << 8);
    lsf_type |= (uint16_t)((uint16_t)meta_contents << 4);
    lsf_type |= can;

    uint8_t meta[14];
    DSD_MEMSET(meta, 0, sizeof(meta));
    meta[0] = 0x11U;
    meta[13] = 0xEEU;

    uint8_t lsf_bits[240];
    build_lsf_bits(lsf_bits, dst, src, lsf_type, meta);

    struct m17_lsf_result res;
    int rc = m17_parse_lsf(lsf_bits, sizeof(lsf_bits), &res);
    if (rc != 0) {
        DSD_FPRINTF(stderr, "m17_parse_lsf v3 failed: rc=%d\n", rc);
        return 1;
    }

    int err = 0;
    err |= expect_eq_u64("v3 dst", res.dst, dst);
    err |= expect_eq_u64("v3 src", res.src, src);
    err |= expect_eq_u64("v3 type_word", res.type_word, lsf_type);
    err |= expect_eq_u8("v3 version", res.version, 3U);
    err |= expect_eq_u8("v3 payload_contents", res.payload_contents, payload_contents);
    err |= expect_eq_u8("v3 encryption_type", res.encryption_type, encryption_type);
    err |= expect_eq_u8("v3 signature", res.signature, signature);
    err |= expect_eq_u8("v3 meta_contents", res.meta_contents, meta_contents);
    err |= expect_eq_u8("v3 cn", res.cn, can);
    err |= expect_eq_u8("v3 dt", res.dt, payload_contents);
    err |= expect_eq_u8("v3 et", res.et, 2U);
    err |= expect_eq_u8("v3 es", res.es, 1U);
    err |= expect_eq_u8("v3 rs", res.rs, 0U);
    err |= expect_eq_u8("v3 meta_is_iv", res.meta_is_iv, 0U);
    err |= expect_eq_u8("v3 has_meta", res.has_meta, 1U);

    if (res.meta[0] != meta[0] || res.meta[13] != meta[13]) {
        DSD_FPRINTF(stderr, "v3 meta: got %02X %02X want %02X %02X\n", res.meta[0], res.meta[13], meta[0], meta[13]);
        err |= 1;
    }

    return err;
}

static int
test_parse_lsf_v3_zero_iv(void) {
    const uint64_t dst = 0x000000100001ULL;
    const uint64_t src = 0x000000200002ULL;
    const uint16_t lsf_type = (uint16_t)((0x2U << 12) | (0x4U << 9) | (0xFU << 4) | 0x6U);

    uint8_t meta[14];
    DSD_MEMSET(meta, 0, sizeof(meta));

    uint8_t lsf_bits[240];
    build_lsf_bits(lsf_bits, dst, src, lsf_type, meta);

    struct m17_lsf_result res;
    int rc = m17_parse_lsf(lsf_bits, sizeof(lsf_bits), &res);
    if (rc != 0) {
        DSD_FPRINTF(stderr, "m17_parse_lsf v3 iv failed: rc=%d\n", rc);
        return 1;
    }

    int err = 0;
    err |= expect_eq_u8("v3 iv version", res.version, 3U);
    err |= expect_eq_u8("v3 iv payload_contents", res.payload_contents, 0x2U);
    err |= expect_eq_u8("v3 iv encryption_type", res.encryption_type, 0x4U);
    err |= expect_eq_u8("v3 iv meta_contents", res.meta_contents, 0xFU);
    err |= expect_eq_u8("v3 iv et", res.et, 2U);
    err |= expect_eq_u8("v3 iv es", res.es, 0U);
    err |= expect_eq_u8("v3 iv meta_is_iv", res.meta_is_iv, 1U);
    err |= expect_eq_u8("v3 iv has_meta", res.has_meta, 0U);
    return err;
}

static int
test_parse_gnss_v2(void) {
    const uint16_t bearing = 300U;
    const uint32_t latitude_raw = 0x400000U;
    const uint32_t longitude_raw = 0xC00000U;
    const uint16_t altitude = 3000U;
    const uint16_t speed = 0x123U;
    const uint16_t reserved = 0x456U;

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

    return err;
}

int
main(void) {
    int err = 0;
    err |= test_parse_lsf_v2();
    err |= test_parse_lsf_v3_meta();
    err |= test_parse_lsf_v3_zero_iv();
    err |= test_parse_gnss_v2();

    if (err == 0) {
        printf("M17_LSF_PARSE: OK\n");
    }
    return err;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
