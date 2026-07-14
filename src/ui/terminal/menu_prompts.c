// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Prompt, chooser, and help overlay implementations for menu subsystem.
 *
 * This file owns the state for string/int/double prompts, the generic chooser overlay,
 * and the help overlay. It provides the public API declared in menu_prompts.h.
 */

#include "menu_prompts.h"
#include <ctype.h>
#include <curses.h>
#include <dsd-neo/core/string_utils.h>
#include <dsd-neo/platform/curses_compat.h>
#include <dsd-neo/ui/keymap.h>
#include <dsd-neo/ui/ui_prims.h>
#include <stdlib.h>
#include <string.h>
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/platform/platform.h"
#include "dsd-neo/ui/menu_core.h"
#include "menu_internal.h"

typedef struct {
    int active;
    const char* title;
    WINDOW* win;
    // string mode fields
    char* buf;
    size_t cap;
    size_t len;
    size_t cursor;                        // cursor position within buf [0..len]
    ui_prompt_string_done_fn on_done_str; // NULL text indicates cancel
    void* user;
} UiPrompt;

static UiPrompt g_prompt = {0};

// ---- Typed prompt context structures ----
typedef struct {
    ui_prompt_int_done_fn cb;
    void* user;
} PromptIntCtx;

typedef struct {
    ui_prompt_double_done_fn cb;
    void* user;
} PromptDblCtx;

// ---- Chooser overlay state ----
typedef struct {
    int active;
    const char* title;
    const char* const* items;
    int count;
    int sel;
    int top;
    int page_rows;
    WINDOW* win;
    ui_chooser_done_fn on_done;
    void* user;
} UiChooser;

static UiChooser g_chooser = {0};

// ---- Help overlay state ----
typedef struct {
    int active;
    const char* text;
    WINDOW* win;
    int scroll;
    int line_count;
    int page_rows;
} UiHelp;

static UiHelp g_help = {0};

enum {
    UI_HELP_MAX_LINES = 256,
    UI_HELP_MAX_LINE_CHARS = 256,
};

static int
ui_help_push_line(char lines[][UI_HELP_MAX_LINE_CHARS], int max_lines, int* count, const char* src, int len) {
    if (!lines || !count || max_lines <= 0) {
        return 0;
    }
    if (*count >= max_lines) {
        return 0;
    }
    if (!src || len <= 0) {
        lines[*count][0] = '\0';
        (*count)++;
        return 1;
    }
    if (len >= UI_HELP_MAX_LINE_CHARS) {
        len = UI_HELP_MAX_LINE_CHARS - 1;
    }
    DSD_MEMCPY(lines[*count], src, (size_t)len);
    lines[*count][len] = '\0';
    (*count)++;
    return 1;
}

static void
ui_help_skip_spaces(const char** p) {
    if (!p || !*p) {
        return;
    }
    while (**p == ' ') {
        (*p)++;
    }
}

static int
ui_help_scan_word(const char** p, const char** wstart) {
    if (!p || !*p || !wstart) {
        return 0;
    }
    *wstart = *p;
    int wlen = 0;
    while (**p != '\0' && **p != ' ' && **p != '\n') {
        if (wlen < (UI_HELP_MAX_LINE_CHARS - 1)) {
            wlen++;
        }
        (*p)++;
    }
    return wlen;
}

static int
ui_help_append_word(char cur[], int* cur_len, int width, const char* wstart, int wlen,
                    char lines[][UI_HELP_MAX_LINE_CHARS], int max_lines, int* out_count) {
    if (!cur || !cur_len || !wstart || !lines || !out_count || wlen <= 0) {
        return 0;
    }
    if (*cur_len > 0 && (*cur_len + 1 + wlen) <= width) {
        cur[(*cur_len)++] = ' ';
        DSD_MEMCPY(cur + *cur_len, wstart, (size_t)wlen);
        *cur_len += wlen;
        return 1;
    }
    if (*cur_len > 0) {
        ui_help_push_line(lines, max_lines, out_count, cur, *cur_len);
        *cur_len = 0;
    }
    if (wlen <= width) {
        DSD_MEMCPY(cur, wstart, (size_t)wlen);
        *cur_len = wlen;
        return 1;
    }
    return 0;
}

static void
ui_help_hard_wrap_word(const char* wstart, int wlen, int width, char lines[][UI_HELP_MAX_LINE_CHARS], int max_lines,
                       int* out_count) {
    if (!wstart || !lines || !out_count || wlen <= 0 || width <= 0) {
        return;
    }
    int off = 0;
    while (off < wlen) {
        int chunk = width;
        if (chunk > wlen - off) {
            chunk = wlen - off;
        }
        if (!ui_help_push_line(lines, max_lines, out_count, wstart + off, chunk)) {
            break;
        }
        off += chunk;
    }
}

static int
ui_help_wrap_text(const char* text, int width, char lines[][UI_HELP_MAX_LINE_CHARS], int max_lines) {
    if (!lines || max_lines <= 0 || width <= 0) {
        return 0;
    }
    if (!text) {
        text = "";
    }
    int out_count = 0;
    char cur[UI_HELP_MAX_LINE_CHARS];
    int cur_len = 0;
    const char* p = text;

    while (*p != '\0') {
        if (*p == '\n') {
            ui_help_push_line(lines, max_lines, &out_count, cur, cur_len);
            cur_len = 0;
            p++;
            continue;
        }
        ui_help_skip_spaces(&p);
        if (*p == '\0') {
            break;
        }
        if (*p == '\n') {
            continue;
        }

        const char* wstart = NULL;
        int wlen = ui_help_scan_word(&p, &wstart);
        if (wlen <= 0) {
            continue;
        }
        if (!ui_help_append_word(cur, &cur_len, width, wstart, wlen, lines, max_lines, &out_count)) {
            // Long unbreakable token: hard-wrap it to keep the overlay stable.
            ui_help_hard_wrap_word(wstart, wlen, width, lines, max_lines, &out_count);
        }
    }

    if (cur_len > 0) {
        ui_help_push_line(lines, max_lines, &out_count, cur, cur_len);
    }
    if (out_count == 0) {
        lines[0][0] = '\0';
        out_count = 1;
    }
    return out_count;
}

