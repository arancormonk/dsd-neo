// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * ui_read_key() keeps characters and function keys apart: on a wide PDCurses build a code point that equals a key
 * code (U+0157 is KEY_ENTER, U+01CB is PADENTER) must not read as that key. A mouse event reads as a wheel code for
 * a button 4 or 5 press and as ERR for anything else.
 */

#include <assert.h>
#include <curses.h>
#include <stdio.h>

#include "ui_keys.h"

#if UI_KEY_HAVE_WHEEL
/* The mouse events getmouse() has queued: ncurses keeps a queue and reports ERR once it is empty. */
static unsigned long g_mouse_queue[8];
static int g_mouse_queued;
static int g_mouse_next;

static int
fake_getmouse(unsigned long* bstate) {
    if (g_mouse_next >= g_mouse_queued) {
        return ERR;
    }
    *bstate = g_mouse_queue[g_mouse_next++];
    return OK;
}

static void
queue_mouse_events(const unsigned long* events, int n) {
    for (int i = 0; i < n; i++) {
        g_mouse_queue[i] = events[i];
    }
    g_mouse_queued = n;
    g_mouse_next = 0;
}

/* One KEY_MOUSE read with @p n events queued behind it (0: getmouse() refuses). */
static int read_mouse_run(const unsigned long* events, int n);

static int
read_mouse_as(int rc, unsigned long bstate) {
    return read_mouse_run(&bstate, rc == OK ? 1 : 0);
}

/* What one mouse event reads as, on either read path. */
static void
test_mouse_event_reads(void) {
    ui_read_key_set_mouse_hook_for_test(fake_getmouse);

    assert(read_mouse_as(OK, BUTTON4_PRESSED) == UI_KEY_WHEEL_UP);
    assert(read_mouse_as(OK, BUTTON5_PRESSED) == UI_KEY_WHEEL_DOWN);

    /* getmouse() refused the event, or it carries nothing (PDCurses reports KEY_MOUSE for a filtered event). */
    assert(read_mouse_as(ERR, BUTTON4_PRESSED) == ERR);
    assert(read_mouse_as(OK, 0UL) == ERR);

    /* Clicks and releases of buttons 1-3. */
    assert(read_mouse_as(OK, BUTTON1_PRESSED) == ERR);
    assert(read_mouse_as(OK, BUTTON1_RELEASED) == ERR);
    assert(read_mouse_as(OK, BUTTON2_PRESSED) == ERR);
    assert(read_mouse_as(OK, BUTTON3_PRESSED) == ERR);
    assert(read_mouse_as(OK, BUTTON3_RELEASED) == ERR);

    /* Both wheel bits at once says neither direction; a wheel release is not a notch. */
    assert(read_mouse_as(OK, BUTTON4_PRESSED | BUTTON5_PRESSED) == ERR);
    assert(read_mouse_as(OK, BUTTON4_RELEASED) == ERR);

#if defined(DSD_USE_PDCURSES)
    /* PDCurses collects no modifiers for a wheel event, and nc_getmouse() masks them out of the state anyway, so
       every vertical notch it delivers counts. (Its horizontal wheel sets neither button: bstate 0, above.) */
    assert(read_mouse_as(OK, BUTTON4_PRESSED | BUTTON_MODIFIER_SHIFT) == UI_KEY_WHEEL_UP);

    /* PDCurses has no queue: one KEY_MOUSE is one event, and the read takes exactly one. */
    static const unsigned long run[] = {BUTTON1_PRESSED, BUTTON5_PRESSED};
    assert(read_mouse_run(run, 2) == ERR);
    assert(g_mouse_next == 1);
#else
    /* ncurses reports the modifiers held: a notch with Shift, Ctrl or Alt is not a step. */
    assert(read_mouse_as(OK, BUTTON4_PRESSED | BUTTON_SHIFT) == ERR);
    assert(read_mouse_as(OK, BUTTON5_PRESSED | BUTTON_CTRL) == ERR);
    assert(read_mouse_as(OK, BUTTON4_PRESSED | BUTTON_ALT) == ERR);

    /* ncurses returns one KEY_MOUSE for every report already waiting: the read empties the queue, so nothing is left
       to surface with a later report, and the run's first plain notch counts. */
    static const unsigned long click_then_notches[] = {
        BUTTON1_PRESSED,
        BUTTON1_RELEASED,
        BUTTON5_PRESSED,
        BUTTON4_PRESSED,
    };
    assert(read_mouse_run(click_then_notches, 4) == UI_KEY_WHEEL_DOWN);
    assert(g_mouse_next == 4);
    static const unsigned long modified_then_plain[] = {BUTTON4_PRESSED | BUTTON_SHIFT, BUTTON4_PRESSED};
    assert(read_mouse_run(modified_then_plain, 2) == UI_KEY_WHEEL_UP);
    assert(g_mouse_next == 2);
    static const unsigned long clicks_only[] = {BUTTON1_PRESSED, BUTTON1_RELEASED, BUTTON3_PRESSED};
    assert(read_mouse_run(clicks_only, 3) == ERR);
    assert(g_mouse_next == 3);
#endif

    ui_read_key_set_mouse_hook_for_test(NULL);
}
#endif

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

#if UI_KEY_HAVE_WHEEL
/* The wide read reports KEY_MOUSE as a key code. A character equal to KEY_MOUSE is still a character. */
static int
read_mouse_run(const unsigned long* events, int n) {
    ui_read_key_set_hooks_for_test(NULL, fake_get_wch);
    queue_mouse_events(events, n);
    int key = read_as(KEY_CODE_YES, KEY_MOUSE);
    ui_read_key_set_hooks_for_test(NULL, NULL);
    return key;
}

static void
test_wide_mouse_char_is_not_an_event(void) {
    static const unsigned long notch = BUTTON4_PRESSED;
    ui_read_key_set_hooks_for_test(NULL, fake_get_wch);
    ui_read_key_set_mouse_hook_for_test(fake_getmouse);
    queue_mouse_events(&notch, 1);
    assert(read_as(OK, KEY_MOUSE) == UI_KEY_WIDE_CHAR);
    assert(g_mouse_next == 0);
    ui_read_key_set_mouse_hook_for_test(NULL);
    ui_read_key_set_hooks_for_test(NULL, NULL);
}
#endif

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

#if UI_KEY_HAVE_WHEEL
static int
read_mouse_run(const unsigned long* events, int n) {
    ui_read_key_set_hooks_for_test(fake_getch, NULL);
    queue_mouse_events(events, n);
    int key = read_as(KEY_MOUSE);
    ui_read_key_set_hooks_for_test(NULL, NULL);
    return key;
}
#endif

#endif

int
main(void) {
#if defined(DSD_USE_PDCURSES) && defined(DSD_HAS_PDCURSES_WIDE_API)
    test_wide_read_keeps_characters_and_keys_apart();
#if UI_KEY_HAVE_WHEEL
    test_wide_mouse_char_is_not_an_event();
#endif
#else
    test_narrow_read_passes_getch_through();
#endif
#if UI_KEY_HAVE_WHEEL
    test_mouse_event_reads();
#endif
    printf("UI_READ_KEY: OK\n");
    return 0;
}
