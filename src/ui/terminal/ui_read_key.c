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

#ifdef DSD_NEO_TEST_HOOKS
#if UI_READ_KEY_WIDE
static ui_read_key_get_wch_fn g_read_wch;
#else
static ui_read_key_getch_fn g_read_getch;
#endif

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

// ncurses delivers characters as bytes and keys at or above KEY_MIN; a narrow PDCurses has bytes only.
int
ui_read_key(void) {
#ifdef DSD_NEO_TEST_HOOKS
    if (g_read_getch) {
        return g_read_getch();
    }
#endif
    return getch();
}

#endif
