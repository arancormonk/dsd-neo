// SPDX-License-Identifier: GPL-3.0-or-later
// Coverage fixtures intentionally use private-source inclusion, synthetic sentinels,
// invalid-value negative vectors, or wrapper symbols to exercise guarded behavior.
// NOLINTBEGIN(bugprone-suspicious-include)
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <assert.h>
#include <curses.h>
#include <dsd-neo/core/safe_api.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static int g_fake_stdscr_storage;
static int g_fake_window_storage;
static int g_other_window_storage;
WINDOW* stdscr;

static int g_rows;
static int g_cols;
static int g_cur_y;
static int g_cur_x;
static int g_newwin_h;
static int g_newwin_w;
static int g_newwin_y;
static int g_newwin_x;
static int g_keypad_count;
static bool g_keypad_enabled;
static int g_wtimeout_count;
static int g_wtimeout_delay;
static int g_wborder_count;
static int g_wnoutrefresh_count;
static int g_delwin_count;
static WINDOW* g_delwin_arg;
static int g_doupdate_count;
static int g_wmove_count;
static int g_last_move_y;
static int g_last_move_x;
static int g_whline_count;
static int g_last_hline_y;
static int g_last_hline_x;
static chtype g_last_hline_ch;
static int g_last_hline_len;
static int g_waddch_count;
static chtype g_last_addch;
static int g_waddnstr_count;
static char g_added_text[64];
static int g_wattr_get_count;
static int g_wattr_on_count;
static attr_t g_last_attr_on;
static int g_wattr_set_count;
static attr_t g_last_attr_set;
static NCURSES_PAIRS_T g_last_pair_set;
static WINDOW* g_newwin_result;

static void
reset_curses_stubs(void) {
    stdscr = (WINDOW*)&g_fake_stdscr_storage;
    g_newwin_result = (WINDOW*)&g_fake_window_storage;
    g_rows = 24;
    g_cols = 80;
    g_cur_y = 0;
    g_cur_x = 0;
    g_newwin_h = 0;
    g_newwin_w = 0;
    g_newwin_y = 0;
    g_newwin_x = 0;
    g_keypad_count = 0;
    g_keypad_enabled = false;
    g_wtimeout_count = 0;
    g_wtimeout_delay = -1;
    g_wborder_count = 0;
    g_wnoutrefresh_count = 0;
    g_delwin_count = 0;
    g_delwin_arg = NULL;
    g_doupdate_count = 0;
    g_wmove_count = 0;
    g_last_move_y = -1;
    g_last_move_x = -1;
    g_whline_count = 0;
    g_last_hline_y = -1;
    g_last_hline_x = -1;
    g_last_hline_ch = 0;
    g_last_hline_len = -1;
    g_waddch_count = 0;
    g_last_addch = 0;
    g_waddnstr_count = 0;
    g_added_text[0] = '\0';
    g_wattr_get_count = 0;
    g_wattr_on_count = 0;
    g_last_attr_on = 0;
    g_wattr_set_count = 0;
    g_last_attr_set = 0;
    g_last_pair_set = 0;
}

WINDOW*
newwin(int h, int w, int y, int x) {
    g_newwin_h = h;
    g_newwin_w = w;
    g_newwin_y = y;
    g_newwin_x = x;
    return g_newwin_result;
}

int
keypad(WINDOW* win, bool enabled) {
    assert(win == g_newwin_result);
    g_keypad_count++;
    g_keypad_enabled = enabled;
    return OK;
}

void
wtimeout(WINDOW* win, int delay) {
    assert(win == g_newwin_result);
    g_wtimeout_count++;
    g_wtimeout_delay = delay;
}

int
wborder(WINDOW* win, chtype ls, chtype rs, chtype ts, chtype bs, chtype tl, chtype tr, chtype bl, chtype br) {
    (void)ls;
    (void)rs;
    (void)ts;
    (void)bs;
    (void)tl;
    (void)tr;
    (void)bl;
    (void)br;
    assert(win == g_newwin_result);
    g_wborder_count++;
    return OK;
}

int
wnoutrefresh(WINDOW* win) {
    assert(win == g_newwin_result);
    g_wnoutrefresh_count++;
    return OK;
}

int
delwin(WINDOW* win) {
    g_delwin_count++;
    g_delwin_arg = win;
    return OK;
}

int
doupdate(void) {
    g_doupdate_count++;
    return OK;
}

int
getmaxy(const WINDOW* win) {
    assert(win == stdscr);
    return g_rows;
}

