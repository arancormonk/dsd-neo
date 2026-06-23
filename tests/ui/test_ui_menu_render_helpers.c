// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <assert.h>
#include <curses.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/ui/menu_core.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "menu_internal.h"

typedef struct {
    int enabled_mask;
    const char* dynamic_suffix;
} RenderCtx;

static int g_make_window_calls;
static int g_make_window_h;
static int g_make_window_w;
static int g_make_window_y;
static int g_make_window_x;
static FILE* g_term_in;
static FILE* g_term_out;
static SCREEN* g_screen;
static int g_status_peek_calls;
static int g_status_clear_calls;
static int g_status_peek_result;
static char g_status_text[64];

WINDOW* ui_make_window(int h, int w, int y, int x);  // NOLINT(misc-use-internal-linkage)
int ui_status_peek(char* buf, size_t n, time_t now); // NOLINT(misc-use-internal-linkage)
void ui_status_clear_if_expired(time_t now);         // NOLINT(misc-use-internal-linkage)

WINDOW*
ui_make_window(int h, int w, int y, int x) { // NOLINT(misc-use-internal-linkage)
    g_make_window_calls++;
    g_make_window_h = h;
    g_make_window_w = w;
    g_make_window_y = y;
    g_make_window_x = x;
    return NULL;
}

int
ui_status_peek(char* buf, size_t n, time_t now) { // NOLINT(misc-use-internal-linkage)
    (void)now;
    g_status_peek_calls++;
    if (!g_status_peek_result) {
        return 0;
    }
    if (buf && n > 0) {
        DSD_SNPRINTF(buf, n, "%s", g_status_text);
    }
    return 1;
}

void
ui_status_clear_if_expired(time_t now) { // NOLINT(misc-use-internal-linkage)
    (void)now;
    g_status_clear_calls++;
}

static bool
enabled_bit0(const void* ctx) {
    const RenderCtx* render_ctx = (const RenderCtx*)ctx;
    return render_ctx && ((render_ctx->enabled_mask & 0x01) != 0);
}

static bool
enabled_bit1(const void* ctx) {
    const RenderCtx* render_ctx = (const RenderCtx*)ctx;
    return render_ctx && ((render_ctx->enabled_mask & 0x02) != 0);
}

static const char*
dynamic_label(const void* ctx, char* buf, size_t buf_len) {
    const RenderCtx* render_ctx = (const RenderCtx*)ctx;
    DSD_SNPRINTF(buf, buf_len, "Dynamic-%s",
                 (render_ctx && render_ctx->dynamic_suffix) ? render_ctx->dynamic_suffix : "off");
    return buf;
}

static const char*
empty_dynamic_label(const void* ctx, char* buf, size_t buf_len) {
    (void)ctx;
    if (buf_len > 0) {
        buf[0] = '\0';
    }
    return buf;
}

static const NcMenuItem EMPTY_CHILDREN[] = {
    {.id = "hidden0", .label = "Hidden 0", .is_enabled = enabled_bit0},
    {.id = "hidden1", .label = "Hidden 1", .is_enabled = enabled_bit0},
};

static const NcMenuItem MIXED_CHILDREN[] = {
    {.id = "hidden", .label = "Hidden", .is_enabled = enabled_bit0},
    {.id = "visible", .label = "Visible child", .is_enabled = enabled_bit1},
};

static const NcMenuItem ITEMS[] = {
    {.id = "plain-id"},
    {.id = "off0", .label = "Off 0", .is_enabled = enabled_bit0},
    {.id = "submenu-empty",
     .label = "Empty submenu",
     .submenu = EMPTY_CHILDREN,
     .submenu_len = sizeof EMPTY_CHILDREN / sizeof EMPTY_CHILDREN[0]},
    {.id = "submenu-visible",
     .label = "Shown submenu",
     .submenu = MIXED_CHILDREN,
     .submenu_len = sizeof MIXED_CHILDREN / sizeof MIXED_CHILDREN[0]},
    {.id = "dynamic", .label = "Fallback dynamic", .label_fn = dynamic_label},
    {.id = "empty-dynamic", .label = "Fallback", .label_fn = empty_dynamic_label},
};

static void
test_visibility_helpers(void) {
    RenderCtx none = {0, "on"};
    RenderCtx child_visible = {0x02, "on"};

    assert(ui_is_enabled(NULL, &none) == 0);
    assert(ui_is_enabled(&ITEMS[0], &none) == 1);
    assert(ui_is_enabled(&ITEMS[1], &none) == 0);
    assert(ui_is_enabled(&ITEMS[2], &none) == 0);
    assert(ui_submenu_has_visible(EMPTY_CHILDREN, sizeof EMPTY_CHILDREN / sizeof EMPTY_CHILDREN[0], &none) == 0);

    assert(ui_is_enabled(&ITEMS[3], &child_visible) == 1);
    assert(ui_submenu_has_visible(MIXED_CHILDREN, sizeof MIXED_CHILDREN / sizeof MIXED_CHILDREN[0], &child_visible)
           == 1);
    assert(ui_submenu_has_visible(NULL, 2, &child_visible) == 0);
    assert(ui_submenu_has_visible(MIXED_CHILDREN, 0, &child_visible) == 0);
}

