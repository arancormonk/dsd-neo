// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * M17 LSF parsing helpers.
 */

#include <dsd-neo/core/bit_packing.h>

#include <dsd-neo/protocol/m17/m17_parse.h>
#include <dsd-neo/protocol/m17/m17_tables.h>
#include <stddef.h>
#include <stdint.h>
#include "dsd-neo/core/safe_api.h"

typedef struct {
    uint32_t protocol;
    const char* name;
} m17_packet_protocol_label;

const char*
m17_packet_protocol_name_u32(uint32_t protocol) {
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
    return m17_stream_signature_frame_index(frame_number) >= 0;
}

int
m17_stream_signature_frame_index(uint16_t frame_number) {
    switch (frame_number) {
        case M17_STREAM_SIGNATURE_FN0: return 0;
        case M17_STREAM_SIGNATURE_FN1: return 1;
        case M17_STREAM_SIGNATURE_FN2: return 2;
        case M17_STREAM_SIGNATURE_FN3: return 3;
        default: return -1;
    }
}

void
m17_signature_digest_init(uint8_t digest[M17_SIGNATURE_DIGEST_BYTES]) {
    if (digest == NULL) {
        return;
    }
    DSD_MEMSET(digest, 0, M17_SIGNATURE_DIGEST_BYTES);
}

void
m17_signature_digest_update(uint8_t digest[M17_SIGNATURE_DIGEST_BYTES],
                            const uint8_t payload[M17_SIGNATURE_DIGEST_BYTES]) {
    if (digest == NULL || payload == NULL) {
        return;
    }

    for (uint8_t i = 0U; i < M17_SIGNATURE_DIGEST_BYTES; i++) {
        digest[i] ^= payload[i];
    }

    const uint8_t first = digest[0];
    DSD_MEMMOVE(digest, digest + 1, M17_SIGNATURE_DIGEST_BYTES - 1U);
    digest[M17_SIGNATURE_DIGEST_BYTES - 1U] = first;
}

void
m17_signature_collector_reset(struct m17_signature_collector* collector) {
    if (collector == NULL) {
        return;
    }
    DSD_MEMSET(collector, 0, sizeof(*collector));
}

int
m17_signature_collector_push(struct m17_signature_collector* collector, uint16_t frame_number,
                             const uint8_t payload[M17_SIGNATURE_DIGEST_BYTES]) {
    if (collector == NULL || payload == NULL) {
        return -1;
    }

    const int index = m17_stream_signature_frame_index(frame_number);
    if (index < 0 || index > 3) {
        return -2;
    }

    if (index > 0 && (collector->received_mask & (uint8_t)(1U << (uint8_t)(index - 1))) == 0U) {
        collector->bad_sequence = 1U;
    }
    DSD_MEMCPY(&collector->signature[(size_t)index * M17_SIGNATURE_DIGEST_BYTES], payload, M17_SIGNATURE_DIGEST_BYTES);
    collector->received_mask = (uint8_t)(collector->received_mask | (uint8_t)(1U << (uint8_t)index));

    if (index == 3 && collector->received_mask == 0x0FU) {
        collector->complete = 1U;
        return 1;
    }

    return collector->bad_sequence != 0U ? -3 : 0;
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
        default: return 0U;
    }
}

static uint8_t
m17_meta_text_v2_bitmap_segment(uint8_t bitmap) {
    switch (bitmap) {
        case 0x1U: return 0U;
        case 0x2U: return 1U;
        case 0x4U: return 2U;
        case 0x8U: return 3U;
        default: return 0xFFU;
    }
}

static uint8_t
m17_meta_text_v2_bitmap_for_index(uint8_t block_index) {
    return (uint8_t)(1U << block_index);
}

int
m17_meta_text_segment_info(uint8_t protocol, uint8_t control, uint8_t* segment_num, uint8_t* segment_len) {
    if (segment_num == NULL || segment_len == NULL) {
        return -1;
    }

    if (protocol == 0x80U) {
        if (control == 0U) {
            *segment_len = 0U;
            *segment_num = 0U;
            return 0;
        }

        const uint8_t len = m17_meta_text_v2_bitmap_len((uint8_t)(control >> 4U));
        const uint8_t index = m17_meta_text_v2_bitmap_segment((uint8_t)(control & 0xFU));
        if (len == 0U || index == 0xFFU || index >= len) {
            return -3;
        }

        *segment_len = len;
        *segment_num = (uint8_t)(index + 1U);
        return 0;
    }

    if (protocol == 0x83U) {
        *segment_len = (uint8_t)((control >> 4U) & 0xFU);
        *segment_num = (uint8_t)(control & 0xFU);
        return 0;
    }

    return -2;
}

