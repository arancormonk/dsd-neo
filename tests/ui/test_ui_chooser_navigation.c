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
#include <locale.h>
#include <stdio.h>
#include <string.h>

#include "menu_prompts.h"
#include "test_support.h"

static WINDOW* g_chooser_win = NULL;
static SCREEN* g_screen = NULL;
static FILE* g_in = NULL;
static FILE* g_out = NULL;
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

static void
trim_trailing_spaces(char* text) {
    size_t len = strlen(text);
    while (len > 0 && text[len - 1] == ' ') {
        text[--len] = '\0';
    }
}

static void
trim_window_padding(char* text) {
    char* pad = strstr(text, "  ");
    if (pad) {
        *pad = '\0';
    }
    trim_trailing_spaces(text);
}

static void
read_title(char* buf, size_t n) {
    assert(g_chooser_win != NULL);
    memset(buf, 0, n);
    int rc = mvwinnstr(g_chooser_win, 1, 2, buf, (int)n - 1);
    assert(rc != ERR);
    trim_window_padding(buf);
}

static void
init_screen(void) {
    setlocale(LC_ALL, "");
    assert(dsd_test_setenv("TERM", "xterm-256color", 0) == 0);
    g_in = tmpfile();
    g_out = tmpfile();
    assert(g_in != NULL);
    assert(g_out != NULL);
    g_screen = newterm(NULL, g_out, g_in);
    assert(g_screen != NULL);
    set_term(g_screen);
    noecho();
    cbreak();
    keypad(stdscr, TRUE);
    resizeterm(17, 80);
    clear();
    refresh();
}

static void
shutdown_screen(void) {
    if (g_screen) {
        endwin();
        delscreen(g_screen);
        g_screen = NULL;
    }
    if (g_in) {
        fclose(g_in);
        g_in = NULL;
    }
    if (g_out) {
        fclose(g_out);
        g_out = NULL;
    }
}

WINDOW*
ui_make_window(int h, int w, int y, int x) {
    g_chooser_win = newwin(h, w, y, x);
    return g_chooser_win;
}

void
ui_statusf(const char* fmt, ...) {
    (void)fmt;
}

int
main(void) {
    char title[128];

    init_screen();

    ui_chooser_start("Devices", ITEMS, (int)(sizeof ITEMS / sizeof ITEMS[0]), capture_done, NULL);
    ui_chooser_render();
    read_title(title, sizeof title);
    assert(strcmp(title, "Devices (1-10/20)") == 0);

    assert(ui_chooser_handle_key(KEY_NPAGE) == 1);
    ui_chooser_render();
    read_title(title, sizeof title);
    assert(strcmp(title, "Devices (10-19/20)") == 0);

    assert(ui_chooser_handle_key(KEY_PPAGE) == 1);
    ui_chooser_render();
    read_title(title, sizeof title);
    assert(strcmp(title, "Devices (1-10/20)") == 0);

    assert(ui_chooser_handle_key(KEY_END) == 1);
    ui_chooser_render();
    read_title(title, sizeof title);
    assert(strcmp(title, "Devices (11-20/20)") == 0);

    assert(ui_chooser_handle_key(KEY_HOME) == 1);
    ui_chooser_render();
    read_title(title, sizeof title);
    assert(strcmp(title, "Devices (1-10/20)") == 0);

    assert(ui_chooser_handle_key('\r') == 1);
    assert(g_done_sel == 0);

    ui_chooser_start("Devices", ITEMS, (int)(sizeof ITEMS / sizeof ITEMS[0]), capture_done, NULL);
    assert(ui_chooser_handle_key('q') == 1);
    assert(g_done_sel == -1);

    shutdown_screen();
    printf("UI_CHOOSER_NAVIGATION: OK\n");
    return 0;
}
