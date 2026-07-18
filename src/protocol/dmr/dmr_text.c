// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Bounded, locale-independent Unicode rendering for DMR Defined Short Data.
 */

#include "dmr_text.h"

#include <dsd-neo/core/safe_api.h>

#include <stddef.h>
#include <stdint.h>

typedef struct {
    dmr_text_result* result;
    size_t length;
    uint8_t stopped;
} dmr_text_builder;

const char*
dmr_defined_data_encoding_name(uint8_t dd_format) {
    switch (dd_format) {
        case 0x12: return "UTF-8";
        case 0x13: return "UTF-16";
        case 0x14: return "UTF-16BE";
        case 0x15: return "UTF-16LE";
        case 0x16: return "UTF-32";
        case 0x17: return "UTF-32BE";
        case 0x18: return "UTF-32LE";
        default: return "unsupported";
    }
}

static int
dmr_text_is_control(uint32_t scalar) {
    return scalar < 0x20U || (scalar >= 0x7FU && scalar <= 0x9FU);
}

static int
dmr_text_is_scalar(uint32_t scalar) {
    return scalar <= 0x10FFFFU && !(scalar >= 0xD800U && scalar <= 0xDFFFU);
}

static size_t
dmr_text_encode_utf8(uint32_t scalar, uint8_t encoded[4]) {
    if (scalar <= 0x7FU) {
        encoded[0] = (uint8_t)scalar;
        return 1U;
    }
    if (scalar <= 0x7FFU) {
        encoded[0] = (uint8_t)(0xC0U | (scalar >> 6));
        encoded[1] = (uint8_t)(0x80U | (scalar & 0x3FU));
        return 2U;
    }
    if (scalar <= 0xFFFFU) {
        encoded[0] = (uint8_t)(0xE0U | (scalar >> 12));
        encoded[1] = (uint8_t)(0x80U | ((scalar >> 6) & 0x3FU));
        encoded[2] = (uint8_t)(0x80U | (scalar & 0x3FU));
        return 3U;
    }
    encoded[0] = (uint8_t)(0xF0U | (scalar >> 18));
    encoded[1] = (uint8_t)(0x80U | ((scalar >> 12) & 0x3FU));
    encoded[2] = (uint8_t)(0x80U | ((scalar >> 6) & 0x3FU));
    encoded[3] = (uint8_t)(0x80U | (scalar & 0x3FU));
    return 4U;
}

static void
dmr_text_remove_last_scalar(dmr_text_builder* builder) {
    if (builder->length == 0U) {
        return;
    }
    size_t start = builder->length - 1U;
    while (start > 0U && (((uint8_t)builder->result->text[start] & 0xC0U) == 0x80U)) {
        start--;
    }
    builder->length = start;
    builder->result->text[start] = '\0';
}

static void
dmr_text_append_ellipsis(dmr_text_builder* builder) {
    static const uint8_t ellipsis[] = {0xE2U, 0x80U, 0xA6U};
    const size_t limit = sizeof(builder->result->text) - 1U;
    while (builder->length + sizeof(ellipsis) > limit && builder->length > 0U) {
        dmr_text_remove_last_scalar(builder);
    }
    if (builder->length + sizeof(ellipsis) <= limit) {
        for (size_t i = 0; i < sizeof(ellipsis); i++) {
            builder->result->text[builder->length++] = (char)ellipsis[i];
        }
        builder->result->text[builder->length] = '\0';
    }
}

static void
dmr_text_append_scalar(dmr_text_builder* builder, uint32_t scalar) {
    if (builder->stopped || builder->result->truncated) {
        return;
    }
    if (scalar == 0U) {
        builder->stopped = 1U;
        return;
    }

    if (!dmr_text_is_control(scalar)) {
        builder->result->has_content = 1U;
    }
    if (scalar == 0x09U || scalar == 0x0AU || scalar == 0x0DU) {
        scalar = 0x20U;
    } else if (dmr_text_is_control(scalar)) {
        scalar = 0xFFFDU;
    }

    uint8_t encoded[4];
    size_t encoded_len = dmr_text_encode_utf8(scalar, encoded);
    const size_t limit = sizeof(builder->result->text) - 1U;
    if (builder->length + encoded_len > limit) {
        builder->result->truncated = 1U;
        dmr_text_append_ellipsis(builder);
        return;
    }
    for (size_t i = 0; i < encoded_len; i++) {
        builder->result->text[builder->length++] = (char)encoded[i];
    }
    builder->result->text[builder->length] = '\0';
}

static void
dmr_text_append_malformed(dmr_text_builder* builder) {
    builder->result->malformed = 1U;
    dmr_text_append_scalar(builder, 0xFFFDU);
}