int
m17_meta_text_parse_block(const uint8_t meta[M17_META_BYTES], struct m17_meta_text_block* out) {
    if (meta == NULL || out == NULL) {
        return -1;
    }

    DSD_MEMSET(out, 0, sizeof(*out));
    if (meta[0] == 0U) {
        return 0;
    }

    const uint8_t length_bitmap = (uint8_t)(meta[0] >> 4U);
    const uint8_t block_bitmap = (uint8_t)(meta[0] & 0xFU);
    const uint8_t total_blocks = m17_meta_text_v2_bitmap_len(length_bitmap);
    const uint8_t block_index = m17_meta_text_v2_bitmap_segment(block_bitmap);
    if (total_blocks == 0U || block_index == 0xFFU || block_index >= total_blocks) {
        return -2;
    }

    out->has_text = 1U;
    out->total_blocks = total_blocks;
    out->block_index = block_index;
    out->length_bitmap = length_bitmap;
    out->block_bitmap = block_bitmap;
    DSD_MEMCPY(out->text, meta + 1, M17_TEXT_BLOCK_BYTES);
    return 0;
}

void
m17_meta_text_assembler_reset(struct m17_meta_text_assembler* assembler) {
    if (assembler == NULL) {
        return;
    }
    DSD_MEMSET(assembler, 0, sizeof(*assembler));
}

static int
m17_meta_text_block_is_usable(const struct m17_meta_text_block* block) {
    return block->total_blocks != 0U && block->total_blocks <= M17_TEXT_MAX_BLOCKS
           && block->block_index < block->total_blocks && block->length_bitmap != 0U
           && block->block_bitmap == m17_meta_text_v2_bitmap_for_index(block->block_index);
}

static int
m17_meta_text_assembler_complete(const struct m17_meta_text_assembler* assembler) {
    return (uint8_t)(assembler->control_or >> 4U) == (uint8_t)(assembler->control_or & 0xFU)
           && assembler->received_bitmap == assembler->expected_bitmap;
}

static void
m17_meta_text_copy_trimmed(const struct m17_meta_text_assembler* assembler, uint8_t total_blocks,
                           char out_text[M17_TEXT_MAX_BYTES + 1U], uint8_t* out_len) {
    size_t len = (size_t)total_blocks * (size_t)M17_TEXT_BLOCK_BYTES;
    while (len > 0U && assembler->text[len - 1U] == ' ') {
        len--;
    }
    if (len > M17_TEXT_MAX_BYTES) {
        len = M17_TEXT_MAX_BYTES;
    }

    for (size_t i = 0U; i < len; i++) {
        out_text[i] = (char)assembler->text[i];
    }
    out_text[len] = '\0';
    *out_len = (uint8_t)len;
}

int
m17_meta_text_assembler_push(struct m17_meta_text_assembler* assembler, const struct m17_meta_text_block* block,
                             char out_text[M17_TEXT_MAX_BYTES + 1U], uint8_t* out_len) {
    if (assembler == NULL || block == NULL || out_text == NULL || out_len == NULL) {
        return -1;
    }

    *out_len = 0U;
    out_text[0] = '\0';

    if (block->has_text == 0U) {
        m17_meta_text_assembler_reset(assembler);
        return 0;
    }

    if (!m17_meta_text_block_is_usable(block)) {
        return -2;
    }

    if (assembler->expected_bitmap != 0U && assembler->expected_bitmap != block->length_bitmap) {
        m17_meta_text_assembler_reset(assembler);
    }

    assembler->expected_bitmap = block->length_bitmap;
    assembler->received_bitmap |= block->block_bitmap;
    assembler->control_or =
        (uint8_t)(assembler->control_or | (uint8_t)(block->length_bitmap << 4U) | block->block_bitmap);
    DSD_MEMCPY(assembler->text + ((size_t)block->block_index * M17_TEXT_BLOCK_BYTES), block->text,
               M17_TEXT_BLOCK_BYTES);

    if (!m17_meta_text_assembler_complete(assembler)) {
        return 0;
    }

    m17_meta_text_copy_trimmed(assembler, block->total_blocks, out_text, out_len);
    return 1;
}