static void
ui_chooser_keep_selection_visible(void) {
    g_chooser.top = ui_scroll_follow_selection(g_chooser.count, g_chooser.page_rows, g_chooser.top, g_chooser.sel);
}

static int DSD_ATTR_USED
ui_prompt_curs_set(int visibility) {
#ifdef DSD_NEO_TEST_HOOKS
    (void)visibility;
    return 0;
#else
    return curs_set(visibility);
#endif
}

// ---- Prompt implementations ----

void
ui_prompt_close_all(void) {
    // If an active prompt is being closed without an explicit completion,
    // signal a cancel to allow user context cleanup.
    if (g_prompt.active && g_prompt.on_done_str) {
        void (*cb)(void*, const char*) = g_prompt.on_done_str;
        void* up = g_prompt.user;
        g_prompt.on_done_str = NULL; // prevent double-callback
        cb(up, NULL);
    }

    if (g_prompt.win) {
        delwin(g_prompt.win);
        g_prompt.win = NULL;
    }
    if (g_prompt.buf) {
        free(g_prompt.buf);
        g_prompt.buf = NULL;
    }
    (void)ui_prompt_curs_set(0); // hide cursor when no prompt is active
    DSD_MEMSET(&g_prompt, 0, sizeof(g_prompt));
}

// Cppcheck 2.21 loses the final prototype name after a callback typedef parameter.
// cppcheck-suppress-begin funcArgNamesDifferentUnnamed
void
ui_prompt_open_string_async(const char* title, const char* prefill, size_t cap, ui_prompt_string_done_fn on_done,
                            void* user_ctx) {
    ui_prompt_close_all();
    g_prompt.active = 1;
    g_prompt.title = title;
    g_prompt.on_done_str = on_done;
    g_prompt.user = user_ctx;
    if (cap < 2) {
        cap = 2;
    }
    g_prompt.buf = (char*)calloc(cap, 1);
    g_prompt.cap = cap;
    g_prompt.len = 0;
    if (!g_prompt.buf) {
        // Allocation failed: immediately signal cancel to ensure user context can be freed.
        if (on_done) {
            on_done(user_ctx, NULL);
        }
        g_prompt.active = 0;
        return;
    }
    if (prefill && *prefill) {
        dsd_strncpy_s(g_prompt.buf, cap, prefill, cap - 1);
        g_prompt.buf[cap - 1] = '\0';
        g_prompt.len = strlen(g_prompt.buf);
        g_prompt.cursor = g_prompt.len;
    }
}

// cppcheck-suppress-end funcArgNamesDifferentUnnamed

static void
ui_prompt_int_finish(void* u, const char* text) {
    PromptIntCtx* pic = (PromptIntCtx*)u;
    if (!pic) {
        return;
    }
    if (!text || !*text) {
        if (pic->cb) {
            pic->cb(pic->user, 0, 0);
        }
        free(pic);
        return;
    }
    char* end = NULL;
    long v = strtol(text, &end, 10);
    if (!end || *end != '\0') {
        ui_statusf("Invalid integer input");
        if (pic->cb) {
            pic->cb(pic->user, 0, 0);
        }
    } else {
        if (pic->cb) {
            pic->cb(pic->user, 1, (int)v);
        }
    }
    free(pic);
}

static void
ui_prompt_double_finish(void* u, const char* text) {
    PromptDblCtx* pdc = (PromptDblCtx*)u;
    if (!pdc) {
        return;
    }
    if (!text || !*text) {
        if (pdc->cb) {
            pdc->cb(pdc->user, 0, 0.0);
        }
        free(pdc);
        return;
    }
    char* end = NULL;
    double v = strtod(text, &end);
    if (!end || *end != '\0') {
        ui_statusf("Invalid numeric input");
        if (pdc->cb) {
            pdc->cb(pdc->user, 0, 0.0);
        }
    } else {
        if (pdc->cb) {
            pdc->cb(pdc->user, 1, v);
        }
    }
    free(pdc);
}

// Cppcheck 2.21 loses the final prototype name after a callback typedef parameter.
// cppcheck-suppress-begin funcArgNamesDifferentUnnamed
void
ui_prompt_open_int_async(const char* title, int initial, ui_prompt_int_done_fn cb, void* user_ctx) {
    char pre[64];
    DSD_SNPRINTF(pre, sizeof pre, "%d", initial);
    PromptIntCtx* pic = (PromptIntCtx*)calloc(1, sizeof(PromptIntCtx));
    if (!pic) {
        // Allocation failed: immediately signal cancel so caller can clean up.
        if (cb) {
            cb(user_ctx, 0, 0);
        }
        return;
    }
    pic->cb = cb;
    pic->user = user_ctx;
    ui_prompt_open_string_async(title, pre, 64, ui_prompt_int_finish, pic);
}

// cppcheck-suppress-end funcArgNamesDifferentUnnamed