static size_t
dmr_text_utf8_lead(uint8_t first, uint32_t* scalar) {
    if (first <= 0x7FU) {
        *scalar = first;
        return 1U;
    }
    if (first >= 0xC2U && first <= 0xDFU) {
        *scalar = (uint32_t)(first & 0x1FU);
        return 2U;
    }
    if (first >= 0xE0U && first <= 0xEFU) {
        *scalar = (uint32_t)(first & 0x0FU);
        return 3U;
    }
    if (first >= 0xF0U && first <= 0xF4U) {
        *scalar = (uint32_t)(first & 0x07U);
        return 4U;
    }
    return 0U;
}

static int
dmr_text_utf8_second_is_valid(uint8_t first, uint8_t second) {
    if ((second & 0xC0U) != 0x80U) {
        return 0;
    }
    if (first == 0xE0U && second < 0xA0U) {
        return 0;
    }
    if (first == 0xEDU && second > 0x9FU) {
        return 0;
    }
    if (first == 0xF0U && second < 0x90U) {
        return 0;
    }
    if (first == 0xF4U && second > 0x8FU) {
        return 0;
    }
    return 1;
}

static int
dmr_text_utf8_sequence_is_valid(const uint8_t* input, size_t input_len, size_t offset, size_t count) {
    if (count > input_len - offset) {
        return 0;
    }
    if (count == 1U) {
        return 1;
    }
    if (!dmr_text_utf8_second_is_valid(input[offset], input[offset + 1U])) {
        return 0;
    }
    for (size_t j = 2U; j < count; j++) {
        if ((input[offset + j] & 0xC0U) != 0x80U) {
            return 0;
        }
    }
    return 1;
}

static uint32_t
dmr_text_utf8_finish_scalar(const uint8_t* input, size_t offset, size_t count, uint32_t scalar) {
    for (size_t j = 1U; j < count; j++) {
        scalar = (scalar << 6) | (uint32_t)(input[offset + j] & 0x3FU);
    }
    return scalar;
}

static void
dmr_text_decode_utf8(const uint8_t* input, size_t input_len, dmr_text_builder* builder) {
    size_t i = 0U;
    if (input_len >= 3U && input[0] == 0xEFU && input[1] == 0xBBU && input[2] == 0xBFU) {
        i = 3U;
    }

    while (i < input_len) {
        uint8_t first = input[i];
        uint32_t scalar = 0U;
        size_t count = dmr_text_utf8_lead(first, &scalar);
        if (count == 0U || !dmr_text_utf8_sequence_is_valid(input, input_len, i, count)) {
            dmr_text_append_malformed(builder);
            i++;
            continue;
        }
        scalar = dmr_text_utf8_finish_scalar(input, i, count, scalar);
        dmr_text_append_scalar(builder, scalar);
        i += count;
    }
}

static uint16_t
dmr_text_read_u16(const uint8_t* input, int little_endian) {
    if (little_endian) {
        return (uint16_t)((uint16_t)input[0] | ((uint16_t)input[1] << 8));
    }
    return (uint16_t)(((uint16_t)input[0] << 8) | (uint16_t)input[1]);
}

static uint32_t
dmr_text_read_u32(const uint8_t* input, int little_endian) {
    if (little_endian) {
        return (uint32_t)input[0] | ((uint32_t)input[1] << 8) | ((uint32_t)input[2] << 16) | ((uint32_t)input[3] << 24);
    }
    return ((uint32_t)input[0] << 24) | ((uint32_t)input[1] << 16) | ((uint32_t)input[2] << 8) | (uint32_t)input[3];
}

static size_t
dmr_text_utf16_bom_offset(const uint8_t* input, size_t input_len, int detect_bom, int* little_endian) {
    if (!detect_bom || input_len < 2U) {
        return 0U;
    }
    if (input[0] == 0xFEU && input[1] == 0xFFU) {
        *little_endian = 0;
        return 2U;
    }
    if (input[0] == 0xFFU && input[1] == 0xFEU) {
        *little_endian = 1;
        return 2U;
    }
    return 0U;
}

static int
dmr_text_utf16_is_high_surrogate(uint16_t value) {
    return value >= 0xD800U && value <= 0xDBFFU;
}

static int
dmr_text_utf16_is_low_surrogate(uint16_t value) {
    return value >= 0xDC00U && value <= 0xDFFFU;
}

static int
dmr_text_utf16_decode_pair(const uint8_t* input, size_t input_len, size_t offset, int little_endian, uint16_t first,
                           uint32_t* scalar) {
    if (input_len - offset < 2U) {
        return 0;
    }
    uint16_t second = dmr_text_read_u16(input + offset, little_endian);
    if (!dmr_text_utf16_is_low_surrogate(second)) {
        return 0;
    }
    *scalar = 0x10000U + (((uint32_t)first - 0xD800U) << 10) + ((uint32_t)second - 0xDC00U);
    return 1;
}

