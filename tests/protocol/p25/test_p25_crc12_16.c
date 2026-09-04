// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/protocol/p25/p25_crc.h>
#include <stdint.h>
#include <stdio.h>
#include "dsd-neo/core/safe_api.h"

static void
set_wire_crc(int* frame, int payload_len, int width, unsigned int crc) {
    for (int bit = 0; bit < width; bit++) {
        frame[payload_len + bit] = (int)((crc >> (unsigned int)(width - 1 - bit)) & 1U);
    }
}

static void
copy_payload(const uint8_t* payload, int payload_len, int* frame) {
    for (int bit = 0; bit < payload_len; bit++) {
        frame[bit] = payload[bit] & 1U;
    }
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
check_crc16_vector(const char* label, const uint8_t* payload, int payload_len, uint16_t wire_crc, int tamper_bit) {
    int frame[190] = {0};
    copy_payload(payload, payload_len, frame);
    set_wire_crc(frame, payload_len, 16, wire_crc);

    int rc = 0;
    rc |= expect_int(label, crc16_lb_bridge(frame, payload_len), 0);
    if (tamper_bit >= 0) {
        frame[tamper_bit] ^= 1;
        rc |= expect_int("CRC16 tamper rejection", crc16_lb_bridge(frame, payload_len) != 0, 1);
    }
    return rc;
}

static int
check_crc12_vector(const char* label, const uint8_t* payload, int payload_len, uint16_t wire_crc, int tamper_bit) {
    int frame[190] = {0};
    copy_payload(payload, payload_len, frame);
    set_wire_crc(frame, payload_len, 12, wire_crc);

    int rc = 0;
    rc |= expect_int(label, crc12_xb_bridge(frame, payload_len), 0);
    if (tamper_bit >= 0) {
        frame[tamper_bit] ^= 1;
        rc |= expect_int("CRC12 tamper rejection", crc12_xb_bridge(frame, payload_len) != 0, 1);
    }
    return rc;
}

int
main(void) {
    int rc = 0;
    uint8_t zero[176] = {0};
    uint8_t alternating[176] = {0};
    uint8_t lcch[164] = {0};
    uint8_t short_lcch[64] = {0};
    uint8_t crc12_pattern[176] = {0};

    for (int bit = 0; bit < 176; bit++) {
        alternating[bit] = (uint8_t)(bit & 1);
        crc12_pattern[bit] = (uint8_t)(((bit * 3) ^ (bit >> 1)) & 1);
    }

    static const uint8_t lcch_header[16] = {0, 1, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0};
    static const uint8_t short_header[16] = {0, 0, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0};
    DSD_MEMCPY(lcch, lcch_header, sizeof lcch_header);
    DSD_MEMCPY(short_lcch, short_header, sizeof short_header);
    for (int bit = 16; bit < 164; bit++) {
        lcch[bit] = (uint8_t)(bit & 1);
    }
    for (int bit = 16; bit < 64; bit++) {
        short_lcch[bit] = (uint8_t)((bit - 16) & 1);
    }

    rc |= expect_int("CRC16 zero-length reference", ComputeCrcCCITT16b(zero, 0), 0xFFFF);
    rc |= expect_int("CRC16 zero reference", ComputeCrcCCITT16b(zero, 164), 0xFFFF);
    rc |= expect_int("CRC16 alternating reference", ComputeCrcCCITT16b(alternating, 164), 0x237C);
    rc |= expect_int("CRC16 LCCH reference", ComputeCrcCCITT16b(lcch, 164), 0xDCF2);
    rc |= expect_int("CRC16 short reference", ComputeCrcCCITT16b(short_lcch, 64), 0xADC4);

    rc |= check_crc16_vector("CRC16 zero-length bridge", zero, 0, 0xFFFF, 0);
    rc |= check_crc16_vector("CRC16 zero bridge", zero, 164, 0xFFFF, -1);
    rc |= check_crc16_vector("CRC16 alternating bridge", alternating, 164, 0x237C, 17);
    rc |= check_crc16_vector("CRC16 LCCH bridge", lcch, 164, 0xDCF2, 32);
    rc |= check_crc16_vector("CRC16 short bridge", short_lcch, 64, 0xADC4, 20);

    rc |= check_crc12_vector("CRC12 zero bridge", zero, 176, 0x0FFF, -1);
    rc |= check_crc12_vector("CRC12 patterned bridge", crc12_pattern, 176, 0x091F, 77);

    if (rc == 0) {
        DSD_FPRINTF(stderr, "P25 CRC12/16 fixed vectors passed\n");
    }
    return rc;
}