// Cppcheck 2.21 loses the final prototype name after a callback typedef parameter.
// cppcheck-suppress-begin funcArgNamesDifferentUnnamed
void
ui_prompt_open_double_async(const char* title, double initial, ui_prompt_double_done_fn cb, void* user_ctx) {
    char pre[64];
    DSD_SNPRINTF(pre, sizeof pre, "%.6f", initial);
    PromptDblCtx* pdc = (PromptDblCtx*)calloc(1, sizeof(PromptDblCtx));
    if (!pdc) {
        // Allocation failed: immediately signal cancel so caller can clean up.
        if (cb) {
            cb(user_ctx, 0, 0.0);
        }
        return;
    }
    pdc->cb = cb;
    pdc->user = user_ctx;
    ui_prompt_open_string_async(title, pre, 64, ui_prompt_double_finish, pdc);
}

// cppcheck-suppress-end funcArgNamesDifferentUnnamed

// ---- Prompt active/handle_key/render for menu_core delegation ----

int
ui_prompt_active(void) {
    return g_prompt.active;
}

static void
ui_prompt_handle_resize_event(void) {
#if DSD_CURSES_NEEDS_EXPLICIT_RESIZE
    resize_term(0, 0);
#endif
    if (g_prompt.win) {
        delwin(g_prompt.win);
        g_prompt.win = NULL;
    }
}

static void
ui_prompt_cancel(void) {
    void (*cb)(void*, const char*) = g_prompt.on_done_str;
    void* up = g_prompt.user;
    // Close first so callbacks can safely open a new prompt.
    g_prompt.on_done_str = NULL; // prevent close_all() from calling again
    ui_prompt_close_all();
    if (cb) {
        cb(up, NULL);
    }
}

static int
ui_prompt_handle_cursor_key(int ch) {
    if (ch == KEY_LEFT) {
        if (g_prompt.cursor > 0) {
            g_prompt.cursor--;
        }
        return 1;
    }
    if (ch == KEY_RIGHT) {
        if (g_prompt.cursor < g_prompt.len) {
            g_prompt.cursor++;
        }
        return 1;
    }
    if (ch == KEY_HOME) {
        g_prompt.cursor = 0;
        return 1;
    }
    if (ch == KEY_END) {
        g_prompt.cursor = g_prompt.len;
        return 1;
    }
    return 0;
}

static void
ui_prompt_delete_left(void) {
    if (g_prompt.cursor > 0) {
        DSD_MEMMOVE(g_prompt.buf + g_prompt.cursor - 1, g_prompt.buf + g_prompt.cursor,
                    g_prompt.len - g_prompt.cursor + 1);
        g_prompt.cursor--;
        g_prompt.len--;
    }
}

static void
ui_prompt_delete_at_cursor(void) {
    if (g_prompt.cursor < g_prompt.len) {
        DSD_MEMMOVE(g_prompt.buf + g_prompt.cursor, g_prompt.buf + g_prompt.cursor + 1, g_prompt.len - g_prompt.cursor);
        g_prompt.len--;
    }
}

static void
ui_prompt_submit(void) {
    void (*cb)(void*, const char*) = g_prompt.on_done_str;
    void* up = g_prompt.user;
    const int have_text = (g_prompt.len > 0 && g_prompt.buf != NULL);
    char* text_copy = NULL;
    if (have_text) {
        text_copy = (char*)malloc(g_prompt.len + 1);
        if (text_copy) {
            DSD_MEMCPY(text_copy, g_prompt.buf, g_prompt.len + 1);
        }
    }
    // Close first so callbacks can safely open a new prompt.
    g_prompt.on_done_str = NULL; // prevent close_all() from calling again
    ui_prompt_close_all();
    if (cb) {
        cb(up, have_text ? text_copy : "");
    }
    free(text_copy);
}

static void
ui_prompt_insert_char(int ch) {
    if (g_prompt.len + 1 < g_prompt.cap) {
        DSD_MEMMOVE(g_prompt.buf + g_prompt.cursor + 1, g_prompt.buf + g_prompt.cursor,
                    g_prompt.len - g_prompt.cursor + 1);
        g_prompt.buf[g_prompt.cursor++] = (char)ch;
        g_prompt.len++;
    }
}

int
ui_prompt_handle_key(int ch) {
    if (!g_prompt.active) {
        return 0;
    }
    if (ch == KEY_RESIZE) {
        ui_prompt_handle_resize_event();
        return 1;
    }
    if (ch == ERR) {
        return 1;
    }
    // Prompts must allow any printable characters (including 'q') so users can
    // type filenames like "iq.bin" without accidentally cancelling.
    if (ch == DSD_KEY_ESC) {
        ui_prompt_cancel();
        return 1;
    }
    if (ui_prompt_handle_cursor_key(ch)) {
        return 1;
    }
    if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
        ui_prompt_delete_left();
        return 1;
    }
    if (ch == KEY_DC) {
        ui_prompt_delete_at_cursor();
        return 1;
    }
    if (ch == 10 || ch == KEY_ENTER || ch == '\r') {
        ui_prompt_submit();
        return 1;
    }
    if (isprint(ch)) {
        ui_prompt_insert_char(ch);
        return 1;
    }
    return 1;
}

static int
ui_center_axis(int screen_extent, int window_extent) {
    int pos = (screen_extent - window_extent) / 2;
    if (pos < 0) {
        pos = 0;
    }
    return pos;
}

static int
ui_prompt_fit_width(int desired_width, int screen_width) {
    int max_width = screen_width - 2;
    if (max_width < 10) {
        max_width = screen_width;
    }
    if (max_width < 4) {
        return 0;
    }
    if (desired_width > max_width) {
        desired_width = max_width;
    }
    if (desired_width < 10 && max_width >= 10) {
        desired_width = 10;
    }
    return desired_width;
}

