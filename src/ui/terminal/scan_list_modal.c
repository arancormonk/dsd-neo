// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The Scan lists modal (issue #697). See scan_list_modal.h for its contract.
 *
 * What a row shows between a switch and the snapshot that shows it is app-control's row rule
 * (dsd-neo/app_control/scan_list_rows.h), which the Qt/Android Scan lists sheet shares: the retained result of a row's
 * own request says what that request did, and the frame's view (trunk_scan_lists_seq) says what the lists are. This
 * file keeps the wording, the keys and the drawing.
 */

#include "scan_list_modal.h"

#include <curses.h>
#include <dsd-neo/app_control/commands.h>
#include <dsd-neo/app_control/scan_list_rows.h>
#include <dsd-neo/app_control/scan_list_view.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/trunk_scan_lists.h>
#include <dsd-neo/platform/curses_compat.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/ui/keymap.h>
#include <dsd-neo/ui/ui_prims.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "dsd-neo/ui/menu_core.h"
#include "menu_internal.h"
#include "ui_keys.h"
#include "ui_text.h"
#include "ui_text_draw.h"

enum {
    SCAN_LIST_MODAL_ROWS_MAX = DSD_TRUNK_SCAN_MAX_LISTS + 1, /* every list, then All lists on */
    SCAN_LIST_MODAL_TEXT_MAX = 256,
    SCAN_LIST_MODAL_RESULTS_BATCH = 16,
};

typedef struct {
    int active;
    uint32_t session;            /* the trunk-scan session the modal opened on */
    uint64_t after;              /* results read up to this sequence */
    int sel;                     /* highlighted row */
    int top;                     /* first row drawn */
    int unicode;                 /* output decision, made once per open */
    WINDOW* win;                 /* NULL until drawn, and after a resize */
    dsd_app_scan_list_view view; /* the last frame's, owned */
    dsd_app_scan_list_row rows[SCAN_LIST_MODAL_ROWS_MAX];
} ScanListModal;

static ScanListModal g_modal;

static const char k_footer[] = "Arrows  Enter: switch on/off  Esc/Left: close";
/* Paused, Enter can only be refused: the footer names the way out instead (issue #697 D7). */
static const char k_footer_paused[] = "Arrows  Esc/Left: close, then y resumes the scan";
static const char k_all_lists_on[] = "All lists on";

static int
scan_list_modal_row_count(void) {
    return (int)g_modal.view.list_count + 1;
}

/* Short forms of the decoder's toasts, beside the row they answer; the main screen's toast line has the whole one. */
static const char*
scan_list_answer_note(int32_t code) {
    switch (code) {
        case DSD_APP_SCAN_LIST_SET_OK:
        case DSD_APP_SCAN_LIST_SET_UNCHANGED: return "";
        case DSD_APP_SCAN_LIST_SET_REFUSED_LAST: return "Nothing else to scan";
        case DSD_APP_SCAN_LIST_SET_BUSY: return "Busy; try again";
        case DSD_APP_SCAN_LIST_SET_PAUSED: return "Scan paused: resume it first";
        case DSD_APP_SCAN_LIST_SET_STALE: return "Scan changed; nothing applied";
        case DSD_APP_SCAN_LIST_SET_UNAVAILABLE: return "Trunk scan not running";
        case DSD_APP_SCAN_LIST_SET_SEARCH: return "Stop Signal search first";
        case DSD_APP_SCAN_LIST_SET_DROPPED: return "Not applied; try again";
        default: return "Not applied";
    }
}

/* The longest note a row can carry: the window is sized for it once, so it does not widen when one appears. */
static const char k_longest_note[] = "Scan changed; nothing applied";

static const char*
scan_list_row_note(const dsd_app_scan_list_row* row, char* buf, size_t buf_size) {
    if (row->request != 0U) {
        DSD_SNPRINTF(buf, buf_size, "Switching %s%s", row->want ? "on" : "off", ui_text_marker(g_modal.unicode));
        return buf;
    }
    switch (row->note) {
        case DSD_APP_SCAN_LIST_NOTE_NOT_SENT: return "Not sent; try again";
        case DSD_APP_SCAN_LIST_NOTE_ANSWER: return scan_list_answer_note(row->code);
        default: return "";
    }
}

