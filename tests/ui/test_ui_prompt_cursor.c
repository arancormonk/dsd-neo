// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Regression coverage for prompt cursor movement:
 *  - LEFT/RIGHT should move cursor within text
 *  - Home/End should jump to start/end
 *  - Backspace should delete before cursor
 *  - Delete should delete at cursor
 *  - Insert should occur at cursor position
 */

#include <assert.h>
#include <curses.h>
#include <dsd-neo/core/string_utils.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "menu_prompts.h"

static int g_done_called = 0;
static char g_done_text[256];
static FILE* g_term_in = NULL;
static FILE* g_term_out = NULL;
static SCREEN* g_screen = NULL;
static WINDOW* g_last_prompt_window = NULL;

static void
capture_done(void* user, const char* text) {
    (void)user;
    g_done_called = 1;
    if (text) {
        DSD_STRNCPY(g_done_text, text, sizeof g_done_text - 1);
        g_done_text[sizeof g_done_text - 1] = '\0';
    } else {
        g_done_text[0] = '\0';
    }
}

WINDOW* ui_make_window(int h, int w, int y, int x); // NOLINT(misc-use-internal-linkage)
void ui_statusf(const char* fmt, ...);              // NOLINT(misc-use-internal-linkage)

WINDOW*
ui_make_window(int h, int w, int y, int x) { // NOLINT(misc-use-internal-linkage)
    g_last_prompt_window = newwin(h, w, y, x);
    return g_last_prompt_window;
}

void
ui_statusf(const char* fmt, ...) { // NOLINT(misc-use-internal-linkage)
    (void)fmt;
}

static int
start_curses_render_test(void) {
#ifndef _WIN32
    const char* term = getenv("TERM");
    if (term == NULL || term[0] == '\0') {
        (void)setenv("TERM", "xterm-256color", 1);
    }
    (void)setenv("LINES", "24", 1);
    (void)setenv("COLUMNS", "80", 1);
#endif
    g_term_in = tmpfile();
    g_term_out = tmpfile();
    if (!g_term_in || !g_term_out) {
        return 0;
    }
    g_screen = newterm(NULL, g_term_out, g_term_in);
    if (!g_screen) {
        return 0;
    }
    (void)set_term(g_screen);
    return 1;
}

static void
stop_curses_render_test(void) {
    if (g_screen) {
        endwin();
        g_screen = NULL;
    }
    if (g_term_in) {
        fclose(g_term_in);
        g_term_in = NULL;
    }
    if (g_term_out) {
        fclose(g_term_out);
        g_term_out = NULL;
    }
}

static int
expected_prompt_input_y(int h) {
    int interior_rows = h - 2;
    if (interior_rows >= 4) {
        return 3;
    }
    if (interior_rows >= 2) {
        return 2;
    }
    return 1;
}

static void
test_render_leaves_cursor_on_input_field(void) {
    const char* path = "/home/mark/.config/dsd-neo/config.in";
    if (!start_curses_render_test()) {
        stop_curses_render_test();
        printf("UI_PROMPT_CURSOR: render cursor test skipped\n");
        return;
    }

    g_last_prompt_window = NULL;
    ui_prompt_open_string_async("Load config from path", path, 256, capture_done, NULL);
    ui_prompt_render();
    assert(g_last_prompt_window != NULL);

    int h = 0;
    int w = 0;
    getmaxyx(g_last_prompt_window, h, w);
    int y = -1;
    int x = -1;
    getyx(g_last_prompt_window, y, x);

    int expected_x = 4 + (int)strlen(path);
    int field_right = w - 2;
    if (expected_x > field_right) {
        expected_x = field_right;
    }
    assert(y == expected_prompt_input_y(h));
    assert(x == expected_x);

    ui_prompt_close_all();
    stop_curses_render_test();
}

