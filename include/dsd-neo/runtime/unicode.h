// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Unicode/ASCII fallback utilities for terminal output.
 */

#ifndef DSD_NEO_UNICODE_H
#define DSD_NEO_UNICODE_H

#include <dsd-neo/core/utf16.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Return 1 if UTF-8 output is likely supported, else 0 (cached). */
int dsd_unicode_supported(void);

/**
 * @brief Print one Unicode scalar value without fprintf("%lc").
 *
 * UTF-8 output encodes the value itself (U+FFFD for anything that is not a scalar value); the
 * ASCII fallback keeps the historical best effort of the printable low byte, else '?'. Radio
 * text must come through here: %lc hands the C runtime a UTF-16 unit it may be unable to
 * encode, and the Windows UCRT turns that into an unbounded write of the stack (issue #358).
 */
static inline void
dsd_unicode_fput_scalar(uint32_t scalar, FILE* stream) {
    if (stream == NULL) {
        return;
    }
    if (dsd_unicode_supported()) {
        char utf8[DSD_UTF8_MAX_BYTES + 1];
        if (dsd_utf8_encode_scalar(scalar, utf8, sizeof utf8) > 0U) {
            (void)fputs(utf8, stream);
        }
        return;
    }
    const unsigned char lo = (unsigned char)(scalar & 0xFFU);
    (void)fputc((lo >= 0x20U && lo < 0x7FU) ? (int)lo : '?', stream);
}

/**
 * @brief Best-effort initialization to make UTF-8 output usable.
 *
 * Attempts to select a UTF-8 locale (LC_CTYPE) and, on native Windows, set the
 * console code page to UTF-8. Safe to call multiple times.
 */
void dsd_unicode_init_locale(void);

/** @brief Convenience helper to pick Unicode or ASCII string based on support. */
const char* dsd_unicode_or_ascii(const char* unicode_str, const char* ascii_str);

/**
 * @brief Return 1 when dense UI glyphs such as block meters are safe to use.
 *
 * This is intentionally stricter than dsd_unicode_supported(): a terminal may
 * accept UTF-8 or wide-character output while its active console font still
 * cannot render block glyphs.
 */
int dsd_unicode_block_glyphs_supported(void);

/** @brief Degree glyph string with ASCII fallback ("\xC2\xB0" vs " deg"). */
const char* dsd_degrees_glyph(void);

/**
 * @brief Convert a UTF-8 string to ASCII-safe form in out buffer.
 * @return Pointer to out buffer.
 */
char* dsd_ascii_fallback(const char* in, char* out, size_t out_sz);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_UNICODE_H */
