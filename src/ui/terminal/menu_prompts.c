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
#include <dsd-neo/core/parse.h>
#include <dsd-neo/core/string_utils.h>
#include <dsd-neo/platform/curses_compat.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/ui/keymap.h>
#include <dsd-neo/ui/ui_prims.h>
#include <float.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/platform/platform.h"
#include "dsd-neo/ui/menu_core.h"
#include "menu_internal.h"
#include "ui_keys.h"
#include "ui_text.h"

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
    ui_context_dispose_fn dispose; // owned prompt: frees user once the prompt is done with it
    int mask;                      // 1: render '*' per byte, and scrub buf + the submit copy
} UiPrompt;

static UiPrompt g_prompt = {0};

/*
 * The owned context of the prompt or chooser whose done-callback is running (issue #672, B8). The callback may take it
 * (ui_prompt_take_context(), ui_chooser_take_context()); otherwise it is disposed once the callback returns. A callback
 * whose next widget answers synchronously (an empty list, a failed allocation) runs that widget's callback inside its
 * own, so entering saves the outer record by value and leaving puts it back.
 */
typedef struct {
    void* user;
    ui_context_dispose_fn dispose;
} UiPendingContext;

static UiPendingContext g_prompt_pending;
static UiPendingContext g_chooser_pending;

static UiPendingContext
ui_pending_enter(UiPendingContext* slot, void* user, ui_context_dispose_fn dispose) {
    const UiPendingContext outer = *slot;
    slot->user = user;
    slot->dispose = dispose;
    return outer;
}

/* Put the outer record back, then dispose this callback's context unless the callback took it. */
static void
ui_pending_leave(UiPendingContext* slot, UiPendingContext outer) {
    const UiPendingContext mine = *slot;
    *slot = outer;
    if (mine.dispose) {
        mine.dispose(mine.user);
    }
}

static void*
ui_pending_take(UiPendingContext* slot) {
    if (!slot->dispose) {
        return NULL;
    }
    void* user = slot->user;
    slot->user = NULL;
    slot->dispose = NULL;
    return user;
}

#ifdef DSD_NEO_TEST_HOOKS
static int g_chooser_fail_next_start;
static int g_prompt_fail_next_alloc;
#endif

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
    ui_context_dispose_fn dispose; // owned chooser: frees user once the chooser is done with it
    /* Measured once per start (issue #672, B7) under one output decision, so the window width is not recomputed over
       every item each frame: cells, not bytes. */
    int unicode;
    int title_cells;
    int max_item_cells;
} UiChooser;

static UiChooser g_chooser = {0};

static const char k_chooser_default_title[] = "Select";

enum {
    /* Bytes of one item or title the chooser draws; text past it is dropped (a line this long is cut on any terminal),
       as the status row drops text past UI_ROW_TEXT_MAX. */
    UI_CHOOSER_TEXT_MAX = 768,
};

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

/**
 * @brief Overwrite a buffer in a way the optimizer may not discard.
 *
 * DSD_MEMSET is __builtin_memset and is a dead store here by definition, so a
 * compiler is free to delete it; this tree has no explicit_bzero or memset_s.
 * The volatile pointer is what makes the writes observable. This mirrors
 * rr_secure_zero() in src/runtime/radioreference/rr_client.c, which is static
 * to that translation unit and cannot be shared.
 *
 * @param p Buffer to clear.
 * @param n Length in bytes.
 */
#ifdef DSD_NEO_TEST_HOOKS
/* Bytes the last close zeroed in the input buffer (ui_prompt_close_all()). */
static size_t g_prompt_zeroed_on_close;
#endif

#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
static void
ui_prompt_secure_zero(void* p, size_t n) {
    volatile unsigned char* v = (volatile unsigned char*)p;
    while (n-- > 0U) {
        *v = 0;
        v++;
    }
}

/* Run a prompt's done-callback, then dispose its owned context unless the callback took it. */
static void
ui_prompt_deliver(ui_prompt_string_done_fn cb, void* user, ui_context_dispose_fn dispose, const char* text) {
    const UiPendingContext outer = ui_pending_enter(&g_prompt_pending, user, dispose);
    if (cb) {
        cb(user, text);
    }
    ui_pending_leave(&g_prompt_pending, outer);
}

