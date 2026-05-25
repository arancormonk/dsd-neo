// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Core menu overlay driver and public API.
 *
 * This file contains only the overlay state management and the main event loop.
 * All callbacks, actions, labels, and menu items have been extracted to:
 *   - menu_callbacks.c / menu_callbacks.h
 *   - menu_actions.c / menu_actions.h
 *   - menu_labels.c / menu_labels.h
 *   - menu_items.c / menu_items.h
 *   - menu_prompts.c / menu_prompts.h
 *   - menu_render.c
 *   - menu_env.c / menu_env.h
 */

#include <curses.h>
#include <dsd-neo/platform/curses_compat.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/exitflag.h>
#include <dsd-neo/ui/keymap.h>
#include <dsd-neo/ui/menu_core.h>
#include <dsd-neo/ui/menu_defs.h>
#include <dsd-neo/ui/ui_prims.h>
#include <stdio.h>
#include <sys/stat.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "menu_internal.h"
#include "menu_prompts.h"

static int g_overlay_open = 0;
static UiMenuFrame g_stack[8];
static int g_depth = 0;

static int
ui_frame_items_rows(const UiMenuFrame* f) {
    int rows = 1;
    if (f && f->h > 7) {
        rows = f->h - 7;
    }
    if (rows < 1) {
        rows = 1;
    }
    return rows;
}

static int
ui_first_enabled_idx(const NcMenuItem* items, size_t n, const UiCtx* ctx) {
    if (!items || n == 0) {
        return 0;
    }
    for (size_t i = 0; i < n; i++) {
        if (ui_is_enabled(&items[i], ctx)) {
            return (int)i;
        }
    }
    return 0;
}

static int
ui_last_enabled_idx(const NcMenuItem* items, size_t n, const UiCtx* ctx) {
    if (!items || n == 0) {
        return 0;
    }
    for (size_t i = n; i > 0; i--) {
        if (ui_is_enabled(&items[i - 1], ctx)) {
            return (int)(i - 1);
        }
    }
    return 0;
}

static int
ui_step_enabled(const NcMenuItem* items, size_t n, const UiCtx* ctx, int from, int dir, int steps) {
    if (!items || n == 0) {
        return 0;
    }
    if (steps < 1) {
        steps = 1;
    }
    const int delta = (dir < 0) ? -1 : 1;
    int idx = from;
    for (int i = 0; i < steps; i++) {
        int next = idx;
        while (1) {
            next += delta;
            if (next < 0 || next >= (int)n) {
                next = idx;
                break;
            }
            if (ui_is_enabled(&items[next], ctx)) {
                break;
            }
        }
        if (next == idx) {
            break;
        }
        idx = next;
    }
    return idx;
}

static void
ui_frame_keep_highlight_visible(UiMenuFrame* f, const UiCtx* ctx) {
    if (!f || !f->items || f->n == 0) {
        return;
    }
    int page_rows = ui_frame_items_rows(f);
    int vis_total = ui_visible_count_and_maxlab(f->items, f->n, ctx, NULL);
    int hi_pos = ui_visible_index_for_item(f->items, f->n, ctx, f->hi);
    f->top = ui_scroll_follow_selection(vis_total, page_rows, f->top, hi_pos);
}

static void
ui_overlay_breadcrumb(char* buf, size_t n) {
    if (!buf || n == 0) {
        return;
    }
    buf[0] = '\0';
    size_t off = 0;
    for (int i = 0; i < g_depth; i++) {
        const char* t = g_stack[i].title;
        if (!t || !*t) {
            continue;
        }
        int wrote = DSD_SNPRINTF(buf + off, n - off, "%s%s", (off > 0) ? " > " : "", t);
        if (wrote < 0) {
            break;
        }
        size_t w = (size_t)wrote;
        if (w >= (n - off)) {
            buf[n - 1] = '\0';
            break;
        }
        off += w;
    }
}

static void
ui_overlay_close_all(void) {
    for (int i = 0; i < g_depth; i++) {
        if (g_stack[i].win) {
            delwin(g_stack[i].win);
            g_stack[i].win = NULL;
        }
    }
    g_depth = 0;
    g_overlay_open = 0;
}

static void
ui_overlay_pop_one(void) {
    if (g_depth <= 0) {
        return;
    }
    UiMenuFrame* cur = &g_stack[g_depth - 1];
    if (cur->win) {
        delwin(cur->win);
        cur->win = NULL;
    }
    g_depth--;
    if (g_depth <= 0) {
        g_depth = 0;
        g_overlay_open = 0;
    }
}

static int
ui_menu_modal_handle_key(int ch) {
    if (ui_prompt_active()) {
        return ui_prompt_handle_key(ch);
    }
    if (ui_help_active()) {
        return ui_help_handle_key(ch);
    }
    if (ui_chooser_active()) {
        return ui_chooser_handle_key(ch);
    }
    return -1;
}

