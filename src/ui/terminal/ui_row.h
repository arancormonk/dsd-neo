// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * One status row that never wraps (issue #669): the row is built from atomic fields, then drawn whole when it fits the
 * terminal and cut after the last whole field that fits when it does not, with a marker ("…", or "..." without UTF-8)
 * saying that more was there. A cut never splits a field, so a padded reading is shown whole or not at all, and a row
 * that redraws every frame keeps one screen line while its readings change.
 *
 * Widths follow the curses library's own model, so the writer and the virtual cursor agree: on ncurses each code point
 * takes wcwidth() cells in the UTF-8 locale dsd_unicode_init_locale() set; on PDCurses, which has no width handling,
 * one cell per code point of the Basic Multilingual Plane, and ASCII alone on a build without its wide-character API.
 * Control characters, invalid UTF-8, code points the library cannot draw, and any non-ASCII text without UTF-8 output
 * are drawn as '?'.
 */

#ifndef DSD_NEO_SRC_UI_TERMINAL_UI_ROW_H_
#define DSD_NEO_SRC_UI_TERMINAL_UI_ROW_H_

#include <dsd-neo/platform/platform.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    UI_ROW_TEXT_MAX = 768,   /* bytes of payload; text past it is dropped (a row this long is cut anyway) */
    UI_ROW_FIELDS_MAX = 48,  /* fields past it join the last one */
    UI_ROW_ATTR_OPS_MAX = 24 /* attribute changes past it are dropped */
};

typedef struct {
    size_t at; /* byte offset in text where the change takes effect */
    int on;    /* 1 attron(attrs), 0 attroff(attrs) */
    int attrs;
} ui_row_attr_op;

typedef struct ui_row {
    char text[UI_ROW_TEXT_MAX];
    size_t len;
    size_t field_end[UI_ROW_FIELDS_MAX]; /* where each field ends: the only places a cut may fall */
    int fields;
    ui_row_attr_op ops[UI_ROW_ATTR_OPS_MAX];
    int ops_count;
    int budget;  /* cells the row may take: the panel width less the column it starts at and the last column */
    int unicode; /* UTF-8 output: the user's preference and a locale that decodes it */
} ui_row;

/*
 * Start a row that begins at column @p start_col (0 at the left edge; 1 after a border its caller drew) of a panel
 * @p cols wide. The last column is never written: ncurses wraps the cursor as soon as it is.
 */
void ui_row_begin(ui_row* row, int cols, int start_col);

/* Append one atomic field: a label with its reading and unit, and any qualifier that belongs to it. */
void ui_row_field(ui_row* row, const char* fmt, ...) DSD_ATTR_FORMAT(printf, 2, 3);

/* Attribute changes between fields (a colour pair for a highlighted reading). */
void ui_row_attr_on(ui_row* row, int attrs);
void ui_row_attr_off(ui_row* row, int attrs);

/*
 * Draw the row, cut to the budget, and end it with exactly one newline. Trailing blanks are dropped first. The
 * attributes in force when the row was drawn are put back before the marker and the newline, also when the cut dropped
 * a field's own attroff.
 */
void ui_row_end(ui_row* row);

/* Cells @p text takes under the row's width model (after the substitutions above). For tests and fit checks. */
int ui_row_cells(const ui_row* row, const char* text, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_SRC_UI_TERMINAL_UI_ROW_H_ */