static int
ui_prompt_fit_height(int desired_height, int screen_height) {
    int max_height = screen_height - 2;
    if (max_height < 6) {
        max_height = screen_height;
    }
    if (max_height < 3) {
        return 0;
    }
    if (desired_height > max_height) {
        desired_height = max_height;
    }
    if (desired_height < 6 && max_height >= 6) {
        desired_height = 6;
    }
    if (desired_height < 3) {
        desired_height = 3;
    }
    return desired_height;
}

static int
ui_prompt_compute_window_rect(const char* title, int* h, int* w, int* py, int* px) {
    if (!title || !h || !w || !py || !px) {
        return 0;
    }
    int scr_h = 0, scr_w = 0;
    getmaxyx(stdscr, scr_h, scr_w);
    if (scr_h < 4 || scr_w < 8) {
        return 0;
    }
    int local_w = (int)strlen(title) + 16;
    if (local_w < 54) {
        local_w = 54;
    }
    local_w = ui_prompt_fit_width(local_w, scr_w);
    if (local_w <= 0) {
        return 0;
    }

    int local_h = ui_prompt_fit_height(8, scr_h);
    if (local_h <= 0) {
        return 0;
    }

    int local_py = ui_center_axis(scr_h, local_h);
    int local_px = ui_center_axis(scr_w, local_w);
    *h = local_h;
    *w = local_w;
    *py = local_py;
    *px = local_px;
    return 1;
}

static void
ui_prompt_compute_row_positions(int h, int* title_y, int* input_y, int* footer_y) {
    if (!title_y || !input_y || !footer_y) {
        return;
    }
    int local_title_y = -1;
    int local_input_y = 1;
    int local_footer_y = -1;
    int interior_rows = h - 2;
    if (interior_rows >= 4) {
        local_title_y = 1;
        local_input_y = 3;
        local_footer_y = h - 2;
    } else if (interior_rows == 3) {
        local_title_y = 1;
        local_input_y = 2;
        local_footer_y = h - 2;
    } else if (interior_rows == 2) {
        local_title_y = 1;
        local_input_y = 2;
    }
    *title_y = local_title_y;
    *input_y = local_input_y;
    *footer_y = local_footer_y;
}

static void
ui_prompt_compute_field_geometry(int w, int* field_col, int* field_right, int* field_width) {
    if (!field_col || !field_right || !field_width) {
        return;
    }
    int local_col = 4;
    int local_right = w - 2;
    if (local_col > local_right) {
        local_col = local_right;
    }
    if (local_col < 2) {
        local_col = 2;
    }
    int local_width = local_right - local_col;
    if (local_width < 1) {
        local_width = 1;
    }
    *field_col = local_col;
    *field_right = local_right;
    *field_width = local_width;
}

typedef struct {
    size_t start;
    size_t cursor;
    int show_left_ellipsis;
} UiPromptViewState;

static UiPromptViewState
ui_prompt_compute_view_state(const char* text, size_t cursor, int field_width) {
    UiPromptViewState view = {0, 0, 0};
    size_t text_len = text ? strlen(text) : 0;
    size_t cpos = cursor;
    if (cpos > text_len) {
        cpos = text_len;
    }
    view.cursor = cpos;
    if (text_len <= (size_t)field_width) {
        return view;
    }
    int usable = field_width;
    if (cpos > (size_t)usable) {
        view.start = cpos - (size_t)(usable - 1);
    }
    if (view.start > 0 && field_width >= 4) {
        view.show_left_ellipsis = 1;
        usable = field_width - 3;
        if (cpos > view.start + (size_t)usable) {
            view.start = cpos - (size_t)(usable - 1);
        }
        if (cpos < view.start) {
            view.start = cpos;
        }
        if (view.start == 0) {
            view.show_left_ellipsis = 0;
        }
    }
    return view;
}

static int
ui_prompt_compute_cursor_x(int field_col, int field_right, const UiPromptViewState* view) {
    if (!view) {
        return field_col;
    }
    int prefix = view->show_left_ellipsis ? 3 : 0;
    int cursor_x = field_col + prefix + (int)(view->cursor - view->start);
    if (cursor_x > field_right) {
        cursor_x = field_right;
    }
    if (cursor_x < 2) {
        cursor_x = 2;
    }
    return cursor_x;
}

void
ui_prompt_render(void) {
    if (!g_prompt.active) {
        return;
    }
    const char* title = g_prompt.title ? g_prompt.title : "Input";
    int h = 0, w = 0, py = 0, px = 0;
    if (!ui_prompt_compute_window_rect(title, &h, &w, &py, &px)) {
        return;
    }
    if (!g_prompt.win) {
        g_prompt.win = ui_make_window(h, w, py, px);
        if (!g_prompt.win) {
            return;
        }
        wtimeout(g_prompt.win, 0);
    }
    WINDOW* win = g_prompt.win;
    int title_y = -1;
    int input_y = 1;
    int footer_y = -1;
    ui_prompt_compute_row_positions(h, &title_y, &input_y, &footer_y);
    int field_col = 2;
    int field_right = 2;
    int field_width = 1;
    ui_prompt_compute_field_geometry(w, &field_col, &field_right, &field_width);
    const char* text = g_prompt.buf ? g_prompt.buf : "";
    UiPromptViewState view = ui_prompt_compute_view_state(text, g_prompt.cursor, field_width);
    int cursor_x = ui_prompt_compute_cursor_x(field_col, field_right, &view);
    int body_w = (w > 4) ? (w - 4) : 1;

    (void)ui_prompt_curs_set(1); // show cursor while editing prompt text
    werase(win);
    box(win, 0, 0);
    if (title_y > 0) {
        mvwaddnstr(win, title_y, 2, title, body_w);
    }
    mvwaddnstr(win, input_y, 2, "> ", (w > 5) ? 2 : 1);
    if (view.show_left_ellipsis) {
        mvwaddnstr(win, input_y, field_col, "...", 3);
        mvwaddnstr(win, input_y, field_col + 3, text + view.start, field_width - 3);
    } else {
        mvwaddnstr(win, input_y, field_col, text + view.start, field_width);
    }
    if (footer_y > 0 && footer_y != input_y) {
        mvwaddnstr(win, footer_y, 2, "Enter=OK  Esc=Cancel", body_w);
    }
    // Footer/title writes also move the curses cursor; place it last so input editing stays visible.
    wmove(win, input_y, cursor_x);
    wnoutrefresh(win);
}