static void
ui_menu_push_submenu(const NcMenuItem* it, const UiCtx* ctx) {
    if (!it || !it->submenu || it->submenu_len == 0) {
        return;
    }
    if (g_depth >= (int)(sizeof g_stack / sizeof g_stack[0])) {
        return;
    }
    UiMenuFrame* nf = &g_stack[g_depth++];
    DSD_MEMSET(nf, 0, sizeof(*nf));
    nf->items = it->submenu;
    nf->n = it->submenu_len;
    nf->hi = ui_first_enabled_idx(nf->items, nf->n, ctx);
    nf->top = 0;
    nf->title = it->label ? it->label : it->id;
    ui_overlay_layout(nf, ctx);
}

static void
ui_menu_refresh_current_frame(const UiCtx* ctx) {
    if (!g_overlay_open || g_depth <= 0) {
        return;
    }
    UiMenuFrame* cf = &g_stack[g_depth - 1];
    if (!cf->items || cf->n == 0) {
        return;
    }
    if (!ui_is_enabled(&cf->items[cf->hi], ctx)) {
        cf->hi = ui_next_enabled(cf->items, cf->n, ctx, cf->hi, +1);
    }
    ui_overlay_layout(cf, ctx);
    ui_frame_keep_highlight_visible(cf, ctx);
    ui_overlay_recreate_if_needed(cf);
}

static void
ui_menu_open_help_if_leaf(const NcMenuItem* it) {
    if (!it || it->on_select) {
        return;
    }
    if ((it->submenu && it->submenu_len > 0) || !it->help || !*it->help) {
        return;
    }
    ui_help_open(it->help);
}

static int
ui_menu_activate_current(UiCtx* ctx) {
    if (!g_overlay_open || g_depth <= 0) {
        return 0;
    }
    UiMenuFrame* f = &g_stack[g_depth - 1];
    const NcMenuItem* it = &f->items[f->hi];
    if (!ui_is_enabled(it, ctx)) {
        return 1;
    }
    ui_menu_push_submenu(it, ctx);
    if (it->on_select) {
        it->on_select(ctx);
        if (exitflag) {
            ui_overlay_close_all();
            return 1;
        }
        ui_menu_refresh_current_frame(ctx);
    }
    ui_menu_open_help_if_leaf(it);
    return 1;
}

static int
ui_menu_handle_resize(UiMenuFrame* f, const UiCtx* ctx, int ch) {
    if (ch != KEY_RESIZE) {
        return 0;
    }
#if DSD_CURSES_NEEDS_EXPLICIT_RESIZE
    // PDCurses doesn't auto-update dimensions on resize;
    // resize_term(0,0) queries actual console size.
    resize_term(0, 0);
#endif
    // Recompute layout and recreate window on next tick
    if (f->win) {
        delwin(f->win);
        f->win = NULL;
    }
    ui_overlay_layout(f, ctx);
    ui_frame_keep_highlight_visible(f, ctx);
    return 1;
}

static int
ui_menu_handle_arrow_keys(UiMenuFrame* f, const UiCtx* ctx, int ch) {
    if (ch == KEY_UP) {
        f->hi = ui_next_enabled(f->items, f->n, ctx, f->hi, -1);
        ui_frame_keep_highlight_visible(f, ctx);
        return 1;
    }
    if (ch == KEY_DOWN) {
        f->hi = ui_next_enabled(f->items, f->n, ctx, f->hi, +1);
        ui_frame_keep_highlight_visible(f, ctx);
        return 1;
    }
    return 0;
}

static int
ui_menu_handle_edge_keys(UiMenuFrame* f, const UiCtx* ctx, int ch) {
    if (ch == KEY_HOME) {
        f->hi = ui_first_enabled_idx(f->items, f->n, ctx);
        f->top = 0;
        return 1;
    }
    if (ch == KEY_END) {
        f->hi = ui_last_enabled_idx(f->items, f->n, ctx);
        f->top =
            ui_scroll_last_page_top(ui_visible_count_and_maxlab(f->items, f->n, ctx, NULL), ui_frame_items_rows(f));
        return 1;
    }
    return 0;
}

static int
ui_menu_handle_page_keys(UiMenuFrame* f, const UiCtx* ctx, int ch) {
    int dir = 0;
    if (ch == KEY_PPAGE) {
        dir = -1;
    } else if (ch == KEY_NPAGE) {
        dir = +1;
    } else {
        return 0;
    }
    int page = ui_scroll_page_step_from_rows(ui_frame_items_rows(f));
    f->hi = ui_step_enabled(f->items, f->n, ctx, f->hi, dir, page);
    f->top += (dir < 0) ? -page : page;
    ui_frame_keep_highlight_visible(f, ctx);
    return 1;
}

