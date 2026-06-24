// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <assert.h>
#include <curses.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/platform/platform.h>
#include <dsd-neo/ui/keymap.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "menu_prompts.h"

static int g_string_done_called = 0;
static int g_string_was_null = 0;
static char g_string_text[128];
static int g_int_done_called = 0;
static int g_int_ok = -1;
static int g_int_value = -1;
static int g_double_done_called = 0;
static int g_double_ok = -1;
static double g_double_value = -1.0;
static int g_status_called = 0;
static char g_status_text[128];

WINDOW* ui_make_window(int h, int w, int y, int x);                  // NOLINT(misc-use-internal-linkage)
void ui_statusf(const char* fmt, ...) DSD_ATTR_FORMAT(printf, 1, 2); // NOLINT(misc-use-internal-linkage)

WINDOW*
ui_make_window(int h, int w, int y, int x) { // NOLINT(misc-use-internal-linkage)
    (void)h;
    (void)w;
    (void)y;
    (void)x;
    return NULL;
}

void
ui_statusf(const char* fmt, ...) { // NOLINT(misc-use-internal-linkage)
    va_list ap;
    va_start(ap, fmt);
    DSD_VSNPRINTF(g_status_text, sizeof g_status_text, fmt, ap);
    va_end(ap);
    g_status_called++;
}

static void
reset_string_capture(void) {
    g_string_done_called = 0;
    g_string_was_null = 0;
    g_string_text[0] = '\0';
}

static void
capture_string_done(void* user, const char* text) {
    (void)user;
    g_string_done_called++;
    g_string_was_null = (text == NULL);
    if (text) {
        DSD_SNPRINTF(g_string_text, sizeof g_string_text, "%s", text);
    } else {
        g_string_text[0] = '\0';
    }
}

static void
reset_int_capture(void) {
    g_int_done_called = 0;
    g_int_ok = -1;
    g_int_value = -1;
    g_status_called = 0;
    g_status_text[0] = '\0';
}

static void
capture_int_done(void* user, int ok, int value) {
    (void)user;
    g_int_done_called++;
    g_int_ok = ok;
    g_int_value = value;
}

static void
reset_double_capture(void) {
    g_double_done_called = 0;
    g_double_ok = -1;
    g_double_value = -1.0;
    g_status_called = 0;
    g_status_text[0] = '\0';
}

static void
capture_double_done(void* user, int ok, double value) {
    (void)user;
    g_double_done_called++;
    g_double_ok = ok;
    g_double_value = value;
}

static void
clear_prompt_prefill(void) {
    for (int i = 0; i < 16; i++) {
        assert(ui_prompt_handle_key(KEY_BACKSPACE) == 1);
    }
}

static void
type_text(const char* text) {
    for (const char* p = text; p && *p; p++) {
        assert(ui_prompt_handle_key((unsigned char)*p) == 1);
    }
}

static void
test_string_prompt_completion_paths(void) {
    assert(ui_prompt_handle_key('x') == 0);

    reset_string_capture();
    ui_prompt_open_string_async("Small", "abc", 1, capture_string_done, NULL);
    assert(ui_prompt_active() == 1);
    assert(ui_prompt_handle_key('Z') == 1);
    assert(ui_prompt_handle_key('\r') == 1);
    assert(g_string_done_called == 1);
    assert(g_string_was_null == 0);
    assert(strcmp(g_string_text, "a") == 0);
    assert(ui_prompt_active() == 0);

    reset_string_capture();
    ui_prompt_open_string_async("Empty", NULL, 8, capture_string_done, NULL);
    assert(ui_prompt_handle_key('\r') == 1);
    assert(g_string_done_called == 1);
    assert(g_string_was_null == 0);
    assert(strcmp(g_string_text, "") == 0);

    reset_string_capture();
    ui_prompt_open_string_async("Cancel", "kept", 16, capture_string_done, NULL);
    assert(ui_prompt_handle_key(DSD_KEY_ESC) == 1);
    assert(g_string_done_called == 1);
    assert(g_string_was_null == 1);

    reset_string_capture();
    ui_prompt_open_string_async("Force close", "pending", 16, capture_string_done, NULL);
    ui_prompt_close_all();
    assert(g_string_done_called == 1);
    assert(g_string_was_null == 1);
    ui_prompt_close_all();
    assert(g_string_done_called == 1);
}

