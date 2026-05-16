// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Regression coverage for overflowed chooser paging:
 *  - PgDn should advance the visible range by a full page step
 *  - PgUp should restore the previous page
 *  - Home/End should anchor to the first/last page
 */

#include <assert.h>
#include <curses.h>
#include <stdio.h>

#include "menu_prompts.h"

static int g_done_sel = -2;

static const char* const ITEMS[] = {
    "Item 01", "Item 02", "Item 03", "Item 04", "Item 05", "Item 06", "Item 07", "Item 08", "Item 09", "Item 10",
    "Item 11", "Item 12", "Item 13", "Item 14", "Item 15", "Item 16", "Item 17", "Item 18", "Item 19", "Item 20",
};

static void
capture_done(void* user, int sel) {
    (void)user;
    g_done_sel = sel;
}

void
ui_statusf(const char* fmt, ...) {
    (void)fmt;
}

WINDOW*
ui_make_window(int h, int w, int y, int x) {
    (void)h;
    (void)w;
    (void)y;
    (void)x;
    return NULL;
}

int
main(void) {
    UiChooserTestSnapshot snapshot;

    ui_chooser_start("Devices", ITEMS, (int)(sizeof ITEMS / sizeof ITEMS[0]), capture_done, NULL);
    ui_chooser_test_set_page_rows(10);
    snapshot = ui_chooser_test_snapshot();
    assert(snapshot.top == 0);
    assert(snapshot.sel == 0);
    assert(snapshot.count == 20);

    assert(ui_chooser_handle_key(KEY_NPAGE) == 1);
    snapshot = ui_chooser_test_snapshot();
    assert(snapshot.top == 9);
    assert(snapshot.sel == 9);

    assert(ui_chooser_handle_key(KEY_PPAGE) == 1);
    snapshot = ui_chooser_test_snapshot();
    assert(snapshot.top == 0);
    assert(snapshot.sel == 0);

    assert(ui_chooser_handle_key(KEY_END) == 1);
    snapshot = ui_chooser_test_snapshot();
    assert(snapshot.top == 10);
    assert(snapshot.sel == 19);

    assert(ui_chooser_handle_key(KEY_HOME) == 1);
    snapshot = ui_chooser_test_snapshot();
    assert(snapshot.top == 0);
    assert(snapshot.sel == 0);

    assert(ui_chooser_handle_key('\r') == 1);
    assert(g_done_sel == 0);

    ui_chooser_start("Devices", ITEMS, (int)(sizeof ITEMS / sizeof ITEMS[0]), capture_done, NULL);
    assert(ui_chooser_handle_key('q') == 1);
    assert(g_done_sel == -1);

    printf("UI_CHOOSER_NAVIGATION: OK\n");
    return 0;
}
