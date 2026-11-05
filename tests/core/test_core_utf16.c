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

/* Well-formed per RFC 3629, at each sequence length's edges; every other byte pattern is refused. */
static void
test_utf8_validation(void) {
    assert(dsd_utf8_is_valid(""));
    assert(dsd_utf8_is_valid("plain \x01\x7F"));
    assert(dsd_utf8_is_valid("\xC2\x80\xDF\xBF"));                     /* U+0080, U+07FF */
    assert(dsd_utf8_is_valid("\xE0\xA0\x80\xED\x9F\xBF\xEF\xBF\xBF")); /* U+0800, U+D7FF, U+FFFF */
    assert(dsd_utf8_is_valid("\xF0\x90\x80\x80\xF4\x8F\xBF\xBF"));     /* U+10000, U+10FFFF */
    assert(!dsd_utf8_is_valid(NULL));
    assert(!dsd_utf8_is_valid("caf\xE9"));  /* Latin-1 */
    assert(!dsd_utf8_is_valid("\x80"));     /* a lone continuation byte */
    assert(!dsd_utf8_is_valid("\xC0\xAF")); /* overlong, two bytes */
    assert(!dsd_utf8_is_valid("\xC1\xBF"));
    assert(!dsd_utf8_is_valid("\xE0\x9F\xBF"));     /* overlong, three bytes */
    assert(!dsd_utf8_is_valid("\xED\xA0\x80"));     /* a surrogate */
    assert(!dsd_utf8_is_valid("\xF0\x8F\xBF\xBF")); /* overlong, four bytes */
    assert(!dsd_utf8_is_valid("\xF4\x90\x80\x80")); /* above U+10FFFF */
    assert(!dsd_utf8_is_valid("\xF5\x80\x80\x80"));
    assert(!dsd_utf8_is_valid("\xE2\x82")); /* cut short by the terminator */
    assert(!dsd_utf8_is_valid("\xF0\x9F\x93"));
    assert(!dsd_utf8_is_valid("\xE2\x82\x41")); /* a non-continuation byte inside */
}

/* The Windows user-file opener (path_policy.c) opens a path through the wide calls when it is well-formed UTF-8, as the
   app writes a list's file named in any script, and through the ANSI calls otherwise, as a command line in the ANSI
   code page gives it. */
static void
test_utf8_validation_of_paths(void) {
    assert(dsd_utf8_is_valid("C:\\data\\scan_lists\\u-1\\Pol\xC3\xAD"
                             "cia.csv"));
    assert(dsd_utf8_is_valid("scan_lists/u-1/\xD7\xA9\xD7\x9E\xD7\xA8.csv")); /* Hebrew */
    assert(dsd_utf8_is_valid("C:\\lists\\plain.csv"));
    assert(!dsd_utf8_is_valid("C:\\lists\\Pol\xED"
                              "cia.csv")); /* the same name in code page 1252 */
}

/* Encode @p count units, expecting @p want (NULL: a refusal). */
static void
expect_utf16_to_utf8(const uint16_t* units, size_t count, const char* want) {
    char out[64];
    DSD_MEMSET(out, 'x', sizeof out);
    const size_t n = dsd_utf16_to_utf8(units, count, out, sizeof out);
    if (want == NULL) {
        assert(n == DSD_UTF16_TO_UTF8_FAILED && out[0] == '\0');
        return;
    }
    if (n != strlen(want) || strcmp(out, want) != 0) {
        DSD_FPRINTF(stderr, "utf16 to utf8: got %zu \"%s\", want %zu \"%s\"\n", n, out, strlen(want), want);
    }
    assert(n == strlen(want) && strcmp(out, want) == 0);
}

/* A Java string's UTF-16 encodes as standard UTF-8 (issue #644: a target id typed on Android, read through JNI): ASCII,
   2- and 3-byte BMP values, a supplementary character as one 4-byte sequence (never the two 3-byte surrogates modified
   UTF-8 writes), unpaired halves as U+FFFD, and an embedded U+0000 refused rather than ending the text early. */
static void
test_utf16_to_utf8_encodes_standard_utf8(void) {
    static const uint16_t ascii[] = {'f', 'i', 'r', 'e'};
    expect_utf16_to_utf8(ascii, 4, "fire");
    expect_utf16_to_utf8(ascii, 0, "");
    static const uint16_t bmp[] = {'P', 'o', 'l', 0x00EDU, 'c', 'i', 'a', ' ', 0x20ACU};
    expect_utf16_to_utf8(bmp, 9,
                         "Pol\xC3\xAD"
                         "cia \xE2\x82\xAC");
    static const uint16_t engine[] = {0xD83DU, 0xDE92U, '-', '1'}; /* U+1F692 FIRE ENGINE */
    expect_utf16_to_utf8(engine, 4, "\xF0\x9F\x9A\x92-1");
    static const uint16_t lone_high[] = {'a', 0xD83DU, 'b'};
    expect_utf16_to_utf8(lone_high, 3,
                         "a\xEF\xBF\xBD"
                         "b");
    static const uint16_t high_at_end[] = {'a', 0xD83DU};
    expect_utf16_to_utf8(high_at_end, 2, "a\xEF\xBF\xBD");
    static const uint16_t lone_low[] = {0xDE92U, 'z'};
    expect_utf16_to_utf8(lone_low, 2, "\xEF\xBF\xBDz");
    static const uint16_t nul[] = {'-', 'Y', 0x0000U, 'x'};
    expect_utf16_to_utf8(nul, 4, NULL);
    /* The bound is exact: 3 bytes a unit and the terminator. */
    char exact[7];
    assert(dsd_utf16_to_utf8(ascii, 1, exact, 4) == 1U && strcmp(exact, "f") == 0);
    assert(dsd_utf16_to_utf8(engine, 2, exact, sizeof exact) == 4U && strcmp(exact, "\xF0\x9F\x9A\x92") == 0);
    assert(dsd_utf16_to_utf8(lone_low, 2, exact, sizeof exact) == 4U && strcmp(exact, "\xEF\xBF\xBDz") == 0);
    /* Too small an output is refused, never cut. */
    char small[4];
    assert(dsd_utf16_to_utf8(engine, 4, small, sizeof small) == DSD_UTF16_TO_UTF8_FAILED && small[0] == '\0');
    assert(dsd_utf16_to_utf8(NULL, 1, small, sizeof small) == DSD_UTF16_TO_UTF8_FAILED);
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
    test_utf8_validation();
    test_utf8_validation_of_paths();
    test_utf16_to_utf8_encodes_standard_utf8();
    DSD_FPRINTF(stderr, "CORE_UTF16: PASS\n");
    return 0;
}
