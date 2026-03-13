// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Internal shared structures and helpers for menu subsystem.
 *
 * This header is internal to src/ui/terminal/ and should NOT be installed.
 */
#pragma once

#include <stddef.h>

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/platform/curses_compat.h>

typedef struct NcMenuItem NcMenuItem;

// Shared UI context passed to callbacks (full definition; forward-declared in menu_defs.h)
typedef struct UiCtx {
    dsd_opts* opts;
    dsd_state* state;
} UiCtx;

// Menu frame in overlay stack (owned/managed by menu_core.c)
typedef struct {
    const NcMenuItem* items;
    size_t n;
    int hi;
    int top;
    const char* title;
    WINDOW* win;
    int w, h;
    int y, x;
} UiMenuFrame;

// ---- Context structures shared between actions and callbacks ----
// These are allocated by act_* functions and consumed by cb_* functions

typedef struct {
    UiCtx* c;
    char host[256];
    int port;
} UdpOutCtx;

typedef struct {
    UiCtx* c;
    char host[256];
    int port;
} TcpLinkCtx;

typedef struct {
    UiCtx* c;
} TcpWavSymCtx;

typedef struct {
    UiCtx* c;
    char addr[128];
    int port;
} UdpInCtx;

typedef struct {
    UiCtx* c;
    char host[256];
    int port;
} RigCtx;

typedef struct {
    UiCtx* c;
    int step;
    unsigned long long w, s, n;
} P2Ctx;

typedef struct {
    UiCtx* c;
    const char* name;
} P25NumCtx;

typedef struct {
    UiCtx* c;
    int step;
    unsigned long long H, K1, K2, K3, K4;
} HyCtx;

typedef struct {
    UiCtx* c;
    int step;
    unsigned long long K1, K2, K3, K4;
} AesCtx;

typedef struct {
    UiCtx* c;
    char name[64];
} EnvEditCtx;

typedef struct {
    UiCtx* c;
} M17Ctx;

// Pulse device selection context
typedef struct {
    UiCtx* c;
    const char** labels;
    const char** names;
    char** bufs;
    int n;
} PulseSelCtx;

static inline int
ui_scroll_page_step_from_rows(int page_rows) {
    if (page_rows <= 1) {
        return 1;
    }
    return page_rows - 1;
}

static inline int
ui_scroll_clamp_top(int total, int page_rows, int top) {
    if (total <= 0 || page_rows <= 0 || total <= page_rows) {
        return 0;
    }
    if (top < 0) {
        top = 0;
    }
    if (top > total - page_rows) {
        top = total - page_rows;
    }
    return top;
}

static inline int
ui_scroll_last_page_top(int total, int page_rows) {
    return ui_scroll_clamp_top(total, page_rows, total - page_rows);
}

static inline int
ui_scroll_follow_selection(int total, int page_rows, int top, int sel_pos) {
    if (total <= 0 || page_rows <= 0) {
        return 0;
    }
    if (sel_pos < 0) {
        sel_pos = 0;
    }
    if (sel_pos >= total) {
        sel_pos = total - 1;
    }
    top = ui_scroll_clamp_top(total, page_rows, top);
    if (sel_pos < top) {
        top = sel_pos;
    } else if (sel_pos >= top + page_rows) {
        top = sel_pos - page_rows + 1;
    }
    return ui_scroll_clamp_top(total, page_rows, top);
}

// ---- Visibility helpers (from menu_render.c) ----
int ui_is_enabled(const NcMenuItem* it, void* ctx);
int ui_submenu_has_visible(const NcMenuItem* items, size_t n, void* ctx);
int ui_next_enabled(const NcMenuItem* items, size_t n, void* ctx, int from, int dir);
int ui_visible_index_for_item(const NcMenuItem* items, size_t n, void* ctx, int idx);

// ---- Render helpers (from menu_render.c) ----
void ui_draw_menu(WINDOW* win, const NcMenuItem* items, size_t n, int hi, int* top_io, const char* title, void* ctx);
void ui_overlay_layout(UiMenuFrame* f, void* ctx);
void ui_overlay_ensure_window(UiMenuFrame* f);
void ui_overlay_recreate_if_needed(UiMenuFrame* f);
int ui_visible_count_and_maxlab(const NcMenuItem* items, size_t n, void* ctx, int* out_maxlab);

// Chooser helpers are declared in menu_prompts.h
