// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * UTF-16 -> scalar decoding and UTF-8 encoding for radio-sourced text (issue #358).
 *
 * Every 16-bit unit of DMR UDT/SMS text and of talker aliases used to go straight to
 * fprintf("%lc"). A unit in the surrogate range - which the garbage an OTA-encrypted repeater
 * emits reaches one time in 32, and which every emoji reaches by design - has no encoding of
 * its own. The Windows UCRT reports that failed conversion as a string of length -1 and then
 * streams the stack to stderr until the process faults. These helpers pair surrogates and map
 * unpaired halves to U+FFFD so the printers never depend on the CRT's wide-character
 * conversion at all.
 */

#include <assert.h>
#include <dsd-neo/core/utf16.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "dsd-neo/core/safe_api.h"

static size_t
push(dsd_utf16_decoder* dec, uint16_t unit, uint32_t* out) {
    return dsd_utf16_decoder_push(dec, unit, out, 2U);
}

static void
test_bmp_units_pass_through(void) {
    dsd_utf16_decoder dec;
    uint32_t out[2];
    dsd_utf16_decoder_reset(&dec);
    assert(push(&dec, 0x0041U, out) == 1U && out[0] == 0x41U);
    assert(push(&dec, 0x4739U, out) == 1U && out[0] == 0x4739U);
    assert(push(&dec, 0x0000U, out) == 1U && out[0] == 0U);
    /* Noncharacters are valid scalar values; they are not the decoder's problem. */
    assert(push(&dec, 0xFFFFU, out) == 1U && out[0] == 0xFFFFU);
    assert(dsd_utf16_decoder_finish(&dec, out, 1U) == 0U);
}

static void
test_surrogate_pair_combines(void) {
    dsd_utf16_decoder dec;
    uint32_t out[2];
    dsd_utf16_decoder_reset(&dec);
    assert(push(&dec, 0xD83DU, out) == 0U); /* the high half is held back */
    assert(push(&dec, 0xDE00U, out) == 1U && out[0] == 0x1F600U);
    assert(dsd_utf16_decoder_finish(&dec, out, 1U) == 0U);

    /* Both ends of the supplementary range. */
    assert(push(&dec, 0xD800U, out) == 0U);
    assert(push(&dec, 0xDC00U, out) == 1U && out[0] == 0x10000U);
    assert(push(&dec, 0xDBFFU, out) == 0U);
    assert(push(&dec, 0xDFFFU, out) == 1U && out[0] == 0x10FFFFU);
}

static void
test_lone_low_surrogate_is_replaced(void) {
    dsd_utf16_decoder dec;
    uint32_t out[2];
    dsd_utf16_decoder_reset(&dec);
    assert(push(&dec, 0xDE00U, out) == 1U && out[0] == DSD_UNICODE_REPLACEMENT);
    assert(push(&dec, 0x0041U, out) == 1U && out[0] == 0x41U);
}

static void
test_orphaned_high_surrogate_is_replaced_and_unit_kept(void) {
    dsd_utf16_decoder dec;
    uint32_t out[2];
    dsd_utf16_decoder_reset(&dec);
    assert(push(&dec, 0xD83DU, out) == 0U);
    assert(push(&dec, 0x0041U, out) == 2U);
    assert(out[0] == DSD_UNICODE_REPLACEMENT && out[1] == 0x41U);

    /* Two highs in a row: the first is orphaned, the second still pairs. */
    assert(push(&dec, 0xD83DU, out) == 0U);
    assert(push(&dec, 0xD83DU, out) == 1U && out[0] == DSD_UNICODE_REPLACEMENT);
    assert(push(&dec, 0xDE00U, out) == 1U && out[0] == 0x1F600U);
}

static void
test_finish_flushes_dangling_high_surrogate(void) {
    dsd_utf16_decoder dec;
    uint32_t out[2];
    dsd_utf16_decoder_reset(&dec);
    assert(push(&dec, 0xD83DU, out) == 0U);
    assert(dsd_utf16_decoder_finish(&dec, out, 1U) == 1U && out[0] == DSD_UNICODE_REPLACEMENT);
    /* The flush consumed it; a second finish has nothing to say. */
    assert(dsd_utf16_decoder_finish(&dec, out, 1U) == 0U);
}

