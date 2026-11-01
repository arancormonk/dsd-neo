// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Terminal mouse wheel: whether this terminal can offer it, and turning the capture on and off.
 *
 * Terminal-private. Every call runs on the UI thread with curses open, except ui_mouse_xterm_admitted(), which is
 * pure.
 *
 * ncurses: the first capture calls mousemask() with the press and release of buttons 1-3 and the press of buttons 4
 * and 5, and mouseinterval(0). That parse mask is never cleared at runtime: with a narrower mask, ncurses' getch()
 * loops back into a blocking read when a queued click does not match it, and the UI thread would freeze until the
 * next key. On and off afterwards is the terminal's own tracking switch, the terminfo XM string (or ncurses' default
 * for it), so a late report still parses and reads as ERR. endwin() turns tracking off at close.
 *
 * PDCurses: mousemask() with those buttons plus MOUSE_WHEEL_SCROLL on, mousemask(0) off (its read never blocks, and
 * mask 0 gives the console its QuickEdit selection back), and mouseinterval(0) once.
 */
#ifndef DSD_NEO_SRC_UI_TERMINAL_UI_MOUSE_H
#define DSD_NEO_SRC_UI_TERMINAL_UI_MOUSE_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Menu row predicate (is_enabled) for "Mouse wheel steps channels", and the last term of the capture gate: true when
 * this build and terminal can report wheel events.
 *
 * False when curses has no button 5 (UI_KEY_HAVE_WHEEL), when ncurses would not use its xterm mouse driver
 * (ui_mouse_xterm_admitted()), or when ncurses refused the mouse mask.
 *
 * @param ctx UiCtx pointer (unused).
 * @return true when the row is offered.
 */
bool ui_mouse_wheel_available(const void* ctx);

/**
 * Whether ncurses reads the mouse through its xterm driver, which the capture needs: its GPM driver (the Linux
 * console) cannot be switched off and on without clearing the parse mask.
 *
 * ncurses tries GPM first, when NCURSES_GPM_TERMS names TERM or, with that variable unset, when TERM contains
 * "linux". This rule is stricter: any NCURSES_GPM_TERMS (even empty) refuses, as does a TERM containing "linux" or a
 * kmous that does not begin with ESC '['.
 *
 * @param term      TERM, or NULL when unset.
 * @param gpm_terms NCURSES_GPM_TERMS, or NULL when unset.
 * @param kmous     The terminfo kmous string, or NULL when the terminal has none.
 * @return 1 when admitted, else 0.
 */
int ui_mouse_xterm_admitted(const char* term, const char* gpm_terms, const char* kmous);

/**
 * Turn the wheel capture on or off. Acts only on a change, so the frame loop calls it every frame. A request to turn
 * it on where the wheel is unavailable leaves it off.
 */
void ui_mouse_capture_set(int on);

/** 1 while the wheel capture is on. */
int ui_mouse_capture_is_on(void);

/**
 * Turn the capture off before the terminal closes, and forget what this terminal offered, so a reopened one starts
 * over.
 */
void ui_mouse_capture_close(void);

#ifdef DSD_NEO_TEST_HOOKS
/** Forget the capture state without a curses call. */
void ui_mouse_reset_for_test(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_SRC_UI_TERMINAL_UI_MOUSE_H */