static void
test_enabled_navigation_and_visible_index(void) {
    RenderCtx ctx = {0x02, "wide"};
    size_t count = sizeof ITEMS / sizeof ITEMS[0];

    assert(ui_next_enabled(NULL, count, &ctx, 0, 1) == 0);
    assert(ui_next_enabled(ITEMS, 0, &ctx, 0, 1) == 0);
    assert(ui_next_enabled(ITEMS, count, &ctx, 0, 1) == 3);
    assert(ui_next_enabled(ITEMS, count, &ctx, 3, 1) == 4);
    assert(ui_next_enabled(ITEMS, count, &ctx, 0, -1) == 5);

    RenderCtx none = {0, "wide"};
    assert(ui_next_enabled(&ITEMS[1], 1, &none, 0, 1) == 0);

    assert(ui_visible_index_for_item(NULL, count, &ctx, 0) == 0);
    assert(ui_visible_index_for_item(ITEMS, count, &ctx, -1) == 0);
    assert(ui_visible_index_for_item(ITEMS, count, &ctx, (int)count) == 0);
    assert(ui_visible_index_for_item(ITEMS, count, &ctx, 0) == 0);
    assert(ui_visible_index_for_item(ITEMS, count, &ctx, 3) == 1);
    assert(ui_visible_index_for_item(ITEMS, count, &ctx, 4) == 2);
    assert(ui_visible_index_for_item(ITEMS, count, &ctx, 1) == 0);
}

static void
test_count_and_labels(void) {
    RenderCtx ctx = {0x02, "wide"};
    int maxlab = -1;
    int visible = ui_visible_count_and_maxlab(ITEMS, sizeof ITEMS / sizeof ITEMS[0], &ctx, &maxlab);
    assert(visible == 4);
    assert(maxlab == (int)strlen("Shown submenu >"));

    assert(ui_visible_count_and_maxlab(ITEMS, sizeof ITEMS / sizeof ITEMS[0], &ctx, NULL) == 4);

    char label[64];
    const char* got = ui_menu_item_label_for_test(NULL, &ctx, label, sizeof label);
    assert(strcmp(got, "") == 0);

    got = ui_menu_item_label_for_test(&ITEMS[0], &ctx, label, sizeof label);
    assert(strcmp(got, "plain-id") == 0);

    got = ui_menu_item_label_for_test(&ITEMS[4], &ctx, label, sizeof label);
    assert(got == label);
    assert(strcmp(got, "Dynamic-wide") == 0);

    got = ui_menu_item_label_for_test(&ITEMS[5], &ctx, label, sizeof label);
    assert(strcmp(got, "Fallback") == 0);

    got = ui_menu_item_label_for_test(&ITEMS[3], &ctx, label, sizeof label);
    assert(got == label);
    assert(strcmp(got, "Shown submenu >") == 0);

    char tiny[3];
    got = ui_menu_item_label_for_test(&ITEMS[3], &ctx, tiny, sizeof tiny);
    assert(got == tiny);
    assert(strcmp(tiny, "Sh") == 0);
}

static void
test_overlay_size_helpers(void) {
    UiMenuFrame frame = {0};
    frame.title = "Long overlay title";

    assert(ui_overlay_cap_then_floor_for_test(90, 78, 10) == 78);
    assert(ui_overlay_cap_then_floor_for_test(4, 78, 10) == 10);
    assert(ui_overlay_cap_then_floor_for_test(42, 78, 10) == 42);

    assert(ui_overlay_center_axis_for_test(80, 20) == 30);
    assert(ui_overlay_center_axis_for_test(20, 80) == 0);

    assert(ui_overlay_compute_height_for_test(0) == 9);
    assert(ui_overlay_compute_height_for_test(2) == 9);
    assert(ui_overlay_compute_height_for_test(8) == 15);

    assert(ui_overlay_compute_width_for_test(NULL, 0) == 50);
    assert(ui_overlay_compute_width_for_test(&frame, 8) == 50);
    frame.title = "A title that is deliberately longer than the footer helper text";
    assert(ui_overlay_compute_width_for_test(&frame, 8) == (int)strlen(frame.title) + 4);
    frame.title = "";
    assert(ui_overlay_compute_width_for_test(&frame, 80) == 84);
}

