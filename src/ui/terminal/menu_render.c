// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Menu rendering and layout functions for menu subsystem.
 *
 * This file provides drawing, layout, and visibility helper functions
 * that are shared across the menu subsystem.
 */

#include <curses.h>
#include <dsd-neo/ui/ui_prims.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "dsd-neo/ui/menu_core.h"
#include "menu_internal.h"

// ---- Visibility helpers ----

int
ui_is_enabled(const NcMenuItem* it, void* ctx) {
    if (!it) {
        return 0;
    }
    if (it->is_enabled) {
        return it->is_enabled(ctx) ? 1 : 0;
    }
    // If no explicit predicate, but this item has a submenu, hide it when the submenu is empty
    if (it->submenu && it->submenu_len > 0) {
        return ui_submenu_has_visible(it->submenu, it->submenu_len, ctx);
    }
    return 1;
}

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
ui_next_enabled(const NcMenuItem* items, size_t n, void* ctx, int from, int dir) {
    if (!items || n == 0) {
        return 0;
    }
    int idx = from;
    for (size_t k = 0; k < n; k++) {
        idx = (idx + ((dir > 0) ? 1 : -1) + (int)n) % (int)n;
        if (ui_is_enabled(&items[idx], ctx)) {
            return idx;
        }
    }
    return from;
}

// ---- Render helpers ----

void
ui_draw_menu(WINDOW* menu_win, const NcMenuItem* items, size_t n, int hi, const char* title, void* ctx) {
    int x = 2;
    werase(menu_win);
    box(menu_win, 0, 0);
    int mh = 0, mw = 0;
    getmaxyx(menu_win, mh, mw);
    int items_top = 2;     // row 1 is reserved for breadcrumb/title
    int spacer_y = mh - 5; // blank line above footer
    int items_bottom = spacer_y - 1;
    int items_rows = items_bottom - items_top + 1;
    if (items_rows < 1) {
        items_rows = 1;
    }
    int items_last_row = items_top + items_rows - 1;
    int footer_min_y = items_last_row + 1;

    // Top-line breadcrumb/title for context in nested menus.
    if (title && *title) {
        mvwhline(menu_win, 1, 1, ' ', mw - 2);
        mvwaddnstr(menu_win, 1, x, title, (mw > 4) ? (mw - 4) : 1);
    }

    int vis_total = 0;
    int hi_pos = 0;
    for (size_t i = 0; i < n; i++) {
        if (!ui_is_enabled(&items[i], ctx)) {
            continue;
        }
        if ((int)i == hi) {
            hi_pos = vis_total;
        }
        vis_total++;
    }

    int top = 0;
    if (vis_total > items_rows) {
        top = hi_pos - (items_rows / 2);
        if (top < 0) {
            top = 0;
        }
        if (top > vis_total - items_rows) {
            top = vis_total - items_rows;
        }
    }

    int vis_pos = 0;
    int drawn = 0;
    for (size_t i = 0; i < n; i++) {
        if (!ui_is_enabled(&items[i], ctx)) {
            continue;
        }
        if (vis_pos < top) {
            vis_pos++;
            continue;
        }
        if (drawn >= items_rows) {
            break;
        }
        int y = items_top + drawn;
        mvwhline(menu_win, y, 1, ' ', mw - 2);
        if ((int)i == hi) {
            wattron(menu_win, A_REVERSE);
        }
        char dyn[128];
        const char* lab = items[i].label ? items[i].label : items[i].id;
        if (items[i].label_fn) {
            const char* got = items[i].label_fn(ctx, dyn, sizeof dyn);
            if (got && *got) {
                lab = got;
            }
        }
        mvwaddnstr(menu_win, y, x, lab, (mw > 4) ? (mw - 4) : 1);
        wattroff(menu_win, A_REVERSE);
        vis_pos++;
        drawn++;
    }
    // Clear remaining rows in item viewport when visible item count shrinks.
    while (drawn < items_rows) {
        mvwhline(menu_win, items_top + drawn, 1, ' ', mw - 2);
        drawn++;
    }

    // Ensure a blank spacer line above footer, but never erase visible item rows.
    if (spacer_y >= footer_min_y && spacer_y <= mh - 2) {
        mvwhline(menu_win, spacer_y, 1, ' ', mw - 2);
    }

    // Footer includes a position indicator so long menus remain navigable.
    char navline[96];
    if (vis_total > 0) {
        snprintf(navline, sizeof navline, "Arrows: move  Enter: select  (%d/%d)", hi_pos + 1, vis_total);
    } else {
        snprintf(navline, sizeof navline, "Arrows: move  Enter: select");
    }
    int nav_y = mh - 4;
    if (nav_y >= footer_min_y && nav_y <= mh - 2) {
        mvwhline(menu_win, nav_y, 1, ' ', mw - 2);
        mvwaddnstr(menu_win, nav_y, x, navline, (mw > 4) ? (mw - 4) : 1);
    }
    int help_y = mh - 3;
    if (help_y >= footer_min_y && help_y <= mh - 2) {
        mvwhline(menu_win, help_y, 1, ' ', mw - 2);
        mvwaddnstr(menu_win, help_y, x, "h: help  Esc/q: back", (mw > 4) ? (mw - 4) : 1);
    }

    // transient status
    time_t now = time(NULL);
    char sline[256];
    if (ui_status_peek(sline, sizeof sline, now)) {
        int status_y = mh - 2;
        if (status_y >= footer_min_y) {
            // clear line then print
            mvwhline(menu_win, status_y, 1, ' ', mw - 2);
            if (x <= mw - 2) {
                char status_line[288];
                snprintf(status_line, sizeof status_line, "Status: %s", sline);
                mvwaddnstr(menu_win, status_y, x, status_line, mw - x - 1);
            }
        }
    } else {
        ui_status_clear_if_expired(now);
    }
    wnoutrefresh(menu_win);
}