static void
test_string_prompt_key_edges(void) {
    reset_string_capture();
    ui_prompt_open_string_async("Capacity", "a", 3, capture_string_done, NULL);
    assert(ui_prompt_handle_key('b') == 1);
    assert(ui_prompt_handle_key('c') == 1);
    assert(ui_prompt_handle_key('d') == 1);
    assert(ui_prompt_handle_key(KEY_ENTER) == 1);
    assert(g_string_done_called == 1);
    assert(g_string_was_null == 0);
    assert(strcmp(g_string_text, "ab") == 0);

    reset_string_capture();
    ui_prompt_open_string_async("Alternate delete", "abc", 16, capture_string_done, NULL);
    assert(ui_prompt_handle_key(127) == 1);
    assert(ui_prompt_handle_key(8) == 1);
    assert(ui_prompt_handle_key(1) == 1);
    assert(ui_prompt_handle_key('\n') == 1);
    assert(g_string_done_called == 1);
    assert(strcmp(g_string_text, "a") == 0);

    reset_string_capture();
    ui_prompt_open_string_async("Printable q", "", 16, capture_string_done, NULL);
    assert(ui_prompt_handle_key(KEY_RESIZE) == 1);
    assert(ui_prompt_handle_key(ERR) == 1);
    assert(g_string_done_called == 0);
    assert(ui_prompt_handle_key('q') == 1);
    assert(ui_prompt_handle_key('\r') == 1);
    assert(g_string_done_called == 1);
    assert(g_string_was_null == 0);
    assert(strcmp(g_string_text, "q") == 0);
}

static void
test_string_prompt_cursor_editing(void) {
    reset_string_capture();
    ui_prompt_open_string_async("Edit", "abcd", 16, capture_string_done, NULL);
    assert(ui_prompt_handle_key(KEY_HOME) == 1);
    assert(ui_prompt_handle_key('X') == 1);
    assert(ui_prompt_handle_key(KEY_RIGHT) == 1);
    assert(ui_prompt_handle_key(KEY_RIGHT) == 1);
    assert(ui_prompt_handle_key(KEY_DC) == 1);
    assert(ui_prompt_handle_key(KEY_END) == 1);
    assert(ui_prompt_handle_key('Y') == 1);
    assert(ui_prompt_handle_key(KEY_LEFT) == 1);
    assert(ui_prompt_handle_key(KEY_BACKSPACE) == 1);
    assert(ui_prompt_handle_key('\r') == 1);
    assert(g_string_done_called == 1);
    assert(g_string_was_null == 0);
    assert(strcmp(g_string_text, "XabY") == 0);
}

static void
test_typed_prompt_validation(void) {
    reset_int_capture();
    ui_prompt_open_int_async("Integer", 42, capture_int_done, NULL);
    assert(ui_prompt_handle_key('\r') == 1);
    assert(g_int_done_called == 1);
    assert(g_int_ok == 1);
    assert(g_int_value == 42);
    assert(g_status_called == 0);

    reset_int_capture();
    ui_prompt_open_int_async("Integer", 0, capture_int_done, NULL);
    clear_prompt_prefill();
    assert(ui_prompt_handle_key('x') == 1);
    assert(ui_prompt_handle_key('\r') == 1);
    assert(g_int_done_called == 1);
    assert(g_int_ok == 0);
    assert(g_int_value == 0);
    assert(g_status_called == 1);
    assert(strcmp(g_status_text, "Invalid integer input") == 0);

    reset_int_capture();
    ui_prompt_open_int_async("Integer", 0, capture_int_done, NULL);
    clear_prompt_prefill();
    assert(ui_prompt_handle_key('\r') == 1);
    assert(g_int_done_called == 1);
    assert(g_int_ok == 0);
    assert(g_int_value == 0);
    assert(g_status_called == 0);

    reset_double_capture();
    ui_prompt_open_double_async("Number", 1.25, capture_double_done, NULL);
    assert(ui_prompt_handle_key('\r') == 1);
    assert(g_double_done_called == 1);
    assert(g_double_ok == 1);
    assert(g_double_value > 1.249 && g_double_value < 1.251);
    assert(g_status_called == 0);

    reset_double_capture();
    ui_prompt_open_double_async("Number", 0.0, capture_double_done, NULL);
    clear_prompt_prefill();
    type_text("1.2x");
    assert(ui_prompt_handle_key('\r') == 1);
    assert(g_double_done_called == 1);
    assert(g_double_ok == 0);
    assert(g_double_value == 0.0);
    assert(g_status_called == 1);
    assert(strcmp(g_status_text, "Invalid numeric input") == 0);
}

