// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Protocol-internal DMR Defined Short Data text decoding.
 */

#ifndef DSD_NEO_SRC_PROTOCOL_DMR_DMR_TEXT_H_
#define DSD_NEO_SRC_PROTOCOL_DMR_DMR_TEXT_H_

#include <stddef.h>
#include <stdint.h>

enum { DMR_TEXT_RESULT_CAPACITY = 2000 };

typedef struct {
    char text[DMR_TEXT_RESULT_CAPACITY];
    const char* declared_encoding;
    const char* effective_encoding;
    uint8_t supported;
    uint8_t malformed;
    uint8_t truncated;
    uint8_t compatibility;
    uint8_t has_content;
} dmr_text_result;

const char* dmr_defined_data_encoding_name(uint8_t dd_format);

/* Convert a complete byte-oriented short-data span to UTF-8. */
int dmr_decode_defined_short_data(uint8_t dd_format, const uint8_t* input, size_t input_len, int packet_crc_valid,
                                  dmr_text_result* result);

/* Remove protocol bit padding and require a byte-aligned Unicode span. */
int dmr_short_data_payload_bytes(size_t assembled_bits, uint8_t bit_padding, size_t* payload_bytes);

#endif /* DSD_NEO_SRC_PROTOCOL_DMR_DMR_TEXT_H_ */
