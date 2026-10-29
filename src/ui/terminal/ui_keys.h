// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Terminal key reads: one boundary that keeps characters and function keys apart.
 *
 * Terminal-private. A wide PDCurses build reports a code point and a function key code in the same int space
 * through getch(), so U+0157 reads as KEY_ENTER and U+01CB as PADENTER. ui_read_key() reads through wget_wch()
 * there and returns a function key as its code, a character up to 0xFF as itself, and any other character as
 * UI_KEY_WIDE_CHAR. ncurses delivers characters as bytes and keys at or above KEY_MIN, so it stays getch().
 */
#ifndef DSD_NEO_SRC_UI_TERMINAL_UI_KEYS_H
#define DSD_NEO_SRC_UI_TERMINAL_UI_KEYS_H

#include <curses.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * A character above 0xFF read by a wide PDCurses build. Outside every curses key range, so no hotkey handles it;
 * a prompt shows it as '?'.
 */
#define UI_KEY_WIDE_CHAR 0x7FFF0001

/** Non-zero when @p ch submits: Enter as '\n' or '\r', or KEY_ENTER. */
static inline int
ui_key_is_enter(int ch) {
    return ch == '\n' || ch == '\r' || ch == KEY_ENTER;
}

/**
 * Read one key without changing the window's delay mode.
 *
 * @return ERR when no key is waiting, a function key code, a character up to 0xFF, or UI_KEY_WIDE_CHAR.
 */
int ui_read_key(void);

#ifdef DSD_NEO_TEST_HOOKS
/** Replaces getch() in a build that reads through it. */
typedef int (*ui_read_key_getch_fn)(void);
/** Replaces wget_wch(stdscr, ...) in a wide PDCurses build: returns OK, KEY_CODE_YES or ERR and sets *wch. */
typedef int (*ui_read_key_get_wch_fn)(unsigned long* wch);

/** Install the reads ui_read_key() uses; NULL restores the curses call. */
void ui_read_key_set_hooks_for_test(ui_read_key_getch_fn read_getch, ui_read_key_get_wch_fn read_wch);
#endif

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_SRC_UI_TERMINAL_UI_KEYS_H */
