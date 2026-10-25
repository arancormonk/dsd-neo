// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* A status row cut between whole fields instead of wrapped (issue #669). See ui_row.h. */

#include "ui_row.h"

#include <curses.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/runtime/unicode.h>
#include <stdarg.h>
#include <stdint.h>
#include <wchar.h>

/* The cut marker: U+2026 HORIZONTAL ELLIPSIS, or three periods. Arrays, so the choice can be read off the pointer
   dsd_unicode_or_ascii() returns. */
static const char k_marker_utf8[] = "\xE2\x80\xA6";
static const char k_marker_ascii[] = "...";

/* One UTF-8 sequence at s (n bytes left): its length, or 0 when it is not a valid, shortest-form scalar value. */
static size_t
ui_row_decode(const unsigned char* s, size_t n, uint32_t* cp) {
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
ui_row_code_point_cells(const ui_row* row, uint32_t cp) {
    if (cp < 0x20U || cp == 0x7FU) {
        return -1;
    }
    if (cp < 0x80U) {
        return 1;
    }
    if (!row->unicode || cp <= 0x9FU) {
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

/* Whether text reaches the screen as UTF-8: on ncurses, the locale dsd_unicode_init_locale() set before initscr(); on
   PDCurses, a build with its wide-character API (as the SNR meter requires, ncurses_snr.c). */
static int
ui_row_locale_is_utf8(void) {
#if defined(DSD_USE_PDCURSES) && defined(DSD_HAS_PDCURSES_WIDE_API)
    return 1;
#elif defined(DSD_USE_PDCURSES)
    /* A narrow PDCurses writes each byte of a UTF-8 sequence as a character of its own: ASCII only. */
    return 0;
#else
    mbstate_t st;
    DSD_MEMSET(&st, 0, sizeof st);
    wchar_t wc = 0;
    const size_t n = mbrtowc(&wc, k_marker_utf8, 3U, &st);
    return n == 3U && (uint32_t)wc == 0x2026U;
#endif
}

void
ui_row_begin(ui_row* row, int cols, int start_col) {
    if (!row) {
        return;
    }
    row->text[0] = '\0';
    row->len = 0U;
    row->fields = 0;
    row->ops_count = 0;
    row->budget = cols - 1 - start_col;
    /* The marker needs both the user's preference (DSD_FORCE_ASCII turns it off, a forced UTF-8 can turn it on under a
       C locale) and a locale that can actually encode it. */
    row->unicode = dsd_unicode_or_ascii(k_marker_utf8, k_marker_ascii) == k_marker_utf8 && ui_row_locale_is_utf8();
}

/* Append @p n bytes of formatted text, each code point the row cannot draw as '?'. */
static void
ui_row_append_sanitized(ui_row* row, const char* text, size_t n) {
    const unsigned char* s = (const unsigned char*)text;
    size_t i = 0;
    while (i < n) {
        uint32_t cp = 0;
        size_t len = ui_row_decode(s + i, n - i, &cp);
        const char* out = (const char*)(s + i);
        size_t out_len = len;
        if (len == 0U) {
            len = 1U;
            out = "?";
            out_len = 1U;
        } else if (ui_row_code_point_cells(row, cp) < 0) {
            out = "?";
            out_len = 1U;
        }
        if (row->len + out_len >= sizeof row->text) {
            break;
        }
        DSD_MEMCPY(row->text + row->len, out, out_len);
        row->len += out_len;
        row->text[row->len] = '\0';
        i += len;
    }
}

void
ui_row_field(ui_row* row, const char* fmt, ...) {
    if (!row || !fmt) {
        return;
    }
    char buf[UI_ROW_TEXT_MAX];
    va_list ap;
    va_start(ap, fmt);
    int n = DSD_VSNPRINTF(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n <= 0) {
        return;
    }
    if ((size_t)n >= sizeof buf) {
        n = (int)sizeof buf - 1;
    }
    ui_row_append_sanitized(row, buf, (size_t)n);
    if (row->fields < UI_ROW_FIELDS_MAX) {
        row->field_end[row->fields++] = row->len;
    } else {
        row->field_end[UI_ROW_FIELDS_MAX - 1] = row->len;
    }
}

static void
ui_row_attr_change(ui_row* row, int attrs, int on) {
    if (!row || row->ops_count >= UI_ROW_ATTR_OPS_MAX) {
        return;
    }
    row->ops[row->ops_count].at = row->len;
    row->ops[row->ops_count].on = on;
    row->ops[row->ops_count].attrs = attrs;
    row->ops_count++;
}

void
ui_row_attr_on(ui_row* row, int attrs) {
    ui_row_attr_change(row, attrs, 1);
}

void
ui_row_attr_off(ui_row* row, int attrs) {
    ui_row_attr_change(row, attrs, 0);
}

int
ui_row_cells(const ui_row* row, const char* text, size_t len) {
    if (!row || !text) {
        return 0;
    }
    const unsigned char* s = (const unsigned char*)text;
    int cells = 0;
    size_t i = 0;
    while (i < len) {
        uint32_t cp = 0;
        size_t n = ui_row_decode(s + i, len - i, &cp);
        if (n == 0U) {
            n = 1U;
            cells += 1;
        } else {
            const int w = ui_row_code_point_cells(row, cp);
            cells += (w < 0) ? 1 : w;
        }
        i += n;
    }
    return cells;
}

/* The longest prefix of the row's text that fits @p limit cells, cut between code points; a zero-width code point
   (a combining mark) stays with the one before it. */
static size_t
ui_row_code_point_cut(const ui_row* row, int limit) {
    const unsigned char* s = (const unsigned char*)row->text;
    size_t i = 0;
    int cells = 0;
    while (i < row->len) {
        uint32_t cp = 0;
        size_t n = ui_row_decode(s + i, row->len - i, &cp);
        int w = 1;
        if (n == 0U) {
            n = 1U;
        } else {
            w = ui_row_code_point_cells(row, cp);
            w = (w < 0) ? 1 : w;
        }
        if (cells + w > limit) {
            break;
        }
        cells += w;
        i += n;
    }
    return i;
}

static void
ui_row_print_bytes(const char* text, size_t from, size_t to) {
    if (to > from) {
        printw("%.*s", (int)(to - from), text + from);
    }
}

/* Blanks a field kept for a reading that is not there draw nothing at the end of a row: drop them before fitting, so
   they never cost a field that would otherwise fit. */
static void
ui_row_trim_trailing_blanks(ui_row* row) {
    while (row->len > 0U && row->text[row->len - 1U] == ' ') {
        row->len--;
    }
    row->text[row->len] = '\0';
    for (int i = 0; i < row->fields; i++) {
        if (row->field_end[i] > row->len) {
            row->field_end[i] = row->len;
        }
    }
}

/* Where the row is cut, and the marker after it (NULL when the row fits, or when no marker fits beside any text). */
static size_t
ui_row_fit(const ui_row* row, const char** marker) {
    *marker = NULL;
    if (row->budget <= 0) {
        return 0U;
    }
    if (ui_row_cells(row, row->text, row->len) <= row->budget) {
        return row->len;
    }
    const int marker_cells = row->unicode ? 1 : 3;
    if (row->budget <= marker_cells) {
        /* No room for a marker beside any text: cut without one. */
        return ui_row_code_point_cut(row, row->budget);
    }
    const int room = row->budget - marker_cells;
    int k = -1;
    for (int i = 0; i < row->fields; i++) {
        if (ui_row_cells(row, row->text, row->field_end[i]) > room) {
            break;
        }
        k = i;
    }
    size_t cut = (k >= 0) ? row->field_end[k] : ui_row_code_point_cut(row, room);
    /* The marker follows the last character shown, not the blanks a padded field kept after it. */
    while (cut > 0U && row->text[cut - 1U] == ' ') {
        cut--;
    }
    *marker = row->unicode ? k_marker_utf8 : k_marker_ascii;
    return cut;
}

void
ui_row_end(ui_row* row) {
    if (!row) {
        return;
    }
    attr_t saved_attrs = 0;
    short saved_pair = 0;
    (void)attr_get(&saved_attrs, &saved_pair, NULL);

    ui_row_trim_trailing_blanks(row);
    const char* marker = NULL;
    const size_t cut = ui_row_fit(row, &marker);

    size_t pos = 0U;
    for (int i = 0; i < row->ops_count && row->ops[i].at <= cut; i++) {
        ui_row_print_bytes(row->text, pos, row->ops[i].at);
        pos = row->ops[i].at;
        if (row->ops[i].on) {
            attron(row->ops[i].attrs);
        } else {
            attroff(row->ops[i].attrs);
        }
    }
    ui_row_print_bytes(row->text, pos, cut);
    /* Back to the row's own attributes before the marker, whatever the cut left switched on. */
    (void)attr_set(saved_attrs, saved_pair, NULL);
    if (marker) {
        printw("%s", marker);
    }
    printw("\n");
}
