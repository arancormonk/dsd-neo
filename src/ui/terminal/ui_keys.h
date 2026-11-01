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
#define UI_KEY_WIDE_CHAR  0x7FFF0001

/**
 * One notch of the mouse wheel away from the user (button 4) or toward the user (button 5), read from a KEY_MOUSE
 * event while the wheel capture (ui_mouse.h) is on. Outside every curses key range like UI_KEY_WIDE_CHAR: the main
 * screen's frame loop (ui_async.c) handles them itself, and no hotkey, menu or prompt sees them.
 */
#define UI_KEY_WHEEL_UP   0x7FFF0002
#define UI_KEY_WHEEL_DOWN 0x7FFF0003

/**
 * 1 when this curses reports the wheel: KEY_MOUSE with BUTTON4_PRESSED and BUTTON5_PRESSED. ncurses defines button 5
 * only for mouse version 2 and later (ABI 6), so an older ncurses (the macOS SDK's 5.7) builds the wheel out.
 */
#if defined(KEY_MOUSE) && defined(BUTTON4_PRESSED) && defined(BUTTON5_PRESSED)
#define UI_KEY_HAVE_WHEEL 1
#else
#define UI_KEY_HAVE_WHEEL 0
#endif

/**
 * Non-zero when @p ch submits: Enter as '\n' or '\r', KEY_ENTER, or PDCurses keypad Enter (PADENTER) or Shift+keypad
 * Enter (SHF_PADENTER).
 *
 * PDCurses reports keypad Enter as PADENTER and never KEY_ENTER; ncurses reports it as '\n' or KEY_ENTER and has
 * no PADENTER. Shift+Enter on the main screen opens the frequency entry (issue #671), so inside a prompt, help, a
 * chooser or the menu it submits like Enter, as Shift+main Enter (Enter with the Shift modifier) already does. The
 * Ctrl and Alt keypad Enter codes stay unbound.
 */
static inline int
ui_key_is_enter(int ch) {
    return ch == '\n' || ch == '\r' || ch == KEY_ENTER
#ifdef PADENTER
           || ch == PADENTER || ch == SHF_PADENTER
#endif
        ;
}

/**
 * Read one key without changing the window's delay mode.
 *
 * A KEY_MOUSE event reads as UI_KEY_WHEEL_UP or UI_KEY_WHEEL_DOWN for a button 4 or 5 press and as ERR for anything
 * else: a click, a horizontal wheel, an event getmouse() refuses or reports empty, and on ncurses a notch with Shift,
 * Ctrl or Alt held. PDCurses reports no modifiers with the wheel, so every vertical notch counts there.
 *
 * @return ERR when no key is waiting, a function key code, a character up to 0xFF, UI_KEY_WIDE_CHAR, or a wheel code.
 */
int ui_read_key(void);

#ifdef DSD_NEO_TEST_HOOKS
/** Replaces getch() in a build that reads through it. */
typedef int (*ui_read_key_getch_fn)(void);
/** Replaces wget_wch(stdscr, ...) in a wide PDCurses build: returns OK, KEY_CODE_YES or ERR and sets *wch. */
typedef int (*ui_read_key_get_wch_fn)(unsigned long* wch);

/** Install the reads ui_read_key() uses; NULL restores the curses call. */
void ui_read_key_set_hooks_for_test(ui_read_key_getch_fn read_getch, ui_read_key_get_wch_fn read_wch);

/** Replaces getmouse() (nc_getmouse() on PDCurses) after KEY_MOUSE: returns OK or ERR and sets *bstate. */
typedef int (*ui_read_key_getmouse_fn)(unsigned long* bstate);

/** Install the mouse event read ui_read_key() uses; NULL restores the curses call. */
void ui_read_key_set_mouse_hook_for_test(ui_read_key_getmouse_fn read_mouse);
#endif

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_SRC_UI_TERMINAL_UI_KEYS_H */
