// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * ncurses defines the stdscr attribute and output calls below as macros over the w* calls, so a test that stubs
 * wattr_on(), waddch() and the rest sees every call made through them. PDCurses (Windows) implements them as
 * functions in its DLL, which call its own w* functions, so such calls would bypass the stubs. Included once by a
 * test that stubs the w* calls and links PDCurses, after <curses.h>; it defines each one as the ncurses macro
 * expands. Built with CURSES_LIBRARY on MSVC, so the definitions do not clash with PDCurses' dllimport declarations.
 * CURSES_LIBRARY also makes the stdscr they reference a direct reference, which PDCurses' import library cannot
 * satisfy (it exports data only through __imp_ entries), so the including test defines stdscr itself.
 */

#ifndef DSD_NEO_TESTS_UI_PDCURSES_MACRO_STUBS_H
#define DSD_NEO_TESTS_UI_PDCURSES_MACRO_STUBS_H

#if defined(DSD_USE_PDCURSES)

int
attron(chtype attrs) {
    return wattr_on(stdscr, (attr_t)attrs, NULL);
}

int
attroff(chtype attrs) {
    return wattr_off(stdscr, (attr_t)attrs, NULL);
}

int
attr_get(attr_t* attrs, short* pair, void* opts) {
    return wattr_get(stdscr, attrs, pair, opts);
}

int
attr_set(attr_t attrs, short pair, void* opts) {
    return wattr_set(stdscr, attrs, pair, opts);
}

int
addch(const chtype ch) {
    return waddch(stdscr, ch);
}

int
addstr(const char* str) {
    return waddnstr(stdscr, str, -1);
}

int
addnstr(const char* str, int n) {
    return waddnstr(stdscr, str, n);
}

#endif /* DSD_USE_PDCURSES */

#endif /* DSD_NEO_TESTS_UI_PDCURSES_MACRO_STUBS_H */
