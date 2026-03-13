// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Regression coverage for ncurses menu navigation:
 *  - leaf items should activate via Right as well as Enter
 *  - Down should skip disabled items
 *  - Home/End/PageUp/PageDown should move predictably across visible items
 *  - Left should back out of submenus and close the root overlay
 */

#include <assert.h>
#include <curses.h>
#include <dsd-neo/ui/menu_core.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"
#include "menu_internal.h"

volatile uint8_t exitflag = 0;

static int g_action_calls = 0;
static const char* g_last_action = NULL;

static void
capture_action(const char* id) {
    g_action_calls++;
    g_last_action = id;
}

static void
act_root0(void* ctx) {
    (void)ctx;
    capture_action("root0");
}

static void
act_root2(void* ctx) {
    (void)ctx;
    capture_action("root2");
}

static void
act_root3(void* ctx) {
    (void)ctx;
    capture_action("root3");
}

static void
act_root4(void* ctx) {
    (void)ctx;
    capture_action("root4");
}

static void
act_root6(void* ctx) {
    (void)ctx;
    capture_action("root6");
}

static void
act_sub0(void* ctx) {
    (void)ctx;
    capture_action("sub0");
}

static void
act_sub1(void* ctx) {
    (void)ctx;
    capture_action("sub1");
}

static bool
item_disabled(void* ctx) {
    (void)ctx;
    return false;
}

static const NcMenuItem SUB_ITEMS[] = {
    {.id = "sub0", .label = "Sub Item", .on_select = act_sub0},
    {.id = "sub1", .label = "Sub Item 2", .on_select = act_sub1},
};

static const NcMenuItem ROOT_ITEMS[] = {
    {.id = "root0", .label = "Root 0", .on_select = act_root0},
    {.id = "root1", .label = "Root 1", .is_enabled = item_disabled, .on_select = act_root0},
    {.id = "root2", .label = "Root 2", .on_select = act_root2},
    {.id = "root3", .label = "Root 3", .on_select = act_root3},
    {.id = "root4", .label = "Root 4", .on_select = act_root4},
    {.id = "root5", .label = "Root 5", .submenu = SUB_ITEMS, .submenu_len = sizeof SUB_ITEMS / sizeof SUB_ITEMS[0]},
    {.id = "root6", .label = "Root 6", .on_select = act_root6},
};

static void
reset_capture(void) {
    g_action_calls = 0;
    g_last_action = NULL;
}

static void
assert_last_action(const char* want) {
    assert(g_action_calls == 1);
    assert(g_last_action != NULL);
    assert(strcmp(g_last_action, want) == 0);
}

void
ui_statusf(const char* fmt, ...) {
    (void)fmt;
}

const char*
dsd_user_config_default_path(void) {
    return "";
}

void
ui_menu_get_main_items(const NcMenuItem** out_items, size_t* out_n, UiCtx* ctx) {
    (void)ctx;
    if (out_items) {
        *out_items = ROOT_ITEMS;
    }
    if (out_n) {
        *out_n = sizeof ROOT_ITEMS / sizeof ROOT_ITEMS[0];
    }
}

int
ui_prompt_active(void) {
    return 0;
}

int
ui_prompt_handle_key(int ch) {
    (void)ch;
    return 0;
}

void
ui_prompt_render(void) {}

int
ui_help_active(void) {
    return 0;
}

int
ui_help_handle_key(int ch) {
    (void)ch;
    return 0;
}

void
ui_help_open(const char* help) {
    (void)help;
}

void
ui_help_render(void) {}

int
ui_chooser_active(void) {
    return 0;
}

int
ui_chooser_handle_key(int ch) {
    (void)ch;
    return 0;
}

void
ui_chooser_render(void) {}

int
ui_submenu_has_visible(const NcMenuItem* items, size_t n, void* ctx) {
    if (!items || n == 0) {
        return 0;
    }
    for (size_t i = 0; i < n; i++) {
        if (ui_is_enabled(&items[i], ctx)) {
            return 1;
        }
    }
    return 0;
}

int
ui_is_enabled(const NcMenuItem* it, void* ctx) {
    if (!it) {
        return 0;
    }
    if (it->is_enabled) {
        return it->is_enabled(ctx) ? 1 : 0;
    }
    if (it->submenu && it->submenu_len > 0) {
        return ui_submenu_has_visible(it->submenu, it->submenu_len, ctx);
    }
    return 1;
}

int
ui_next_enabled(const NcMenuItem* items, size_t n, void* ctx, int from, int dir) {
    if (!items || n == 0) {
        return 0;
    }
    int idx = from;
    for (size_t i = 0; i < n; i++) {
        idx = (idx + ((dir > 0) ? 1 : -1) + (int)n) % (int)n;
        if (ui_is_enabled(&items[idx], ctx)) {
            return idx;
        }
    }
    return from;
}

int
ui_visible_index_for_item(const NcMenuItem* items, size_t n, void* ctx, int idx) {
    if (!items || n == 0 || idx < 0 || idx >= (int)n) {
        return 0;
    }
    int vis_pos = 0;
    for (size_t i = 0; i < n; i++) {
        if (!ui_is_enabled(&items[i], ctx)) {
            continue;
        }
        if ((int)i == idx) {
            return vis_pos;
        }
        vis_pos++;
    }
    return 0;
}

