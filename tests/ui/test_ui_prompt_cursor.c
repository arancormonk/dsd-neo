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
#include <locale.h>
#include <stdio.h>
#include <string.h>

#include "menu_prompts.h"
#include "test_support.h"

static SCREEN* g_screen = NULL;
static FILE* g_in = NULL;
static FILE* g_out = NULL;
static int g_done_called = 0;
static char g_done_text[256];

static void
capture_done(void* user, const char* text) {
    (void)user;
    g_done_called = 1;
    if (text) {
        strncpy(g_done_text, text, sizeof g_done_text - 1);
        g_done_text[sizeof g_done_text - 1] = '\0';
    } else {
        g_done_text[0] = '\0';
    }
}

WINDOW*
ui_make_window(int h, int w, int y, int x) {
    return newwin(h, w, y, x);
}

void
ui_statusf(const char* fmt, ...) {
    (void)fmt;
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
    resizeterm(24, 80);
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

int
main(void) {
    init_screen();

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

    shutdown_screen();
    printf("UI_PROMPT_CURSOR: OK\n");
    return 0;
}