/* Whether list row @p i reads On: what its answered request asked for until the snapshot moves, then the snapshot. */
static int
scan_list_row_enabled(int i) {
    return dsd_app_scan_list_row_on(&g_modal.rows[i], &g_modal.view, i);
}

static int
scan_list_modal_lists_on(void) {
    int on = 0;
    for (int i = 0; i < (int)g_modal.view.list_count; i++) {
        on += scan_list_row_enabled(i);
    }
    return on;
}

/* ---- Row text ---- */

typedef struct {
    int name_cells;   /* the widest list name */
    int count_digits; /* the widest target count */
    int prefix_cells; /* everything before the note: the note column */
} ScanListColumns;

static int
scan_list_digits(unsigned int v) {
    int d = 1;
    while (v >= 10U) {
        v /= 10U;
        d++;
    }
    return d;
}

static ScanListColumns
scan_list_columns(void) {
    ScanListColumns cols = {0, 1, 0};
    for (int i = 0; i < (int)g_modal.view.list_count; i++) {
        const dsd_app_scan_list_entry* e = &g_modal.view.lists[i];
        const int cells = ui_text_cells(g_modal.unicode, e->name, strnlen(e->name, sizeof e->name));
        if (cells > cols.name_cells) {
            cols.name_cells = cells;
        }
        const int digits = scan_list_digits(e->targets);
        if (digits > cols.count_digits) {
            cols.count_digits = digits;
        }
    }
    /* "1  " name "  " "Off" "  " count " targets" "  " "on air" "  " */
    cols.prefix_cells = 1 + 2 + cols.name_cells + 2 + 3 + 2 + cols.count_digits + 8 + 2 + 6 + 2;
    return cols;
}

/* Append @p text to @p out at @p *used, then pad it with blanks to @p cells cells; text is measured under the modal's
   output decision, so a name in another script keeps the columns after it. */
static void
scan_list_append_padded(char* out, size_t out_size, size_t* used, const char* text, int cells) {
    if (*used >= out_size) {
        return;
    }
    DSD_SNPRINTF(out + *used, out_size - *used, "%s", text);
    *used = strlen(out);
    int pad = cells - ui_text_cells(g_modal.unicode, text, strlen(text));
    while (pad-- > 0 && *used + 1U < out_size) {
        out[(*used)++] = ' ';
        out[*used] = '\0';
    }
}

static void
scan_list_trim(char* out) {
    size_t len = strlen(out);
    while (len > 0U && out[len - 1U] == ' ') {
        out[--len] = '\0';
    }
}

/* "1  security      On   12 targets  on air  Switching off…": the list's number, name, state and size, whether the
   target on air is one of its, then what its request is doing or what the decoder answered. */
static void
scan_list_format_list_row(int i, const ScanListColumns* cols, char* out, size_t out_size) {
    const dsd_app_scan_list_entry* e = &g_modal.view.lists[i];
    char cell[64];
    size_t used = 0U;
    out[0] = '\0';
    DSD_SNPRINTF(cell, sizeof cell, "%u", (unsigned)e->number);
    scan_list_append_padded(out, out_size, &used, cell, 3);
    scan_list_append_padded(out, out_size, &used, e->name, cols->name_cells + 2);
    scan_list_append_padded(out, out_size, &used, scan_list_row_enabled(i) ? "On" : "Off", 5);
    DSD_SNPRINTF(cell, sizeof cell, "%*u %s", cols->count_digits, (unsigned)e->targets,
                 e->targets == 1U ? "target" : "targets");
    scan_list_append_padded(out, out_size, &used, cell, cols->count_digits + 8 + 2);
    scan_list_append_padded(out, out_size, &used, e->has_active ? "on air" : "", 8);
    char note[64];
    scan_list_append_padded(out, out_size, &used, scan_list_row_note(&g_modal.rows[i], note, sizeof note), 0);
    scan_list_trim(out);
}