int
ui_visible_count_and_maxlab(const NcMenuItem* items, size_t n, void* ctx, int* out_maxlab) {
    int vis = 0;
    int maxlab = 0;
    for (size_t i = 0; i < n; i++) {
        if (!ui_is_enabled(&items[i], ctx)) {
            continue;
        }
        const char* label = items[i].label ? items[i].label : items[i].id;
        int len = (int)strlen(label);
        if (len > maxlab) {
            maxlab = len;
        }
        vis++;
    }
    if (out_maxlab) {
        *out_maxlab = maxlab;
    }
    return vis;
}

void
ui_overlay_layout(UiMenuFrame* f, void* ctx) {
    (void)ctx;
    assert(f != NULL);
    f->h = 11; // 4 visible rows -> page step of 3
    f->w = 40;
    f->y = 0;
    f->x = 0;
}

void
ui_overlay_ensure_window(UiMenuFrame* f) {
    (void)f;
}

void
ui_overlay_recreate_if_needed(UiMenuFrame* f) {
    (void)f;
}

void
ui_draw_menu(WINDOW* win, const NcMenuItem* items, size_t n, int hi, int* top_io, const char* title, void* ctx) {
    (void)win;
    (void)items;
    (void)n;
    (void)hi;
    (void)top_io;
    (void)title;
    (void)ctx;
}

int
delwin(WINDOW* win) {
    (void)win;
    return OK;
}

int
resize_term(int lines, int columns) {
    (void)lines;
    (void)columns;
    return OK;
}

int
main(void) {
    dsd_opts* opts = (dsd_opts*)(uintptr_t)0x1;
    dsd_state* state = (dsd_state*)(uintptr_t)0x2;

    reset_capture();
    ui_menu_open_async(opts, state);
    assert(ui_menu_is_open() == 1);

    assert(ui_menu_handle_key(KEY_RIGHT, opts, state) == 1);
    assert_last_action("root0");

    reset_capture();
    assert(ui_menu_handle_key(KEY_DOWN, opts, state) == 1);
    assert(ui_menu_handle_key('\r', opts, state) == 1);
    assert_last_action("root2");

    assert(ui_menu_handle_key(KEY_LEFT, opts, state) == 1);
    assert(ui_menu_is_open() == 0);
    ui_menu_open_async(opts, state);
    assert(ui_menu_is_open() == 1);

    reset_capture();
    assert(ui_menu_handle_key(KEY_PPAGE, opts, state) == 1);
    assert(ui_menu_handle_key('\r', opts, state) == 1);
    assert_last_action("root0");

    reset_capture();
    assert(ui_menu_handle_key(KEY_NPAGE, opts, state) == 1);
    assert(ui_menu_handle_key('\r', opts, state) == 1);
    assert_last_action("root4");

    reset_capture();
    assert(ui_menu_handle_key(KEY_HOME, opts, state) == 1);
    assert(ui_menu_handle_key('\r', opts, state) == 1);
    assert_last_action("root0");

    reset_capture();
    assert(ui_menu_handle_key(KEY_END, opts, state) == 1);
    assert(ui_menu_handle_key(KEY_NPAGE, opts, state) == 1);
    assert(ui_menu_handle_key('\r', opts, state) == 1);
    assert_last_action("root6");

    reset_capture();
    assert(ui_menu_handle_key(KEY_END, opts, state) == 1);
    assert(ui_menu_handle_key('\r', opts, state) == 1);
    assert_last_action("root6");

    reset_capture();
    assert(ui_menu_handle_key(KEY_PPAGE, opts, state) == 1);
    assert(ui_menu_handle_key('\r', opts, state) == 1);
    assert_last_action("root3");

    reset_capture();
    assert(ui_menu_handle_key(KEY_DOWN, opts, state) == 1);
    assert(ui_menu_handle_key(KEY_DOWN, opts, state) == 1);
    assert(ui_menu_handle_key(KEY_RIGHT, opts, state) == 1);
    assert(g_action_calls == 0);
    assert(ui_menu_is_open() == 1);

    reset_capture();
    assert(ui_menu_handle_key(KEY_PPAGE, opts, state) == 1);
    assert(ui_menu_handle_key('\r', opts, state) == 1);
    assert_last_action("sub0");

    reset_capture();
    assert(ui_menu_handle_key(KEY_END, opts, state) == 1);
    assert(ui_menu_handle_key(KEY_NPAGE, opts, state) == 1);
    assert(ui_menu_handle_key('\r', opts, state) == 1);
    assert_last_action("sub1");

    reset_capture();
    assert(ui_menu_handle_key(KEY_HOME, opts, state) == 1);
    assert(ui_menu_handle_key('\r', opts, state) == 1);
    assert_last_action("sub0");

    reset_capture();
    assert(ui_menu_handle_key(KEY_LEFT, opts, state) == 1);
    assert(ui_menu_is_open() == 1);
    assert(ui_menu_handle_key(KEY_END, opts, state) == 1);
    assert(ui_menu_handle_key('\r', opts, state) == 1);
    assert_last_action("root6");

    assert(ui_menu_handle_key(KEY_LEFT, opts, state) == 1);
    assert(ui_menu_is_open() == 0);

    printf("UI_MENU_NAVIGATION: OK\n");
    return 0;
}
