// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* A status row cut between whole fields instead of wrapped (issue #669). See ui_row.h. */

#include "ui_row.h"

#include <curses.h>
#include <dsd-neo/core/safe_api.h>
#include <stdarg.h>
#include "ui_text.h"

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
    row->unicode = ui_text_unicode_output();
}

/* Append @p n bytes of formatted text, each code point the row cannot draw as '?'. */
static void
ui_row_append_sanitized(ui_row* row, const char* text, size_t n) {
    row->len += ui_text_sanitize(row->unicode, text, n, row->text + row->len, sizeof row->text - row->len);
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
    return ui_text_cells(row->unicode, text, len);
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
    const int marker_cells = ui_text_marker_cells(row->unicode);
    if (row->budget <= marker_cells) {
        /* No room for a marker beside any text: cut without one. */
        return ui_text_cut(row->unicode, row->text, row->len, row->budget);
    }
    const int room = row->budget - marker_cells;
    int k = -1;
    for (int i = 0; i < row->fields; i++) {
        if (ui_row_cells(row, row->text, row->field_end[i]) > room) {
            break;
        }
        k = i;
    }
    size_t cut = (k >= 0) ? row->field_end[k] : ui_text_cut(row->unicode, row->text, row->len, room);
    /* The marker follows the last character shown, not the blanks a padded field kept after it. */
    while (cut > 0U && row->text[cut - 1U] == ' ') {
        cut--;
    }
    *marker = ui_text_marker(row->unicode);
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