static void
test_typed_prompt_cancel_paths(void) {
    reset_int_capture();
    ui_prompt_open_int_async("Integer cancel", 123, capture_int_done, NULL);
    assert(ui_prompt_handle_key(DSD_KEY_ESC) == 1);
    assert(g_int_done_called == 1);
    assert(g_int_ok == 0);
    assert(g_int_value == 0);
    assert(g_status_called == 0);

    reset_double_capture();
    ui_prompt_open_double_async("Number cancel", 3.5, capture_double_done, NULL);
    ui_prompt_close_all();
    assert(g_double_done_called == 1);
    assert(g_double_ok == 0);
    assert(g_double_value == 0.0);
    assert(g_status_called == 0);
}

typedef struct {
    int calls;
    int first_sel;
    int second_sel;
} ChooserCapture;

static const char* const CHOOSER_ONE[] = {"one", "two", "three"};
static const char* const CHOOSER_TWO[] = {"red", "green"};
static const char* const CHOOSER_MANY[] = {"zero", "one", "two", "three", "four", "five"};

static void
capture_chooser_reentry(void* user, int selected) {
    ChooserCapture* capture = (ChooserCapture*)user;
    capture->calls++;
    if (capture->calls == 1) {
        capture->first_sel = selected;
        ui_chooser_start("Second", CHOOSER_TWO, 2, capture_chooser_reentry, user);
        return;
    }
    capture->second_sel = selected;
}

static void
capture_chooser_selected(void* user, int selected) {
    int* out = (int*)user;
    *out = selected;
}

static void
test_chooser_edge_paths(void) {
    assert(ui_chooser_handle_key('x') == 0);

    int selected = -2;
    ui_chooser_start("Empty", NULL, 0, capture_chooser_selected, &selected);
    assert(selected == -1);
    assert(ui_chooser_active() == 0);

    selected = -2;
    ui_chooser_start("Single", CHOOSER_ONE, 1, capture_chooser_selected, &selected);
    assert(ui_chooser_handle_key(KEY_RESIZE) == 1);
    assert(ui_chooser_handle_key(KEY_UP) == 1);
    assert(ui_chooser_handle_key(KEY_DOWN) == 1);
    assert(ui_chooser_handle_key(KEY_ENTER) == 1);
    assert(selected == 0);
    assert(ui_chooser_active() == 0);

    ChooserCapture capture = {0, -2, -2};
    ui_chooser_start("First", CHOOSER_ONE, 3, capture_chooser_reentry, &capture);
    assert(ui_chooser_active() == 1);
    assert(ui_chooser_handle_key(KEY_DOWN) == 1);
    assert(ui_chooser_handle_key(KEY_RIGHT) == 1);
    assert(capture.calls == 1);
    assert(capture.first_sel == 1);
    assert(ui_chooser_active() == 1);

    UiChooserTestSnapshot snapshot = ui_chooser_test_snapshot();
    assert(snapshot.count == 2);
    assert(snapshot.sel == 0);
    assert(ui_chooser_handle_key('q') == 1);
    assert(capture.calls == 2);
    assert(capture.second_sel == -1);
    assert(ui_chooser_active() == 0);
}