void*
ui_prompt_take_context(void) {
    return ui_pending_take(&g_prompt_pending);
}

void
ui_prompt_close_all(void) {
    // An active prompt closed without an explicit completion gets a cancel, so its context is released. Delivered
    // after the prompt is gone (below), so the callback may open another prompt without this close discarding it.
    const int deliver = g_prompt.active;
    const ui_prompt_string_done_fn cb = g_prompt.on_done_str;
    void* up = g_prompt.user;
    const ui_context_dispose_fn dispose = g_prompt.dispose;

    if (g_prompt.win) {
        delwin(g_prompt.win);
        g_prompt.win = NULL;
    }
    if (g_prompt.buf) {
        /* Every prompt, masked or not: the environment editor's value prompt shows its text, yet the value can be a
           key or token (#782). One cap-sized pass per close. */
        ui_prompt_secure_zero(g_prompt.buf, g_prompt.cap);
#ifdef DSD_NEO_TEST_HOOKS
        g_prompt_zeroed_on_close = g_prompt.cap;
#endif
        free(g_prompt.buf);
        g_prompt.buf = NULL;
    }
    (void)ui_prompt_curs_set(0); // hide cursor when no prompt is active
    DSD_MEMSET(&g_prompt, 0, sizeof(g_prompt));
    if (deliver) {
        ui_prompt_deliver(cb, up, dispose, NULL);
    }
}

enum {
    /* A replaced prompt's cancel may open a prompt of its own; a few closes clear what that opens too. */
    UI_PROMPT_REPLACE_PASSES = 4,
};

/* Close whatever prompt is open before another opens, and whatever its cancel opened in turn. */
static void
ui_prompt_close_for_open(void) {
    for (int pass = 0; pass < UI_PROMPT_REPLACE_PASSES && g_prompt.active; pass++) {
        ui_prompt_close_all();
    }
}

static char*
ui_prompt_alloc_buffer(size_t cap) {
#ifdef DSD_NEO_TEST_HOOKS
    if (g_prompt_fail_next_alloc) {
        g_prompt_fail_next_alloc = 0;
        return NULL;
    }
#endif
    return (char*)calloc(cap, 1);
}

// Cppcheck 2.21 loses the final prototype name after a callback typedef parameter.
// cppcheck-suppress-begin funcArgNamesDifferentUnnamed
void
ui_prompt_open_string_owned(const char* title, const char* prefill, size_t cap, ui_prompt_string_done_fn on_done,
                            void* user_ctx, ui_context_dispose_fn dispose) {
    ui_prompt_close_for_open();
    if (cap < 2) {
        cap = 2;
    }
    char* buf = ui_prompt_alloc_buffer(cap);
    if (!buf) {
        // Allocation failed: answer cancel at once, so the user context is released.
        ui_prompt_deliver(on_done, user_ctx, dispose, NULL);
        return;
    }
    g_prompt.active = 1;
    g_prompt.title = title;
    g_prompt.on_done_str = on_done;
    g_prompt.user = user_ctx;
    g_prompt.dispose = dispose;
    g_prompt.buf = buf;
    g_prompt.cap = cap;
    g_prompt.len = 0;
    if (prefill && *prefill) {
        dsd_strncpy_s(g_prompt.buf, cap, prefill, cap - 1);
        g_prompt.buf[cap - 1] = '\0';
        g_prompt.len = strlen(g_prompt.buf);
        g_prompt.cursor = g_prompt.len;
    }
}

void
ui_prompt_open_string_async(const char* title, const char* prefill, size_t cap, ui_prompt_string_done_fn on_done,
                            void* user_ctx) {
    ui_prompt_open_string_owned(title, prefill, cap, on_done, user_ctx, NULL);
}

// cppcheck-suppress-end funcArgNamesDifferentUnnamed

