// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_TESTS_UI_CURSES_H
#define DSD_NEO_TESTS_UI_CURSES_H

typedef struct WINDOW WINDOW;

#define ERR       (-1)
#define KEY_ENTER 343

extern WINDOW* stdscr;
int wgetch(WINDOW* win);

#define getch() wgetch(stdscr)

#endif /* DSD_NEO_TESTS_UI_CURSES_H */
