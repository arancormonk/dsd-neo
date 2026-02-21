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
#include <dsd-neo/platform/curses_compat.h>
#include <dsd-neo/ui/keymap.h>
#include <dsd-neo/ui/ui_prims.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---- Prompt overlay state ----
typedef struct {
    int active;
    const char* title;
    WINDOW* win;
    // string mode fields
    char* buf;
    size_t cap;
    size_t len;
    void (*on_done_str)(void* user, const char* text); // NULL text indicates cancel
    void* user;
} UiPrompt;

static UiPrompt g_prompt = {0};

// ---- Typed prompt context structures ----
typedef struct {
    void (*cb)(void*, int, int);
    void* user;
} PromptIntCtx;

typedef struct {
    void (*cb)(void*, int, double);
    void* user;
} PromptDblCtx;

// ---- Chooser overlay state ----
typedef struct {
    int active;
    const char* title;
    const char* const* items;
    int count;
    int sel;
    WINDOW* win;
    void (*on_done)(void* user, int sel);
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
    memcpy(lines[*count], src, (size_t)len);
    lines[*count][len] = '\0';
    (*count)++;
    return 1;
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

        while (*p == ' ') {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        if (*p == '\n') {
            continue;
        }

        const char* wstart = p;
        int wlen = 0;
        while (*p != '\0' && *p != ' ' && *p != '\n') {
            if (wlen < (UI_HELP_MAX_LINE_CHARS - 1)) {
                wlen++;
            }
            p++;
        }

        if (wlen <= 0) {
            continue;
        }

        if (cur_len > 0 && (cur_len + 1 + wlen) <= width) {
            cur[cur_len++] = ' ';
            memcpy(cur + cur_len, wstart, (size_t)wlen);
            cur_len += wlen;
            continue;
        }

        if (cur_len > 0) {
            ui_help_push_line(lines, max_lines, &out_count, cur, cur_len);
            cur_len = 0;
        }

        if (wlen <= width) {
            memcpy(cur, wstart, (size_t)wlen);
            cur_len = wlen;
            continue;
        }

        // Long unbreakable token: hard-wrap it to keep the overlay stable.
        int off = 0;
        while (off < wlen) {
            int chunk = width;
            if (chunk > wlen - off) {
                chunk = wlen - off;
            }
            if (!ui_help_push_line(lines, max_lines, &out_count, wstart + off, chunk)) {
                break;
            }
            off += chunk;
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
    (void)curs_set(0); // hide cursor when no prompt is active
    memset(&g_prompt, 0, sizeof(g_prompt));
}

void
ui_prompt_open_string_async(const char* title, const char* prefill, size_t cap,
                            void (*on_done)(void* user, const char* text), void* user) {
    ui_prompt_close_all();
    g_prompt.active = 1;
    g_prompt.title = title;
    g_prompt.on_done_str = on_done;
    g_prompt.user = user;
    if (cap < 2) {
        cap = 2;
    }
    g_prompt.buf = (char*)calloc(cap, 1);
    g_prompt.cap = cap;
    g_prompt.len = 0;
    if (!g_prompt.buf) {
        // Allocation failed: immediately signal cancel to ensure user context can be freed.
        if (on_done) {
            on_done(user, NULL);
        }
        g_prompt.active = 0;
        return;
    }
    if (prefill && *prefill) {
        strncpy(g_prompt.buf, prefill, cap - 1);
        g_prompt.buf[cap - 1] = '\0';
        g_prompt.len = strlen(g_prompt.buf);
    }
}

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

void
ui_prompt_open_int_async(const char* title, int initial, void (*cb)(void* user, int ok, int value), void* user) {
    char pre[64];
    snprintf(pre, sizeof pre, "%d", initial);
    PromptIntCtx* pic = (PromptIntCtx*)calloc(1, sizeof(PromptIntCtx));
    if (!pic) {
        // Allocation failed: immediately signal cancel so caller can clean up.
        if (cb) {
            cb(user, 0, 0);
        }
        return;
    }
    pic->cb = cb;
    pic->user = user;
    ui_prompt_open_string_async(title, pre, 64, ui_prompt_int_finish, pic);
}

void
ui_prompt_open_double_async(const char* title, double initial, void (*cb)(void* user, int ok, double value),
                            void* user) {
    char pre[64];
    snprintf(pre, sizeof pre, "%.6f", initial);
    PromptDblCtx* pdc = (PromptDblCtx*)calloc(1, sizeof(PromptDblCtx));
    if (!pdc) {
        // Allocation failed: immediately signal cancel so caller can clean up.
        if (cb) {
            cb(user, 0, 0.0);
        }
        return;
    }
    pdc->cb = cb;
    pdc->user = user;
    ui_prompt_open_string_async(title, pre, 64, ui_prompt_double_finish, pdc);
}

// ---- Prompt active/handle_key/render for menu_core delegation ----

int
ui_prompt_active(void) {
    return g_prompt.active;
}

int
ui_prompt_handle_key(int ch) {
    if (!g_prompt.active) {
        return 0;
    }
    if (ch == KEY_RESIZE) {
#if DSD_CURSES_NEEDS_EXPLICIT_RESIZE
        resize_term(0, 0);
#endif
        if (g_prompt.win) {
            delwin(g_prompt.win);
            g_prompt.win = NULL;
        }
        return 1;
    }
    if (ch == ERR) {
        return 1;
    }
    // Prompts must allow any printable characters (including 'q') so users can
    // type filenames like "iq.bin" without accidentally cancelling.
    if (ch == DSD_KEY_ESC) {
        void (*cb)(void*, const char*) = g_prompt.on_done_str;
        void* up = g_prompt.user;
        // Close first so callbacks can safely open a new prompt.
        g_prompt.on_done_str = NULL; // prevent close_all() from calling again
        ui_prompt_close_all();
        if (cb) {
            cb(up, NULL);
        }
        return 1;
    }
    if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
        if (g_prompt.len > 0) {
            g_prompt.buf[--g_prompt.len] = '\0';
        }
        return 1;
    }
    if (ch == 10 || ch == KEY_ENTER || ch == '\r') {
        void (*cb)(void*, const char*) = g_prompt.on_done_str;
        void* up = g_prompt.user;
        const int have_text = (g_prompt.len > 0 && g_prompt.buf != NULL);
        char* text_copy = NULL;
        if (have_text) {
            text_copy = (char*)malloc(g_prompt.len + 1);
            if (text_copy) {
                memcpy(text_copy, g_prompt.buf, g_prompt.len + 1);
            }
        }
        // Close first so callbacks can safely open a new prompt.
        g_prompt.on_done_str = NULL; // prevent close_all() from calling again
        ui_prompt_close_all();
        if (cb) {
            cb(up, have_text ? text_copy : "");
        }
        free(text_copy);
        return 1;
    }
    if (isprint(ch)) {
        if (g_prompt.len + 1 < g_prompt.cap) {
            g_prompt.buf[g_prompt.len++] = (char)ch;
            g_prompt.buf[g_prompt.len] = '\0';
        }
        return 1;
    }
    return 1;
}

void
ui_prompt_render(void) {
    if (!g_prompt.active) {
        return;
    }
    const char* title = g_prompt.title ? g_prompt.title : "Input";
    int h = 8;
    int w = (int)strlen(title) + 16;
    if (w < 54) {
        w = 54;
    }
    int scr_h = 0, scr_w = 0;
    getmaxyx(stdscr, scr_h, scr_w);
    if (scr_h < 4 || scr_w < 8) {
        return;
    }
    int max_w = scr_w - 2;
    if (max_w < 10) {
        // On narrow terminals, relax side margins before forcing a larger minimum width.
        max_w = scr_w;
    }
    if (max_w < 4) {
        return;
    }
    if (w > max_w) {
        w = max_w;
    }
    if (w < 10 && max_w >= 10) {
        w = 10;
    }

    int max_h = scr_h - 2;
    if (max_h < 6) {
        // On short terminals, relax vertical margins before forcing a larger minimum height.
        max_h = scr_h;
    }
    if (max_h < 3) {
        return;
    }
    if (h > max_h) {
        h = max_h;
    }
    if (h < 6 && max_h >= 6) {
        h = 6;
    }
    if (h < 3) {
        h = 3;
    }
    int py = (scr_h - h) / 2;
    int px = (scr_w - w) / 2;
    if (py < 0) {
        py = 0;
    }
    if (px < 0) {
        px = 0;
    }
    if (!g_prompt.win) {
        g_prompt.win = ui_make_window(h, w, py, px);
        if (!g_prompt.win) {
            return;
        }
        wtimeout(g_prompt.win, 0);
    }
    WINDOW* win = g_prompt.win;
    (void)curs_set(1); // show cursor while editing prompt text
    werase(win);
    box(win, 0, 0);

    int interior_rows = h - 2;
    int title_y = -1;
    int input_y = 1;
    int footer_y = -1;
    if (interior_rows >= 4) {
        title_y = 1;
        input_y = 3;
        footer_y = h - 2;
    } else if (interior_rows == 3) {
        title_y = 1;
        input_y = 2;
        footer_y = h - 2;
    } else if (interior_rows == 2) {
        title_y = 1;
        input_y = 2;
    } else {
        input_y = 1;
    }
    int body_w = (w > 4) ? (w - 4) : 1;
    if (title_y > 0) {
        mvwaddnstr(win, title_y, 2, title, body_w);
    }

    const char* text = g_prompt.buf ? g_prompt.buf : "";
    int field_col = 4; // after "> " in normal-width prompts
    int field_right = w - 2;
    if (field_col > field_right) {
        field_col = field_right;
    }
    if (field_col < 2) {
        field_col = 2;
    }
    int field_width = field_right - field_col;
    if (field_width < 1) {
        field_width = 1;
    }
    size_t text_len = strlen(text);
    size_t start = 0;
    int show_left_ellipsis = 0;
    int visible_chars = field_width;
    if ((int)text_len > field_width && field_width >= 4) {
        show_left_ellipsis = 1;
        visible_chars = field_width - 3;
    }
    if ((int)text_len > visible_chars) {
        start = text_len - (size_t)visible_chars;
    }
    mvwaddnstr(win, input_y, 2, "> ", (w > 5) ? 2 : 1);
    if (show_left_ellipsis) {
        mvwaddnstr(win, input_y, field_col, "...", 3);
        mvwaddnstr(win, input_y, field_col + 3, text + start, visible_chars);
    } else {
        mvwaddnstr(win, input_y, field_col, text + start, field_width);
    }
    int cursor_prefix = show_left_ellipsis ? 3 : 0;
    int cursor_x = field_col + cursor_prefix + (int)(text_len - start);
    if (cursor_x > field_right) {
        cursor_x = field_right;
    }
    if (cursor_x < 2) {
        cursor_x = 2;
    }
    wmove(win, input_y, cursor_x);
    if (footer_y > 0 && footer_y != input_y) {
        mvwaddnstr(win, footer_y, 2, "Enter=OK  Esc=Cancel", body_w);
    }
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
    (void)curs_set(0);
    memset(&g_help, 0, sizeof(g_help));
}

int
ui_help_active(void) {
    return g_help.active;
}

int
ui_help_handle_key(int ch) {
    if (!g_help.active) {
        return 0;
    }
    if (ch == KEY_RESIZE) {
#if DSD_CURSES_NEEDS_EXPLICIT_RESIZE
        resize_term(0, 0);
#endif
        if (g_help.win) {
            delwin(g_help.win);
            g_help.win = NULL;
        }
        return 1;
    }
    int max_scroll = 0;
    if (g_help.line_count > g_help.page_rows && g_help.page_rows > 0) {
        max_scroll = g_help.line_count - g_help.page_rows;
    }
    int page_step = (g_help.page_rows > 1) ? (g_help.page_rows - 1) : 1;
    if (ch != ERR) {
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
        if (ch == DSD_KEY_ESC || ch == 'q' || ch == 'Q' || ch == 'h' || ch == 'H' || ch == 10 || ch == KEY_ENTER
            || ch == '\r') {
            ui_help_close();
            return 1;
        }
        // Keep previous ergonomics: any non-navigation key closes help.
        ui_help_close();
    }
    return 1;
}

void
ui_help_render(void) {
    if (!g_help.active) {
        return;
    }
    const char* t = g_help.text ? g_help.text : "";
    int h = 14;
    int w = 68;
    int scr_h = 0, scr_w = 0;
    getmaxyx(stdscr, scr_h, scr_w);
    if (scr_h < 4 || scr_w < 8) {
        ui_help_close();
        return;
    }
    int max_w = scr_w - 2;
    int max_h = scr_h - 2;
    if (max_w < 10 || max_h < 6) {
        // Overlay cannot be rendered at its minimum usable size.
        ui_help_close();
        return;
    }
    if (w > max_w) {
        w = max_w;
    }
    if (w < 30) {
        w = max_w;
    }
    if (h > max_h) {
        h = max_h;
    }
    if (h < 6) {
        h = 6;
    }
    int hy = (scr_h - h) / 2;
    int hx = (scr_w - w) / 2;
    if (hy < 0) {
        hy = 0;
    }
    if (hx < 0) {
        hx = 0;
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
    const int body_w = (w > 4) ? (w - 4) : 1;
    const int page_rows = (h > 4) ? (h - 4) : 1;
    if (page_rows < 1) {
        return;
    }

    char lines[UI_HELP_MAX_LINES][UI_HELP_MAX_LINE_CHARS];
    int line_count = ui_help_wrap_text(t, body_w, lines, UI_HELP_MAX_LINES);
    if (line_count < 1) {
        line_count = 1;
    }
    g_help.line_count = line_count;
    g_help.page_rows = page_rows;

    int max_scroll = (line_count > page_rows) ? (line_count - page_rows) : 0;
    if (max_scroll < 0) {
        max_scroll = 0;
    }
    if (g_help.scroll < 0) {
        g_help.scroll = 0;
    }
    if (g_help.scroll > max_scroll) {
        g_help.scroll = max_scroll;
    }

    int first = g_help.scroll;
    int last = first + page_rows;
    if (last > line_count) {
        last = line_count;
    }

    if (max_scroll > 0) {
        mvwprintw(hw, 1, 2, "Help (%d-%d/%d)", first + 1, last, line_count);
    } else {
        mvwprintw(hw, 1, 2, "Help");
    }

    int y = 2;
    for (int i = first; i < last && y <= (h - 3); i++, y++) {
        if (i < 0 || i >= line_count || i >= UI_HELP_MAX_LINES) {
            break;
        }
        mvwaddnstr(hw, y, 2, lines[i], body_w);
    }

    if (max_scroll > 0) {
        mvwaddnstr(hw, h - 2, 2, "Up/Down/PgUp/PgDn: scroll  Esc/q: close", body_w);
    } else {
        mvwaddnstr(hw, h - 2, 2, "Esc/q/Enter: close", body_w);
    }
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

void
ui_chooser_start(const char* title, const char* const* items, int count, void (*on_done)(void*, int), void* user) {
    if (!items || count <= 0) {
        ui_chooser_close();
        if (on_done) {
            on_done(user, -1);
        }
        return;
    }
    g_chooser.active = 1;
    g_chooser.title = title;
    g_chooser.items = items;
    g_chooser.count = count;
    g_chooser.sel = 0;
    g_chooser.on_done = on_done;
    g_chooser.user = user;
    if (g_chooser.win) {
        delwin(g_chooser.win);
        g_chooser.win = NULL;
    }
}

void
ui_chooser_close(void) {
    if (g_chooser.win) {
        delwin(g_chooser.win);
        g_chooser.win = NULL;
    }
    (void)curs_set(0);
    memset(&g_chooser, 0, sizeof(g_chooser));
}

int
ui_chooser_active(void) {
    return g_chooser.active;
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
#if DSD_CURSES_NEEDS_EXPLICIT_RESIZE
        resize_term(0, 0);
#endif
        if (g_chooser.win) {
            delwin(g_chooser.win);
            g_chooser.win = NULL;
        }
        return 1;
    }
    if (ch == KEY_UP) {
        g_chooser.sel = (g_chooser.sel - 1 + g_chooser.count) % g_chooser.count;
        return 1;
    }
    if (ch == KEY_DOWN) {
        g_chooser.sel = (g_chooser.sel + 1) % g_chooser.count;
        return 1;
    }
    if (ch == 'q' || ch == 'Q' || ch == DSD_KEY_ESC) {
        ui_chooser_finish(-1);
        return 1;
    }
    if (ch == 10 || ch == KEY_ENTER || ch == '\r') {
        int sel = g_chooser.sel;
        ui_chooser_finish(sel);
        return 1;
    }
    return 1;
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
    int max_item = 0;
    for (int i = 0; i < g_chooser.count; i++) {
        int L = (int)strlen(g_chooser.items[i]);
        if (L > max_item) {
            max_item = L;
        }
    }
    const char* footer = "Arrows = Move   Enter = Select   Esc/q = Cancel";
    int w = 4 + (int)strlen(title);
    int need = 4 + max_item;
    if (need > w) {
        w = need;
    }
    need = 4 + (int)strlen(footer);
    if (need > w) {
        w = need;
    }
    w += 2;
    int h = g_chooser.count + 5;
    if (h < 7) {
        h = 7;
    }
    int scr_h = 0, scr_w = 0;
    getmaxyx(stdscr, scr_h, scr_w);
    if (scr_h < 4 || scr_w < 8) {
        ui_chooser_finish(-1);
        return;
    }
    int max_w = scr_w - 2;
    int max_h = scr_h - 2;
    if (max_w < 10 || max_h < 6) {
        // Overlay cannot be rendered at its minimum usable size.
        ui_chooser_finish(-1);
        return;
    }
    if (w > max_w) {
        w = max_w;
    }
    if (w < 10) {
        w = 10;
    }
    if (h > max_h) {
        h = max_h;
    }
    if (h < 6) {
        h = 6;
    }
    int wy = (scr_h - h) / 2;
    int wx = (scr_w - w) / 2;
    if (wy < 0) {
        wy = 0;
    }
    if (wx < 0) {
        wx = 0;
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
    mvwprintw(win, 1, 2, "%s", title);
    int y = 3;
    for (int i = 0; i < g_chooser.count; i++) {
        if (i == g_chooser.sel) {
            wattron(win, A_REVERSE);
        }
        mvwprintw(win, y++, 2, "%s", g_chooser.items[i]);
        if (i == g_chooser.sel) {
            wattroff(win, A_REVERSE);
        }
    }
    mvwprintw(win, h - 2, 2, "%s", footer);
    wnoutrefresh(win);
}
