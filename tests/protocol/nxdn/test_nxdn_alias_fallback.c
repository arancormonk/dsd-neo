// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Focused checks for the no-iconv NXDN alias text fallback.
 */

#include <dsd-neo/protocol/nxdn/nxdn_alias_decode.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "dsd-neo/core/safe_api.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

static int
expect_str(const char* tag, const char* got, const char* want) {
    if (strcmp(got, want) != 0) {
        DSD_FPRINTF(stderr, "%s: got '%s' want '%s'\n", tag, got, want);
        return 1;
    }
    return 0;
}

static int
expect_size(const char* tag, size_t got, size_t want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %zu want %zu\n", tag, got, want);
        return 1;
    }
    return 0;
}

static int
test_no_iconv_fallback_text_classes(void) {
    char out[32];
    int rc = 0;

    static const uint8_t ascii_padded[] = {'N', 'X', 'D', 'N', ' ', ' ', 0x00, 'Z'};
    size_t out_len = nxdn_alias_decode_shift_jis_like(ascii_padded, sizeof(ascii_padded), out, sizeof(out));
    rc |= expect_str("fallback ASCII trims trailing spaces", out, "NXDN");
    rc |= expect_size("fallback ASCII length", out_len, 4U);

    static const uint8_t halfwidth_katakana[] = {0xA1U, 0x00U};
    out_len = nxdn_alias_decode_shift_jis_like(halfwidth_katakana, sizeof(halfwidth_katakana), out, sizeof(out));
    rc |= expect_str("fallback half-width katakana", out, "\xEF\xBD\xA1");
    rc |= expect_size("fallback half-width katakana length", out_len, strlen(out));

    static const uint8_t unsupported_multibyte[] = {0x93U, 0xFAU, 0x00U};
    out_len = nxdn_alias_decode_shift_jis_like(unsupported_multibyte, sizeof(unsupported_multibyte), out, sizeof(out));
    rc |= expect_str("fallback unsupported multibyte replacement", out, "\xEF\xBF\xBD");
    rc |= expect_size("fallback unsupported multibyte length", out_len, strlen(out));

    static const uint8_t invalid_byte[] = {0x80U, 0x00U};
    out_len = nxdn_alias_decode_shift_jis_like(invalid_byte, sizeof(invalid_byte), out, sizeof(out));
    rc |= expect_str("fallback invalid byte marker", out, "?");
    rc |= expect_size("fallback invalid byte length", out_len, 1U);

    return rc;
}

static int
test_no_iconv_fallback_bounds(void) {
    char out[4];
    static const uint8_t input[] = {'A', 'B', 'C', 'D', 'E', 0x00U};
    size_t out_len = nxdn_alias_decode_shift_jis_like(input, sizeof(input), out, sizeof(out));

    int rc = 0;
    rc |= expect_str("fallback bounded output", out, "ABC");
    rc |= expect_size("fallback bounded length", out_len, 3U);
    return rc;
}

int
main(void) {
    int rc = 0;

    rc |= test_no_iconv_fallback_text_classes();
    rc |= test_no_iconv_fallback_bounds();

    if (rc == 0) {
        printf("NXDN_ALIAS_FALLBACK: OK\n");
    }
    return rc;
}
