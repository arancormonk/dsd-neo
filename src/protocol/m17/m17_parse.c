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
#include <stddef.h>
#include <stdint.h>
#include "dsd-neo/core/safe_api.h"

typedef struct {
    uint8_t protocol;
    const char* name;
} m17_packet_protocol_label;

const char*
m17_packet_protocol_name(uint8_t protocol) {
    static const m17_packet_protocol_label labels[] = {
        {0x00, "Raw"},
        {0x01, "AX.25"},
        {0x02, "APRS"},
        {0x03, "6LoWPAN"},
        {0x04, "IPv4"},
        {0x05, "SMS"},
        {0x06, "Winlink"},
        {0x07, "TLE"},
        {0x69, "OTA Key Delivery"},
        {0x80, "Meta Text Data V2"},
        {0x81, "Meta GNSS Position Data"},
        {0x82, "Meta Extended CSD"},
        {0x83, "Meta Text Data V3"},
        {0x89, "1600 Arbitrary Data"},
        {0x91, "PDU GNSS Position Data"},
        {0x99, "1600 Arbitrary Data"},
    };

    for (size_t i = 0; i < sizeof(labels) / sizeof(labels[0]); i++) {
        if (labels[i].protocol == protocol) {
            return labels[i].name;
        }
    }
    return NULL;
}

int
m17_stream_frame_is_signature(uint16_t frame_number) {
    return frame_number >= 0x7FFCU;
}

int
m17_stream_1600_arbitrary_assemble(uint8_t accumulator[48], uint16_t frame_number, const uint8_t chunk[8],
                                   uint8_t out_packet[49]) {
    if (accumulator == NULL || chunk == NULL || out_packet == NULL) {
        return -1;
    }

    const size_t slot = (size_t)(frame_number % 6U);
    DSD_MEMCPY(accumulator + (slot * 8U), chunk, 8U);
    if (slot != 5U) {
        return 0;
    }

    out_packet[0] = 0x99U;
    DSD_MEMCPY(out_packet + 1, accumulator, 48U);
    DSD_MEMSET(accumulator, 0, 48U);
    return 1;
}

static uint8_t
m17_meta_text_v2_bitmap_len(uint8_t bitmap) {
    switch (bitmap) {
        case 0x1U: return 1U;
        case 0x3U: return 2U;
        case 0x7U: return 3U;
        case 0xFU: return 4U;
        default: return 1U;
    }
}

static uint8_t
m17_meta_text_v2_bitmap_segment(uint8_t bitmap) {
    switch (bitmap) {
        case 0x1U: return 1U;
        case 0x2U: return 2U;
        case 0x4U: return 3U;
        case 0x8U: return 4U;
        default: return 1U;
    }
}

