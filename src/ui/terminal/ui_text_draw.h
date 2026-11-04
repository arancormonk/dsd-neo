// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */
/*
 * Drawing text cut by the shared text model (ui_text.h) into a curses window: the one sequence the chooser and the Scan
 * lists modal draw every row and title with. Apart from ui_text.h, which names no curses type, so the targets that
 * measure text without a terminal keep compiling ui_text.c alone.
 */
#ifndef DSD_NEO_SRC_UI_TERMINAL_UI_TEXT_DRAW_H_
#define DSD_NEO_SRC_UI_TERMINAL_UI_TEXT_DRAW_H_

#include <curses.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bytes of one text the helper draws; text past it is dropped (a line this long is cut on any terminal). */
enum { UI_TEXT_DRAW_MAX = 768 };

/*
 * Draw @p text (NULL draws nothing) at (@p y, @p x) of @p win in at most @p budget cells: sanitized, measured and cut
 * between code points under @p unicode (ui_text_unicode_output()), with the marker when anything was cut, so a long
 * name never splits a UTF-8 character and never loses its end without saying so. What reaches curses is the byte span
 * of the cut.
 */
void ui_text_draw_fit(WINDOW* win, int y, int x, int unicode, const char* text, int budget);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_SRC_UI_TERMINAL_UI_TEXT_DRAW_H_ */