uint8_t
m17_address_classify(unsigned long long address) {
    if (address == 0ULL) {
        return M17_ADDRESS_RESERVED;
    }
    if (address <= M17_ADDRESS_STANDARD_MAX) {
        return M17_ADDRESS_STANDARD;
    }
    if (address <= M17_ADDRESS_EXTENDED_MAX) {
        return M17_ADDRESS_EXTENDED;
    }
    return M17_ADDRESS_BROADCAST_KIND;
}

int
m17_address_is_valid_destination(unsigned long long address) {
    const uint8_t kind = m17_address_classify(address);
    return kind == M17_ADDRESS_STANDARD || kind == M17_ADDRESS_EXTENDED || kind == M17_ADDRESS_BROADCAST_KIND;
}

int
m17_address_is_valid_source(unsigned long long address) {
    return m17_address_classify(address) == M17_ADDRESS_STANDARD;
}

int
m17_address_decode_csd(unsigned long long address, char out_csd[10]) {
    if (out_csd == NULL) {
        return -1;
    }

    DSD_MEMSET(out_csd, 0, 10U);
    if (m17_address_classify(address) != M17_ADDRESS_STANDARD) {
        return -2;
    }

    for (int i = 0; i < 9; i++) {
        if (address == 0ULL) {
            break;
        }
        int idx = (int)(address % 40ULL);
        out_csd[i] = m17_base40_alphabet[idx];
        address /= 40ULL;
    }

    return 0;
}

int
m17_lsf_type_reserved_bits_valid(const struct m17_lsf_result* lsf) {
    if (lsf == NULL || lsf->rs != 0U) {
        return 0;
    }
    if (lsf->packet_stream == 0U && (lsf->dt != 0U || lsf->et != 0U || lsf->es != 0U || lsf->signature != 0U)) {
        return 0;
    }
    if (lsf->packet_stream != 0U && lsf->et == 3U) {
        return 0;
    }
    if (lsf->packet_stream != 0U && lsf->et != 0U && lsf->es == 3U) {
        return 0;
    }
    return 1;
}

uint8_t
m17_null_meta_protocol_for_subtype(uint8_t subtype) {
    switch (subtype) {
        case 0U: return 0x80U;
        case 1U: return 0x81U;
        case 2U: return 0x82U;
        default: return 0U;
    }
}

int
m17_can_filter_allows(int configured_can, uint8_t received_can) {
    if (configured_can < 0) {
        return 1;
    }
    if (configured_can > 15) {
        return 0;
    }
    return (uint8_t)configured_can == (received_can & 0xFU);
}

