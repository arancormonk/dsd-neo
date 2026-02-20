// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/protocol/nxdn/nxdn_deperm.h>

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

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

uint8_t
nxdn_scch_crc7_check_from_trellis(const uint8_t trellis_bits[32]) {
    if (trellis_bits == NULL) {
        return 0U;
    }
    return nxdn_bits_to_u8(trellis_bits, 25U, 7U);
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
    int written = snprintf(out, out_sz, "CSM %s", digits);
    if (written < 0 || (size_t)written >= out_sz) {
        out[0] = '\0';
        return 0;
    }
    return 1;
}