/* "All lists on", its note in the lists' note column. */
static void
scan_list_format_all_row(const ScanListColumns* cols, char* out, size_t out_size) {
    char note[64];
    const char* text = scan_list_row_note(&g_modal.rows[g_modal.view.list_count], note, sizeof note);
    size_t used = 0U;
    out[0] = '\0';
    scan_list_append_padded(out, out_size, &used, k_all_lists_on, cols->prefix_cells);
    scan_list_append_padded(out, out_size, &used, text, 0);
    scan_list_trim(out);
}

/* Row @p row as drawn: a list's for 0 .. list_count - 1, All lists on's for list_count, nothing for any other. */
static void
scan_list_format_row(int row, const ScanListColumns* cols, char* out, size_t out_size) {
    out[0] = '\0';
    if (row >= 0 && row < (int)g_modal.view.list_count) {
        scan_list_format_list_row(row, cols, out, out_size);
    } else if (row == (int)g_modal.view.list_count) {
        scan_list_format_all_row(cols, out, out_size);
    }
}

/* "Scan lists (2 of 3 on, 14 targets)"; while paused the count matters less than why nothing switches. */
static void
scan_list_format_title(char* out, size_t out_size) {
    const int on = scan_list_modal_lists_on();
    if (g_modal.view.paused) {
        DSD_SNPRINTF(out, out_size, "Scan lists (%d of %u on, scan paused)", on, (unsigned)g_modal.view.list_count);
        return;
    }
    unsigned int targets = 0U;
    for (int i = 0; i < (int)g_modal.view.list_count; i++) {
        targets += scan_list_row_enabled(i) ? g_modal.view.lists[i].targets : 0U;
    }
    DSD_SNPRINTF(out, out_size, "Scan lists (%d of %u on, %u %s)", on, (unsigned)g_modal.view.list_count, targets,
                 targets == 1U ? "target" : "targets");
}

/* ---- Requests and answers ---- */

static void
scan_list_modal_take_results(void) {
    dsd_app_scan_list_result results[SCAN_LIST_MODAL_RESULTS_BATCH];
    for (;;) {
        const int n = dsd_app_scan_list_results_since(g_modal.after, results, SCAN_LIST_MODAL_RESULTS_BATCH);
        for (int i = 0; i < n; i++) {
            g_modal.after = results[i].sequence;
            (void)dsd_app_scan_list_rows_answer(g_modal.rows, scan_list_modal_row_count(), (int)g_modal.view.list_count,
                                                &results[i]);
        }
        if (n < SCAN_LIST_MODAL_RESULTS_BATCH) {
            return;
        }
    }
}

/* Enter: ask for the other state of a list (every list on, for the last row). A row already waiting ignores it. */
static void
scan_list_modal_submit(int r) {
    if (r < 0 || r >= scan_list_modal_row_count()) {
        return;
    }
    dsd_app_scan_list_row* row = &g_modal.rows[r];
    if (row->request != 0U) {
        return;
    }
    const int is_list = r < (int)g_modal.view.list_count;
    dsd_app_scan_list_set_payload p;
    DSD_MEMSET(&p, 0, sizeof p);
    p.session = g_modal.session;
    p.request = dsd_app_scan_list_request_next();
    p.list = (int16_t)(is_list ? r + 1 : DSD_APP_SCAN_LIST_ALL);
    p.enabled = (uint8_t)(is_list ? !scan_list_row_enabled(r) : 1);
    if (dsd_app_command_scan_list_set(&p) == DSD_APP_COMMAND_SUBMIT_REJECTED) {
        /* Never queued, so no result will come: say so now. */
        dsd_app_scan_list_row_not_sent(row, g_modal.view.lists_seq);
        ui_statusf("Scan list switch not sent: too many commands waiting; try again");
        return;
    }
    dsd_app_scan_list_row_sent(row, p.request, p.enabled, g_modal.view.lists_seq);
}

/* ---- Lifecycle ---- */

static int
scan_list_modal_take_view(const dsd_opts* opts, const dsd_state* state, dsd_app_scan_list_view* view) {
    return dsd_app_scan_list_view_get(opts, state, view) == 0 && view->session != 0U && view->list_count >= 2U
           && (view->running || view->paused);
}

