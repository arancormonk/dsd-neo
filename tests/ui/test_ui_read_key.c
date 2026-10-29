// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * ui_read_key() keeps characters and function keys apart: on a wide PDCurses build a code point that equals a key
 * code (U+0157 is KEY_ENTER, U+01CB is PADENTER) must not read as that key.
 */

#include <assert.h>
#include <curses.h>
#include <stdio.h>

#include "ui_keys.h"

#if defined(DSD_USE_PDCURSES) && defined(DSD_HAS_PDCURSES_WIDE_API)

static int g_wch_status;
static unsigned long g_wch_value;
static int g_wch_calls;

static int
fake_get_wch(unsigned long* wch) {
    g_wch_calls++;
    *wch = g_wch_value;
    return g_wch_status;
}

static int
read_as(int status, unsigned long value) {
    g_wch_status = status;
    g_wch_value = value;
    return ui_read_key();
}

static void
test_wide_read_keeps_characters_and_keys_apart(void) {
    ui_read_key_set_hooks_for_test(NULL, fake_get_wch);

    /* Characters: up to 0xFF as themselves, above as the sentinel, whatever key code they equal. */
    assert(read_as(OK, 'a') == 'a');
    assert(read_as(OK, '\r') == '\r');
    assert(read_as(OK, 0xE9) == 0xE9);
    assert(read_as(OK, 0xFF) == 0xFF);
    assert(read_as(OK, 0x100) == UI_KEY_WIDE_CHAR);
    assert(read_as(OK, 0x157) == UI_KEY_WIDE_CHAR);
    assert(read_as(OK, 0x1CB) == UI_KEY_WIDE_CHAR);
    assert(read_as(OK, 0xFF0E) == UI_KEY_WIDE_CHAR);

    /* Function keys keep their codes. */
    int reads = 12;
    assert(read_as(KEY_CODE_YES, KEY_ENTER) == KEY_ENTER);
#ifdef PADENTER
    assert(read_as(KEY_CODE_YES, PADENTER) == PADENTER);
    reads++;
#endif
    assert(read_as(KEY_CODE_YES, KEY_UP) == KEY_UP);
    assert(read_as(KEY_CODE_YES, KEY_RESIZE) == KEY_RESIZE);

    /* No key waiting. */
    assert(read_as(ERR, 0) == ERR);
    assert(g_wch_calls == reads);

    ui_read_key_set_hooks_for_test(NULL, NULL);
}

#else

static int g_getch_value;
static int g_getch_calls;

static int
fake_getch(void) {
    g_getch_calls++;
    return g_getch_value;
}

static int
read_as(int value) {
    g_getch_value = value;
    return ui_read_key();
}

/* ncurses (and a narrow PDCurses) delivers characters as bytes and keys as key codes: getch() as is. */
static void
test_narrow_read_passes_getch_through(void) {
    ui_read_key_set_hooks_for_test(fake_getch, NULL);

    assert(read_as('a') == 'a');
    assert(read_as('\r') == '\r');
    assert(read_as(0xEF) == 0xEF);
    assert(read_as(KEY_ENTER) == KEY_ENTER);
    assert(read_as(KEY_UP) == KEY_UP);
    assert(read_as(KEY_MAX + 75) == KEY_MAX + 75);
    assert(read_as(ERR) == ERR);
    assert(g_getch_calls == 7);

    ui_read_key_set_hooks_for_test(NULL, NULL);
}

#endif

int
main(void) {
#if defined(DSD_USE_PDCURSES) && defined(DSD_HAS_PDCURSES_WIDE_API)
    test_wide_read_keeps_characters_and_keys_apart();
#else
    test_narrow_read_passes_getch_through();
#endif
    printf("UI_READ_KEY: OK\n");
    return 0;
}