int
getmaxx(const WINDOW* win) {
    assert(win == stdscr);
    return g_cols;
}

int
getcury(const WINDOW* win) {
    assert(win == stdscr);
    return g_cur_y;
}

int
getcurx(const WINDOW* win) {
    assert(win == stdscr);
    return g_cur_x;
}

int
wmove(WINDOW* win, int y, int x) {
    assert(win == stdscr);
    g_wmove_count++;
    g_last_move_y = y;
    g_last_move_x = x;
    return OK;
}

int
whline(WINDOW* win, chtype ch, int n) {
    assert(win == stdscr);
    g_whline_count++;
    g_last_hline_y = g_last_move_y;
    g_last_hline_x = g_last_move_x;
    g_last_hline_ch = ch;
    g_last_hline_len = n;
    return OK;
}

int
waddch(WINDOW* win, const chtype ch) {
    assert(win == stdscr);
    g_waddch_count++;
    g_last_addch = ch;
    return OK;
}

int
waddnstr(WINDOW* win, const char* str, int n) {
    assert(win == stdscr);
    (void)n;
    g_waddnstr_count++;
    if (str) {
        const size_t used = strlen(g_added_text);
        if (used < sizeof(g_added_text) - 1U) {
            size_t copy_len = strlen(str);
            const size_t avail = sizeof(g_added_text) - used - 1U;
            if (copy_len > avail) {
                copy_len = avail;
            }
            DSD_MEMCPY(g_added_text + used, str, copy_len);
            g_added_text[used + copy_len] = '\0';
        }
    }
    return OK;
}

int
wattr_get(WINDOW* win, attr_t* attrs, NCURSES_PAIRS_T* pair, void* opts) {
    (void)opts;
    assert(win == stdscr);
    g_wattr_get_count++;
    *attrs = (attr_t)0x1234;
    *pair = (NCURSES_PAIRS_T)9;
    return OK;
}

int
wattr_on(WINDOW* win, attr_t attrs, void* opts) {
    (void)opts;
    assert(win == stdscr);
    g_wattr_on_count++;
    g_last_attr_on = attrs;
    return OK;
}

int
wattr_set(WINDOW* win, attr_t attrs, NCURSES_PAIRS_T pair, void* opts) {
    (void)opts;
    assert(win == stdscr);
    g_wattr_set_count++;
    g_last_attr_set = attrs;
    g_last_pair_set = pair;
    return OK;
}

#include "../../src/ui/terminal/ui_prims.c"

static void
test_status_lifecycle(void) {
    char buf[32];
    time_t now = time(NULL);

    ui_status_clear_if_expired(now + 10);
    assert(ui_status_peek(NULL, sizeof buf, now) == 0);
    assert(ui_status_peek(buf, 0, now) == 0);
    assert(ui_status_peek(buf, sizeof buf, now) == 0);

    ui_statusf(NULL);
    assert(ui_status_peek(buf, sizeof buf, now) == 0);

    ui_statusf("level %d", 7);
    now = time(NULL);
    assert(ui_status_peek(buf, sizeof buf, now) == 1);
    assert(strcmp(buf, "level 7") == 0);

    ui_status_clear_if_expired(now);
    assert(ui_status_peek(buf, sizeof buf, now) == 1);
    assert(strcmp(buf, "level 7") == 0);

    assert(ui_status_peek(buf, 5, now) == 1);
    assert(strcmp(buf, "leve") == 0);

    assert(ui_status_peek(buf, sizeof buf, now + 10) == 0);
    ui_status_clear_if_expired(now + 10);
    assert(ui_status_peek(buf, sizeof buf, now) == 0);
}

static void
test_iden_color_mapping(void) {
    assert(ui_iden_color_pair(-4) == 21);
    assert(ui_iden_color_pair(0) == 21);
    assert(ui_iden_color_pair(7) == 28);
    assert(ui_iden_color_pair(8) == 21);
    assert(ui_iden_color_pair(15) == 28);
}

static void
test_gamma_map_contract(void) {
    assert(ui_gamma_map01(-0.25) == 0.0);
    assert(ui_gamma_map01(0.0) == 0.0);
    assert(ui_gamma_map01(1.0) == 1.0);
    assert(ui_gamma_map01(1.25) == 1.0);

    double quarter = ui_gamma_map01(0.25);
    double half = ui_gamma_map01(0.5);
    double three_quarters = ui_gamma_map01(0.75);

    assert(quarter > 0.49 && quarter < 0.52);
    assert(half > quarter);
    assert(half > 0.70 && half < 0.72);
    assert(three_quarters > half);
    assert(three_quarters > 0.86 && three_quarters < 0.88);
}

