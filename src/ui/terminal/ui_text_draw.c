// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "ui_text_draw.h"

#include <curses.h>
#include <stddef.h>
#include <string.h>

#include "ui_text.h"

void
ui_text_draw_fit(WINDOW* win, int y, int x, int unicode, const char* text, int budget) {
    char shown[UI_TEXT_DRAW_MAX];
    const char* src = text ? text : "";
    const size_t len = ui_text_sanitize(unicode, src, strnlen(src, sizeof shown), shown, sizeof shown);
    const char* marker = NULL;
    const size_t cut = ui_text_fit(unicode, shown, len, budget, &marker);
    wmove(win, y, x);
    if (cut > 0U) {
        waddnstr(win, shown, (int)cut);
    }
    if (marker) {
        waddstr(win, marker);
    }
}
