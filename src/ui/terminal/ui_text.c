// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Text measured and cut in terminal cells, for the status row and the chooser (issues #669, #672). See ui_text.h. */

#include "ui_text.h"

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/runtime/unicode.h>
#include <stdint.h>
#include <wchar.h>

/* The cut marker: U+2026 HORIZONTAL ELLIPSIS, or three periods. Arrays, so the choice can be read off the pointer
   dsd_unicode_or_ascii() returns. */
static const char k_marker_utf8[] = "\xE2\x80\xA6";
static const char k_marker_ascii[] = "...";

/* One UTF-8 sequence at s (n bytes left): its length, or 0 when it is not a valid, shortest-form scalar value. */
static size_t
ui_text_decode(const unsigned char* s, size_t n, uint32_t* cp) {
    const unsigned char c = s[0];
    size_t len = 0;
    uint32_t v = 0;
    uint32_t min = 0;
    if (c < 0x80U) {
        *cp = c;
        return 1;
    }
    if (c >= 0xC2U && c <= 0xDFU) {
        len = 2;
        v = c & 0x1FU;
        min = 0x80U;
    } else if (c >= 0xE0U && c <= 0xEFU) {
        len = 3;
        v = c & 0x0FU;
        min = 0x800U;
    } else if (c >= 0xF0U && c <= 0xF4U) {
        len = 4;
        v = c & 0x07U;
        min = 0x10000U;
    } else {
        return 0;
    }
    if (n < len) {
        return 0;
    }
    for (size_t i = 1; i < len; i++) {
        if ((s[i] & 0xC0U) != 0x80U) {
            return 0;
        }
        v = (v << 6) | (s[i] & 0x3FU);
    }
    if (v < min || v > 0x10FFFFU || (v >= 0xD800U && v <= 0xDFFFU)) {
        return 0;
    }
    *cp = v;
    return len;
}

/* Cells a code point takes, or -1 when the library cannot draw it and it is shown as '?'. */
static int
ui_text_code_point_cells(int unicode, uint32_t cp) {
    if (cp < 0x20U || cp == 0x7FU) {
        return -1;
    }
    if (cp < 0x80U) {
        return 1;
    }
    if (!unicode || cp <= 0x9FU) {
        return -1;
    }
#if defined(DSD_USE_PDCURSES)
    /* PDCurses draws one cell per character and decodes the Basic Multilingual Plane only. */
    return (cp > 0xFFFFU) ? -1 : 1;
#else
    const int w = wcwidth((wchar_t)cp);
    return (w < 0) ? -1 : w;
#endif
}

/* The next code point at s + i: its byte length (a stray byte is one) and the cells it is drawn in. */
static size_t
ui_text_step(int unicode, const unsigned char* s, size_t i, size_t len, int* cells) {
    uint32_t cp = 0;
    const size_t n = ui_text_decode(s + i, len - i, &cp);
    if (n == 0U) {
        *cells = 1;
        return 1U;
    }
    const int w = ui_text_code_point_cells(unicode, cp);
    *cells = (w < 0) ? 1 : w;
    return n;
}

#if !defined(DSD_USE_PDCURSES)
/* Whether the locale dsd_unicode_init_locale() set before initscr() decodes UTF-8, as ncurses will. */
static int
ui_text_locale_is_utf8(void) {
    mbstate_t st;
    DSD_MEMSET(&st, 0, sizeof st);
    wchar_t wc = 0;
    const size_t n = mbrtowc(&wc, k_marker_utf8, 3U, &st);
    return n == 3U && (uint32_t)wc == 0x2026U;
}
#endif

int
ui_text_unicode_output(void) {
#if defined(DSD_USE_PDCURSES) && !defined(DSD_HAS_PDCURSES_WIDE_API)
    /* A narrow PDCurses writes each byte of a UTF-8 sequence as a character of its own: ASCII only. */
    return 0;
#else
    /* The user's preference (DSD_FORCE_ASCII turns it off, a forced UTF-8 can turn it on under a C locale), and a
       library that draws UTF-8: a wide PDCurses (as the SNR meter requires, ncurses_snr.c), or ncurses in a locale that
       decodes it. */
    const int wanted = dsd_unicode_or_ascii(k_marker_utf8, k_marker_ascii) == k_marker_utf8;
#if defined(DSD_USE_PDCURSES)
    return wanted;
#else
    return wanted && ui_text_locale_is_utf8();
#endif
#endif
}

const char*
ui_text_marker(int unicode) {
    return unicode ? k_marker_utf8 : k_marker_ascii;
}

int
ui_text_marker_cells(int unicode) {
    return unicode ? 1 : 3;
}

size_t
ui_text_sanitize(int unicode, const char* text, size_t len, char* out, size_t out_size) {
    if (!out || out_size == 0U) {
        return 0U;
    }
    out[0] = '\0';
    if (!text) {
        return 0U;
    }
    const unsigned char* s = (const unsigned char*)text;
    size_t written = 0U;
    size_t i = 0U;
    while (i < len) {
        uint32_t cp = 0;
        size_t n = ui_text_decode(s + i, len - i, &cp);
        const char* piece = (const char*)(s + i);
        size_t piece_len = n;
        if (n == 0U) {
            n = 1U;
            piece = "?";
            piece_len = 1U;
        } else if (ui_text_code_point_cells(unicode, cp) < 0) {
            piece = "?";
            piece_len = 1U;
        }
        if (written + piece_len >= out_size) {
            break;
        }
        DSD_MEMCPY(out + written, piece, piece_len);
        written += piece_len;
        out[written] = '\0';
        i += n;
    }
    return written;
}

int
ui_text_cells(int unicode, const char* text, size_t len) {
    if (!text) {
        return 0;
    }
    const unsigned char* s = (const unsigned char*)text;
    int cells = 0;
    size_t i = 0U;
    while (i < len) {
        int w = 0;
        i += ui_text_step(unicode, s, i, len, &w);
        cells += w;
    }
    return cells;
}

size_t
ui_text_cut(int unicode, const char* text, size_t len, int limit) {
    if (!text) {
        return 0U;
    }
    const unsigned char* s = (const unsigned char*)text;
    size_t i = 0U;
    int cells = 0;
    while (i < len) {
        int w = 0;
        const size_t n = ui_text_step(unicode, s, i, len, &w);
        if (cells + w > limit) {
            break;
        }
        cells += w;
        i += n;
    }
    return i;
}

size_t
ui_text_fit(int unicode, const char* text, size_t len, int budget, const char** marker) {
    *marker = NULL;
    if (!text || budget <= 0) {
        return 0U;
    }
    if (ui_text_cells(unicode, text, len) <= budget) {
        return len;
    }
    const int marker_cells = ui_text_marker_cells(unicode);
    if (budget <= marker_cells) {
        /* No room for a marker beside any text: cut without one. */
        return ui_text_cut(unicode, text, len, budget);
    }
    size_t cut = ui_text_cut(unicode, text, len, budget - marker_cells);
    /* The marker follows the last character shown, not the blanks before it. */
    while (cut > 0U && text[cut - 1U] == ' ') {
        cut--;
    }
    *marker = ui_text_marker(unicode);
    return cut;
}