// ---- Help implementations ----

void
ui_help_open(const char* help) {
    if (!help || !*help) {
        return;
    }
    g_help.active = 1;
    g_help.text = help;
    g_help.scroll = 0;
    g_help.line_count = 0;
    g_help.page_rows = 0;
    if (g_help.win) {
        delwin(g_help.win);
        g_help.win = NULL;
    }
}

void
ui_help_close(void) {
    if (g_help.win) {
        delwin(g_help.win);
        g_help.win = NULL;
    }
    (void)ui_prompt_curs_set(0);
    DSD_MEMSET(&g_help, 0, sizeof(g_help));
}

int
ui_help_active(void) {
    return g_help.active;
}

static void
ui_help_handle_resize_event(void) {
#if DSD_CURSES_NEEDS_EXPLICIT_RESIZE
    resize_term(0, 0);
#endif
    if (g_help.win) {
        delwin(g_help.win);
        g_help.win = NULL;
    }
}

static int
ui_help_max_scroll(void) {
    if (g_help.line_count > g_help.page_rows && g_help.page_rows > 0) {
        return g_help.line_count - g_help.page_rows;
    }
    return 0;
}

static int
ui_help_is_close_key(int ch) {
    return (ch == DSD_KEY_ESC || ch == 'q' || ch == 'Q' || ch == 'h' || ch == 'H' || ch == 10 || ch == KEY_ENTER
            || ch == '\r' || ch == KEY_LEFT);
}

static int
ui_help_handle_scroll_key(int ch, int max_scroll, int page_step) {
    if (ch == KEY_UP) {
        if (g_help.scroll > 0) {
            g_help.scroll--;
        }
        return 1;
    }
    if (ch == KEY_DOWN) {
        if (g_help.scroll < max_scroll) {
            g_help.scroll++;
        }
        return 1;
    }
    if (ch == KEY_PPAGE) {
        g_help.scroll -= page_step;
        if (g_help.scroll < 0) {
            g_help.scroll = 0;
        }
        return 1;
    }
    if (ch == KEY_NPAGE) {
        g_help.scroll += page_step;
        if (g_help.scroll > max_scroll) {
            g_help.scroll = max_scroll;
        }
        return 1;
    }
    if (ch == KEY_HOME) {
        g_help.scroll = 0;
        return 1;
    }
    if (ch == KEY_END) {
        g_help.scroll = max_scroll;
        return 1;
    }
    return 0;
}

int
ui_help_handle_key(int ch) {
    if (!g_help.active) {
        return 0;
    }
    if (ch == KEY_RESIZE) {
        ui_help_handle_resize_event();
        return 1;
    }
    if (ch == ERR) {
        return 1;
    }
    int max_scroll = ui_help_max_scroll();
    int page_step = ui_scroll_page_step_from_rows(g_help.page_rows);
    if (ui_help_handle_scroll_key(ch, max_scroll, page_step)) {
        return 1;
    }
    if (ui_help_is_close_key(ch)) {
        ui_help_close();
        return 1;
    }
    return 1;
}

static int
ui_help_compute_window_rect(int* h, int* w, int* hy, int* hx) {
    if (!h || !w || !hy || !hx) {
        return 0;
    }
    int local_h = 14;
    int local_w = 68;
    int scr_h = 0, scr_w = 0;
    getmaxyx(stdscr, scr_h, scr_w);
    if (scr_h < 4 || scr_w < 8) {
        return 0;
    }
    int max_w = scr_w - 2;
    int max_h = scr_h - 2;
    if (max_w < 10 || max_h < 6) {
        return 0;
    }
    if (local_w > max_w) {
        local_w = max_w;
    }
    if (local_w < 30) {
        local_w = max_w;
    }
    if (local_h > max_h) {
        local_h = max_h;
    }
    int local_hy = (scr_h - local_h) / 2;
    int local_hx = (scr_w - local_w) / 2;
    if (local_hy < 0) {
        local_hy = 0;
    }
    if (local_hx < 0) {
        local_hx = 0;
    }
    *h = local_h;
    *w = local_w;
    *hy = local_hy;
    *hx = local_hx;
    return 1;
}

static void
ui_help_clamp_scroll(int max_scroll) {
    if (max_scroll < 0) {
        max_scroll = 0;
    }
    if (g_help.scroll < 0) {
        g_help.scroll = 0;
    }
    if (g_help.scroll > max_scroll) {
        g_help.scroll = max_scroll;
    }
}

static void
ui_help_draw_title(WINDOW* hw, int max_scroll, int first, int last, int line_count) {
    if (!hw) {
        return;
    }
    if (max_scroll > 0) {
        mvwprintw(hw, 1, 2, "Help (%d-%d/%d)", first + 1, last, line_count);
    } else {
        mvwprintw(hw, 1, 2, "Help");
    }
}