static uint32_t
m17_extract_meta_bytes(const uint8_t* lsf_bits, uint8_t meta[M17_META_BYTES]) {
    uint32_t meta_sum = 0U;

    for (int i = 0; i < (int)M17_META_BYTES; i++) {
        meta[i] = (uint8_t)convert_bits_into_output((uint8_t*)&lsf_bits[((size_t)i * 8U) + 112U], 8U);
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

    unsigned long long lsf_dst = convert_bits_into_output(&lsf_bits[0], 48);
    unsigned long long lsf_src = convert_bits_into_output(&lsf_bits[48], 48);
    uint16_t lsf_type = (uint16_t)convert_bits_into_output(&lsf_bits[96], 16);

    out->dst = lsf_dst;
    out->src = lsf_src;
    out->type_word = lsf_type;

    out->version = 2U;
    out->packet_stream = (uint8_t)(lsf_type & 0x1U);
    out->dt = (uint8_t)((lsf_type >> 1U) & 0x3U);
    out->et = (uint8_t)((lsf_type >> 3U) & 0x3U);
    out->es = (uint8_t)((lsf_type >> 5U) & 0x3U);
    out->cn = (uint8_t)((lsf_type >> 7U) & 0xFU);
    out->signature = (uint8_t)((lsf_type >> 11U) & 0x1U);
    out->rs = (uint8_t)((lsf_type >> 12U) & 0xFU);
    out->meta_is_iv = (out->et == 2U) ? 1U : 0U;
    out->dst_address_kind = m17_address_classify(lsf_dst);
    out->src_address_kind = m17_address_classify(lsf_src);
    out->dst_is_valid = (uint8_t)m17_address_is_valid_destination(lsf_dst);
    out->src_is_valid = (uint8_t)m17_address_is_valid_source(lsf_src);
    out->type_reserved_valid = (uint8_t)m17_lsf_type_reserved_bits_valid(out);

    /* Decode base-40 CSD strings for destination and source. */
    (void)m17_address_decode_csd(lsf_dst, out->dst_csd);
    (void)m17_address_decode_csd(lsf_src, out->src_csd);

    /* Extract Meta/IV bytes starting at bit 112 (14 octets). */
    uint8_t meta[M17_META_BYTES];
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

static int
m17_gnss_numeric_nonzero(uint32_t value) {
    return value != 0U;
}

struct m17_gnss_raw_fields {
    uint32_t latitude_raw;
    uint32_t longitude_raw;
    uint16_t altitude;
    uint16_t speed;
};

static void
m17_read_gnss_v2_fields(const uint8_t* input, struct m17_gnss_result* out, struct m17_gnss_raw_fields* raw) {
    out->data_source = (uint8_t)(input[1] >> 4U);
    out->station_type = (uint8_t)(input[1] & 0xFU);
    out->validity = (uint8_t)(input[2] >> 4U);
    out->radius_exponent = (uint8_t)((input[2] >> 1U) & 0x7U);
    out->bearing_deg = (uint16_t)(((uint16_t)(input[2] & 0x1U) << 8U) | input[3]);

    raw->latitude_raw = ((uint32_t)input[4] << 16U) | ((uint32_t)input[5] << 8U) | input[6];
    raw->longitude_raw = ((uint32_t)input[7] << 16U) | ((uint32_t)input[8] << 8U) | input[9];
    raw->altitude = (uint16_t)(((uint16_t)input[10] << 8U) | input[11]);
    raw->speed = (uint16_t)(((uint16_t)input[12] << 4U) | (input[13] >> 4U));

    const int32_t latitude = m17_s24_to_i32(raw->latitude_raw);
    const int32_t longitude = m17_s24_to_i32(raw->longitude_raw);
    out->latitude_deg = ((double)latitude * 90.0) / 8388607.0;
    out->longitude_deg = ((double)longitude * 180.0) / 8388607.0;
    out->reserved = (uint16_t)(((uint16_t)(input[13] & 0xFU) << 8U) | input[14]);
}

static int
m17_validate_gnss_v2_fields(struct m17_gnss_result* out, const struct m17_gnss_raw_fields* raw) {
    if (out->reserved != 0U) {
        return -4;
    }
    if (((out->validity & M17_GNSS_VALID_VELOCITY) != 0U) && out->bearing_deg > 359U) {
        return -5;
    }
    if (((out->validity & M17_GNSS_VALID_LATLON) == 0U)
        && (m17_gnss_numeric_nonzero(raw->latitude_raw) || m17_gnss_numeric_nonzero(raw->longitude_raw))) {
        out->invalid_zero_fields |= M17_GNSS_VALID_LATLON;
    }
    if (((out->validity & M17_GNSS_VALID_ALTITUDE) == 0U) && raw->altitude != 0U) {
        out->invalid_zero_fields |= M17_GNSS_VALID_ALTITUDE;
    }
    if (((out->validity & M17_GNSS_VALID_VELOCITY) == 0U) && (out->bearing_deg != 0U || raw->speed != 0U)) {
        out->invalid_zero_fields |= M17_GNSS_VALID_VELOCITY;
    }
    if (((out->validity & M17_GNSS_VALID_RADIUS) == 0U) && out->radius_exponent != 0U) {
        out->invalid_zero_fields |= M17_GNSS_VALID_RADIUS;
    }
    return 0;
}

static void
m17_apply_gnss_v2_validity(struct m17_gnss_result* out, const struct m17_gnss_raw_fields* raw) {
    out->radius_m = (float)(1U << out->radius_exponent);
    out->speed_kmh = (float)raw->speed * 0.5f;
    out->altitude_m = ((float)raw->altitude * 0.5f) - 500.0f;
    if ((out->validity & M17_GNSS_VALID_LATLON) == 0U) {
        out->latitude_deg = 0.0;
        out->longitude_deg = 0.0;
    }
    if ((out->validity & M17_GNSS_VALID_ALTITUDE) == 0U) {
        out->altitude_m = 0.0f;
    }
    if ((out->validity & M17_GNSS_VALID_VELOCITY) == 0U) {
        out->bearing_deg = 0U;
        out->speed_kmh = 0.0f;
    }
    if ((out->validity & M17_GNSS_VALID_RADIUS) == 0U) {
        out->radius_m = 0.0f;
    }
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

    struct m17_gnss_raw_fields raw;
    m17_read_gnss_v2_fields(input, out, &raw);
    const int validation = m17_validate_gnss_v2_fields(out, &raw);
    if (validation != 0) {
        return validation;
    }
    m17_apply_gnss_v2_validity(out, &raw);
    return 0;
}

static unsigned long long
m17_read_be48(const uint8_t* bytes) {
    return ((unsigned long long)bytes[0] << 40ULL) | ((unsigned long long)bytes[1] << 32ULL)
           | ((unsigned long long)bytes[2] << 24ULL) | ((unsigned long long)bytes[3] << 16ULL)
           | ((unsigned long long)bytes[4] << 8ULL) | (unsigned long long)bytes[5];
}

int
m17_parse_extended_callsign_meta(const uint8_t* input, size_t len, struct m17_extended_callsign_result* out) {
    if (input == NULL || out == NULL) {
        return -1;
    }
    if (len < 15U) {
        return -2;
    }
    if (input[0] != 0x82U) {
        return -3;
    }

    DSD_MEMSET(out, 0, sizeof(*out));
    if (input[13] != 0U || input[14] != 0U) {
        return -4;
    }

    out->field1 = m17_read_be48(input + 1);
    out->field2 = m17_read_be48(input + 7);
    if (m17_address_classify(out->field1) != M17_ADDRESS_STANDARD) {
        return -5;
    }
    if (out->field2 != 0ULL && m17_address_classify(out->field2) != M17_ADDRESS_STANDARD) {
        return -6;
    }

    (void)m17_address_decode_csd(out->field1, out->field1_csd);
    if (out->field2 != 0ULL) {
        out->has_field2 = 1U;
        (void)m17_address_decode_csd(out->field2, out->field2_csd);
    }
    return 0;
}

static int
m17_packet_continuation_valid(uint8_t byte) {
    return (byte & 0xC0U) == 0x80U;
}

int
m17_packet_protocol_decode(const uint8_t* input, size_t len, struct m17_packet_protocol_result* out) {
    if (input == NULL || out == NULL) {
        return -1;
    }
    if (len == 0U) {
        return -2;
    }

    DSD_MEMSET(out, 0, sizeof(*out));
    const uint8_t b0 = input[0];
    if (b0 < 0x80U) {
        out->identifier = b0;
        out->length = 1U;
        return 0;
    }

    uint8_t need = 0U;
    uint32_t value = 0U;
    uint32_t min_value = 0U;
    if ((b0 & 0xE0U) == 0xC0U) {
        need = 2U;
        value = (uint32_t)(b0 & 0x1FU);
        min_value = 0x80U;
    } else if ((b0 & 0xF0U) == 0xE0U) {
        need = 3U;
        value = (uint32_t)(b0 & 0x0FU);
        min_value = 0x800U;
    } else if ((b0 & 0xF8U) == 0xF0U) {
        need = 4U;
        value = (uint32_t)(b0 & 0x07U);
        min_value = 0x10000U;
    } else {
        return -3;
    }

    if (len < need) {
        return -2;
    }

    for (uint8_t i = 1U; i < need; i++) {
        if (!m17_packet_continuation_valid(input[i])) {
            return -4;
        }
        value = (uint32_t)((value << 6U) | (uint32_t)(input[i] & 0x3FU));
    }
    if (value < min_value || value > M17_PACKET_PROTOCOL_MAX) {
        return -5;
    }

    out->identifier = value;
    out->length = need;
    return 0;
}
