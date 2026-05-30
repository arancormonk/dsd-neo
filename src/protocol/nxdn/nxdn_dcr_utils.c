// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/protocol/nxdn/nxdn_deperm.h>
#include <stdint.h>
#include <stdio.h>
#include "dsd-neo/core/safe_api.h"

static uint8_t
nxdn_bits_to_u8(const uint8_t* bits, size_t start, size_t nbits) {
    if (bits == NULL || nbits == 0U || nbits > 8U) {
        return 0U;
    }
    uint8_t v = 0U;
    for (size_t i = 0U; i < nbits; i++) {
        v = (uint8_t)((v << 1U) | (bits[start + i] & 1U));
    }
    return v;
}

static uint16_t
nxdn_bits_to_u16(const uint8_t* bits, size_t start, size_t nbits) {
    if (bits == NULL || nbits == 0U || nbits > 16U) {
        return 0U;
    }
    uint16_t v = 0U;
    for (size_t i = 0U; i < nbits; i++) {
        v = (uint16_t)((v << 1U) | (bits[start + i] & 1U));
    }
    return v;
}

static uint16_t
nxdn_load_i_u16(const uint8_t val[], int len) {
    uint16_t acc = 0U;
    for (int i = 0; i < len; i++) {
        acc = (uint16_t)((acc << 1U) | (uint16_t)(val[i] & 1U));
    }
    return acc;
}

uint8_t
nxdn_scch_crc7_check_from_trellis(const uint8_t trellis_bits[32]) {
    if (trellis_bits == NULL) {
        return 0U;
    }
    return nxdn_bits_to_u8(trellis_bits, 25U, 7U);
}

uint16_t
crc12f(const uint8_t buf[], int len) {
    uint8_t s[12];
    for (int i = 0; i < 12; i++) {
        s[i] = 1;
    }
    for (int i = 0; i < len; i++) {
        const uint8_t a = buf[i] ^ s[0];
        s[0] = a ^ s[1];
        s[1] = s[2];
        s[2] = s[3];
        s[3] = s[4];
        s[4] = s[5];
        s[5] = s[6];
        s[6] = s[7];
        s[7] = s[8];
        s[8] = a ^ s[9];
        s[9] = a ^ s[10];
        s[10] = a ^ s[11];
        s[11] = a;
    }
    return nxdn_load_i_u16(s, 12);
}

uint16_t
crc15(const uint8_t buf[], int len) {
    uint8_t s[15];
    for (int i = 0; i < 15; i++) {
        s[i] = 1;
    }
    for (int i = 0; i < len; i++) {
        const uint8_t a = buf[i] ^ s[0];
        s[0] = a ^ s[1];
        s[1] = s[2];
        s[2] = s[3];
        s[3] = a ^ s[4];
        s[4] = a ^ s[5];
        s[5] = s[6];
        s[6] = s[7];
        s[7] = a ^ s[8];
        s[8] = a ^ s[9];
        s[9] = s[10];
        s[10] = s[11];
        s[11] = s[12];
        s[12] = a ^ s[13];
        s[13] = s[14];
        s[14] = a;
    }
    return nxdn_load_i_u16(s, 15);
}

uint16_t
nxdn_facch_crc12_payload_from_trellis(const uint8_t trellis_bits[96]) {
    if (trellis_bits == NULL) {
        return 0U;
    }
    return crc12f(trellis_bits, 80);
}

uint16_t
nxdn_facch_crc12_check_from_trellis(const uint8_t trellis_bits[96]) {
    return nxdn_bits_to_u16(trellis_bits, 80U, 12U);
}

uint16_t
nxdn_facch2_udch_crc15_payload_from_trellis(const uint8_t trellis_bits[208]) {
    if (trellis_bits == NULL) {
        return 0U;
    }
    return crc15(trellis_bits, 184);
}

uint16_t
nxdn_facch2_udch_crc15_check_from_trellis(const uint8_t trellis_bits[208]) {
    return nxdn_bits_to_u16(trellis_bits, 184U, 15U);
}

int
nxdn_sacch_segment_sequence_is_valid(uint8_t crc_ok, int previous_part_of_frame, int part_of_frame) {
    if (!crc_ok) {
        return 0;
    }
    if (part_of_frame == 0) {
        return 1;
    }
    return part_of_frame == ((previous_part_of_frame + 1) % 4);
}

int
nxdn_dcr_decode_csm_alias(const uint8_t trellis_bits[96], char* out, size_t out_sz) {
    if (trellis_bits == NULL || out == NULL || out_sz == 0U) {
        return 0;
    }

    char digits[10];
    for (size_t i = 0U; i < 9U; i++) {
        uint8_t nibble = nxdn_bits_to_u8(trellis_bits, i * 4U, 4U);
        if (nibble > 9U) {
            out[0] = '\0';
            return 0;
        }
        digits[i] = (char)('0' + nibble);
    }
    digits[9] = '\0';
    int written = DSD_SNPRINTF(out, out_sz, "CSM %s", digits);
    if (written < 0 || (size_t)written >= out_sz) {
        out[0] = '\0';
        return 0;
    }
    return 1;
}