int
ui_visible_count_and_maxlab(const NcMenuItem* items, size_t n, void* ctx, int* out_maxlab) {
    int vis = 0;
    int maxlab = 0;
    for (size_t i = 0; i < n; i++) {
        if (!ui_is_enabled(&items[i], ctx)) {
            continue;
        }
        const char* lab = items[i].label ? items[i].label : items[i].id;
        char dyn[128];
        if (items[i].label_fn) {
            const char* got = items[i].label_fn(ctx, dyn, sizeof dyn);
            if (got && *got) {
                lab = got;
            }
        }
        int L = (int)strlen(lab);
        if (L > maxlab) {
            maxlab = L;
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
    if (!f || !f->items || f->n == 0) {
        return;
    }
    const char* f1 = "Arrows: move  Enter: select";
    const char* f2 = "h: help  Esc/q: back";
    int pad_x = 2;
    int maxlab = 0;
    int vis = ui_visible_count_and_maxlab(f->items, f->n, ctx, &maxlab);
    int width = pad_x + ((maxlab > 0) ? maxlab : 1);
    int f1w = pad_x + (int)strlen(f1);
    int f2w = pad_x + (int)strlen(f2);
    if (f1w > width) {
        width = f1w;
    }
    if (f2w > width) {
        width = f2w;
    }
    if (f->title && *f->title) {
        int tw = pad_x + (int)strlen(f->title);
        if (tw > width) {
            width = tw;
        }
    }
    width += 2; // borders
    int height = vis + 7;
    if (height < 9) {
        height = 9;
    }
    int term_h = 24, term_w = 80;
    getmaxyx(stdscr, term_h, term_w);
    if (width > term_w - 2) {
        width = term_w - 2;
        if (width < 10) {
            width = 10;
        }
    }
    if (height > term_h - 2) {
        height = term_h - 2;
        if (height < 6) {
            height = 6;
        }
    }
    int my = (term_h - height) / 2;
    int mx = (term_w - width) / 2;
    if (my < 0) {
        my = 0;
    }
    if (mx < 0) {
        mx = 0;
    }
    f->h = height;
    f->w = width;
    f->y = my;
    f->x = mx;
}

void
ui_overlay_ensure_window(UiMenuFrame* f) {
    if (!f) {
        return;
    }
    if (!f->win) {
        f->win = ui_make_window(f->h, f->w, f->y, f->x);
        if (!f->win) {
            return;
        }
        keypad(f->win, TRUE);
        wtimeout(f->win, 0);
    }
}

void
ui_overlay_recreate_if_needed(UiMenuFrame* f) {
    if (!f || !f->win) {
        return;
    }
    int cur_h = 0, cur_w = 0;
    int cur_y = 0, cur_x = 0;
    getmaxyx(f->win, cur_h, cur_w);
    getbegyx(f->win, cur_y, cur_x);
    if (cur_h != f->h || cur_w != f->w || cur_y != f->y || cur_x != f->x) {
        delwin(f->win);
        f->win = NULL;
    }
}