static int
ui_menu_handle_help_key(UiMenuFrame* f, const UiCtx* ctx, int ch) {
    if (ch != 'h' && ch != 'H') {
        return 0;
    }
    const NcMenuItem* it = &f->items[f->hi];
    if (ui_is_enabled(it, ctx) && it->help && *it->help) {
        ui_help_open(it->help);
    }
    return 1;
}

static int
ui_menu_handle_back_key(int ch) {
    if (ch != KEY_LEFT && ch != DSD_KEY_ESC && ch != 'q' && ch != 'Q') {
        return 0;
    }
    if (g_depth > 1) {
        ui_overlay_pop_one();
    } else {
        ui_overlay_close_all();
    }
    return 1;
}

static int
ui_menu_is_activate_key(int ch) {
    return ch == KEY_RIGHT || ch == 10 || ch == KEY_ENTER || ch == '\r';
}

void
ui_menu_open_async(dsd_opts* opts, dsd_state* state) {
    UiCtx ctx = {opts, state};
    const NcMenuItem* items = NULL;
    size_t n = 0;
    ui_menu_get_main_items(&items, &n, &ctx);
    if (!items || n == 0) {
        return;
    }
    // If no default config exists yet, provide a small hint so users starting
    // from CLI arguments discover the Config menu for saving defaults.
    const char* cfg_path = dsd_user_config_default_path();
    if (cfg_path && *cfg_path) {
        struct stat st;
        if (stat(cfg_path, &st) != 0) {
            ui_statusf("No default config; use Config menu to save to %s", cfg_path);
        }
    }
    g_overlay_open = 1;
    g_depth = 1;
    DSD_MEMSET(g_stack, 0, sizeof(g_stack));
    g_stack[0].items = items;
    g_stack[0].n = n;
    g_stack[0].hi = ui_first_enabled_idx(items, n, &ctx);
    g_stack[0].top = 0;
    g_stack[0].title = "Main Menu";
    ui_overlay_layout(&g_stack[0], &ctx);
}

int
ui_menu_is_open(void) {
    return g_overlay_open;
}

int
ui_menu_handle_key(int ch, dsd_opts* opts, dsd_state* state) {
    UiCtx ctx = {opts, state};
    if (!g_overlay_open || g_depth <= 0) {
        return 0;
    }
    int modal_rc = ui_menu_modal_handle_key(ch);
    if (modal_rc >= 0) {
        return modal_rc;
    }
    UiMenuFrame* f = &g_stack[g_depth - 1];
    if (!f->items || f->n == 0) {
        ui_overlay_close_all();
        return 1;
    }
    if (ui_menu_handle_resize(f, &ctx, ch)) {
        return 1;
    }
    if (ch == ERR) {
        return 0;
    }
    if (ui_menu_handle_arrow_keys(f, &ctx, ch)) {
        return 1;
    }
    if (ui_menu_handle_edge_keys(f, &ctx, ch)) {
        return 1;
    }
    if (ui_menu_handle_page_keys(f, &ctx, ch)) {
        return 1;
    }
    if (ui_menu_handle_help_key(f, &ctx, ch)) {
        return 1;
    }
    if (ui_menu_handle_back_key(ch)) {
        return 1;
    }
    if (ui_menu_is_activate_key(ch)) {
        return ui_menu_activate_current(&ctx);
    }
    return 0;
}

void
ui_menu_tick(dsd_opts* opts, dsd_state* state) {
    UiCtx ctx = {opts, state};
    if (!g_overlay_open || g_depth <= 0) {
        return;
    }
    // Render Prompt overlay (highest priority) - delegate to menu_prompts.c
    if (ui_prompt_active()) {
        ui_prompt_render();
        return;
    }
    // Render Help overlay if active - delegate to menu_prompts.c
    if (ui_help_active()) {
        ui_help_render();
        return;
    }
    // Render chooser if active - delegate to menu_prompts.c
    if (ui_chooser_active()) {
        ui_chooser_render();
        return;
    }
    UiMenuFrame* f = &g_stack[g_depth - 1];
    if (f->items && f->n > 0 && !ui_is_enabled(&f->items[f->hi], &ctx)) {
        f->hi = ui_next_enabled(f->items, f->n, &ctx, f->hi, +1);
    }
    // Ensure window exists with up-to-date geometry
    ui_overlay_layout(f, &ctx);
    ui_frame_keep_highlight_visible(f, &ctx);
    ui_overlay_recreate_if_needed(f);
    ui_overlay_ensure_window(f);
    if (!f->win) {
        return;
    }
    char breadcrumb[256];
    ui_overlay_breadcrumb(breadcrumb, sizeof breadcrumb);
    ui_draw_menu(f->win, f->items, f->n, f->hi, &f->top, breadcrumb, &ctx);
}