int
scan_list_modal_open(const dsd_opts* opts, const dsd_state* state) {
    dsd_app_scan_list_view view;
    if (!scan_list_modal_take_view(opts, state, &view)) {
        return 0;
    }
    scan_list_modal_close();
    g_modal.active = 1;
    g_modal.session = view.session;
    g_modal.view = view;
    g_modal.unicode = ui_text_unicode_output();
    /* Only answers to requests made from here on are this modal's. */
    dsd_app_scan_list_result latest;
    g_modal.after = dsd_app_scan_list_result_get(&latest) ? latest.sequence : 0U;
    return 1;
}

int
scan_list_modal_active(void) {
    return g_modal.active;
}

void
scan_list_modal_close(void) {
    if (g_modal.win) {
        delwin(g_modal.win);
    }
    DSD_MEMSET(&g_modal, 0, sizeof g_modal);
}

void
scan_list_modal_tick(const dsd_opts* opts, const dsd_state* state) {
    if (!g_modal.active) {
        return;
    }
    dsd_app_scan_list_view view;
    if (!scan_list_modal_take_view(opts, state, &view) || view.session != g_modal.session) {
        /* Stopped, restarted or replaced: the lists and every request were the old session's. */
        scan_list_modal_close();
        return;
    }
    g_modal.view = view;
    scan_list_modal_take_results();
    dsd_app_scan_list_rows_expire(g_modal.rows, scan_list_modal_row_count(), &g_modal.view);
}

/* ---- Keys ---- */

static void
scan_list_modal_move(int delta) {
    const int n = scan_list_modal_row_count();
    g_modal.sel = (g_modal.sel + delta + n) % n;
}

static int
scan_list_modal_handle_nav_key(int ch) {
    const int n = scan_list_modal_row_count();
    switch (ch) {
        case KEY_UP: scan_list_modal_move(-1); return 1;
        case KEY_DOWN: scan_list_modal_move(+1); return 1;
        case KEY_HOME:
        case KEY_PPAGE: g_modal.sel = 0; return 1;
        case KEY_END:
        case KEY_NPAGE: g_modal.sel = n - 1; return 1;
        default: return 0;
    }
}

int
scan_list_modal_handle_key(int ch) {
    if (!g_modal.active) {
        return 0;
    }
    if (ch == KEY_RESIZE) {
#if DSD_CURSES_NEEDS_EXPLICIT_RESIZE
        resize_term(0, 0);
#endif
        if (g_modal.win) {
            delwin(g_modal.win);
            g_modal.win = NULL;
        }
        return 1;
    }
    if (ch == DSD_KEY_ESC || ch == KEY_LEFT) {
        scan_list_modal_close();
        return 1;
    }
    if (scan_list_modal_handle_nav_key(ch)) {
        return 1;
    }
    if (ui_key_is_enter(ch)) {
        scan_list_modal_submit(g_modal.sel);
    }
    return 1;
}

/* ---- Drawing ---- */

static int
scan_list_modal_cells(const char* text) {
    return ui_text_cells(g_modal.unicode, text, strlen(text));
}

/* The window: as wide as the widest row with the longest note, the title or either footer, as tall as every row with a
   blank line before the footer, as the menu has, and centred. */
static int
scan_list_modal_rect(int* h, int* w, int* y, int* x) {
    int scr_h = 0, scr_w = 0;
    getmaxyx(stdscr, scr_h, scr_w);
    const ScanListColumns cols = scan_list_columns();
    char title[SCAN_LIST_MODAL_TEXT_MAX];
    scan_list_format_title(title, sizeof title);
    int content = cols.prefix_cells + scan_list_modal_cells(k_longest_note);
    const int title_cells = scan_list_modal_cells(title) + 8; /* room for the title to grow by a digit or a word */
    const int footer = scan_list_modal_cells(k_footer);
    const int footer_paused = scan_list_modal_cells(k_footer_paused);
    const int footer_cells = footer > footer_paused ? footer : footer_paused;
    content = content > title_cells ? content : title_cells;
    content = content > footer_cells ? content : footer_cells;
    int local_w = content + 4;
    if (local_w > scr_w - 2) {
        local_w = scr_w - 2;
    }
    int local_h = scan_list_modal_row_count() + 6;
    if (local_h > scr_h - 2) {
        local_h = scr_h - 2;
    }
    if (local_w < 10 || local_h < 6) {
        return 0;
    }
    *h = local_h;
    *w = local_w;
    *y = (scr_h - local_h) / 2;
    *x = (scr_w - local_w) / 2;
    return 1;
}

