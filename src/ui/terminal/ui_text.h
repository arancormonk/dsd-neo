// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Text measured and cut in terminal cells: the width model of the status row (issue #669, ui_row.h), shared with the
 * chooser overlay (issue #672), so a row and a list cut text the same way.
 *
 * Widths follow the curses library's own model, so the writer and the virtual cursor agree: on ncurses each code point
 * takes wcwidth() cells in the UTF-8 locale dsd_unicode_init_locale() set; on PDCurses, which has no width handling,
 * one cell per code point of the Basic Multilingual Plane, and ASCII alone on a build without its wide-character API
 * (a narrow PDCurses writes each byte of a UTF-8 sequence as a character of its own). Control characters, invalid
 * UTF-8, code points the library cannot draw, and any non-ASCII text without UTF-8 output are drawn as '?'. A cut
 * falls between code points, a zero-width code point (a combining mark) stays with the one before it, and the marker
 * after a cut is "…", or "..." without UTF-8 output.
 *
 * Every function takes the output decision (@p unicode, from ui_text_unicode_output()) so a caller decides it once
 * per row or list and measures, cuts and draws under the same answer.
 */

#ifndef DSD_NEO_SRC_UI_TERMINAL_UI_TEXT_H_
#define DSD_NEO_SRC_UI_TERMINAL_UI_TEXT_H_

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* UTF-8 output: the user's preference (DSD_FORCE_ASCII turns it off, a forced UTF-8 can turn it on under a C locale)
   and a locale, or a PDCurses build, that can actually draw it. */
int ui_text_unicode_output(void);

/* The cut marker and the cells it takes. */
const char* ui_text_marker(int unicode);
int ui_text_marker_cells(int unicode);

/*
 * Copy @p len bytes of @p text into @p out (@p out_size bytes, NUL-terminated), each code point that cannot be drawn
 * as '?'. Stops before a code point that would not fit. Returns the bytes written.
 */
size_t ui_text_sanitize(int unicode, const char* text, size_t len, char* out, size_t out_size);

/* Cells @p len bytes of @p text take once sanitized (an undrawable code point or a stray byte is one '?'). */
int ui_text_cells(int unicode, const char* text, size_t len);

/* The longest prefix of @p text, in bytes, that fits @p limit cells, cut between code points. */
size_t ui_text_cut(int unicode, const char* text, size_t len, int limit);

/*
 * Fit @p text to @p budget cells: the whole text when it fits, else the bytes to draw before the marker
 * (*@p marker set; trailing blanks before it dropped). With no room for the marker beside any text, the cut has no
 * marker. *@p marker is NULL when none is drawn.
 */
size_t ui_text_fit(int unicode, const char* text, size_t len, int budget, const char** marker);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_SRC_UI_TERMINAL_UI_TEXT_H_ */