static void
dmr_text_decode_utf16(const uint8_t* input, size_t input_len, int little_endian, int detect_bom,
                      dmr_text_builder* builder) {
    size_t i = dmr_text_utf16_bom_offset(input, input_len, detect_bom, &little_endian);

    while (i + 1U < input_len) {
        uint16_t first = dmr_text_read_u16(input + i, little_endian);
        i += 2U;
        if (dmr_text_utf16_is_high_surrogate(first)) {
            uint32_t scalar = 0U;
            if (dmr_text_utf16_decode_pair(input, input_len, i, little_endian, first, &scalar)) {
                dmr_text_append_scalar(builder, scalar);
                i += 2U;
            } else {
                dmr_text_append_malformed(builder);
            }
        } else if (dmr_text_utf16_is_low_surrogate(first)) {
            dmr_text_append_malformed(builder);
        } else {
            dmr_text_append_scalar(builder, first);
        }
    }
    if (i != input_len) {
        dmr_text_append_malformed(builder);
    }
}

static void
dmr_text_decode_utf32(const uint8_t* input, size_t input_len, int little_endian, int detect_bom,
                      dmr_text_builder* builder) {
    size_t i = 0U;
    if (detect_bom && input_len >= 4U) {
        if (input[0] == 0x00U && input[1] == 0x00U && input[2] == 0xFEU && input[3] == 0xFFU) {
            little_endian = 0;
            i = 4U;
        } else if (input[0] == 0xFFU && input[1] == 0xFEU && input[2] == 0x00U && input[3] == 0x00U) {
            little_endian = 1;
            i = 4U;
        }
    }

    while (i + 3U < input_len) {
        uint32_t scalar = dmr_text_read_u32(input + i, little_endian);
        if (dmr_text_is_scalar(scalar)) {
            dmr_text_append_scalar(builder, scalar);
        } else {
            dmr_text_append_malformed(builder);
        }
        i += 4U;
    }
    if (i != input_len) {
        dmr_text_append_malformed(builder);
    }
}

static void
dmr_text_result_init(uint8_t dd_format, dmr_text_result* result) {
    DSD_MEMSET(result, 0, sizeof(*result));
    result->declared_encoding = dmr_defined_data_encoding_name(dd_format);
    result->effective_encoding = result->declared_encoding;
    result->supported = (uint8_t)(dd_format >= 0x12U && dd_format <= 0x18U);
}

static void
dmr_text_decode_format(uint8_t dd_format, const uint8_t* input, size_t input_len, dmr_text_result* result) {
    dmr_text_builder builder = {.result = result, .length = 0U, .stopped = 0U};
    switch (dd_format) {
        case 0x12: dmr_text_decode_utf8(input, input_len, &builder); break;
        case 0x13: dmr_text_decode_utf16(input, input_len, 0, 1, &builder); break;
        case 0x14: dmr_text_decode_utf16(input, input_len, 0, 0, &builder); break;
        case 0x15: dmr_text_decode_utf16(input, input_len, 1, 0, &builder); break;
        case 0x16: dmr_text_decode_utf32(input, input_len, 0, 1, &builder); break;
        case 0x17: dmr_text_decode_utf32(input, input_len, 0, 0, &builder); break;
        case 0x18: dmr_text_decode_utf32(input, input_len, 1, 0, &builder); break;
        default: break;
    }
}

int
dmr_decode_defined_short_data(uint8_t dd_format, const uint8_t* input, size_t input_len, int packet_crc_valid,
                              dmr_text_result* result) {
    if (result == NULL || (input == NULL && input_len != 0U)) {
        return -1;
    }
    dmr_text_result_init(dd_format, result);
    if (!result->supported) {
        return 0;
    }

    dmr_text_decode_format(dd_format, input, input_len, result);
    if (dd_format == 0x16U && packet_crc_valid && result->malformed) {
        dmr_text_result compatibility;
        dmr_text_result_init(0x14U, &compatibility);
        dmr_text_decode_format(0x14U, input, input_len, &compatibility);
        if (!compatibility.malformed && compatibility.has_content) {
            *result = compatibility;
            result->declared_encoding = dmr_defined_data_encoding_name(0x16U);
            result->effective_encoding = "UTF-16BE compatibility";
            result->compatibility = 1U;
        }
    }
    return 0;
}

int
dmr_short_data_payload_bytes(size_t assembled_bits, uint8_t bit_padding, size_t* payload_bytes) {
    if (payload_bytes == NULL || (size_t)bit_padding > assembled_bits) {
        return -1;
    }
    size_t payload_bits = assembled_bits - (size_t)bit_padding;
    if ((payload_bits & 7U) != 0U) {
        return -1;
    }
    *payload_bytes = payload_bits / 8U;
    return 0;
}