static void
ui_help_draw_page_lines(WINDOW* hw, int h, int body_w, char lines[][UI_HELP_MAX_LINE_CHARS], int first, int last,
                        int line_count) {
    if (!hw || !lines) {
        return;
    }
    int y = 2;
    for (int i = first; i < last && y <= (h - 3); i++, y++) {
        if (i < 0 || i >= line_count || i >= UI_HELP_MAX_LINES) {
            break;
        }
        mvwaddnstr(hw, y, 2, lines[i], body_w);
    }
}

static void
ui_help_draw_footer(WINDOW* hw, int h, int body_w, int max_scroll) {
    if (!hw) {
        return;
    }
    if (max_scroll > 0) {
        mvwaddnstr(hw, h - 2, 2, "Up/Down/PgUp/PgDn: scroll  Esc/q: close", body_w);
    } else {
        mvwaddnstr(hw, h - 2, 2, "Esc/q/Enter: close", body_w);
    }
}

void
ui_help_render(void) {
    if (!g_help.active) {
        return;
    }
    int h = 0, w = 0, hy = 0, hx = 0;
    if (!ui_help_compute_window_rect(&h, &w, &hy, &hx)) {
        ui_help_close();
        return;
    }
    if (!g_help.win) {
        g_help.win = ui_make_window(h, w, hy, hx);
        if (!g_help.win) {
            ui_help_close();
            return;
        }
        wtimeout(g_help.win, 0);
    }
    WINDOW* hw = g_help.win;
    werase(hw);
    box(hw, 0, 0);
    int body_w = (w > 4) ? (w - 4) : 1;
    int page_rows = (h > 4) ? (h - 4) : 1;

    const char* text = g_help.text ? g_help.text : "";
    char lines[UI_HELP_MAX_LINES][UI_HELP_MAX_LINE_CHARS];
    int line_count = ui_help_wrap_text(text, body_w, lines, UI_HELP_MAX_LINES);
    if (line_count < 1) {
        line_count = 1;
    }
    g_help.line_count = line_count;
    g_help.page_rows = page_rows;

    int max_scroll = (line_count > page_rows) ? (line_count - page_rows) : 0;
    ui_help_clamp_scroll(max_scroll);
    int first = g_help.scroll;
    int last = first + page_rows;
    if (last > line_count) {
        last = line_count;
    }

    ui_help_draw_title(hw, max_scroll, first, last, line_count);
    ui_help_draw_page_lines(hw, h, body_w, lines, first, last, line_count);
    ui_help_draw_footer(hw, h, body_w, max_scroll);
    wnoutrefresh(hw);
}

// ---- Chooser implementations ----

static void
ui_chooser_finish(int sel) {
    void (*cb)(void*, int) = g_chooser.on_done;
    void* userp = g_chooser.user;
    g_chooser.on_done = NULL; // prevent double-callback during close/reentry
    ui_chooser_close();
    if (cb) {
        cb(userp, sel);
    }
}

// Cppcheck 2.21 loses the final prototype name after a callback typedef parameter.
// cppcheck-suppress-begin funcArgNamesDifferentUnnamed
void
ui_chooser_start(const char* title, const char* const* items, int count, ui_chooser_done_fn on_done, void* user_ctx) {
    if (!items || count <= 0) {
        ui_chooser_close();
        if (on_done) {
            on_done(user_ctx, -1);
        }
        return;
    }
    g_chooser.active = 1;
    g_chooser.title = title;
    g_chooser.items = items;
    g_chooser.count = count;
    g_chooser.sel = 0;
    g_chooser.top = 0;
    g_chooser.page_rows = 0;
    g_chooser.on_done = on_done;
    g_chooser.user = user_ctx;
    if (g_chooser.win) {
        delwin(g_chooser.win);
        g_chooser.win = NULL;
    }
}

// cppcheck-suppress-end funcArgNamesDifferentUnnamed

void
ui_chooser_close(void) {
    if (g_chooser.win) {
        delwin(g_chooser.win);
        g_chooser.win = NULL;
    }
    (void)ui_prompt_curs_set(0);
    DSD_MEMSET(&g_chooser, 0, sizeof(g_chooser));
}

int
ui_chooser_active(void) {
    return g_chooser.active;
}

static void
ui_chooser_handle_resize_event(void) {
#if DSD_CURSES_NEEDS_EXPLICIT_RESIZE
    resize_term(0, 0);
#endif
    if (g_chooser.win) {
        delwin(g_chooser.win);
        g_chooser.win = NULL;
    }
}

static void
ui_chooser_page_move(int direction) {
    int page_step = ui_scroll_page_step_from_rows(g_chooser.page_rows);
    if (direction < 0) {
        g_chooser.sel -= page_step;
        if (g_chooser.sel < 0) {
            g_chooser.sel = 0;
        }
        g_chooser.top -= page_step;
    } else {
        g_chooser.sel += page_step;
        if (g_chooser.sel >= g_chooser.count) {
            g_chooser.sel = g_chooser.count - 1;
        }
        g_chooser.top += page_step;
    }
    ui_chooser_keep_selection_visible();
}

static int
ui_chooser_handle_navigation_key(int ch) {
    if (ch == KEY_UP) {
        g_chooser.sel = (g_chooser.sel - 1 + g_chooser.count) % g_chooser.count;
        ui_chooser_keep_selection_visible();
        return 1;
    }
    if (ch == KEY_DOWN) {
        g_chooser.sel = (g_chooser.sel + 1) % g_chooser.count;
        ui_chooser_keep_selection_visible();
        return 1;
    }
    if (ch == KEY_HOME) {
        g_chooser.sel = 0;
        g_chooser.top = 0;
        return 1;
    }
    if (ch == KEY_END) {
        g_chooser.sel = g_chooser.count - 1;
        g_chooser.top = ui_scroll_last_page_top(g_chooser.count, g_chooser.page_rows);
        return 1;
    }
    if (ch == KEY_PPAGE) {
        ui_chooser_page_move(-1);
        return 1;
    }
    if (ch == KEY_NPAGE) {
        ui_chooser_page_move(1);
        return 1;
    }
    return 0;
}