static void
test_overlay_layout_helper(void) {
    RenderCtx ctx = {0x02, "wide"};
    UiMenuFrame frame = {0};
    frame.items = ITEMS;
    frame.n = sizeof ITEMS / sizeof ITEMS[0];
    frame.title = "Menu";

    ui_overlay_layout_for_test(NULL, &ctx, 40, 30);

    ui_overlay_layout_for_test(&frame, &ctx, 40, 30);
    assert(frame.h == 11);
    assert(frame.w == 28);
    assert(frame.y == 14);
    assert(frame.x == 1);

    frame.items = NULL;
    frame.h = 3;
    frame.w = 4;
    frame.y = 5;
    frame.x = 6;
    ui_overlay_layout_for_test(&frame, &ctx, 40, 30);
    assert(frame.h == 3);
    assert(frame.w == 4);
    assert(frame.y == 5);
    assert(frame.x == 6);

    frame.items = ITEMS;
    frame.n = sizeof ITEMS / sizeof ITEMS[0];
    ui_overlay_layout_for_test(&frame, &ctx, 7, 9);
    assert(frame.h == 6);
    assert(frame.w == 10);
    assert(frame.y == 0);
    assert(frame.x == 0);
}

static void
test_render_window_guard_paths(void) {
    int top = 7;
    ui_draw_menu(NULL, ITEMS, sizeof ITEMS / sizeof ITEMS[0], 3, &top, "Ignored", NULL);
    assert(top == 7);

    ui_overlay_ensure_window(NULL);

    UiMenuFrame frame = {0};
    frame.h = 12;
    frame.w = 34;
    frame.y = 5;
    frame.x = 6;
    g_make_window_calls = 0;
    ui_overlay_ensure_window(&frame);
    assert(frame.win == NULL);
    assert(g_make_window_calls == 1);
    assert(g_make_window_h == 12);
    assert(g_make_window_w == 34);
    assert(g_make_window_y == 5);
    assert(g_make_window_x == 6);

    ui_overlay_recreate_if_needed(NULL);
    ui_overlay_recreate_if_needed(&frame);
    assert(frame.win == NULL);
}

static int
start_curses_render_test(void) {
#ifdef _WIN32
    return 0;
#else
    const char* term = getenv("TERM");
    if (term == NULL || term[0] == '\0') {
        (void)setenv("TERM", "xterm-256color", 1);
    }
    (void)setenv("LINES", "24", 1);
    (void)setenv("COLUMNS", "80", 1);
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
#endif
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

static void
read_window_line(WINDOW* win, int y, char* out, size_t out_size) {
    assert(out_size > 0);
    out[0] = '\0';
    assert(mvwinnstr(win, y, 0, out, (int)out_size - 1) != ERR);
    out[out_size - 1] = '\0';
}

static void
test_draw_menu_scrolls_and_renders_enabled_items(void) {
    if (!start_curses_render_test()) {
        stop_curses_render_test();
        printf("UI_MENU_RENDER_HELPERS: curses draw test skipped\n");
        return;
    }

    RenderCtx ctx = {0x02, "wide"};
    WINDOW* win = newwin(9, 52, 0, 0);
    assert(win != NULL);

    int top = 0;
    g_status_peek_calls = 0;
    g_status_clear_calls = 0;
    g_status_peek_result = 1;
    DSD_SNPRINTF(g_status_text, sizeof g_status_text, "ready");

    ui_draw_menu(win, ITEMS, sizeof ITEMS / sizeof ITEMS[0], 4, &top, "Menu Title", &ctx);
    assert(top == 1);
    assert(g_status_peek_calls == 1);
    assert(g_status_clear_calls == 0);

    char line[96];
    read_window_line(win, 1, line, sizeof line);
    assert(strstr(line, "Menu Title") != NULL);

    read_window_line(win, 2, line, sizeof line);
    assert(strstr(line, "Shown submenu >") != NULL);

    read_window_line(win, 3, line, sizeof line);
    assert(strstr(line, "Dynamic-wide") != NULL);

    read_window_line(win, 4, line, sizeof line);
    assert(strstr(line, "Fallback") == NULL);

    read_window_line(win, 5, line, sizeof line);
    assert(strstr(line, "Arrows/PgUp/PgDn/Home/End  (3/4)") != NULL);

    read_window_line(win, 7, line, sizeof line);
    assert(strstr(line, "Status: ready") != NULL);

    g_status_peek_result = 0;
    ui_draw_menu(win, ITEMS, sizeof ITEMS / sizeof ITEMS[0], 0, NULL, NULL, &ctx);
    assert(g_status_clear_calls == 1);

    UiMenuFrame frame = {0};
    frame.items = ITEMS;
    frame.n = sizeof ITEMS / sizeof ITEMS[0];
    frame.title = "Live Layout";
    ui_overlay_layout(&frame, &ctx);
    assert(frame.h == 11);
    assert(frame.w == 50);
    assert(frame.y == 6);
    assert(frame.x == 15);

    delwin(win);
    stop_curses_render_test();
}

int
main(void) {
    test_visibility_helpers();
    test_enabled_navigation_and_visible_index();
    test_count_and_labels();
    test_overlay_size_helpers();
    test_overlay_layout_helper();
    test_render_window_guard_paths();
    test_draw_menu_scrolls_and_renders_enabled_items();
    printf("UI_MENU_RENDER_HELPERS: OK\n");
    return 0;
}