static void
test_chooser_page_navigation_clamps_selection(void) {
    int selected = -2;
    ui_chooser_start("Many", CHOOSER_MANY, 6, capture_chooser_selected, &selected);
    ui_chooser_test_set_page_rows(3);
    UiChooserTestSnapshot snapshot = ui_chooser_test_snapshot();
    assert(snapshot.sel == 0);
    assert(snapshot.top == 0);

    assert(ui_chooser_handle_key(KEY_NPAGE) == 1);
    snapshot = ui_chooser_test_snapshot();
    assert(snapshot.sel == 2);
    assert(snapshot.top == 2);

    assert(ui_chooser_handle_key(KEY_NPAGE) == 1);
    snapshot = ui_chooser_test_snapshot();
    assert(snapshot.sel == 4);
    assert(snapshot.top == 3);

    assert(ui_chooser_handle_key(KEY_END) == 1);
    snapshot = ui_chooser_test_snapshot();
    assert(snapshot.sel == 5);
    assert(snapshot.top == 3);

    assert(ui_chooser_handle_key(KEY_PPAGE) == 1);
    snapshot = ui_chooser_test_snapshot();
    assert(snapshot.sel == 3);
    assert(snapshot.top == 1);

    assert(ui_chooser_handle_key(KEY_HOME) == 1);
    snapshot = ui_chooser_test_snapshot();
    assert(snapshot.sel == 0);
    assert(snapshot.top == 0);

    assert(ui_chooser_handle_key('\r') == 1);
    assert(selected == 0);
    assert(ui_chooser_active() == 0);
}

static void
test_chooser_up_down_wraparound(void) {
    int selected = -2;
    ui_chooser_start("Wrap", CHOOSER_ONE, 3, capture_chooser_selected, &selected);
    ui_chooser_test_set_page_rows(2);

    assert(ui_chooser_handle_key(KEY_UP) == 1);
    UiChooserTestSnapshot snapshot = ui_chooser_test_snapshot();
    assert(snapshot.sel == 2);
    assert(snapshot.top == 1);

    assert(ui_chooser_handle_key(KEY_DOWN) == 1);
    snapshot = ui_chooser_test_snapshot();
    assert(snapshot.sel == 0);
    assert(snapshot.top == 0);

    assert(ui_chooser_handle_key(KEY_LEFT) == 1);
    assert(selected == -1);
    assert(ui_chooser_active() == 0);
}

static void
test_help_activation_and_close_keys(void) {
    ui_help_close();
    ui_help_open(NULL);
    assert(ui_help_active() == 0);
    ui_help_open("");
    assert(ui_help_active() == 0);

    ui_help_open("line one\nline two");
    assert(ui_help_active() == 1);
    assert(ui_help_handle_key(ERR) == 1);
    assert(ui_help_active() == 1);
    assert(ui_help_handle_key('h') == 1);
    assert(ui_help_active() == 0);

    ui_help_open("close with enter");
    assert(ui_help_active() == 1);
    assert(ui_help_handle_key('\r') == 1);
    assert(ui_help_active() == 0);

    ui_help_open("close with escape");
    assert(ui_help_active() == 1);
    assert(ui_help_handle_key(DSD_KEY_ESC) == 1);
    assert(ui_help_active() == 0);

    ui_help_open("close with shifted help");
    assert(ui_help_active() == 1);
    assert(ui_help_handle_key('H') == 1);
    assert(ui_help_active() == 0);

    ui_help_open("close with left");
    assert(ui_help_active() == 1);
    assert(ui_help_handle_key(KEY_LEFT) == 1);
    assert(ui_help_active() == 0);
}

static void
test_help_scroll_navigation_clamps_to_content(void) {
    ui_help_open("scrollable help");
    assert(ui_help_active() == 1);
    ui_help_test_set_metrics(12, 4, -5);
    UiHelpTestSnapshot snapshot = ui_help_test_snapshot();
    assert(snapshot.scroll == 0);
    assert(snapshot.line_count == 12);
    assert(snapshot.page_rows == 4);

    assert(ui_help_handle_key(KEY_DOWN) == 1);
    snapshot = ui_help_test_snapshot();
    assert(snapshot.scroll == 1);

    assert(ui_help_handle_key(KEY_NPAGE) == 1);
    snapshot = ui_help_test_snapshot();
    assert(snapshot.scroll == 4);

    assert(ui_help_handle_key(KEY_END) == 1);
    snapshot = ui_help_test_snapshot();
    assert(snapshot.scroll == 8);

    assert(ui_help_handle_key(KEY_DOWN) == 1);
    snapshot = ui_help_test_snapshot();
    assert(snapshot.scroll == 8);

    assert(ui_help_handle_key(KEY_PPAGE) == 1);
    snapshot = ui_help_test_snapshot();
    assert(snapshot.scroll == 5);

    assert(ui_help_handle_key(KEY_UP) == 1);
    snapshot = ui_help_test_snapshot();
    assert(snapshot.scroll == 4);

    assert(ui_help_handle_key(KEY_HOME) == 1);
    snapshot = ui_help_test_snapshot();
    assert(snapshot.scroll == 0);

    assert(ui_help_handle_key(KEY_UP) == 1);
    snapshot = ui_help_test_snapshot();
    assert(snapshot.scroll == 0);

    assert(ui_help_handle_key(KEY_RESIZE) == 1);
    assert(ui_help_active() == 1);
    assert(ui_help_handle_key('x') == 1);
    assert(ui_help_active() == 1);
    assert(ui_help_handle_key('Q') == 1);
    assert(ui_help_active() == 0);
}

