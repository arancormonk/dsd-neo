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
#include <string.h>
#include <time.h>
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/ui/menu_core.h"
#include "menu_internal.h"

int
ui_is_enabled(const NcMenuItem* it, const void* ctx) {
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
ui_is_selectable(const NcMenuItem* it, const void* ctx) {
    return (it && it->kind == NC_ITEM_ACTION && ui_is_enabled(it, ctx)) ? 1 : 0;
}

int
ui_submenu_has_visible(const NcMenuItem* items, size_t n, const void* ctx) {
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
ui_next_enabled(const NcMenuItem* items, size_t n, const void* ctx, int from, int dir) {
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

int
ui_next_selectable(const NcMenuItem* items, size_t n, const void* ctx, int from, int dir) {
    if (!items || n == 0) {
        return 0;
    }
    int idx = from;
    for (size_t k = 0; k < n; k++) {
        idx = (idx + ((dir > 0) ? 1 : -1) + (int)n) % (int)n;
        if (ui_is_selectable(&items[idx], ctx)) {
            return idx;
        }
    }
    return from;
}

int
ui_visible_index_for_item(const NcMenuItem* items, size_t n, const void* ctx, int idx) {
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

// ---- Render helpers ----

typedef struct {
    int x;
    int mh;
    int mw;
    int text_w;
    int items_top;
    int spacer_y;
    int items_bottom;
    int items_rows;
    int items_last_row;
    int footer_min_y;
} UiMenuDrawLayout;

static int
ui_menu_text_width(int mw) {
    return (mw > 4) ? (mw - 4) : 1;
}

static void
ui_menu_draw_layout_init(WINDOW* menu_win, UiMenuDrawLayout* layout) {
    if (!menu_win || !layout) {
        return;
    }
    DSD_MEMSET(layout, 0, sizeof(*layout));
    layout->x = 2;
    getmaxyx(menu_win, layout->mh, layout->mw);
    layout->text_w = ui_menu_text_width(layout->mw);
    layout->items_top = 2; // row 1 is reserved for breadcrumb/title
    layout->spacer_y = layout->mh - 5;
    layout->items_bottom = layout->spacer_y - 1;
    layout->items_rows = layout->items_bottom - layout->items_top + 1;
    if (layout->items_rows < 1) {
        layout->items_rows = 1;
    }
    layout->items_last_row = layout->items_top + layout->items_rows - 1;
    layout->footer_min_y = layout->items_last_row + 1;
}

static void
ui_menu_draw_title_line(WINDOW* menu_win, const UiMenuDrawLayout* layout, const char* title) {
    if (!menu_win || !layout || !title || !*title) {
        return;
    }
    mvwhline(menu_win, 1, 1, ' ', layout->mw - 2);
    mvwaddnstr(menu_win, 1, layout->x, title, layout->text_w);
}

static const char*
ui_menu_item_label(const NcMenuItem* it, const void* ctx, char* out, size_t out_size) {
    if (!it) {
        return "";
    }
    const char* lab = it->label ? it->label : it->id;
    if (it->label_fn && out && out_size > 0) {
        const char* got = it->label_fn(ctx, out, out_size);
        if (got && *got) {
            lab = got;
        }
    }
    if (it->submenu && it->submenu_len > 0 && out && out_size > 0) {
        if (lab != out) {
            DSD_SNPRINTF(out, out_size, "%s", lab);
        }
        size_t len = strlen(out);
        if (len + 1U < out_size) {
            out[len++] = ' ';
        }
        if (len + 1U < out_size) {
            out[len++] = '>';
        }
        out[len] = '\0';
        return out;
    }
    return lab;
}

/* Columns the hotkey takes on its row: the key text plus a two-column gap from the label. */
static int
ui_menu_item_hotkey_cols(const NcMenuItem* it) {
    if (!it || !it->hotkey || !*it->hotkey) {
        return 0;
    }
    return (int)strlen(it->hotkey) + 2;
}

static int
ui_menu_item_label_len(const NcMenuItem* it, const void* ctx) {
    if (it && it->kind == NC_ITEM_SEPARATOR) {
        return 0;
    }
    char dyn[128];
    const char* lab = ui_menu_item_label(it, ctx, dyn, sizeof dyn);
    return (int)strlen(lab) + ui_menu_item_hotkey_cols(it);
}

int
ui_menu_format_row(const NcMenuItem* it, const void* ctx, int width, char* out, size_t out_size) {
    if (!out || out_size == 0) {
        return 0;
    }
    out[0] = '\0';
    if (!it || it->kind == NC_ITEM_SEPARATOR || width < 1) {
        return 0;
    }
    char labfmt[140];
    const char* lab = ui_menu_item_label(it, ctx, labfmt, sizeof labfmt);
    const int hotkey_cols = ui_menu_item_hotkey_cols(it);
    if (hotkey_cols == 0) {
        DSD_SNPRINTF(out, out_size, "%.*s", width, lab);
        return (int)strlen(out);
    }
    int label_cols = width - hotkey_cols;
    if (label_cols < 0) {
        label_cols = 0;
    }
    int pad_to = width - (int)strlen(it->hotkey);
    if (pad_to < 0) {
        pad_to = 0;
    }
    DSD_SNPRINTF(out, out_size, "%-*.*s%s", pad_to, label_cols, lab, it->hotkey);
    return (int)strlen(out);
}

#ifdef DSD_NEO_TEST_HOOKS
const char*
ui_menu_item_label_for_test(const NcMenuItem* it, const void* ctx, char* out, size_t out_size) {
    return ui_menu_item_label(it, ctx, out, out_size);
}
#endif

/* One row: a dimmed rule for a separator, dimmed text for a status row, and for
   an action row the formatted label/hotkey line -- reversed across the whole row
   when highlighted, so the hotkey column reads as part of it. */
static void
ui_menu_draw_one_row(WINDOW* menu_win, const UiMenuDrawLayout* layout, const NcMenuItem* it, int y, int is_hi,
                     const void* ctx) {
    mvwhline(menu_win, y, 1, ' ', layout->mw - 2);
    if (it->kind == NC_ITEM_SEPARATOR) {
        wattron(menu_win, A_DIM);
        mvwhline(menu_win, y, layout->x, ACS_HLINE, layout->text_w);
        wattroff(menu_win, A_DIM);
        return;
    }
    char row[160];
    (void)ui_menu_format_row(it, ctx, layout->text_w, row, sizeof row);
    const int attr = is_hi ? A_REVERSE : ((it->kind == NC_ITEM_STATUS) ? A_DIM : A_NORMAL);
    if (attr != A_NORMAL) {
        wattron(menu_win, attr);
    }
    if (is_hi) {
        mvwhline(menu_win, y, layout->x, ' ', layout->text_w);
    }
    mvwaddnstr(menu_win, y, layout->x, row, layout->text_w);
    if (attr != A_NORMAL) {
        wattroff(menu_win, attr);
    }
}

static void
ui_menu_draw_item_rows(WINDOW* menu_win, const UiMenuDrawLayout* layout, const NcMenuItem* items, size_t n, int hi,
                       int top, const void* ctx) {
    if (!menu_win || !layout || !items || n == 0) {
        return;
    }
    int vis_pos = 0;
    int drawn = 0;
    for (size_t i = 0; i < n; i++) {
        if (!ui_is_enabled(&items[i], ctx)) {
            continue;
        }
        if (vis_pos++ < top) {
            continue;
        }
        if (drawn >= layout->items_rows) {
            break;
        }
        const int y = layout->items_top + drawn++;
        const int is_hi = ((int)i == hi) && items[i].kind == NC_ITEM_ACTION;
        ui_menu_draw_one_row(menu_win, layout, &items[i], y, is_hi, ctx);
    }
    while (drawn < layout->items_rows) {
        mvwhline(menu_win, layout->items_top + drawn, 1, ' ', layout->mw - 2);
        drawn++;
    }
}

static void
ui_menu_draw_footer_lines(WINDOW* menu_win, const UiMenuDrawLayout* layout, int hi_pos, int vis_total) {
    if (!menu_win || !layout) {
        return;
    }
    if (layout->spacer_y >= layout->footer_min_y && layout->spacer_y <= layout->mh - 2) {
        mvwhline(menu_win, layout->spacer_y, 1, ' ', layout->mw - 2);
    }
    char navline[96];
    if (vis_total > 0) {
        DSD_SNPRINTF(navline, sizeof navline, "Arrows/PgUp/PgDn/Home/End  (%d/%d)", hi_pos + 1, vis_total);
    } else {
        DSD_SNPRINTF(navline, sizeof navline, "Arrows/PgUp/PgDn/Home/End");
    }
    int nav_y = layout->mh - 4;
    if (nav_y >= layout->footer_min_y && nav_y <= layout->mh - 2) {
        mvwhline(menu_win, nav_y, 1, ' ', layout->mw - 2);
        mvwaddnstr(menu_win, nav_y, layout->x, navline, layout->text_w);
    }
    int help_y = layout->mh - 3;
    if (help_y >= layout->footer_min_y && help_y <= layout->mh - 2) {
        mvwhline(menu_win, help_y, 1, ' ', layout->mw - 2);
        mvwaddnstr(menu_win, help_y, layout->x, "Enter/Right: select  h: help  Esc/Left: back", layout->text_w);
    }
}

/*
 * The status row is the last interior row. A message too long for it takes the
 * key-help row above as well, and the hints come back when the toast expires:
 * the menu's floor width leaves ~39 columns after "Status: ", which cut every
 * two-clause message in half. Anchored to the bottom so a one-row message sits
 * where it always did.
 */
static void
ui_menu_draw_status_line(WINDOW* menu_win, const UiMenuDrawLayout* layout, time_t now) {
    if (!menu_win || !layout) {
        return;
    }
    const int y_last = layout->mh - 2;
    if (y_last < layout->footer_min_y || layout->x > layout->mw - 2) {
        ui_status_clear_if_expired(now);
        return;
    }
    const int y_first = (y_last - 1 >= layout->footer_min_y) ? (y_last - 1) : y_last;
    (void)ui_status_draw(menu_win, y_first, layout->x, layout->mw - layout->x - 1, y_last - y_first + 1,
                         UI_STATUS_FLAG_PREFIX | UI_STATUS_FLAG_ANCHOR_BOTTOM, now);
}

void
ui_draw_menu(WINDOW* win, const NcMenuItem* items, size_t n, int hi, int* top_io, const char* title, const void* ctx) {
    if (!win) {
        return;
    }
    werase(win);
    box(win, 0, 0);
    UiMenuDrawLayout layout = {0};
    ui_menu_draw_layout_init(win, &layout);
    ui_menu_draw_title_line(win, &layout, title);
    int vis_total = ui_visible_count_and_maxlab(items, n, ctx, NULL);
    int hi_pos = ui_visible_index_for_item(items, n, ctx, hi);
    int top = top_io ? *top_io : 0;
    top = ui_scroll_follow_selection(vis_total, layout.items_rows, top, hi_pos);
    if (top_io) {
        *top_io = top;
    }
    time_t now = time(NULL);
    ui_menu_draw_item_rows(win, &layout, items, n, hi, top, ctx);
    ui_menu_draw_footer_lines(win, &layout, hi_pos, vis_total);
    ui_menu_draw_status_line(win, &layout, now);
    wnoutrefresh(win);
}

int
ui_visible_count_and_maxlab(const NcMenuItem* items, size_t n, const void* ctx, int* out_maxlab) {
    int vis = 0;
    int maxlab = 0;
    for (size_t i = 0; i < n; i++) {
        if (!ui_is_enabled(&items[i], ctx)) {
            continue;
        }
        int L = ui_menu_item_label_len(&items[i], ctx);
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

static int
ui_max_int(int a, int b) {
    return (a > b) ? a : b;
}

static int
ui_overlay_cap_then_floor(int value, int max_value, int min_value) {
    if (value > max_value) {
        value = max_value;
    }
    if (value < min_value) {
        value = min_value;
    }
    return value;
}

static int
ui_overlay_center_axis(int outer, int inner) {
    int pos = (outer - inner) / 2;
    return (pos < 0) ? 0 : pos;
}

static int
ui_overlay_compute_width(const UiMenuFrame* f, int maxlab) {
    const char* footer_keys = "Arrows/PgUp/PgDn/Home/End  (000/000)";
    const char* footer_help = "Enter/Right: select  h: help  Esc/Left: back";
    const int pad_x = 2;
    int width = pad_x + ((maxlab > 0) ? maxlab : 1);
    width = ui_max_int(width, pad_x + (int)strlen(footer_keys));
    width = ui_max_int(width, pad_x + (int)strlen(footer_help));
    if (f && f->title && *f->title) {
        width = ui_max_int(width, pad_x + (int)strlen(f->title));
    }
    return width + 2; // borders
}

static int
ui_overlay_compute_height(int visible_items) {
    int height = visible_items + 7;
    return (height < 9) ? 9 : height;
}

#ifdef DSD_NEO_TEST_HOOKS
int
ui_overlay_cap_then_floor_for_test(int value, int max_value, int min_value) {
    return ui_overlay_cap_then_floor(value, max_value, min_value);
}

int
ui_overlay_center_axis_for_test(int outer, int inner) {
    return ui_overlay_center_axis(outer, inner);
}

int
ui_overlay_compute_width_for_test(const UiMenuFrame* f, int maxlab) {
    return ui_overlay_compute_width(f, maxlab);
}

int
ui_overlay_compute_height_for_test(int visible_items) {
    return ui_overlay_compute_height(visible_items);
}
#endif

static void
ui_overlay_layout_for_terminal(UiMenuFrame* f, const void* ctx, int term_h, int term_w) {
    if (!f || !f->items || f->n == 0) {
        return;
    }
    int maxlab = 0;
    int vis = ui_visible_count_and_maxlab(f->items, f->n, ctx, &maxlab);
    int width = ui_overlay_compute_width(f, maxlab);
    int height = ui_overlay_compute_height(vis);
    width = ui_overlay_cap_then_floor(width, term_w - 2, 10);
    height = ui_overlay_cap_then_floor(height, term_h - 2, 6);
    f->h = height;
    f->w = width;
    f->y = ui_overlay_center_axis(term_h, height);
    f->x = ui_overlay_center_axis(term_w, width);
}

#ifdef DSD_NEO_TEST_HOOKS
void
ui_overlay_layout_for_test(UiMenuFrame* f, const void* ctx, int term_h, int term_w) {
    ui_overlay_layout_for_terminal(f, ctx, term_h, term_w);
}
#endif

void
ui_overlay_layout(UiMenuFrame* f, const void* ctx) {
    if (!f || !f->items || f->n == 0) {
        return;
    }
    int term_h = 24;
    int term_w = 80;
    getmaxyx(stdscr, term_h, term_w);
    ui_overlay_layout_for_terminal(f, ctx, term_h, term_w);
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