static void
test_push_honours_output_capacity(void) {
    dsd_utf16_decoder dec;
    uint32_t out[2] = {0xAAAAAAAAU, 0xAAAAAAAAU};
    dsd_utf16_decoder_reset(&dec);
    assert(dsd_utf16_decoder_push(&dec, 0x0041U, NULL, 0U) == 0U);
    assert(dsd_utf16_decoder_push(&dec, 0x0041U, out, 0U) == 0U);
    assert(out[0] == 0xAAAAAAAAU);
    assert(dsd_utf16_decoder_push(NULL, 0x0041U, out, 2U) == 0U);
    assert(dsd_utf16_decoder_finish(NULL, out, 1U) == 0U);
    assert(dsd_utf16_decoder_finish(&dec, NULL, 0U) == 0U);
}

static void
expect_utf8(uint32_t scalar, const char* want, size_t want_len) {
    char out[DSD_UTF8_MAX_BYTES + 1];
    DSD_MEMSET(out, 0x7E, sizeof out);
    size_t n = dsd_utf8_encode_scalar(scalar, out, sizeof out);
    assert(n == want_len);
    assert(memcmp(out, want, want_len) == 0);
    assert(out[want_len] == '\0');
}

static void
test_utf8_encoding_boundaries(void) {
    expect_utf8(0x41U, "A", 1U);
    expect_utf8(0x7FU, "\x7F", 1U);
    expect_utf8(0x80U, "\xC2\x80", 2U);
    expect_utf8(0xE9U, "\xC3\xA9", 2U);
    expect_utf8(0x7FFU, "\xDF\xBF", 2U);
    expect_utf8(0x800U, "\xE0\xA0\x80", 3U);
    expect_utf8(0x4739U, "\xE4\x9C\xB9", 3U);
    expect_utf8(0xFFFFU, "\xEF\xBF\xBF", 3U);
    expect_utf8(0x10000U, "\xF0\x90\x80\x80", 4U);
    expect_utf8(0x1F600U, "\xF0\x9F\x98\x80", 4U);
    expect_utf8(0x10FFFFU, "\xF4\x8F\xBF\xBF", 4U);
}

static void
test_utf8_encoding_of_nul(void) {
    char out[DSD_UTF8_MAX_BYTES + 1];
    DSD_MEMSET(out, 0x7E, sizeof out);
    assert(dsd_utf8_encode_scalar(0U, out, sizeof out) == 1U);
    assert(out[0] == '\0' && out[1] == '\0');
}

static void
test_invalid_scalars_encode_as_replacement(void) {
    expect_utf8(0xD800U, "\xEF\xBF\xBD", 3U);
    expect_utf8(0xDBFFU, "\xEF\xBF\xBD", 3U);
    expect_utf8(0xDC00U, "\xEF\xBF\xBD", 3U);
    expect_utf8(0xDFFFU, "\xEF\xBF\xBD", 3U);
    expect_utf8(0x110000U, "\xEF\xBF\xBD", 3U);
    expect_utf8(0xFFFFFFFFU, "\xEF\xBF\xBD", 3U);
}

static void
test_utf8_encoding_needs_room_for_terminator(void) {
    char out[DSD_UTF8_MAX_BYTES + 1];
    DSD_MEMSET(out, 0x7E, sizeof out);
    assert(dsd_utf8_encode_scalar(0x1F600U, out, 4U) == 0U);
    assert(out[0] == '\0');
    assert(dsd_utf8_encode_scalar(0x41U, out, 0U) == 0U);
    assert(dsd_utf8_encode_scalar(0x41U, NULL, sizeof out) == 0U);
}

int
main(void) {
    test_bmp_units_pass_through();
    test_surrogate_pair_combines();
    test_lone_low_surrogate_is_replaced();
    test_orphaned_high_surrogate_is_replaced_and_unit_kept();
    test_finish_flushes_dangling_high_surrogate();
    test_push_honours_output_capacity();
    test_utf8_encoding_boundaries();
    test_utf8_encoding_of_nul();
    test_invalid_scalars_encode_as_replacement();
    test_utf8_encoding_needs_room_for_terminator();
    DSD_FPRINTF(stderr, "CORE_UTF16: PASS\n");
    return 0;
}