static void
test_help_wrap_text_contracts(void) {
    char line[64];

    int count = ui_help_wrap_line_for_test("alpha beta", 20, 0, line, sizeof line);
    assert(count == 1);
    assert(strcmp(line, "alpha beta") == 0);

    count = ui_help_wrap_line_for_test("   \nnext", 10, 0, line, sizeof line);
    assert(count == 2);
    assert(strcmp(line, "") == 0);

    count = ui_help_wrap_line_for_test("   \nnext", 10, 1, line, sizeof line);
    assert(count == 2);
    assert(strcmp(line, "next") == 0);

    count = ui_help_wrap_line_for_test("alpha beta\nsuperlongword tail", 6, 0, line, sizeof line);
    assert(count == 6);
    assert(strcmp(line, "alpha") == 0);

    count = ui_help_wrap_line_for_test("alpha beta\nsuperlongword tail", 6, 1, line, sizeof line);
    assert(count == 6);
    assert(strcmp(line, "beta") == 0);

    count = ui_help_wrap_line_for_test("alpha beta\nsuperlongword tail", 6, 2, line, sizeof line);
    assert(count == 6);
    assert(strcmp(line, "superl") == 0);

    count = ui_help_wrap_line_for_test("alpha beta\nsuperlongword tail", 6, 3, line, sizeof line);
    assert(count == 6);
    assert(strcmp(line, "ongwor") == 0);

    count = ui_help_wrap_line_for_test("alpha beta\nsuperlongword tail", 6, 5, line, sizeof line);
    assert(count == 6);
    assert(strcmp(line, "tail") == 0);

    count = ui_help_wrap_line_for_test(NULL, 12, 0, line, sizeof line);
    assert(count == 1);
    assert(strcmp(line, "") == 0);

    count = ui_help_wrap_line_for_test("ignored", 0, 0, line, sizeof line);
    assert(count == 0);
    assert(strcmp(line, "") == 0);
}

static void
test_chooser_layout_contracts(void) {
    static const char* const ITEMS[] = {"short", "a much longer option"};
    int h = -1;
    int w = -1;
    int y = -1;
    int x = -1;

    int max_item = ui_chooser_max_item_width_for_test(ITEMS, 2);
    assert(max_item == (int)strlen("a much longer option"));
    assert(ui_chooser_max_item_width_for_test(ITEMS, 0) == 0);

    assert(ui_chooser_layout_for_test("Pick", "Footer", max_item, 2, 24, 80, &h, &w, &y, &x) == 1);
    assert(h == 7);
    assert(w == 26);
    assert(y == 8);
    assert(x == 27);

    assert(ui_chooser_layout_for_test("Pick", "Footer", max_item, 12, 10, 24, &h, &w, &y, &x) == 1);
    assert(h == 8);
    assert(w == 22);
    assert(y == 1);
    assert(x == 1);

    assert(ui_chooser_layout_for_test("Pick", "Footer", max_item, 2, 3, 80, &h, &w, &y, &x) == 0);
    assert(ui_chooser_layout_for_test("Pick", "Footer", max_item, 2, 24, 7, &h, &w, &y, &x) == 0);
    assert(ui_chooser_layout_for_test(NULL, "Footer", max_item, 2, 24, 80, &h, &w, &y, &x) == 0);
    assert(ui_chooser_layout_for_test("Pick", NULL, max_item, 2, 24, 80, &h, &w, &y, &x) == 0);
}