static int
ui_chooser_is_cancel_key(int ch) {
    return (ch == 'q' || ch == 'Q' || ch == DSD_KEY_ESC || ch == KEY_LEFT);
}

static int
ui_chooser_is_accept_key(int ch) {
    return (ch == 10 || ch == KEY_ENTER || ch == '\r' || ch == KEY_RIGHT);
}

int
ui_chooser_handle_key(int ch) {
    if (!g_chooser.active) {
        return 0;
    }
    if (g_chooser.count <= 0) {
        ui_chooser_finish(-1);
        return 1;
    }
    if (ch == ERR) {
        return 1;
    }
    if (ch == KEY_RESIZE) {
        ui_chooser_handle_resize_event();
        return 1;
    }
    if (ui_chooser_handle_navigation_key(ch)) {
        return 1;
    }
    if (ui_chooser_is_cancel_key(ch)) {
        ui_chooser_finish(-1);
        return 1;
    }
    if (ui_chooser_is_accept_key(ch)) {
        ui_chooser_finish(g_chooser.sel);
        return 1;
    }
    return 1;
}

static int
ui_chooser_max_item_width(void) {
    int max_item = 0;
    for (int i = 0; i < g_chooser.count; i++) {
        int item_len = (int)strlen(g_chooser.items[i]);
        if (item_len > max_item) {
            max_item = item_len;
        }
    }
    return max_item;
}

static int
ui_chooser_initial_width(const char* title, const char* footer, int max_item) {
    int width = 4 + (int)strlen(title);
    int item_need = 4 + max_item;
    if (item_need > width) {
        width = item_need;
    }
    int footer_need = 4 + (int)strlen(footer);
    if (footer_need > width) {
        width = footer_need;
    }
    return width + 2;
}

static int
ui_chooser_fit_width(int desired_width, int screen_width) {
    int max_width = screen_width - 2;
    if (max_width < 10) {
        return 0;
    }
    if (desired_width > max_width) {
        desired_width = max_width;
    }
    if (desired_width < 10) {
        desired_width = 10;
    }
    return desired_width;
}

static int
ui_chooser_fit_height(int desired_height, int screen_height) {
    int max_height = screen_height - 2;
    if (max_height < 6) {
        return 0;
    }
    if (desired_height > max_height) {
        desired_height = max_height;
    }
    if (desired_height < 6) {
        desired_height = 6;
    }
    return desired_height;
}

static int
ui_chooser_compute_window_rect_for_terminal(const char* title, const char* footer, int max_item, int count,
                                            int screen_h, int screen_w, int* h, int* w, int* wy, int* wx) {
    if (!title || !footer || !h || !w || !wy || !wx) {
        return 0;
    }
    int local_w = ui_chooser_initial_width(title, footer, max_item);
    int local_h = count + 5;
    if (local_h < 7) {
        local_h = 7;
    }

    if (screen_h < 4 || screen_w < 8) {
        return 0;
    }
    local_w = ui_chooser_fit_width(local_w, screen_w);
    local_h = ui_chooser_fit_height(local_h, screen_h);
    if (local_w <= 0 || local_h <= 0) {
        return 0;
    }

    int local_wy = ui_center_axis(screen_h, local_h);
    int local_wx = ui_center_axis(screen_w, local_w);
    *h = local_h;
    *w = local_w;
    *wy = local_wy;
    *wx = local_wx;
    return 1;
}

static int
ui_chooser_compute_window_rect(const char* title, const char* footer, int max_item, int* h, int* w, int* wy, int* wx) {
    int scr_h = 0, scr_w = 0;
    getmaxyx(stdscr, scr_h, scr_w);
    return ui_chooser_compute_window_rect_for_terminal(title, footer, max_item, g_chooser.count, scr_h, scr_w, h, w, wy,
                                                       wx);
}

static void
ui_chooser_clamp_selection(void) {
    if (g_chooser.sel < 0) {
        g_chooser.sel = 0;
    }
    if (g_chooser.sel >= g_chooser.count) {
        g_chooser.sel = g_chooser.count - 1;
    }
    ui_chooser_keep_selection_visible();
}

static void
ui_chooser_draw_title(WINDOW* win, const char* title, int body_w, int page_rows) {
    if (!win || !title) {
        return;
    }
    if (g_chooser.count > page_rows) {
        int first = g_chooser.top + 1;
        int last = g_chooser.top + page_rows;
        char title_line[256];
        if (last > g_chooser.count) {
            last = g_chooser.count;
        }
        DSD_SNPRINTF(title_line, sizeof title_line, "%s (%d-%d/%d)", title, first, last, g_chooser.count);
        mvwaddnstr(win, 1, 2, title_line, body_w);
        return;
    }
    mvwaddnstr(win, 1, 2, title, body_w);
}

static void
ui_chooser_draw_items(WINDOW* win, int w, int body_w, int page_rows) {
    if (!win) {
        return;
    }
    int y = 3;
    int drawn = 0;
    for (int i = g_chooser.top; i < g_chooser.count && drawn < page_rows; i++, drawn++) {
        mvwhline(win, y, 1, ' ', w - 2);
        if (i == g_chooser.sel) {
            wattron(win, A_REVERSE);
        }
        mvwaddnstr(win, y++, 2, g_chooser.items[i], body_w);
        if (i == g_chooser.sel) {
            wattroff(win, A_REVERSE);
        }
    }
    while (drawn < page_rows) {
        mvwhline(win, y++, 1, ' ', w - 2);
        drawn++;
    }
}