int
m17_meta_text_segment_info(uint8_t protocol, uint8_t control, uint8_t* segment_num, uint8_t* segment_len) {
    if (segment_num == NULL || segment_len == NULL) {
        return -1;
    }

    if (protocol == 0x80U) {
        *segment_len = m17_meta_text_v2_bitmap_len((uint8_t)(control >> 4U));
        *segment_num = m17_meta_text_v2_bitmap_segment((uint8_t)(control & 0xFU));
        return 0;
    }

    if (protocol == 0x83U) {
        *segment_len = (uint8_t)((control >> 4U) & 0xFU);
        *segment_num = (uint8_t)(control & 0xFU);
        return 0;
    }

    return -2;
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

static void
m17_lsf_map_v3_encryption(uint8_t encryption_type, uint8_t* et, uint8_t* es) {
    if (et == NULL || es == NULL) {
        return;
    }

    *et = 0U;
    *es = 0U;

    switch (encryption_type) {
        case 0x1U:
            *et = 1U;
            *es = 0U;
            break;
        case 0x2U:
            *et = 1U;
            *es = 1U;
            break;
        case 0x3U:
            *et = 1U;
            *es = 2U;
            break;
        case 0x4U:
            *et = 2U;
            *es = 0U;
            break;
        case 0x5U:
            *et = 2U;
            *es = 1U;
            break;
        case 0x6U:
            *et = 2U;
            *es = 2U;
            break;
        case 0x7U:
            *et = 3U;
            *es = 3U;
            break;
        default: break;
    }
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
    out->type_word = lsf_type;

    const uint8_t version_check = (uint8_t)(lsf_type >> 12U);
    if (version_check == 0U) {
        const uint8_t raw_rs = (uint8_t)((lsf_type >> 11U) & 0x1FU);
        out->version = 2U;
        out->dt = (uint8_t)((lsf_type >> 1U) & 0x3U);
        out->et = (uint8_t)((lsf_type >> 3U) & 0x3U);
        out->es = (uint8_t)((lsf_type >> 5U) & 0x3U);
        out->cn = (uint8_t)((lsf_type >> 7U) & 0xFU);
        out->rs = (uint8_t)(raw_rs >> 1U);
        out->payload_contents = out->dt;
        out->encryption_type = out->et;
        out->signature = (uint8_t)(raw_rs & 0x1U);
        out->meta_contents = out->es;
        out->meta_is_iv = (out->et == 2U) ? 1U : 0U;
    } else {
        out->version = 3U;
        out->payload_contents = (uint8_t)((lsf_type >> 12U) & 0xFU);
        out->encryption_type = (uint8_t)((lsf_type >> 9U) & 0x7U);
        out->signature = (uint8_t)((lsf_type >> 8U) & 0x1U);
        out->meta_contents = (uint8_t)((lsf_type >> 4U) & 0xFU);
        out->cn = (uint8_t)(lsf_type & 0xFU);
        out->dt = out->payload_contents;
        out->rs = 0U;
        out->meta_is_iv = (out->meta_contents == 0xFU) ? 1U : 0U;
        m17_lsf_map_v3_encryption(out->encryption_type, &out->et, &out->es);
    }

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

static int32_t
m17_s24_to_i32(uint32_t raw) {
    raw &= 0xFFFFFFU;
    if ((raw & 0x800000U) == 0U) {
        return (int32_t)raw;
    }

    const uint32_t magnitude = ((~raw) & 0xFFFFFFU) + 1U;
    return -(int32_t)magnitude;
}

int
m17_parse_gnss_v2(const uint8_t* input, size_t len, struct m17_gnss_result* out) {
    if (input == NULL || out == NULL) {
        return -1;
    }
    if (len < 15U) {
        return -2;
    }
    if (input[0] != 0x81U && input[0] != 0x91U) {
        return -3;
    }

    DSD_MEMSET(out, 0, sizeof(*out));

    out->data_source = (uint8_t)(input[1] >> 4U);
    out->station_type = (uint8_t)(input[1] & 0xFU);
    out->validity = (uint8_t)(input[2] >> 4U);
    out->radius_exponent = (uint8_t)((input[2] >> 1U) & 0x7U);
    out->bearing_deg = (uint16_t)(((uint16_t)(input[2] & 0x1U) << 8U) | input[3]);

    const uint32_t latitude_raw = ((uint32_t)input[4] << 16U) | ((uint32_t)input[5] << 8U) | input[6];
    const uint32_t longitude_raw = ((uint32_t)input[7] << 16U) | ((uint32_t)input[8] << 8U) | input[9];
    const int32_t latitude = m17_s24_to_i32(latitude_raw);
    const int32_t longitude = m17_s24_to_i32(longitude_raw);

    out->latitude_deg = ((double)latitude * 90.0) / 8388607.0;
    out->longitude_deg = ((double)longitude * 180.0) / 8388607.0;

    const uint16_t altitude = (uint16_t)(((uint16_t)input[10] << 8U) | input[11]);
    const uint16_t speed = (uint16_t)(((uint16_t)input[12] << 4U) | (input[13] >> 4U));
    out->reserved = (uint16_t)(((uint16_t)(input[13] & 0xFU) << 8U) | input[14]);

    out->radius_m = (float)(1U << out->radius_exponent);
    out->speed_kmh = (float)speed * 0.5f;
    out->altitude_m = ((float)altitude * 0.5f) - 500.0f;
    return 0;
}