static void
test_prompt_layout_and_view_contracts(void) {
    assert(ui_prompt_center_axis_for_test(24, 8) == 8);
    assert(ui_prompt_center_axis_for_test(4, 9) == 0);

    assert(ui_prompt_fit_width_for_test(60, 80) == 60);
    assert(ui_prompt_fit_width_for_test(90, 80) == 78);
    assert(ui_prompt_fit_width_for_test(5, 80) == 10);
    assert(ui_prompt_fit_width_for_test(20, 3) == 0);
    assert(ui_prompt_fit_width_for_test(20, 8) == 8);

    assert(ui_prompt_fit_height_for_test(8, 24) == 8);
    assert(ui_prompt_fit_height_for_test(12, 8) == 6);
    assert(ui_prompt_fit_height_for_test(2, 10) == 6);
    assert(ui_prompt_fit_height_for_test(2, 5) == 3);
    assert(ui_prompt_fit_height_for_test(8, 2) == 0);

    int title_y = -2;
    int input_y = -2;
    int footer_y = -2;
    ui_prompt_rows_for_test(6, NULL, &input_y, &footer_y);
    assert(input_y == -2);
    assert(footer_y == -2);

    ui_prompt_rows_for_test(6, &title_y, &input_y, &footer_y);
    assert(title_y == 1);
    assert(input_y == 3);
    assert(footer_y == 4);
    ui_prompt_rows_for_test(5, &title_y, &input_y, &footer_y);
    assert(title_y == 1);
    assert(input_y == 2);
    assert(footer_y == 3);
    ui_prompt_rows_for_test(4, &title_y, &input_y, &footer_y);
    assert(title_y == 1);
    assert(input_y == 2);
    assert(footer_y == -1);
    ui_prompt_rows_for_test(3, &title_y, &input_y, &footer_y);
    assert(title_y == -1);
    assert(input_y == 1);
    assert(footer_y == -1);

    int field_col = -1;
    int field_right = -1;
    int field_width = -1;
    ui_prompt_field_geometry_for_test(12, &field_col, NULL, &field_width);
    assert(field_col == -1);
    assert(field_width == -1);

    ui_prompt_field_geometry_for_test(12, &field_col, &field_right, &field_width);
    assert(field_col == 4);
    assert(field_right == 10);
    assert(field_width == 6);
    ui_prompt_field_geometry_for_test(4, &field_col, &field_right, &field_width);
    assert(field_col == 2);
    assert(field_right == 2);
    assert(field_width == 1);
    ui_prompt_field_geometry_for_test(2, &field_col, &field_right, &field_width);
    assert(field_col == 2);
    assert(field_right == 0);
    assert(field_width == 1);

    UiPromptViewTestSnapshot view = ui_prompt_view_for_test("abc", 99, 4, 12, 8);
    assert(view.start == 0);
    assert(view.cursor == 3);
    assert(view.show_left_ellipsis == 0);
    assert(view.cursor_x == 7);

    view = ui_prompt_view_for_test("abcdefghij", 10, 4, 12, 8);
    assert(view.start == 6);
    assert(view.cursor == 10);
    assert(view.show_left_ellipsis == 1);
    assert(view.cursor_x == 11);

    view = ui_prompt_view_for_test("abcdefghij", 2, 4, 12, 8);
    assert(view.start == 0);
    assert(view.cursor == 2);
    assert(view.show_left_ellipsis == 0);
    assert(view.cursor_x == 6);

    view = ui_prompt_view_for_test("abcdefghij", 9, 4, 12, 3);
    assert(view.start == 7);
    assert(view.cursor == 9);
    assert(view.show_left_ellipsis == 0);
    assert(view.cursor_x == 6);

    view = ui_prompt_view_for_test(NULL, 99, -5, 0, 8);
    assert(view.start == 0);
    assert(view.cursor == 0);
    assert(view.show_left_ellipsis == 0);
    assert(view.cursor_x == 2);
}

int
main(void) {
    test_string_prompt_completion_paths();
    test_string_prompt_key_edges();
    test_string_prompt_cursor_editing();
    test_typed_prompt_validation();
    test_typed_prompt_cancel_paths();
    test_chooser_edge_paths();
    test_chooser_page_navigation_clamps_selection();
    test_chooser_up_down_wraparound();
    test_help_activation_and_close_keys();
    test_help_scroll_navigation_clamps_to_content();
    test_help_wrap_text_contracts();
    test_chooser_layout_contracts();
    test_prompt_layout_and_view_contracts();
    printf("UI_PROMPT_VALIDATION: OK\n");
    return 0;
}