void
ui_chooser_render(void) {
    if (!g_chooser.active) {
        return;
    }
    if (g_chooser.count <= 0 || !g_chooser.items) {
        ui_chooser_finish(-1);
        return;
    }
    const char* title = g_chooser.title ? g_chooser.title : "Select";
    const char* footer = "Arrows/PgUp/PgDn  Right/Enter: select  Esc/q/Left";
    int max_item = ui_chooser_max_item_width();
    int h = 0, w = 0, wy = 0, wx = 0;
    if (!ui_chooser_compute_window_rect(title, footer, max_item, &h, &w, &wy, &wx)) {
        ui_chooser_finish(-1);
        return;
    }
    if (!g_chooser.win) {
        g_chooser.win = ui_make_window(h, w, wy, wx);
        if (!g_chooser.win) {
            ui_chooser_finish(-1);
            return;
        }
        keypad(g_chooser.win, TRUE);
        wtimeout(g_chooser.win, 0);
    }
    WINDOW* win = g_chooser.win;
    werase(win);
    box(win, 0, 0);
    int body_w = (w > 4) ? (w - 4) : 1;
    int page_rows = h - 5;
    if (page_rows < 1) {
        page_rows = 1;
    }
    g_chooser.page_rows = page_rows;
    ui_chooser_clamp_selection();
    ui_chooser_draw_title(win, title, body_w, page_rows);
    ui_chooser_draw_items(win, w, body_w, page_rows);
    mvwaddnstr(win, h - 2, 2, footer, body_w);
    wnoutrefresh(win);
}

#ifdef DSD_NEO_TEST_HOOKS
void
ui_chooser_test_set_page_rows(int page_rows) {
    if (page_rows < 0) {
        page_rows = 0;
    }
    g_chooser.page_rows = page_rows;
    ui_chooser_keep_selection_visible();
}

UiChooserTestSnapshot
ui_chooser_test_snapshot(void) {
    UiChooserTestSnapshot snapshot;
    snapshot.active = g_chooser.active;
    snapshot.count = g_chooser.count;
    snapshot.sel = g_chooser.sel;
    snapshot.top = g_chooser.top;
    snapshot.page_rows = g_chooser.page_rows;
    return snapshot;
}

void
ui_help_test_set_metrics(int line_count, int page_rows, int scroll) {
    if (line_count < 0) {
        line_count = 0;
    }
    if (page_rows < 0) {
        page_rows = 0;
    }
    g_help.line_count = line_count;
    g_help.page_rows = page_rows;
    g_help.scroll = scroll;
    ui_help_clamp_scroll(ui_help_max_scroll());
}

UiHelpTestSnapshot
ui_help_test_snapshot(void) {
    UiHelpTestSnapshot snapshot;
    snapshot.active = g_help.active;
    snapshot.scroll = g_help.scroll;
    snapshot.line_count = g_help.line_count;
    snapshot.page_rows = g_help.page_rows;
    return snapshot;
}

int
ui_help_wrap_line_for_test(const char* text, int width, int index, char* out, size_t out_size) {
    char lines[UI_HELP_MAX_LINES][UI_HELP_MAX_LINE_CHARS];
    int count = ui_help_wrap_text(text, width, lines, UI_HELP_MAX_LINES);
    if (out && out_size > 0) {
        out[0] = '\0';
        if (index >= 0 && index < count && index < UI_HELP_MAX_LINES) {
            DSD_SNPRINTF(out, out_size, "%s", lines[index]);
        }
    }
    return count;
}

int
ui_chooser_max_item_width_for_test(const char* const* items, int count) {
    const char* const* saved_items = g_chooser.items;
    int saved_count = g_chooser.count;
    g_chooser.items = items;
    g_chooser.count = count;
    int max_item = ui_chooser_max_item_width();
    g_chooser.items = saved_items;
    g_chooser.count = saved_count;
    return max_item;
}

int
ui_chooser_layout_for_test(const char* title, const char* footer, int max_item, int count, int screen_h, int screen_w,
                           int* h, int* w, int* wy, int* wx) {
    return ui_chooser_compute_window_rect_for_terminal(title, footer, max_item, count, screen_h, screen_w, h, w, wy,
                                                       wx);
}

int
ui_prompt_center_axis_for_test(int screen_extent, int window_extent) {
    return ui_center_axis(screen_extent, window_extent);
}

int
ui_prompt_fit_width_for_test(int desired_width, int screen_width) {
    return ui_prompt_fit_width(desired_width, screen_width);
}

int
ui_prompt_fit_height_for_test(int desired_height, int screen_height) {
    return ui_prompt_fit_height(desired_height, screen_height);
}

void
ui_prompt_rows_for_test(int height, int* title_y, int* input_y, int* footer_y) {
    ui_prompt_compute_row_positions(height, title_y, input_y, footer_y);
}

void
ui_prompt_field_geometry_for_test(int width, int* field_col, int* field_right, int* field_width) {
    ui_prompt_compute_field_geometry(width, field_col, field_right, field_width);
}

UiPromptViewTestSnapshot
ui_prompt_view_for_test(const char* text, size_t cursor, int field_col, int field_right, int field_width) {
    UiPromptViewState view = ui_prompt_compute_view_state(text, cursor, field_width);
    UiPromptViewTestSnapshot snapshot;
    snapshot.start = view.start;
    snapshot.cursor = view.cursor;
    snapshot.show_left_ellipsis = view.show_left_ellipsis;
    snapshot.cursor_x = ui_prompt_compute_cursor_x(field_col, field_right, &view);
    return snapshot;
}
#endif
