// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "dmr_text.h"
#include "dsd-neo/core/safe_api.h"

static void
expect_text(uint8_t format, const uint8_t* input, size_t input_len, int crc_valid, const char* expected, int malformed,
            int compatibility) {
    dmr_text_result result;
    assert(dmr_decode_defined_short_data(format, input, input_len, crc_valid, &result) == 0);
    assert(result.supported == 1U);
    if (strcmp(result.text, expected) != 0) {
        DSD_FPRINTF(stderr, "format 0x%02X text mismatch: got '%s', expected '%s'\n", format, result.text, expected);
    }
    assert(strcmp(result.text, expected) == 0);
    assert(result.malformed == (uint8_t)malformed);
    assert(result.compatibility == (uint8_t)compatibility);
}

static void
test_utf8_and_controls(void) {
    static const uint8_t multilingual[] = {
        0xEFU, 0xBBU, 0xBFU, 'A', 0xCEU, 0xA9U, 0xF0U, 0x9FU, 0x98U, 0x80U,
    };
    static const uint8_t controls[] = {
        'A', 0x09U, 'B', 0x0AU, 'C', 0x0DU, 'D', 0xC2U, 0x85U, 'E', 0x00U, 'Z',
    };
    static const uint8_t malformed[] = {0xC0U, 0xAFU, 'A'};

    expect_text(0x12U, multilingual, sizeof(multilingual), 0, "A\xCE\xA9\xF0\x9F\x98\x80", 0, 0);
    expect_text(0x12U, controls, sizeof(controls), 0,
                "A B C D\xEF\xBF\xBD"
                "E",
                0, 0);

    dmr_text_result result;
    assert(dmr_decode_defined_short_data(0x12U, malformed, sizeof(malformed), 0, &result) == 0);
    assert(result.malformed == 1U);
    assert(strstr(result.text, "\xEF\xBF\xBD") != NULL);
}

static void
test_utf16_byte_order_and_validation(void) {
    static const uint8_t default_be[] = {0x00U, 0x41U, 0x03U, 0xA9U, 0xD8U, 0x3DU, 0xDEU, 0x00U};
    static const uint8_t bom_le[] = {0xFFU, 0xFEU, 0x42U, 0x00U};
    static const uint8_t explicit_be[] = {0x4FU, 0x60U, 0x59U, 0x7DU};
    static const uint8_t explicit_le[] = {0x60U, 0x4FU, 0x7DU, 0x59U};
    static const uint8_t explicit_be_bom[] = {0xFEU, 0xFFU, 0x00U, 0x41U};
    static const uint8_t malformed[] = {0xD8U, 0x00U, 0x00U, 0x41U, 0xDCU, 0x00U, 0xFFU};

    expect_text(0x13U, default_be, sizeof(default_be), 0, "A\xCE\xA9\xF0\x9F\x98\x80", 0, 0);
    expect_text(0x13U, bom_le, sizeof(bom_le), 0, "B", 0, 0);
    expect_text(0x14U, explicit_be, sizeof(explicit_be), 0, "\xE4\xBD\xA0\xE5\xA5\xBD", 0, 0);
    expect_text(0x15U, explicit_le, sizeof(explicit_le), 0, "\xE4\xBD\xA0\xE5\xA5\xBD", 0, 0);
    expect_text(0x14U, explicit_be_bom, sizeof(explicit_be_bom), 0,
                "\xEF\xBB\xBF"
                "A",
                0, 0);

    dmr_text_result result;
    assert(dmr_decode_defined_short_data(0x14U, malformed, sizeof(malformed), 0, &result) == 0);
    assert(result.malformed == 1U);
    assert(strstr(result.text, "\xEF\xBF\xBD") != NULL);
}

static void
test_utf32_byte_order_and_validation(void) {
    static const uint8_t default_be[] = {0x00U, 0x00U, 0x00U, 0x41U, 0x00U, 0x01U, 0xF6U, 0x00U};
    static const uint8_t bom_le[] = {0xFFU, 0xFEU, 0x00U, 0x00U, 0x42U, 0x00U, 0x00U, 0x00U};
    static const uint8_t explicit_be[] = {0x00U, 0x00U, 0x03U, 0xA9U};
    static const uint8_t explicit_le[] = {0xA9U, 0x03U, 0x00U, 0x00U};
    static const uint8_t explicit_le_bom[] = {0xFFU, 0xFEU, 0x00U, 0x00U, 0x41U, 0x00U, 0x00U, 0x00U};
    static const uint8_t malformed[] = {0x00U, 0x11U, 0x00U, 0x00U, 0x00U, 0x00U, 0xD8U, 0x00U, 0xFFU};

    expect_text(0x16U, default_be, sizeof(default_be), 0, "A\xF0\x9F\x98\x80", 0, 0);
    expect_text(0x16U, bom_le, sizeof(bom_le), 0, "B", 0, 0);
    expect_text(0x17U, explicit_be, sizeof(explicit_be), 0, "\xCE\xA9", 0, 0);
    expect_text(0x18U, explicit_le, sizeof(explicit_le), 0, "\xCE\xA9", 0, 0);
    expect_text(0x18U, explicit_le_bom, sizeof(explicit_le_bom), 0,
                "\xEF\xBB\xBF"
                "A",
                0, 0);

    dmr_text_result result;
    assert(dmr_decode_defined_short_data(0x17U, malformed, sizeof(malformed), 0, &result) == 0);
    assert(result.malformed == 1U);
    assert(strstr(result.text, "\xEF\xBF\xBD") != NULL);
}