// Cppcheck 2.21 loses the final prototype name after a callback typedef parameter.
// cppcheck-suppress-begin funcArgNamesDifferentUnnamed
void
ui_prompt_open_secret_async(const char* title, size_t cap, ui_prompt_string_done_fn on_done, void* user_ctx) {
    // No prefill: a secret is never redisplayed. The open zeroes g_prompt first
    // (ui_prompt_open_string_async starts with ui_prompt_close_all), so setting
    // mask afterwards is what survives.
    ui_prompt_open_string_async(title, NULL, cap, on_done, user_ctx);
    if (ui_prompt_active()) {
        g_prompt.mask = 1;
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
    // Out of range is refused, never wrapped or saturated: 5145979796 must not become 851012500.
    int v = 0;
    if (dsd_parse_int_strict(text, 10, INT_MIN, INT_MAX, &v) != 0) {
        ui_statusf("Invalid integer input");
        if (pic->cb) {
            pic->cb(pic->user, 0, 0);
        }
    } else {
        if (pic->cb) {
            pic->cb(pic->user, 1, v);
        }
    }
    free(pic);
}

/* [+-]?digits[.digits]([eE][+-]?digits)? with at least one mantissa digit. */
static int
ui_prompt_decimal_text_ok(const char* t) {
    size_t i = 0;
    size_t digits = 0;
    if (t[i] == '+' || t[i] == '-') {
        i++;
    }
    while (isdigit((unsigned char)t[i])) {
        i++;
        digits++;
    }
    if (t[i] == '.') {
        i++;
        while (isdigit((unsigned char)t[i])) {
            i++;
            digits++;
        }
    }
    if (digits == 0) {
        return 0;
    }
    if (t[i] == 'e' || t[i] == 'E') {
        i++;
        if (t[i] == '+' || t[i] == '-') {
            i++;
        }
        size_t exp_digits = 0;
        while (isdigit((unsigned char)t[i])) {
            i++;
            exp_digits++;
        }
        if (exp_digits == 0) {
            return 0;
        }
    }
    return t[i] == '\0';
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
    double v = 0.0;
    /* The lexical gate comes first: strtod() also reads nan, inf and hex floats, and the release presets build with
       fast-math, where a NaN comparison inside the range check cannot be trusted. */
    if (!ui_prompt_decimal_text_ok(text) || dsd_parse_double_strict(text, -DBL_MAX, DBL_MAX, &v) != 0) {
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

/* Esc: the close delivers the cancel, after the prompt is gone, so the callback can safely open a new prompt. */
static void
ui_prompt_cancel(void) {
    ui_prompt_close_all();
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
    const ui_prompt_string_done_fn cb = g_prompt.on_done_str;
    void* up = g_prompt.user;
    const ui_context_dispose_fn dispose = g_prompt.dispose;
    const int have_text = (g_prompt.len > 0 && g_prompt.buf != NULL);
    // Captured before close_all() zeroes g_prompt: the heap copy below outlives it.
    const size_t copy_len = g_prompt.len;
    char* text_copy = NULL;
    if (have_text) {
        text_copy = (char*)malloc(g_prompt.len + 1);
        if (text_copy) {
            DSD_MEMCPY(text_copy, g_prompt.buf, g_prompt.len + 1);
        }
    }
    // Close first so callbacks can safely open a new prompt; the close delivers nothing (this submit does).
    g_prompt.active = 0;
    ui_prompt_close_all();
    ui_prompt_deliver(cb, up, dispose, have_text ? text_copy : "");
    // Zeroed for every prompt, as the input buffer is: a plain prompt can carry a key or token too.
    if (text_copy) {
        ui_prompt_secure_zero(text_copy, copy_len + 1);
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

/* The character a keypad operator key (PDCurses PADPLUS, PADMINUS, PADSTAR, PADSLASH) types, or -1 for any other
 * key. */
static int
ui_prompt_keypad_operator_byte(int ch) {
#ifdef PADPLUS
    if (ch == PADPLUS) {
        return '+';
    }
#endif
#ifdef PADMINUS
    if (ch == PADMINUS) {
        return '-';
    }
#endif
#ifdef PADSTAR
    if (ch == PADSTAR) {
        return '*';
    }
#endif
#ifdef PADSLASH
    if (ch == PADSLASH) {
        return '/';
    }
#endif
    (void)ch;
    return -1;
}

/* The byte a key 0x80-0xFF adds, or -1 for none. PDCurses and a single-byte locale deliver each byte as a whole
 * character: itself when printable, else one '?'. In a multibyte (UTF-8) locale ncurses delivers a character one
 * byte at a time, so a continuation byte 0x80-0xBF adds nothing and any other byte (a lead byte, or a byte that
 * cannot start a sequence) adds one '?'. The locale kind decides, not isprint(): macOS calls 0xA0-0xFF printable in
 * a UTF-8 locale, which would insert the raw bytes of a character. */
static int
ui_prompt_high_byte_for_key(int ch) {
#if !defined(DSD_USE_PDCURSES)
    if (MB_CUR_MAX > 1) {
        return (ch >= 0x80 && ch <= 0xBF) ? -1 : '?';
    }
#endif
    return isprint(ch) ? ch : '?';
}

/**
 * @brief The byte a typed key adds to a prompt, or -1 for a key that is not a character.
 *
 * Nothing typed is lost silently: a character the field cannot hold adds one '?'. In a UTF-8 locale ncurses delivers
 * a character one byte at a time, so a lead byte stands for the whole character and its continuation bytes add
 * nothing; PDCurses and a single-byte locale deliver each byte 0x80-0xFF as a whole character, and a wide PDCurses
 * build delivers anything wider as UI_KEY_WIDE_CHAR. Function keys, ncurses extended keys above KEY_MAX and
 * KEY_MOUSE are not characters. isprint() only ever sees 0..0xFF.
 */
static int
ui_prompt_byte_for_key(int ch) {
    if (ch == UI_KEY_WIDE_CHAR) {
        return '?';
    }
    if (ch >= 0x20 && ch <= 0x7E) {
        return ch;
    }
    if (ch < 0x80 || ch > 0xFF) {
        return ui_prompt_keypad_operator_byte(ch); // PDCurses keypad key codes sit above 0xFF
    }
    return ui_prompt_high_byte_for_key(ch);
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
    if (ui_key_is_enter(ch)) {
        ui_prompt_submit();
        return 1;
    }
    int byte = ui_prompt_byte_for_key(ch);
    if (byte >= 0) {
        ui_prompt_insert_char(byte);
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

#define UI_PROMPT_MASK_MAX 512 /* asterisks drawn in one field; cosmetic ceiling, never a leak */

/**
 * @brief Write min(n, out_size - 1) '*' bytes plus a NUL into @p out.
 */
static void
ui_prompt_fill_mask(char* out, size_t out_size, size_t n) {
    if (!out || out_size == 0) {
        return;
    }
    if (n > out_size - 1) {
        n = out_size - 1;
    }
    for (size_t i = 0; i < n; i++) {
        out[i] = '*';
    }
    out[n] = '\0';
}

/**
 * @brief Pick the string ui_prompt_render() draws into the input field.
 *
 * The view state is computed from the REAL buffer by the caller, so start,
 * ellipsis and cursor column are identical masked or not; only the glyphs
 * change here.
 *
 * @param text        Real prompt text (never NULL).
 * @param view        View state computed from @p text.
 * @param field_width Field width in columns.
 * @param masked      Scratch buffer used only when masking is active.
 * @param masked_size Size of @p masked in bytes.
 * @return Pointer to draw from: either into @p text or into @p masked.
 */
static const char*
ui_prompt_display_slice(const char* text, const UiPromptViewState* view, int field_width, char* masked,
                        size_t masked_size) {
    const char* shown = text + view->start;
    if (!g_prompt.mask) {
        return shown;
    }
    int width = view->show_left_ellipsis ? (field_width - 3) : field_width;
    if (width < 0) {
        width = 0;
    }
    size_t n = strlen(shown);
    if (n > (size_t)width) {
        n = (size_t)width;
    }
    ui_prompt_fill_mask(masked, masked_size, n);
    return masked;
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
    char masked[UI_PROMPT_MASK_MAX];
    const char* shown = ui_prompt_display_slice(text, &view, field_width, masked, sizeof masked);

    (void)ui_prompt_curs_set(1); // show cursor while editing prompt text
    werase(win);
    box(win, 0, 0);
    if (title_y > 0) {
        mvwaddnstr(win, title_y, 2, title, body_w);
    }
    mvwaddnstr(win, input_y, 2, "> ", (w > 5) ? 2 : 1);
    if (view.show_left_ellipsis) {
        mvwaddnstr(win, input_y, field_col, "...", 3);
        mvwaddnstr(win, input_y, field_col + 3, shown, field_width - 3);
    } else {
        mvwaddnstr(win, input_y, field_col, shown, field_width);
    }
    if (footer_y > 0 && footer_y != input_y) {
        mvwaddnstr(win, footer_y, 2, "Enter=OK  Esc=Cancel", body_w);
    }
    // The toast, in the blank rows between the field and the footer. A prompt
    // that re-opens after refusing its input ("Enter a ZIP code (digits only).")
    // is the only thing on screen, so this is the one place the refusal can be
    // read; anchored above the footer so a blank row still separates it from
    // the field.
    if (footer_y > input_y + 1) {
        (void)ui_status_draw(win, input_y + 1, 2, body_w, footer_y - input_y - 1,
                             UI_STATUS_FLAG_ANCHOR_BOTTOM | UI_STATUS_FLAG_BOLD, dsd_realtime_time());
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
    /* Esc, Left, Enter and h close help; 'q' is deliberately not a close key
       anywhere inside the menu, because it quits the program from the main
       screen and the habit carried over. */
    return (ch == DSD_KEY_ESC || ch == 'h' || ch == 'H' || ui_key_is_enter(ch) || ch == KEY_LEFT);
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
        mvwaddnstr(hw, h - 2, 2, "Up/Down/PgUp/PgDn: scroll  Esc/h: close", body_w);
    } else {
        mvwaddnstr(hw, h - 2, 2, "Esc/h/Enter: close", body_w);
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

/* Detach, close, call back, then dispose an owned context the callback did not take. */
static void
ui_chooser_finish(int sel) {
    const ui_chooser_done_fn cb = g_chooser.on_done;
    void* userp = g_chooser.user;
    const ui_context_dispose_fn dispose = g_chooser.dispose;
    g_chooser.on_done = NULL; // prevent double-callback during close/reentry
    g_chooser.dispose = NULL; // the close below disposes nothing; this finish does, after the callback
    ui_chooser_close();
    const UiPendingContext outer = ui_pending_enter(&g_chooser_pending, userp, dispose);
    if (cb) {
        cb(userp, sel);
    }
    ui_pending_leave(&g_chooser_pending, outer);
}

void*
ui_chooser_take_context(void) {
    return ui_pending_take(&g_chooser_pending);
}

static int
ui_chooser_start_fails(const char* const* items, int count) {
#ifdef DSD_NEO_TEST_HOOKS
    if (g_chooser_fail_next_start) {
        g_chooser_fail_next_start = 0;
        return 1;
    }
#endif
    return !items || count <= 0;
}

static int
ui_chooser_text_cells(const char* text) {
    return text ? ui_text_cells(g_chooser.unicode, text, strlen(text)) : 0;
}

static int
ui_chooser_max_item_width(void) {
    int max_item = 0;
    for (int i = 0; i < g_chooser.count; i++) {
        int item_cells = ui_chooser_text_cells(g_chooser.items[i]);
        if (item_cells > max_item) {
            max_item = item_cells;
        }
    }
    return max_item;
}

static void
ui_chooser_start_impl(const char* title, const char* const* items, int count, int initial_sel,
                      ui_chooser_done_fn on_done, void* user_ctx, ui_context_dispose_fn dispose) {
    /* A start over an open owned chooser replaces it, and the close disposes its context - unless this start is
       handed the same context, which then stays alive under this start's terms. */
    if (g_chooser.dispose && g_chooser.user == user_ctx) {
        g_chooser.dispose = NULL;
    }
    ui_chooser_close();
    if (ui_chooser_start_fails(items, count)) {
        // An empty list cancels straight away, but it still leaves by the one
        // door every other chooser result uses: publish the callback into
        // g_chooser and let ui_chooser_finish() deliver it. That keeps the
        // reentry guard (on_done cleared before the callback runs) on this path
        // too, and it keeps the callback's context reachable only through the
        // chooser state that pairs it with its callback - never handed straight
        // from one caller's argument list to whatever function pointer the
        // caller passed in.
        g_chooser.on_done = on_done;
        g_chooser.user = user_ctx;
        g_chooser.dispose = dispose;
        ui_chooser_finish(-1);
        return;
    }
    g_chooser.active = 1;
    g_chooser.title = title;
    g_chooser.items = items;
    g_chooser.count = count;
    g_chooser.sel = (initial_sel > 0 && initial_sel < count) ? initial_sel : 0;
    g_chooser.top = 0;
    g_chooser.page_rows = 0;
    g_chooser.on_done = on_done;
    g_chooser.user = user_ctx;
    g_chooser.dispose = dispose;
    g_chooser.unicode = ui_text_unicode_output();
    g_chooser.title_cells = ui_chooser_text_cells(title ? title : k_chooser_default_title);
    g_chooser.max_item_cells = ui_chooser_max_item_width();
}

// Cppcheck 2.21 loses the final prototype name after a callback typedef parameter.
// cppcheck-suppress-begin funcArgNamesDifferentUnnamed
void
ui_chooser_start_owned(const char* title, const char* const* items, int count, int initial_sel,
                       ui_chooser_done_fn on_done, void* user_ctx, ui_context_dispose_fn dispose) {
    ui_chooser_start_impl(title, items, count, initial_sel, on_done, user_ctx, dispose);
}

void
ui_chooser_start_at(const char* title, const char* const* items, int count, int initial_sel, ui_chooser_done_fn on_done,
                    void* user_ctx) {
    ui_chooser_start_impl(title, items, count, initial_sel, on_done, user_ctx, NULL);
}

void
ui_chooser_start(const char* title, const char* const* items, int count, ui_chooser_done_fn on_done, void* user_ctx) {
    ui_chooser_start_at(title, items, count, 0, on_done, user_ctx);
}

// cppcheck-suppress-end funcArgNamesDifferentUnnamed

void
ui_chooser_close(void) {
    void* user = g_chooser.user;
    const ui_context_dispose_fn dispose = g_chooser.dispose;
    if (g_chooser.win) {
        delwin(g_chooser.win);
        g_chooser.win = NULL;
    }
    (void)ui_prompt_curs_set(0);
    DSD_MEMSET(&g_chooser, 0, sizeof(g_chooser));
    /* Closed without an answer: no callback, and an owned context goes now that nothing reads its strings. */
    if (dispose) {
        dispose(user);
    }
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
    /* Esc and Left only -- see ui_help_is_close_key() for why 'q' is not. */
    return (ch == DSD_KEY_ESC || ch == KEY_LEFT);
}

static int
ui_chooser_is_accept_key(int ch) {
    return (ui_key_is_enter(ch) || ch == KEY_RIGHT);
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
ui_chooser_initial_width(int title_cells, const char* footer, int max_item) {
    int width = 4 + title_cells;
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
ui_chooser_compute_window_rect_for_terminal(int title_cells, const char* footer, int max_item, int count, int screen_h,
                                            int screen_w, int* h, int* w, int* wy, int* wx) {
    if (!footer || !h || !w || !wy || !wx) {
        return 0;
    }
    int local_w = ui_chooser_initial_width(title_cells, footer, max_item);
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
ui_chooser_compute_window_rect(const char* footer, int* h, int* w, int* wy, int* wx) {
    int scr_h = 0, scr_w = 0;
    getmaxyx(stdscr, scr_h, scr_w);
    return ui_chooser_compute_window_rect_for_terminal(g_chooser.title_cells, footer, g_chooser.max_item_cells,
                                                       g_chooser.count, scr_h, scr_w, h, w, wy, wx);
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

/*
 * Draw text at (y, x) in at most @p budget cells (issue #672, B7): sanitized, measured and cut between code points by
 * the status row's text model (ui_text.h), with its marker when anything was cut, so a long name never splits a UTF-8
 * character and never loses its end without saying so. What reaches curses is the byte span of the cut.
 */
static void
ui_chooser_draw_text(WINDOW* win, int y, int x, const char* text, int budget) {
    char shown[UI_CHOOSER_TEXT_MAX];
    const char* src = text ? text : "";
    const size_t len = ui_text_sanitize(g_chooser.unicode, src, strnlen(src, sizeof shown), shown, sizeof shown);
    const char* marker = NULL;
    const size_t cut = ui_text_fit(g_chooser.unicode, shown, len, budget, &marker);
    wmove(win, y, x);
    if (cut > 0U) {
        waddnstr(win, shown, (int)cut);
    }
    if (marker) {
        waddstr(win, marker);
    }
}

static void
ui_chooser_draw_title(WINDOW* win, const char* title, int body_w, int page_rows) {
    if (!win || !title) {
        return;
    }
    if (g_chooser.count > page_rows) {
        int first = g_chooser.top + 1;
        int last = g_chooser.top + page_rows;
        if (last > g_chooser.count) {
            last = g_chooser.count;
        }
        char range[48];
        DSD_SNPRINTF(range, sizeof range, " (%d-%d/%d)", first, last, g_chooser.count);
        const int title_budget = body_w - (int)strlen(range);
        if (title_budget > ui_text_marker_cells(g_chooser.unicode)) {
            /* The range stays whole and the title gives way: where in the list this page is outlasts a long name. */
            ui_chooser_draw_text(win, 1, 2, title, title_budget);
            waddstr(win, range);
            return;
        }
        char title_line[UI_CHOOSER_TEXT_MAX];
        DSD_SNPRINTF(title_line, sizeof title_line, "%s%s", title, range);
        ui_chooser_draw_text(win, 1, 2, title_line, body_w);
        return;
    }
    ui_chooser_draw_text(win, 1, 2, title, body_w);
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
        ui_chooser_draw_text(win, y++, 2, g_chooser.items[i], body_w);
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
    const char* title = g_chooser.title ? g_chooser.title : k_chooser_default_title;
    const char* footer = "Arrows/PgUp/PgDn  Right/Enter: select  Esc/Left: back";
    int h = 0, w = 0, wy = 0, wx = 0;
    if (!ui_chooser_compute_window_rect(footer, &h, &w, &wy, &wx)) {
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
    // Row 2 is the spacer under the title: the toast goes there, so a notice
    // raised just before this list opened ("No systems there. Try another
    // search.") is read on the list it explains rather than lost behind it.
    (void)ui_status_draw(win, 2, 2, body_w, 1, UI_STATUS_FLAG_BOLD, dsd_realtime_time());
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

const char*
ui_chooser_test_item(int index) {
    if (!g_chooser.active || g_chooser.items == NULL || index < 0 || index >= g_chooser.count) {
        return NULL;
    }
    return g_chooser.items[index];
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
    int saved_unicode = g_chooser.unicode;
    g_chooser.items = items;
    g_chooser.count = count;
    g_chooser.unicode = ui_text_unicode_output();
    int max_item = ui_chooser_max_item_width();
    g_chooser.items = saved_items;
    g_chooser.count = saved_count;
    g_chooser.unicode = saved_unicode;
    return max_item;
}

int
ui_chooser_layout_for_test(const char* title, const char* footer, int max_item, int count, int screen_h, int screen_w,
                           int* h, int* w, int* wy, int* wx) {
    if (!title) {
        return 0;
    }
    const int title_cells = ui_text_cells(ui_text_unicode_output(), title, strlen(title));
    return ui_chooser_compute_window_rect_for_terminal(title_cells, footer, max_item, count, screen_h, screen_w, h, w,
                                                       wy, wx);
}

void
ui_chooser_test_fail_next_start(void) {
    g_chooser_fail_next_start = 1;
}

void
ui_prompt_test_fail_next_alloc(void) {
    g_prompt_fail_next_alloc = 1;
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

int
ui_prompt_mask_active_for_test(void) {
    return g_prompt.mask;
}

size_t
ui_prompt_zeroed_on_close_for_test(void) {
    const size_t n = g_prompt_zeroed_on_close;
    g_prompt_zeroed_on_close = 0;
    return n;
}

int
ui_prompt_display_text_for_test(char* out, size_t out_size) {
    if (!out || out_size == 0) {
        return 0;
    }
    out[0] = '\0';
    if (!g_prompt.active) {
        return 0;
    }
    if (g_prompt.mask) {
        ui_prompt_fill_mask(out, out_size, g_prompt.len);
        return 1;
    }
    DSD_SNPRINTF(out, out_size, "%s", g_prompt.buf ? g_prompt.buf : "");
    return 1;
}
#endif
