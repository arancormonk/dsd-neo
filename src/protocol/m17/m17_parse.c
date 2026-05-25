// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * M17 LSF parsing helpers.
 */

#include <dsd-neo/protocol/dmr/dmr_utils_api.h>
#include <dsd-neo/protocol/m17/m17_parse.h>
#include <dsd-neo/protocol/m17/m17_tables.h>
#include <stdint.h>
#include <string.h>
#include "dsd-neo/core/safe_api.h"

const char*
m17_packet_protocol_name(uint8_t protocol) {
    switch (protocol) {
        case 0x00: return "Raw";
        case 0x01: return "AX.25";
        case 0x02: return "APRS";
        case 0x03: return "6LoWPAN";
        case 0x04: return "IPv4";
        case 0x05: return "SMS";
        case 0x06: return "Winlink";
        case 0x07: return "TLE";
        case 0x80: return "Meta Text Data";
        case 0x81: return "Meta GNSS Position Data";
        case 0x82: return "Meta Extended CSD";
        case 0x89: return "1600 Arbitrary Data";
        default: break;
    }
    return NULL;
}

static int
m17_lsf_base40_id_is_valid(unsigned long long value) {
    return value != 0ULL && value < 0xEE6B28000000ULL;
}

static void
m17_decode_base40_id(unsigned long long value, char out_csd[10]) {
    if (!m17_lsf_base40_id_is_valid(value)) {
        return;
    }

    for (int i = 0; i < 9; i++) {
        if (value == 0ULL) {
            break;
        }
        int idx = (int)(value % 40ULL);
        out_csd[i] = m17_base40_alphabet[idx];
        value /= 40ULL;
    }
}

static uint32_t
m17_extract_meta_bytes(const uint8_t* lsf_bits, uint8_t meta[14]) {
    uint32_t meta_sum = 0U;

    for (int i = 0; i < 14; i++) {
        meta[i] = (uint8_t)ConvertBitIntoBytes((uint8_t*)&lsf_bits[((size_t)i * 8U) + 112U], 8U);
        meta_sum += meta[i];
    }

    return meta_sum;
}

int
m17_parse_lsf(const uint8_t* lsf_bits, size_t bit_len, struct m17_lsf_result* out) {
    if (out == NULL || lsf_bits == NULL) {
        return -1;
    }
    if (bit_len < 240) {
        return -2;
    }

    DSD_MEMSET(out, 0, sizeof(*out));

    unsigned long long lsf_dst = ConvertBitIntoBytes(&lsf_bits[0], 48);
    unsigned long long lsf_src = ConvertBitIntoBytes(&lsf_bits[48], 48);
    uint16_t lsf_type = (uint16_t)ConvertBitIntoBytes(&lsf_bits[96], 16);

    out->dst = lsf_dst;
    out->src = lsf_src;

    out->dt = (uint8_t)((lsf_type >> 1) & 0x3);
    out->et = (uint8_t)((lsf_type >> 3) & 0x3);
    out->es = (uint8_t)((lsf_type >> 5) & 0x3);
    out->cn = (uint8_t)((lsf_type >> 7) & 0xF);
    out->rs = (uint8_t)((lsf_type >> 11) & 0x1F);

    /* Decode base-40 CSD strings for destination and source. */
    DSD_MEMSET(out->dst_csd, 0, sizeof(out->dst_csd));
    DSD_MEMSET(out->src_csd, 0, sizeof(out->src_csd));
    m17_decode_base40_id(lsf_dst, out->dst_csd);
    m17_decode_base40_id(lsf_src, out->src_csd);

    /* Extract Meta/IV bytes starting at bit 112 (14 octets). */
    uint8_t meta[14];
    DSD_MEMSET(meta, 0, sizeof(meta));
    uint32_t meta_sum = m17_extract_meta_bytes(lsf_bits, meta);

    if (meta_sum != 0U) {
        out->has_meta = 1U;
        DSD_MEMCPY(out->meta, meta, sizeof(meta));
    } else {
        out->has_meta = 0U;
    }

    return 0;
}