static void
test_padding_and_truncation(void) {
    size_t payload_bytes = 0U;
    assert(dmr_short_data_payload_bytes(80U, 16U, &payload_bytes) == 0);
    assert(payload_bytes == 8U);
    assert(dmr_short_data_payload_bytes(80U, 7U, &payload_bytes) == -1);
    assert(dmr_short_data_payload_bytes(8U, 9U, &payload_bytes) == -1);
    assert(dmr_short_data_payload_bytes(8U, 0U, NULL) == -1);

    uint8_t input[2100];
    for (size_t i = 0U; i < sizeof(input); i += 3U) {
        input[i] = 0xE2U;
        input[i + 1U] = 0x82U;
        input[i + 2U] = 0xACU;
    }
    dmr_text_result result;
    assert(dmr_decode_defined_short_data(0x12U, input, sizeof(input), 0, &result) == 0);
    assert(result.truncated == 1U);
    size_t output_len = strlen(result.text);
    assert(output_len <= (DMR_TEXT_RESULT_CAPACITY - 1U));
    assert((output_len % 3U) == 0U);
    assert(output_len >= 3U);
    assert(memcmp(result.text + output_len - 3U, "\xE2\x80\xA6", 3U) == 0);
    for (size_t i = 0U; i + 3U < output_len; i += 3U) {
        assert(memcmp(result.text + i, "\xE2\x82\xAC", 3U) == 0);
    }
}

static void
test_utf32_compatibility_fallback(void) {
    static const char message[] = "helo i am junior, its a text message.";
    uint8_t issue_payload[(sizeof(message) - 1U) * 2U];
    for (size_t i = 0U; i < sizeof(message) - 1U; i++) {
        issue_payload[i * 2U] = 0U;
        issue_payload[(i * 2U) + 1U] = (uint8_t)message[i];
    }

    dmr_text_result result;
    assert(dmr_decode_defined_short_data(0x16U, issue_payload, sizeof(issue_payload), 1, &result) == 0);
    assert(result.compatibility == 1U);
    assert(strcmp(result.declared_encoding, "UTF-32") == 0);
    assert(strcmp(result.effective_encoding, "UTF-16BE compatibility") == 0);
    assert(strcmp(result.text, message) == 0);

    assert(dmr_decode_defined_short_data(0x16U, issue_payload, sizeof(issue_payload), 0, &result) == 0);
    assert(result.compatibility == 0U);
    assert(result.malformed == 1U);

    static const uint8_t valid_utf32[] = {0x00U, 0x00U, 0x00U, 0x41U};
    assert(dmr_decode_defined_short_data(0x16U, valid_utf32, sizeof(valid_utf32), 1, &result) == 0);
    assert(result.compatibility == 0U);
    assert(result.malformed == 0U);
    assert(strcmp(result.text, "A") == 0);

    static const uint8_t invalid_utf16_candidate[] = {0x00U, 0x00U, 0xD8U, 0x00U};
    assert(dmr_decode_defined_short_data(0x16U, invalid_utf16_candidate, sizeof(invalid_utf16_candidate), 1, &result)
           == 0);
    assert(result.compatibility == 0U);
    assert(result.malformed == 1U);
}

static void
test_unsupported_and_invalid_arguments(void) {
    dmr_text_result result;
    assert(dmr_decode_defined_short_data(0x11U, NULL, 0U, 0, &result) == 0);
    assert(result.supported == 0U);
    assert(strcmp(result.text, "") == 0);
    assert(dmr_decode_defined_short_data(0x12U, NULL, 1U, 0, &result) == -1);
    assert(dmr_decode_defined_short_data(0x12U, NULL, 0U, 0, NULL) == -1);
}

int
main(void) {
    test_utf8_and_controls();
    test_utf16_byte_order_and_validation();
    test_utf32_byte_order_and_validation();
    test_padding_and_truncation();
    test_utf32_compatibility_fallback();
    test_unsupported_and_invalid_arguments();
    puts("DMR_TEXT: OK");
    return 0;
}