/* Text in at most @p budget cells, cut between code points with the marker, as the chooser draws its rows. */
static void
scan_list_modal_draw_text(WINDOW* win, int y, int x, const char* text, int budget) {
    ui_text_draw_fit(win, y, x, g_modal.unicode, text, budget);
}

static const char*
scan_list_modal_footer(void) {
    return g_modal.view.paused ? k_footer_paused : k_footer;
}

static void
scan_list_modal_draw_rows(WINDOW* win, int w, int body_w, int page_rows) {
    const int n = scan_list_modal_row_count();
    g_modal.top = ui_scroll_follow_selection(n, page_rows, g_modal.top, g_modal.sel);
    const ScanListColumns cols = scan_list_columns();
    char line[SCAN_LIST_MODAL_TEXT_MAX];
    for (int drawn = 0; drawn < page_rows; drawn++) {
        const int r = g_modal.top + drawn;
        mvwhline(win, 3 + drawn, 1, ' ', w - 2);
        if (r < 0 || r >= n) {
            continue;
        }
        scan_list_format_row(r, &cols, line, sizeof line);
        if (r == g_modal.sel) {
            wattron(win, A_REVERSE);
            mvwhline(win, 3 + drawn, 1, ' ', w - 2);
        }
        scan_list_modal_draw_text(win, 3 + drawn, 2, line, body_w);
        if (r == g_modal.sel) {
            wattroff(win, A_REVERSE);
        }
    }
}

void
scan_list_modal_render(void) {
    if (!g_modal.active) {
        return;
    }
    int h = 0, w = 0, y = 0, x = 0;
    if (!scan_list_modal_rect(&h, &w, &y, &x)) {
        return; /* too small to draw; Esc still closes it */
    }
    if (g_modal.win) {
        int cur_h = 0, cur_w = 0;
        getmaxyx(g_modal.win, cur_h, cur_w);
        if (cur_h != h || cur_w != w) {
            delwin(g_modal.win);
            g_modal.win = NULL;
        }
    }
    if (!g_modal.win) {
        g_modal.win = ui_make_window(h, w, y, x);
        if (!g_modal.win) {
            return;
        }
    }
    WINDOW* win = g_modal.win;
    werase(win);
    box(win, 0, 0);
    const int body_w = w - 4;
    char title[SCAN_LIST_MODAL_TEXT_MAX];
    scan_list_format_title(title, sizeof title);
    scan_list_modal_draw_text(win, 1, 2, title, body_w);
    /* Row 2 is the terminal's own notices, as in the chooser (a submit not sent); the decoder's answer is beside its row,
       and whole on the main screen's toast line. */
    (void)ui_status_draw(win, 2, 2, body_w, 1, UI_STATUS_FLAG_BOLD, dsd_realtime_time());
    scan_list_modal_draw_rows(win, w, body_w, h - 6);
    scan_list_modal_draw_text(win, h - 2, 2, scan_list_modal_footer(), body_w);
    wnoutrefresh(win);
}

#ifdef DSD_NEO_TEST_HOOKS
void
scan_list_modal_title_for_test(char* out, size_t out_size) {
    scan_list_format_title(out, out_size);
}

int
scan_list_modal_row_for_test(int row, char* out, size_t out_size) {
    if (!g_modal.active || row < 0 || row >= scan_list_modal_row_count()) {
        out[0] = '\0';
        return 0;
    }
    const ScanListColumns cols = scan_list_columns();
    scan_list_format_row(row, &cols, out, out_size);
    return 1;
}

int
scan_list_modal_selected_for_test(void) {
    return g_modal.sel;
}

const char*
scan_list_modal_footer_for_test(void) {
    return scan_list_modal_footer();
}
#endif
