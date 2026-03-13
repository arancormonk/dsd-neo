// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Regression coverage for overflowed menu paging:
 *  - PgDn should advance the viewport by a full page step
 *  - PgUp should restore the prior page
 *  - Home/End should anchor to the first/last page
 */

#include <assert.h>
#include <curses.h>
#include <dsd-neo/ui/menu_core.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"
#include "menu_internal.h"

volatile uint8_t exitflag = 0;

static int g_draw_calls = 0;
static int g_last_hi = -1;
static int g_last_top = -1;

static const NcMenuItem ROOT_ITEMS[] = {
    {.id = "root0", .label = "Root 0"}, {.id = "root1", .label = "Root 1"}, {.id = "root2", .label = "Root 2"},
    {.id = "root3", .label = "Root 3"}, {.id = "root4", .label = "Root 4"}, {.id = "root5", .label = "Root 5"},
    {.id = "root6", .label = "Root 6"}, {.id = "root7", .label = "Root 7"},
};

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
    (void)ctx;
    return (items && n > 0) ? 1 : 0;
}

int
ui_is_enabled(const NcMenuItem* it, void* ctx) {
    (void)ctx;
    return it ? 1 : 0;
}

int
ui_next_enabled(const NcMenuItem* items, size_t n, void* ctx, int from, int dir) {
    (void)ctx;
    if (!items || n == 0) {
        return 0;
    }
    int idx = from + ((dir > 0) ? 1 : -1);
    if (idx < 0) {
        idx = 0;
    }
    if (idx >= (int)n) {
        idx = (int)n - 1;
    }
    return idx;
}

int
ui_visible_index_for_item(const NcMenuItem* items, size_t n, void* ctx, int idx) {
    (void)ctx;
    if (!items || n == 0 || idx < 0) {
        return 0;
    }
    if (idx >= (int)n) {
        idx = (int)n - 1;
    }
    return idx;
}

int
ui_visible_count_and_maxlab(const NcMenuItem* items, size_t n, void* ctx, int* out_maxlab) {
    (void)ctx;
    int maxlab = 0;
    for (size_t i = 0; i < n; i++) {
        int len = (int)strlen(items[i].label ? items[i].label : items[i].id);
        if (len > maxlab) {
            maxlab = len;
        }
    }
    if (out_maxlab) {
        *out_maxlab = maxlab;
    }
    return (int)n;
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
    assert(f != NULL);
    f->win = (WINDOW*)(uintptr_t)0x1;
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
    (void)title;
    (void)ctx;
    assert(top_io != NULL);
    g_draw_calls++;
    g_last_hi = hi;
    g_last_top = *top_io;
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

    ui_menu_open_async(opts, state);
    assert(ui_menu_is_open() == 1);

    ui_menu_tick(opts, state);
    assert(g_draw_calls == 1);
    assert(g_last_hi == 0);
    assert(g_last_top == 0);

    assert(ui_menu_handle_key(KEY_NPAGE, opts, state) == 1);
    ui_menu_tick(opts, state);
    assert(g_last_hi == 3);
    assert(g_last_top == 3);

    assert(ui_menu_handle_key(KEY_PPAGE, opts, state) == 1);
    ui_menu_tick(opts, state);
    assert(g_last_hi == 0);
    assert(g_last_top == 0);

    assert(ui_menu_handle_key(KEY_END, opts, state) == 1);
    ui_menu_tick(opts, state);
    assert(g_last_hi == 7);
    assert(g_last_top == 4);

    assert(ui_menu_handle_key(KEY_HOME, opts, state) == 1);
    ui_menu_tick(opts, state);
    assert(g_last_hi == 0);
    assert(g_last_top == 0);

    printf("UI_MENU_PAGING_VIEWPORT: OK\n");
    return 0;
}