int
main(void) {
    // Test: insert at cursor after moving LEFT
    g_done_called = 0;
    ui_prompt_open_string_async("Test", "abc", 64, capture_done, NULL);
    assert(ui_prompt_active() == 1);

    // Move cursor left twice: cursor at position 1 (between 'a' and 'b')
    assert(ui_prompt_handle_key(KEY_LEFT) == 1);
    assert(ui_prompt_handle_key(KEY_LEFT) == 1);

    // Insert 'X' at cursor position 1
    assert(ui_prompt_handle_key('X') == 1);

    // Submit and verify
    assert(ui_prompt_handle_key('\r') == 1);
    assert(g_done_called == 1);
    assert(strcmp(g_done_text, "aXbc") == 0);

    // Test: Home moves to start, type inserts at front
    g_done_called = 0;
    ui_prompt_open_string_async("Test", "def", 64, capture_done, NULL);
    assert(ui_prompt_handle_key(KEY_HOME) == 1);
    assert(ui_prompt_handle_key('Z') == 1);
    assert(ui_prompt_handle_key('\r') == 1);
    assert(g_done_called == 1);
    assert(strcmp(g_done_text, "Zdef") == 0);

    // Test: End moves to end after Home
    g_done_called = 0;
    ui_prompt_open_string_async("Test", "gh", 64, capture_done, NULL);
    assert(ui_prompt_handle_key(KEY_HOME) == 1);
    assert(ui_prompt_handle_key(KEY_END) == 1);
    assert(ui_prompt_handle_key('i') == 1);
    assert(ui_prompt_handle_key('\r') == 1);
    assert(g_done_called == 1);
    assert(strcmp(g_done_text, "ghi") == 0);

    // Test: Backspace deletes before cursor in middle
    g_done_called = 0;
    ui_prompt_open_string_async("Test", "abcd", 64, capture_done, NULL);
    assert(ui_prompt_handle_key(KEY_LEFT) == 1);      // cursor at 3
    assert(ui_prompt_handle_key(KEY_BACKSPACE) == 1); // delete 'c'
    assert(ui_prompt_handle_key('\r') == 1);
    assert(g_done_called == 1);
    assert(strcmp(g_done_text, "abd") == 0);

    // Test: Delete key removes character at cursor
    g_done_called = 0;
    ui_prompt_open_string_async("Test", "wxyz", 64, capture_done, NULL);
    assert(ui_prompt_handle_key(KEY_HOME) == 1); // cursor at 0
    assert(ui_prompt_handle_key(KEY_DC) == 1);   // delete 'w'
    assert(ui_prompt_handle_key('\r') == 1);
    assert(g_done_called == 1);
    assert(strcmp(g_done_text, "xyz") == 0);

    // Test: Delete at end of string does nothing
    g_done_called = 0;
    ui_prompt_open_string_async("Test", "ab", 64, capture_done, NULL);
    assert(ui_prompt_handle_key(KEY_DC) == 1); // cursor at end, no-op
    assert(ui_prompt_handle_key('\r') == 1);
    assert(g_done_called == 1);
    assert(strcmp(g_done_text, "ab") == 0);

    // Test: LEFT at position 0 stays at 0
    g_done_called = 0;
    ui_prompt_open_string_async("Test", "a", 64, capture_done, NULL);
    assert(ui_prompt_handle_key(KEY_HOME) == 1);
    assert(ui_prompt_handle_key(KEY_LEFT) == 1); // should stay at 0
    assert(ui_prompt_handle_key('B') == 1);
    assert(ui_prompt_handle_key('\r') == 1);
    assert(g_done_called == 1);
    assert(strcmp(g_done_text, "Ba") == 0);

    // Test: RIGHT at end stays at end
    g_done_called = 0;
    ui_prompt_open_string_async("Test", "c", 64, capture_done, NULL);
    assert(ui_prompt_handle_key(KEY_RIGHT) == 1); // at end, no-op
    assert(ui_prompt_handle_key('D') == 1);
    assert(ui_prompt_handle_key('\r') == 1);
    assert(g_done_called == 1);
    assert(strcmp(g_done_text, "cD") == 0);

    test_render_leaves_cursor_on_input_field();

    printf("UI_PROMPT_CURSOR: OK\n");
    return 0;
}
