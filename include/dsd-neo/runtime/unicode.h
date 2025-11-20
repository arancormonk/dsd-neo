// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Unicode/ASCII fallback utilities.
 */

#ifndef DSD_NEO_UNICODE_H
#define DSD_NEO_UNICODE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Return 1 if UTF-8 output is likely supported, else 0. Cached after first call. */
int dsd_unicode_supported(void);

/* Convenience: pick Unicode or ASCII string based on support. */
static inline const char*
dsd_unicode_or_ascii(const char* unicode_str, const char* ascii_str) {
    return dsd_unicode_supported() ? unicode_str : ascii_str;
}

/* Degree glyph string with ASCII fallback ("\xC2\xB0" vs " deg"). */
const char* dsd_degrees_glyph(void);

/* Convert a UTF-8 string to ASCII-safe form in out buffer; returns out. */
char* dsd_ascii_fallback(const char* in, char* out, size_t out_sz);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_UNICODE_H */