static void
test_window_lifecycle_contracts(void) {
    reset_curses_stubs();
    WINDOW* win = ui_make_window(4, 20, 2, 3);
    assert(win == g_newwin_result);
    assert(g_newwin_h == 4);
    assert(g_newwin_w == 20);
    assert(g_newwin_y == 2);
    assert(g_newwin_x == 3);
    assert(g_keypad_count == 1);
    assert(g_keypad_enabled == true);
    assert(g_wtimeout_count == 1);
    assert(g_wtimeout_delay == 0);
    assert(g_wborder_count == 1);
    assert(g_wnoutrefresh_count == 1);

    reset_curses_stubs();
    g_newwin_result = NULL;
    assert(ui_make_window(1, 2, 3, 4) == NULL);
    assert(g_keypad_count == 0);
    assert(g_wtimeout_count == 0);
    assert(g_wborder_count == 0);
    assert(g_wnoutrefresh_count == 0);

    reset_curses_stubs();
    win = (WINDOW*)&g_other_window_storage;
    ui_destroy_window(&win);
    assert(g_delwin_count == 1);
    assert(g_delwin_arg == (WINDOW*)&g_other_window_storage);
    assert(win == NULL);
    ui_destroy_window(NULL);
    ui_destroy_window(&win);
    assert(g_delwin_count == 1);

    ui_commit_frame();
    assert(g_doupdate_count == 1);
}

static void
test_drawing_helpers_use_terminal_geometry(void) {
    reset_curses_stubs();
    g_rows = 3;
    g_cols = 6;
    g_cur_y = 1;
    ui_print_hr();
    assert(g_whline_count == 1);
    assert(g_last_hline_y == 1);
    assert(g_last_hline_x == 0);
    assert(g_last_hline_ch == '-');
    assert(g_last_hline_len == 6);
    assert(g_wmove_count == 2);
    assert(g_last_move_y == 2);
    assert(g_last_move_x == 0);
    assert(g_waddch_count == 0);

    reset_curses_stubs();
    g_rows = 0;
    g_cols = 0;
    ui_print_hr();
    assert(g_last_hline_len == 80);
    assert(g_waddch_count == 1);
    assert(g_last_addch == '\n');

    reset_curses_stubs();
    g_rows = 2;
    g_cols = 10;
    g_cur_y = 0;
    ui_print_header("TG");
    assert(g_waddnstr_count == 2);
    assert(strcmp(g_added_text, "--TG") == 0);
    assert(g_whline_count == 1);
    assert(g_last_hline_y == 0);
    assert(g_last_hline_x == 4);
    assert(g_last_hline_len == 6);
    assert(g_last_move_y == 1);
    assert(g_last_move_x == 0);

    reset_curses_stubs();
    g_rows = 1;
    g_cols = 3;
    ui_print_header(NULL);
    assert(strcmp(g_added_text, "--") == 0);
    assert(g_last_hline_len == 78);
    assert(g_waddch_count == 1);
    assert(g_last_addch == '\n');
}

static void
test_border_helpers_restore_saved_attributes(void) {
    reset_curses_stubs();
    ui_print_lborder();
    assert(g_wattr_get_count == 1);
    assert(g_wattr_on_count == 1);
    assert(g_last_attr_on == COLOR_PAIR(4));
    assert(g_waddch_count == 1);
    assert(g_last_addch == '|');
    assert(g_wattr_set_count == 1);
    assert(g_last_attr_set == (attr_t)0x1234);
    assert(g_last_pair_set == (NCURSES_PAIRS_T)9);

    reset_curses_stubs();
    ui_print_lborder_green();
    assert(g_wattr_on_count == 1);
    assert(g_last_attr_on == COLOR_PAIR(3));
    assert(g_wattr_set_count == 1);
    assert(g_last_attr_set == (attr_t)0x1234);
    assert(g_last_pair_set == (NCURSES_PAIRS_T)9);
}

int
main(void) {
    test_status_lifecycle();
    test_iden_color_mapping();
    test_gamma_map_contract();
    test_window_lifecycle_contracts();
    test_drawing_helpers_use_terminal_geometry();
    test_border_helpers_restore_saved_attributes();
    printf("UI_PRIMS_STATUS_GAMMA: OK\n");
    return 0;
}

// NOLINTEND(bugprone-suspicious-include)
