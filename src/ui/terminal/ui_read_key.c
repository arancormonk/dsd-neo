// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The terminal's one key read (see ui_keys.h).
 */

#if defined(DSD_USE_PDCURSES) && defined(DSD_HAS_PDCURSES_WIDE_API) && !defined(PDC_WIDE)
#define PDC_WIDE
#endif
#include <curses.h>
#if defined(DSD_USE_PDCURSES) && defined(DSD_HAS_PDCURSES_WIDE_API)
#include <wchar.h>
#endif

#include "ui_keys.h"

#if defined(DSD_USE_PDCURSES) && defined(DSD_HAS_PDCURSES_WIDE_API)
#define UI_READ_KEY_WIDE 1
#else
#define UI_READ_KEY_WIDE 0
#endif

#if UI_KEY_HAVE_WHEEL
#include <dsd-neo/core/safe_api.h>
#endif

#ifdef DSD_NEO_TEST_HOOKS
#if UI_READ_KEY_WIDE
static ui_read_key_get_wch_fn g_read_wch;
#else
static ui_read_key_getch_fn g_read_getch;
#endif

// Cppcheck 2.21 loses the final prototype name after a callback typedef parameter.
// cppcheck-suppress-begin funcArgNamesDifferentUnnamed
void
ui_read_key_set_hooks_for_test(ui_read_key_getch_fn read_getch, ui_read_key_get_wch_fn read_wch) {
#if UI_READ_KEY_WIDE
    (void)read_getch;
    g_read_wch = read_wch;
#else
    (void)read_wch;
    g_read_getch = read_getch;
#endif
}

// cppcheck-suppress-end funcArgNamesDifferentUnnamed

static ui_read_key_getmouse_fn g_read_mouse;

void
ui_read_key_set_mouse_hook_for_test(ui_read_key_getmouse_fn read_mouse) {
    g_read_mouse = read_mouse;
}
#endif

#if UI_KEY_HAVE_WHEEL

static int
ui_read_mouse_state(unsigned long* bstate) {
#ifdef DSD_NEO_TEST_HOOKS
    if (g_read_mouse) {
        return g_read_mouse(bstate);
    }
#endif
    MEVENT event;
    DSD_MEMSET(&event, 0, sizeof(event));
#if defined(DSD_USE_PDCURSES)
    /* PDCurses' getmouse() is the classic one unless PDC_NCMOUSE; nc_getmouse() is always the ncurses form. */
    int rc = nc_getmouse(&event);
#else
    int rc = getmouse(&event);
#endif
    *bstate = (unsigned long)event.bstate;
    return rc;
}

/* The wheel code for one mouse event, or ERR. Only a button 4 or 5 press is a notch; clicks, releases, a horizontal
   wheel and an empty state (PDCurses reports KEY_MOUSE for an event its mask filtered out) are nothing. */
static int
ui_wheel_key_for_state(unsigned long bstate) {
#if !defined(DSD_USE_PDCURSES)
    /* ncurses reports the modifiers held; a modified notch belongs to the terminal or the user, not to a step. */
    if ((bstate & (unsigned long)(BUTTON_SHIFT | BUTTON_CTRL | BUTTON_ALT)) != 0UL) {
        return ERR;
    }
#endif
    const int up = (bstate & (unsigned long)BUTTON4_PRESSED) != 0UL;
    const int down = (bstate & (unsigned long)BUTTON5_PRESSED) != 0UL;
    if (up == down) {
        return ERR;
    }
    return up ? UI_KEY_WHEEL_UP : UI_KEY_WHEEL_DOWN;
}

#if defined(DSD_USE_PDCURSES)
/* PDCurses keeps no event queue: each KEY_MOUSE is one event, and nc_getmouse() reports it until the next. */
static int
ui_read_wheel_key(void) {
    unsigned long bstate = 0UL;
    if (ui_read_mouse_state(&bstate) != OK) {
        return ERR;
    }
    return ui_wheel_key_for_state(bstate);
}
#else
/* ncurses' queue holds at most 8 events; the bound only guards against a getmouse() that never runs dry. */
#define UI_READ_KEY_MOUSE_RUN_MAX 16

/*
 * ncurses reads every mouse report already waiting into its queue and returns one KEY_MOUSE for the run, leaving the
 * rest for getmouse() calls with no KEY_MOUSE of their own: an event left there would surface with the next report,
 * a notch made with the menu open stepping after it closes. So the whole run is read now, and its first notch
 * counts; any others arrived with it, inside the step throttle.
 */
static int
ui_read_wheel_key(void) {
    int key = ERR;
    for (int i = 0; i < UI_READ_KEY_MOUSE_RUN_MAX; i++) {
        unsigned long bstate = 0UL;
        if (ui_read_mouse_state(&bstate) != OK) {
            break;
        }
        if (key == ERR) {
            key = ui_wheel_key_for_state(bstate);
        }
    }
    return key;
}
#endif

#endif

#if UI_READ_KEY_WIDE

static int
ui_read_wch(unsigned long* wch) {
#ifdef DSD_NEO_TEST_HOOKS
    if (g_read_wch) {
        return g_read_wch(wch);
    }
#endif
    wint_t wc = 0;
    int rc = wget_wch(stdscr, &wc);
    *wch = (unsigned long)wc;
    return rc;
}

int
ui_read_key(void) {
    unsigned long wch = 0;
    int rc = ui_read_wch(&wch);
    if (rc == KEY_CODE_YES) {
#if UI_KEY_HAVE_WHEEL
        if (wch == (unsigned long)KEY_MOUSE) {
            return ui_read_wheel_key();
        }
#endif
        return (int)wch;
    }
    if (rc != OK) {
        return ERR;
    }
    // A character never reads as the key code it equals (U+0157 is KEY_ENTER, U+01CB is PADENTER).
    if (wch <= 0xFFUL) {
        return (int)wch;
    }
    return UI_KEY_WIDE_CHAR;
}

#else

static int
ui_read_getch(void) {
#ifdef DSD_NEO_TEST_HOOKS
    if (g_read_getch) {
        return g_read_getch();
    }
#endif
    return getch();
}

// ncurses delivers characters as bytes and keys at or above KEY_MIN; a narrow PDCurses has bytes only.
int
ui_read_key(void) {
    int ch = ui_read_getch();
#if UI_KEY_HAVE_WHEEL
    if (ch == KEY_MOUSE) {
        return ui_read_wheel_key();
    }
#endif
    return ch;
}

#endif
