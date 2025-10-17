// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/runtime/unicode.h>
#include <dsd-neo/ui/keymap.h>
#include <dsd-neo/ui/menu_core.h>

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <dsd-neo/runtime/config.h>
#include <dsd-neo/ui/menu_services.h>

#ifdef USE_RTLSDR
#include <dsd-neo/io/rtl_stream_c.h>
#endif

/* Forward declarations for new P25p2 RRC UI handlers */
static const char* lbl_p25p2_rrc_autoprobe(void* v, char* b, size_t n);
static void io_toggle_p25p2_rrc_autoprobe(void* vctx);

#ifndef UNUSED
#define UNUSED(x) (void)(x)
#endif

// Internal helpers
static WINDOW*
ui_make_window(int h, int w, int y, int x) {
    WINDOW* win = newwin(h, w, y, x);
    box(win, 0, 0);
    wrefresh(win);
    return win;
}

// global status footer (transient)
static char g_status_msg[256];
static time_t g_status_expire = 0;

void
ui_statusf(const char* fmt, ...) {
    if (!fmt) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    // NOLINTNEXTLINE(clang-analyzer-valist.Uninitialized)
    vsnprintf(g_status_msg, sizeof g_status_msg, fmt, ap); // NOLINT
    va_end(ap);
    g_status_expire = time(NULL) + 3; // show ~3 seconds
}

static void
ui_destroy_window(WINDOW** win) {
    if (win && *win) {
        delwin(*win);
        *win = NULL;
    }
}

static int
ui_is_enabled(const NcMenuItem* it, void* ctx) {
    return (it->is_enabled == NULL) ? 1 : (it->is_enabled(ctx) ? 1 : 0);
}

// forward declarations for actions referenced from multiple menus
static void act_toggle_invert(void* v);
static void act_toggle_payload(void* v);
static void act_reset_eh(void* v);
static void act_p2_params(void* v);
static void act_key_entry(void* v);
// New action prototypes used before definitions
static void act_event_log_set(void* v);
static void act_event_log_disable(void* v);
static void act_static_wav(void* v);
static void act_raw_wav(void* v);
static void act_dsp_out(void* v);
static void act_crc_relax(void* v);
static void act_trunk_toggle(void* v);
static void act_scan_toggle(void* v);
static void act_lcw_toggle(void* v);
static void act_p25_auto_adapt(void* v);
static void act_p25_sm_basic(void* v);
static void act_setmod_bw(void* v);
static void act_import_chan(void* v);
static void act_import_group(void* v);
static void act_allow_toggle(void* v);
static void act_tune_group(void* v);
static void act_tune_priv(void* v);
static void act_tune_data(void* v);
static void act_tg_hold(void* v);
static void act_hangtime(void* v);
static void act_rev_mute(void* v);
static void act_dmr_le(void* v);
static void act_slot_pref(void* v);
static void act_slots_on(void* v);
static void act_keys_dec(void* v);
static void act_keys_hex(void* v);
static void act_tyt_ap(void* v);
static void act_retevis_rc2(void* v);
static void act_tyt_ep(void* v);
static void act_ken_scr(void* v);
static void act_anytone_bp(void* v);
static void act_xor_ks(void* v);
#ifdef USE_RTLSDR
static void act_rtl_opts(void* v);
/* Blanker UI helpers */
static const char* lbl_blanker(void* v, char* b, size_t n);
static const char* lbl_blanker_thr(void* v, char* b, size_t n);
static const char* lbl_blanker_win(void* v, char* b, size_t n);
static void act_toggle_blanker(void* v);
static void act_blanker_thr_up(void* v);
static void act_blanker_thr_dn(void* v);
static void act_blanker_win_up(void* v);
static void act_blanker_win_dn(void* v);
#endif

static void
ui_draw_menu(WINDOW* menu_win, const NcMenuItem* items, size_t n, int hi, void* ctx) {
    int x = 2;
    int y = 1;
    werase(menu_win);
    box(menu_win, 0, 0);
    int mh = 0, mw = 0;
    getmaxyx(menu_win, mh, mw);
    for (size_t i = 0; i < n; i++) {
        if (!ui_is_enabled(&items[i], ctx)) {
            // Hide items that are not enabled for current context
            continue;
        }
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
        mvwprintw(menu_win, y++, x, "%s", lab);
        wattroff(menu_win, A_REVERSE);
    }
    // ensure a blank spacer line above footer to avoid looking like an item
    mvwhline(menu_win, mh - 5, 1, ' ', mw - 2);
    // footer help (split across two lines to avoid overflow)
    mvwprintw(menu_win, mh - 4, x, "Arrows: move  Enter: select");
    mvwprintw(menu_win, mh - 3, x, "h: help  Esc/q: back");
    // transient status
    time_t now = time(NULL);
    if (g_status_msg[0] != '\0' && now < g_status_expire) {
        // clear line then print
        mvwhline(menu_win, mh - 2, 1, ' ', mw - 2);
        mvwprintw(menu_win, mh - 2, x, "Status: %s", g_status_msg);
    } else {
        g_status_msg[0] = '\0';
    }
    wrefresh(menu_win);
}

static void
ui_show_help(const NcMenuItem* it) {
    const char* help = it->help;
    if (!help || !*help) {
        return;
    }
    int h = 8;
    int w = (int)(strlen(help) + 6);
    if (w < 40) {
        w = 40;
    }
    int scr_h = 0, scr_w = 0;
    getmaxyx(stdscr, scr_h, scr_w);
    int hy = (scr_h - h) / 2;
    int hx = (scr_w - w) / 2;
    if (hy < 0) {
        hy = 0;
    }
    if (hx < 0) {
        hx = 0;
    }
    WINDOW* hw = ui_make_window(h, w, hy, hx);
    mvwprintw(hw, 1, 2, "Help:");
    mvwprintw(hw, 3, 2, "%s", help);
    mvwprintw(hw, h - 2, 2, "Press any key to continue...");
    wrefresh(hw);
    wgetch(hw);
    ui_destroy_window(&hw);
    // Restore base and let caller redraw menu
    redrawwin(stdscr);
    refresh();
}

static void
ui_menu_loop(const NcMenuItem* items, size_t n, void* ctx) {
    // Determine number of visible items and the maximum rendered label length
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

    // Footer lines (keep in sync with ui_draw_menu)
    const char* f1 = "Arrows: move  Enter: select";
    const char* f2 = "h: help  Esc/q: back";

    // Layout constants
    int pad_x = 2; // left margin inside box

    // Compute minimal width to hold content and footer
    int width = pad_x + ((maxlab > 0) ? maxlab : 1);
    int f1w = pad_x + (int)strlen(f1);
    int f2w = pad_x + (int)strlen(f2);
    if (f1w > width) {
        width = f1w;
    }
    if (f2w > width) {
        width = f2w;
    }
    width += 2; // account for borders

    // Compute minimal height: items + blank gap(1) + footer(2) + status(1) + borders(2)
    // Footer at mh-4 (line1), mh-3 (line2), status at mh-2, blank line at mh-5 above footer.
    int height = vis + 6;
    if (height < 8) {
        height = 8;
    }

    // Clamp to terminal size
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
        if (height < 7) {
            height = 7;
        }
    }

    // Redraw underlying stdscr so base application remains visible beneath menu
    redrawwin(stdscr);
    refresh();
    int my = (term_h - height) / 2;
    int mx = (term_w - width) / 2;
    if (my < 0) {
        my = 0;
    }
    if (mx < 0) {
        mx = 0;
    }
    WINDOW* menu_win = ui_make_window(height, width, my, mx);
    keypad(menu_win, TRUE);
    // Ensure blocking input for menu navigation regardless of stdscr timeout
    wtimeout(menu_win, -1);

    int hi = 0;
    while (1) {
        if (n == 0) {
            // No items to navigate
            break;
        }
        ui_draw_menu(menu_win, items, n, hi, ctx);
        int c = wgetch(menu_win);
        if (c == KEY_RESIZE) {
            // Terminal resized: recompute geometry and re-center the window
            // Recompute visible items and longest label (content-driven width)
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
            const char* f1 = "Arrows: move  Enter: select";
            const char* f2 = "h: help  Esc/q: back";
            int pad_x = 2;
            int width = pad_x + ((maxlab > 0) ? maxlab : 1);
            int f1w = pad_x + (int)strlen(f1);
            int f2w = pad_x + (int)strlen(f2);
            if (f1w > width) {
                width = f1w;
            }
            if (f2w > width) {
                width = f2w;
            }
            width += 2;           // borders
            int height = vis + 6; // items + blank + footer(2) + status(1) + borders
            if (height < 8) {
                height = 8;
            }

            // Clamp to current terminal and re-center
            getmaxyx(stdscr, term_h, term_w);
            if (width > term_w - 2) {
                width = term_w - 2;
            }
            if (width < 10) {
                width = 10;
            }
            if (height > term_h - 2) {
                height = term_h - 2;
            }
            if (height < 7) {
                height = 7;
            }
            int my = (term_h - height) / 2;
            int mx = (term_w - width) / 2;
            if (my < 0) {
                my = 0;
            }
            if (mx < 0) {
                mx = 0;
            }

            // Recreate the window at the new centered position
            ui_destroy_window(&menu_win);
            menu_win = ui_make_window(height, width, my, mx);
            keypad(menu_win, TRUE);
            // Skip other key handling this iteration
            continue;
        } else if (c == KEY_UP) {
            do {
                hi = (hi - 1 + (int)n) % (int)n;
            } while (!ui_is_enabled(&items[hi], ctx));
        } else if (c == KEY_DOWN) {
            do {
                hi = (hi + 1) % (int)n;
            } while (!ui_is_enabled(&items[hi], ctx));
        } else if (c == 'h' || c == 'H') {
            ui_show_help(&items[hi]);
        } else if (c == DSD_KEY_ESC || c == 'q' || c == 'Q') {
            // Back out of current menu (no program exit)
            break;
        } else if (c == 10 || c == KEY_ENTER || c == '\r') {
            const NcMenuItem* it = &items[hi];
            if (!ui_is_enabled(it, ctx)) {
                continue;
            }
            if (it->submenu && it->submenu_len > 0) {
                ui_menu_loop(it->submenu, it->submenu_len, ctx);
                // Restore base screen after closing submenu
                redrawwin(stdscr);
                refresh();
            }
            if (it->on_select) {
                it->on_select(ctx);
            }
            if (!it->on_select && (!it->submenu || it->submenu_len == 0) && it->help && *it->help) {
                ui_show_help(it);
            }
            if (exitflag) {
                break; // allow actions to request immediate exit
            }
            // After select, re-draw menu
        }
    }

    ui_destroy_window(&menu_win);
    // Leave base application visible after menu closes
    redrawwin(stdscr);
    refresh();
}

void
ui_menu_run(const NcMenuItem* items, size_t n_items, void* ctx) {
    if (!items || n_items == 0) {
        return;
    }
    ui_menu_loop(items, n_items, ctx);
}

static int
ui_prompt_common_prefill(const char* title, char* buf, size_t cap, const char* prefill) {
    if (!buf || cap == 0) {
        return 0;
    }
    buf[0] = '\0';
    size_t len = 0;
    int h = 8, w = (int)(strlen(title) + 16);
    if (w < 54) {
        w = 54;
    }
    int scr_h = 0, scr_w = 0;
    getmaxyx(stdscr, scr_h, scr_w);
    int py = (scr_h - h) / 2;
    int px = (scr_w - w) / 2;
    if (py < 0) {
        py = 0;
    }
    if (px < 0) {
        px = 0;
    }
    WINDOW* win = ui_make_window(h, w, py, px);
    keypad(win, TRUE);
    noecho();
    curs_set(1);
    mvwprintw(win, 1, 2, "%s", title);
    mvwprintw(win, 3, 2, "> ");
    mvwprintw(win, h - 2, 2, "Enter=OK  Esc/q=Cancel");
    if (prefill && *prefill) {
        // copy prefill and render it
        strncpy(buf, prefill, cap - 1);
        buf[cap - 1] = '\0';
        len = strlen(buf);
        mvwprintw(win, 3, 4, "%s", buf);
        wmove(win, 3, 4 + (int)len);
    } else {
        wmove(win, 3, 4);
    }
    wrefresh(win);

    while (1) {
        int ch = wgetch(win);
        if (ch == 27 || ch == 'q' || ch == 'Q') { // ESC/q
            buf[0] = '\0';
            ui_destroy_window(&win);
            curs_set(0);
            return 0;
        } else if (ch == KEY_ENTER || ch == '\n' || ch == '\r') {
            break;
        } else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
            if (len > 0) {
                len--;
                buf[len] = '\0';
                int cy, cx;
                getyx(win, cy, cx);
                UNUSED(cy);
                if (cx > 4) {
                    mvwaddch(win, 3, 4 + (int)len, ' ');
                    wmove(win, 3, 4 + (int)len);
                }
                wrefresh(win);
            }
        } else if (isprint(ch)) {
            if (len < cap - 1) {
                buf[len++] = (char)ch;
                buf[len] = '\0';
                waddch(win, ch);
                wrefresh(win);
            }
        }
    }
    ui_destroy_window(&win);
    curs_set(0);
    if (buf[0] == '\0') {
        return 0;
    }
    return 1;
}

int
ui_prompt_common(const char* title, char* buf, size_t cap) {
    return ui_prompt_common_prefill(title, buf, cap, NULL);
}

int
ui_prompt_string(const char* title, char* out, size_t out_cap) {
    return ui_prompt_common_prefill(title, out, out_cap, NULL);
}

int
ui_prompt_int(const char* title, int* out) {
    char tmp[64] = {0};
    if (!ui_prompt_common_prefill(title, tmp, sizeof tmp, NULL)) {
        return 0;
    }
    char* end = NULL;
    long v = strtol(tmp, &end, 10);
    if (!end || *end != '\0') {
        return 0;
    }
    *out = (int)v;
    return 1;
}

int
ui_prompt_double(const char* title, double* out) {
    char tmp[64] = {0};
    if (!ui_prompt_common_prefill(title, tmp, sizeof tmp, NULL)) {
        return 0;
    }
    char* end = NULL;
    double v = strtod(tmp, &end);
    if (!end || *end != '\0') {
        return 0;
    }
    *out = v;
    return 1;
}

int
ui_prompt_string_prefill(const char* title, const char* current, char* out, size_t out_cap) {
    return ui_prompt_common_prefill(title, out, out_cap, (current && *current) ? current : NULL);
}

int
ui_prompt_int_prefill(const char* title, int current, int* out) {
    char pre[64];
    snprintf(pre, sizeof pre, "%d", current);
    char tmp[64] = {0};
    if (!ui_prompt_common_prefill(title, tmp, sizeof tmp, pre)) {
        return 0;
    }
    char* end = NULL;
    long v = strtol(tmp, &end, 10);
    if (!end || *end != '\0') {
        return 0;
    }
    *out = (int)v;
    return 1;
}

int
ui_prompt_double_prefill(const char* title, double current, double* out) {
    char pre[64];
    snprintf(pre, sizeof pre, "%.6f", current);
    char tmp[64] = {0};
    if (!ui_prompt_common_prefill(title, tmp, sizeof tmp, pre)) {
        return 0;
    }
    char* end = NULL;
    double v = strtod(tmp, &end);
    if (!end || *end != '\0') {
        return 0;
    }
    *out = v;
    return 1;
}

int
ui_prompt_confirm(const char* title) {
    int h = 7, w = (int)(strlen(title) + 14);
    if (w < 48) {
        w = 48;
    }
    int scr_h = 0, scr_w = 0;
    getmaxyx(stdscr, scr_h, scr_w);
    int cy = (scr_h - h) / 2;
    int cx = (scr_w - w) / 2;
    if (cy < 0) {
        cy = 0;
    }
    if (cx < 0) {
        cx = 0;
    }
    WINDOW* win = ui_make_window(h, w, cy, cx);
    mvwprintw(win, 1, 2, "%s", title);
    mvwprintw(win, 3, 2, "y = Yes, n = No, Esc/q = Cancel");
    wrefresh(win);
    int res = 0;
    while (1) {
        int c = wgetch(win);
        if (c == 'y' || c == 'Y') {
            res = 1;
            break;
        }
        if (c == 'n' || c == 'N' || c == 27 || c == 'q' || c == 'Q') {
            res = 0;
            break;
        }
    }
    ui_destroy_window(&win);
    return res;
}

// ---- IO submenu ----

typedef struct {
    dsd_opts* opts;
    dsd_state* state;
} UiCtx;

static bool
io_always_on(void* ctx) {
    (void)ctx;
    return true;
}

// Enable items only when RTL-SDR is the active input
static bool
io_rtl_active(void* ctx) {
    UiCtx* c = (UiCtx*)ctx;
    if (!c || !c->opts) {
        return false;
    }
    return (c->opts->audio_in_type == 3);
}

static void
io_toggle_mute_enc(void* vctx) {
    UiCtx* c = (UiCtx*)vctx;
    svc_toggle_all_mutes(c->opts);
}

static void
io_toggle_call_alert(void* vctx) {
    UiCtx* c = (UiCtx*)vctx;
    svc_toggle_call_alert(c->opts);
}

static void
io_toggle_cc_candidates(void* vctx) {
    UiCtx* c = (UiCtx*)vctx;
    c->opts->p25_prefer_candidates = !c->opts->p25_prefer_candidates;
    if (c->opts->p25_prefer_candidates) {
        fprintf(stderr, "\n P25: Prefer CC Candidates: On\n");
    } else {
        fprintf(stderr, "\n P25: Prefer CC Candidates: Off\n");
    }
}

static __attribute__((unused)) void
io_list_pulse(void* vctx) {
    (void)vctx;
    pulse_list();
    ui_statusf("Pulse devices printed to console");
}

static void
io_enable_per_call_wav(void* vctx) {
    UiCtx* c = (UiCtx*)vctx;
    if (svc_enable_per_call_wav(c->opts, c->state) == 0) {
        ui_statusf("Per-call WAV enabled to %s", c->opts->wav_out_dir);
    } else {
        ui_statusf("Failed to enable per-call WAV");
    }
}

static void
io_save_symbol_capture(void* vctx) {
    UiCtx* c = (UiCtx*)vctx;
    char path[1024] = {0};
    if (ui_prompt_string("Enter Symbol Capture Filename", path, sizeof path)) {
        if (svc_open_symbol_out(c->opts, c->state, path) == 0) {
            ui_statusf("Symbol capture: %s", c->opts->symbol_out_file);
        } else {
            ui_statusf("Failed to open symbol capture");
        }
    }
}

static void
io_read_symbol_bin(void* vctx) {
    UiCtx* c = (UiCtx*)vctx;
    char path[1024] = {0};
    if (!ui_prompt_string("Enter Symbol Capture Filename", path, sizeof path)) {
        return;
    }
    if (svc_open_symbol_in(c->opts, c->state, path) == 0) {
        ui_statusf("Symbol input: %s", path);
    } else {
        ui_statusf("Failed to open: %s", path);
    }
}

static void
io_replay_last_symbol_bin(void* vctx) {
    UiCtx* c = (UiCtx*)vctx;
    if (svc_replay_last_symbol(c->opts, c->state) == 0) {
        ui_statusf("Replaying: %s", c->opts->audio_in_dev);
    } else {
        ui_statusf("Failed to replay last symbol file");
    }
}

static void
io_stop_symbol_playback(void* vctx) {
    UiCtx* c = (UiCtx*)vctx;
    svc_stop_symbol_playback(c->opts);
    ui_statusf("Symbol playback stopped");
}

static void
io_stop_symbol_saving(void* vctx) {
    UiCtx* c = (UiCtx*)vctx;
    svc_stop_symbol_saving(c->opts, c->state);
    ui_statusf("Symbol capture stopped");
}

// Simple list chooser for short lists
static int
ui_choose_from_strings(const char* title, const char* const* items, int count) {
    if (!items || count <= 0) {
        return -1;
    }
    // Compute width from title and item strings
    int max_item = 0;
    for (int i = 0; i < count; i++) {
        int L = (int)strlen(items[i]);
        if (L > max_item) {
            max_item = L;
        }
    }
    const char* footer = "Arrows = Move   Enter = Select   Esc/q = Cancel";
    int w = 4 /*padding*/ + (int)strlen(title);
    int need = 4 + max_item;
    if (need > w) {
        w = need;
    }
    need = 4 + (int)strlen(footer);
    if (need > w) {
        w = need;
    }
    w += 2;            // borders
    int h = count + 5; // title + gap + items + footer + borders
    if (h < 7) {
        h = 7;
    }
    // clamp and center
    int scr_h = 0, scr_w = 0;
    getmaxyx(stdscr, scr_h, scr_w);
    if (w > scr_w - 2) {
        w = scr_w - 2;
    }
    if (h > scr_h - 2) {
        h = scr_h - 2;
    }
    int wy = (scr_h - h) / 2;
    int wx = (scr_w - w) / 2;
    if (wy < 0) {
        wy = 0;
    }
    if (wx < 0) {
        wx = 0;
    }
    WINDOW* win = ui_make_window(h, w, wy, wx);
    keypad(win, TRUE);
    int sel = 0;
    while (1) {
        werase(win);
        box(win, 0, 0);
        mvwprintw(win, 1, 2, "%s", title);
        int y = 3;
        for (int i = 0; i < count; i++) {
            if (i == sel) {
                wattron(win, A_REVERSE);
            }
            mvwprintw(win, y++, 2, "%s", items[i]);
            if (i == sel) {
                wattroff(win, A_REVERSE);
            }
        }
        mvwprintw(win, h - 2, 2, "%s", footer);
        wrefresh(win);
        int c = wgetch(win);
        if (c == KEY_UP) {
            sel = (sel - 1 + count) % count;
        } else if (c == KEY_DOWN) {
            sel = (sel + 1) % count;
        } else if (c == 'q' || c == 'Q' || c == 27) {
            sel = -1;
            break;
        } else if (c == 10 || c == KEY_ENTER || c == '\r') {
            break;
        }
    }
    ui_destroy_window(&win);
    return sel;
}

static void
io_set_pulse_out(void* vctx) {
    UiCtx* c = (UiCtx*)vctx;
    pa_devicelist_t outs[16];
    pa_devicelist_t ins[16];
    if (pa_get_devicelist(ins, outs) < 0) {
        ui_statusf("Failed to get Pulse device list");
        return;
    }
    const char* labels[16];
    const char* names[16];
    char buf[16][768];
    int n = 0;
    for (int i = 0; i < 16; i++) {
        if (!outs[i].initialized) {
            break;
        }
        int name_len = (int)strnlen(outs[i].name, 511);
        int desc_len = (int)strnlen(outs[i].description, 255);
        snprintf(buf[n], sizeof buf[n], "[%d] %.*s %s %.*s", outs[i].index, name_len, outs[i].name,
                 dsd_unicode_or_ascii("—", "-"), desc_len, outs[i].description);
        labels[n] = buf[n];
        names[n] = outs[i].name;
        n++;
    }
    if (n == 0) {
        ui_statusf("No Pulse outputs found");
        return;
    }
    int sel = ui_choose_from_strings("Select Pulse Output", labels, n);
    if (sel >= 0) {
        svc_set_pulse_output(c->opts, names[sel]);
        ui_statusf("Pulse out: %s", names[sel]);
    }
}

static void
io_set_pulse_in(void* vctx) {
    UiCtx* c = (UiCtx*)vctx;
    pa_devicelist_t outs[16];
    pa_devicelist_t ins[16];
    if (pa_get_devicelist(ins, outs) < 0) {
        ui_statusf("Failed to get Pulse device list");
        return;
    }
    const char* labels[16];
    const char* names[16];
    char buf[16][768];
    int n = 0;
    for (int i = 0; i < 16; i++) {
        if (!ins[i].initialized) {
            break;
        }
        int name_len2 = (int)strnlen(ins[i].name, 511);
        int desc_len2 = (int)strnlen(ins[i].description, 255);
        snprintf(buf[n], sizeof buf[n], "[%d] %.*s %s %.*s", ins[i].index, name_len2, ins[i].name,
                 dsd_unicode_or_ascii("—", "-"), desc_len2, ins[i].description);
        labels[n] = buf[n];
        names[n] = ins[i].name;
        n++;
    }
    if (n == 0) {
        ui_statusf("No Pulse inputs found");
        return;
    }
    int sel = ui_choose_from_strings("Select Pulse Input", labels, n);
    if (sel >= 0) {
        svc_set_pulse_input(c->opts, names[sel]);
        ui_statusf("Pulse in: %s", names[sel]);
    }
}

static void
io_set_udp_out(void* vctx) {
    UiCtx* c = (UiCtx*)vctx;
    char host[256] = {0};
    int port = c->opts->udp_portno > 0 ? c->opts->udp_portno : 23456;
    {
        const char* src = c->opts->udp_hostname[0] ? c->opts->udp_hostname : "127.0.0.1";
        snprintf(host, sizeof host, "%.*s", (int)sizeof(host) - 1, src);
    }
    if (!ui_prompt_string_prefill("UDP blaster host", c->opts->udp_hostname, host, sizeof host)) {
        return;
    }
    if (!ui_prompt_int_prefill("UDP blaster port", port, &port)) {
        return;
    }
    if (svc_udp_output_config(c->opts, c->state, host, port) == 0) {
        ui_statusf("UDP out: %s:%d", host, port);
    } else {
        ui_statusf("UDP out failed");
    }
}

// ---- Switch Output helpers ----
static const char*
lbl_current_output(void* vctx, char* b, size_t n) {
    UiCtx* c = (UiCtx*)vctx;
    const char* name;
    switch (c->opts->audio_out_type) {
        case 0: name = "Pulse Digital"; break;
        case 2: name = "OSS (8k/2)"; break;
        case 5: name = "OSS (48k/1)"; break;
        case 8: name = "UDP"; break;
        default: name = "?"; break;
    }
    if (c->opts->audio_out_type == 0) {
        if (c->opts->pa_output_idx[0]) {
            snprintf(b, n, "Current Output: Pulse [%s]", c->opts->pa_output_idx);
        } else {
            snprintf(b, n, "Current Output: Pulse [default]");
        }
    } else if (c->opts->audio_out_type == 8) {
        snprintf(b, n, "Current Output: UDP %s:%d", c->opts->udp_hostname, c->opts->udp_portno);
    } else if (c->opts->audio_out_type == 2 || c->opts->audio_out_type == 5) {
        snprintf(b, n, "Current Output: %s (%s)", c->opts->audio_out_dev, name);
    } else {
        snprintf(b, n, "Current Output: %s", name);
    }
    return b;
}

static void
switch_out_pulse(void* vctx) {
    UiCtx* c = (UiCtx*)vctx;
    // Keep current Pulse sink index if set; else default
    const char* idx = c->opts->pa_output_idx[0] ? c->opts->pa_output_idx : "";
    svc_set_pulse_output(c->opts, idx);
}

static void
switch_out_udp(void* vctx) {
    io_set_udp_out(vctx);
}

static const char*
lbl_out_mute(void* vctx, char* b, size_t n) {
    UiCtx* c = (UiCtx*)vctx;
    snprintf(b, n, "Mute Output [%s]", (c->opts->audio_out == 0) ? "On" : "Off");
    return b;
}

static void
switch_out_toggle_mute(void* vctx) {
    UiCtx* c = (UiCtx*)vctx;
    /* Toggle mute and, on unmute, reinitialize the audio sink to avoid
       potential blocking on a long-idle/stale backend handle. */
    c->opts->audio_out = (c->opts->audio_out == 0) ? 1 : 0;
    if (c->opts->audio_out == 1) {
        if (c->opts->audio_out_type == 0) { /* Pulse */
            closePulseOutput(c->opts);
            openPulseOutput(c->opts);
        } else if (c->opts->audio_out_type == 2 || c->opts->audio_out_type == 5) { /* OSS */
            if (c->opts->audio_out_fd >= 0) {
                close(c->opts->audio_out_fd);
                c->opts->audio_out_fd = -1;
            }
            openOSSOutput(c->opts);
        }
    }
    ui_statusf("Output: %s", c->opts->audio_out ? "On" : "Muted");
}

static void
io_set_gain_dig(void* vctx) {
    UiCtx* c = (UiCtx*)vctx;
    double g = c->opts->audio_gain;
    if (ui_prompt_double("Digital output gain (0=auto; 1..50)", &g)) {
        if (g < 0.0) {
            g = 0.0;
        }
        if (g > 50.0) {
            g = 50.0;
        }
        c->opts->audio_gain = (float)g;
        c->opts->audio_gainR = (float)g;
        ui_statusf("Digital gain set to %.1f", g);
    }
}

static void
io_set_gain_ana(void* vctx) {
    UiCtx* c = (UiCtx*)vctx;
    double g = c->opts->audio_gainA;
    if (ui_prompt_double("Analog output gain (0..100)", &g)) {
        if (g < 0.0) {
            g = 0.0;
        }
        if (g > 100.0) {
            g = 100.0;
        }
        c->opts->audio_gainA = (float)g;
        ui_statusf("Analog gain set to %.1f", g);
    }
}

static void
io_toggle_monitor(void* vctx) {
    UiCtx* c = (UiCtx*)vctx;
    c->opts->monitor_input_audio = !c->opts->monitor_input_audio;
}

static void
io_toggle_cosine(void* vctx) {
    UiCtx* c = (UiCtx*)vctx;
    c->opts->use_cosine_filter = c->opts->use_cosine_filter ? 0 : 1;
}

static void
io_set_input_volume(void* vctx) {
    UiCtx* c = (UiCtx*)vctx;
    int m = c->opts->input_volume_multiplier;
    if (m < 1) {
        m = 1;
    }
    if (m > 16) {
        m = 16;
    }
    if (ui_prompt_int_prefill("Input Volume Multiplier (1..16)", m, &m)) {
        if (m < 1) {
            m = 1;
        }
        if (m > 16) {
            m = 16;
        }
        c->opts->input_volume_multiplier = m;
        ui_statusf("Input Volume set to %dX", m);
    }
}

static void
io_input_vol_up(void* vctx) {
    UiCtx* c = (UiCtx*)vctx;
    int m = c->opts->input_volume_multiplier;
    if (m < 16) {
        m++;
    }
    c->opts->input_volume_multiplier = m;
    ui_statusf("Input Volume: %dX", m);
}

static void
io_input_vol_dn(void* vctx) {
    UiCtx* c = (UiCtx*)vctx;
    int m = c->opts->input_volume_multiplier;
    if (m > 1) {
        m--;
    }
    c->opts->input_volume_multiplier = m;
    ui_statusf("Input Volume: %dX", m);
}

static const char*
lbl_input_volume(void* vctx, char* b, size_t n) {
    UiCtx* c = (UiCtx*)vctx;
    int m = c->opts->input_volume_multiplier;
    if (m < 1) {
        m = 1;
    }
    snprintf(b, n, "Input Volume: %dX", m);
    return b;
}

static void
io_toggle_p25_rrc(void* vctx) {
    UiCtx* c = (UiCtx*)vctx;
    c->opts->p25_c4fm_rrc_fixed = c->opts->p25_c4fm_rrc_fixed ? 0 : 1;
}

static void
io_toggle_p25p2_rrc(void* vctx) {
    UiCtx* c = (UiCtx*)vctx;
    c->opts->p25_p2_rrc_fixed = c->opts->p25_p2_rrc_fixed ? 0 : 1;
#ifdef USE_RTLSDR
    int alpha = c->opts->p25_p2_rrc_fixed ? 50 : 20;
    rtl_stream_cqpsk_set_rrc(1, alpha, 0);
#endif
}

static void
io_toggle_p25p2_rrc_autoprobe(void* vctx) {
    UiCtx* c = (UiCtx*)vctx;
    c->opts->p25_p2_rrc_autoprobe = c->opts->p25_p2_rrc_autoprobe ? 0 : 1;
#ifdef USE_RTLSDR
    rtl_stream_set_p25p2_rrc_autoprobe(c->opts->p25_p2_rrc_autoprobe);
#endif
}

static void
io_toggle_p25_rrc_autoprobe(void* vctx) {
    UiCtx* c = (UiCtx*)vctx;
    c->opts->p25_c4fm_rrc_autoprobe = c->opts->p25_c4fm_rrc_autoprobe ? 0 : 1;
    // Reset auto-probe runtime state on toggle
    if (c->state) {
        c->state->p25_rrc_auto_state = 0;
        c->state->p25_rrc_auto_decided = 0;
        c->state->p25_rrc_auto_start = 0;
        c->state->p25_rrc_auto_fec_ok_base = 0;
        c->state->p25_rrc_auto_fec_err_base = 0;
        c->state->p25_rrc_auto_dyn_fec_err = 0;
        c->state->p25_rrc_auto_fix_fec_err = 0;
        c->state->p25_rrc_auto_dyn_voice_avg = 0.0;
        c->state->p25_rrc_auto_fix_voice_avg = 0.0;
        c->state->p25_rrc_auto_choice = 0;
    }
}

static void
inv_x2(void* v) {
    svc_toggle_inv_x2(((UiCtx*)v)->opts);
}

static void
inv_dmr(void* v) {
    svc_toggle_inv_dmr(((UiCtx*)v)->opts);
}

static void
inv_dpmr(void* v) {
    svc_toggle_inv_dpmr(((UiCtx*)v)->opts);
}

static void
inv_m17(void* v) {
    svc_toggle_inv_m17(((UiCtx*)v)->opts);
}

#ifdef USE_RTLSDR
// ---- RTL-SDR submenu ----
static __attribute__((unused)) const char*
lbl_rtl_summary(void* v, char* b, size_t n) {
    UiCtx* c = (UiCtx*)v;
    snprintf(b, n, "Dev %d  Freq %u Hz  Gain %d  PPM %d  BW %d kHz  SQL %.1f dB  VOL %d", c->opts->rtl_dev_index,
             c->opts->rtlsdr_center_freq, c->opts->rtl_gain_value, c->opts->rtlsdr_ppm_error, c->opts->rtl_bandwidth,
             pwr_to_dB(c->opts->rtl_squelch_level), c->opts->rtl_volume_multiplier);
    return b;
}

static void
rtl_enable(void* v) {
    svc_rtl_enable_input(((UiCtx*)v)->opts);
}

static void
rtl_restart(void* v) {
    svc_rtl_restart(((UiCtx*)v)->opts);
}

static void
rtl_set_dev(void* v) {
    UiCtx* c = (UiCtx*)v;
    int i = c->opts->rtl_dev_index;
    if (ui_prompt_int_prefill("Device index", i, &i)) {
        svc_rtl_set_dev_index(c->opts, i);
    }
}

static void
rtl_set_freq(void* v) {
    UiCtx* c = (UiCtx*)v;
    int f = (int)c->opts->rtlsdr_center_freq;
    if (ui_prompt_int_prefill("Frequency (Hz)", f, &f)) {
        svc_rtl_set_freq(c->opts, (uint32_t)f);
    }
}

static void
rtl_set_gain(void* v) {
    UiCtx* c = (UiCtx*)v;
    int g = c->opts->rtl_gain_value;
    if (ui_prompt_int_prefill("Gain (0=AGC, 0..49)", g, &g)) {
        svc_rtl_set_gain(c->opts, g);
    }
}

static void
rtl_set_ppm(void* v) {
    UiCtx* c = (UiCtx*)v;
    int p = c->opts->rtlsdr_ppm_error;
    if (ui_prompt_int_prefill("PPM error (-200..200)", p, &p)) {
        svc_rtl_set_ppm(c->opts, p);
    }
}

static void
rtl_set_bw(void* v) {
    UiCtx* c = (UiCtx*)v;
    int bw = c->opts->rtl_bandwidth;
    if (ui_prompt_int_prefill("Bandwidth kHz (4,6,8,12,16,24)", bw, &bw)) {
        svc_rtl_set_bandwidth(c->opts, bw);
    }
}

static void
rtl_set_sql(void* v) {
    UiCtx* c = (UiCtx*)v;
    double dB = pwr_to_dB(c->opts->rtl_squelch_level);
    if (ui_prompt_double_prefill("Squelch (dB, negative)", dB, &dB)) {
        svc_rtl_set_sql_db(c->opts, dB);
    }
}

static void
rtl_set_vol(void* v) {
    UiCtx* c = (UiCtx*)v;
    int m = c->opts->rtl_volume_multiplier;
    if (ui_prompt_int_prefill("Volume multiplier (0..3)", m, &m)) {
        svc_rtl_set_volume_mult(c->opts, m);
    }
}

static void
rtl_toggle_bias(void* v) {
    UiCtx* c = (UiCtx*)v;
    svc_rtl_set_bias_tee(c->opts, c->opts->rtl_bias_tee ? 0 : 1);
}

static const char*
lbl_rtl_bias(void* v, char* b, size_t n) {
    UiCtx* c = (UiCtx*)v;
    snprintf(b, n, "Bias Tee: %s", (c->opts->rtl_bias_tee ? "On" : "Off"));
    return b;
}

static void
rtl_toggle_rtltcp_autotune(void* v) {
    UiCtx* c = (UiCtx*)v;
    svc_rtltcp_set_autotune(c->opts, c->opts->rtltcp_autotune ? 0 : 1);
}

static const char*
lbl_rtl_rtltcp_autotune(void* v, char* b, size_t n) {
    UiCtx* c = (UiCtx*)v;
    snprintf(b, n, "RTL-TCP Adaptive Networking: %s", (c->opts->rtltcp_autotune ? "On" : "Off"));
    return b;
}

static void
rtl_toggle_auto_ppm(void* v) {
    UiCtx* c = (UiCtx*)v;
    svc_rtl_set_auto_ppm(c->opts, c->opts->rtl_auto_ppm ? 0 : 1);
}

static const char*
lbl_rtl_auto_ppm(void* v, char* b, size_t n) {
    UiCtx* c = (UiCtx*)v;
    int on = c->opts->rtl_auto_ppm ? 1 : 0;
    /* If stream active, reflect runtime state */
    if (g_rtl_ctx) {
        extern int rtl_stream_get_auto_ppm(void);
        on = rtl_stream_get_auto_ppm();
    }
    snprintf(b, n, "Auto-PPM (Spectrum): %s", on ? "On" : "Off");
    return b;
}

static void
rtl_toggle_tuner_autogain(void* v) {
    UiCtx* c = (UiCtx*)v;
    int on = 0;
    if (g_rtl_ctx) {
        on = rtl_stream_get_tuner_autogain();
        rtl_stream_set_tuner_autogain(on ? 0 : 1);
    } else {
        /* Persist choice into env for the next start */
        const char* e = getenv("DSD_NEO_TUNER_AUTOGAIN");
        on = (e && *e && *e != '0' && *e != 'f' && *e != 'F' && *e != 'n' && *e != 'N');
        setenv("DSD_NEO_TUNER_AUTOGAIN", on ? "0" : "1", 1);
    }
    (void)c;
}

static const char*
lbl_rtl_tuner_autogain(void* v, char* b, size_t n) {
    UNUSED(v);
    int on = 0;
    if (g_rtl_ctx) {
        on = rtl_stream_get_tuner_autogain();
    } else {
        const char* e = getenv("DSD_NEO_TUNER_AUTOGAIN");
        on = (e && *e && *e != '0' && *e != 'f' && *e != 'F' && *e != 'n' && *e != 'N');
    }
    snprintf(b, n, "Tuner Autogain: %s", on ? "On" : "Off");
    return b;
}

static void
ui_menu_rtl_options(dsd_opts* opts, dsd_state* state) {
    UiCtx ctx = {opts, state};
    static const NcMenuItem items[] = {
        /*{.id = "summary",
         .label = "Current Config",
         .label_fn = lbl_rtl_summary,
         .help = "Snapshot of RTL-SDR settings."},*/
        {.id = "enable", .label = "Enable RTL-SDR Input", .help = "Switch input to RTL-SDR.", .on_select = rtl_enable},
        {.id = "restart",
         .label = "Restart RTL Stream",
         .help = "Apply config by restarting the stream.",
         .on_select = rtl_restart},
        {.id = "dev", .label = "Set Device Index...", .help = "Select RTL device index.", .on_select = rtl_set_dev},
        {.id = "freq",
         .label = "Set Frequency (Hz)...",
         .help = "Set center frequency in Hz.",
         .on_select = rtl_set_freq},
        {.id = "gain", .label = "Set Gain...", .help = "0=AGC; else driver gain units.", .on_select = rtl_set_gain},
        {.id = "ppm", .label = "Set PPM error...", .help = "-200..200.", .on_select = rtl_set_ppm},
        {.id = "bw", .label = "Set Bandwidth (kHz)...", .help = "4,6,8,12,16,24.", .on_select = rtl_set_bw},
        {.id = "sql", .label = "Set Squelch (dB)...", .help = "More negative -> tighter.", .on_select = rtl_set_sql},
        {.id = "vol", .label = "Set Volume Multiplier...", .help = "0..3 sample scaler.", .on_select = rtl_set_vol},
        {.id = "auto_ppm",
         .label = "Auto-PPM (Spectrum)",
         .label_fn = lbl_rtl_auto_ppm,
         .help = "Enable/disable spectrum-based auto PPM tracking",
         .on_select = rtl_toggle_auto_ppm},
        {.id = "tuner_autogain",
         .label = "Tuner Autogain",
         .label_fn = lbl_rtl_tuner_autogain,
         .help = "Enable/disable supervisory tuner autogain.",
         .on_select = rtl_toggle_tuner_autogain},
        {.id = "bias",
         .label = "Toggle Bias Tee",
         .label_fn = lbl_rtl_bias,
         .help = "Enable/disable 5V bias tee (USB or rtl_tcp)",
         .on_select = rtl_toggle_bias},
        {.id = "rtltcp_autotune",
         .label = "RTL-TCP Adaptive Networking",
         .label_fn = lbl_rtl_rtltcp_autotune,
         .help = "Enable/disable adaptive buffering for rtl_tcp",
         .on_select = rtl_toggle_rtltcp_autotune},
    };
    ui_menu_run(items, sizeof items / sizeof items[0], &ctx);
}
#endif

static void
io_tcp_direct_link(void* vctx) {
    UiCtx* c = (UiCtx*)vctx;
    // Defaults
    snprintf(c->opts->tcp_hostname, sizeof c->opts->tcp_hostname, "%s", "localhost");
    c->opts->tcp_portno = 7355;

    if (!ui_prompt_string_prefill("Enter TCP Direct Link Hostname", c->opts->tcp_hostname, c->opts->tcp_hostname,
                                  sizeof c->opts->tcp_hostname)) {
        return;
    }
    int port = c->opts->tcp_portno;
    if (!ui_prompt_int_prefill("Enter TCP Direct Link Port Number", port, &port)) {
        return;
    }
    c->opts->tcp_portno = port;

    if (svc_tcp_connect_audio(c->opts, c->opts->tcp_hostname, c->opts->tcp_portno) == 0) {
        ui_statusf("TCP connected: %s:%d", c->opts->tcp_hostname, c->opts->tcp_portno);
    } else {
        ui_statusf("TCP connect failed: %s:%d", c->opts->tcp_hostname, c->opts->tcp_portno);
    }
}

// ---- Switch Input helpers ----
static const char*
lbl_current_input(void* vctx, char* b, size_t n) {
    UiCtx* c = (UiCtx*)vctx;
    const char* name;
    switch (c->opts->audio_in_type) {
        case 0: name = "Pulse"; break;
        case 1: name = "STDIN"; break;
        case 2: name = "WAV/File"; break;
        case 3: name = "RTL-SDR"; break;
        case 4: name = "Symbol .bin"; break;
        case 5: name = "OSS /dev/dsp"; break;
        case 6: name = "UDP"; break;
        case 8: name = "TCP"; break;
        case 44: name = "Symbol Float"; break;
        default: name = "?"; break;
    }
    if (c->opts->audio_in_type == 8) {
        snprintf(b, n, "Current Input: TCP %s:%d", c->opts->tcp_hostname, c->opts->tcp_portno);
    } else if (c->opts->audio_in_type == 6) {
        snprintf(b, n, "Current Input: UDP %s:%d", c->opts->udp_in_bindaddr[0] ? c->opts->udp_in_bindaddr : "127.0.0.1",
                 c->opts->udp_in_portno);
    } else if (c->opts->audio_in_type == 2 || c->opts->audio_in_type == 4 || c->opts->audio_in_type == 44) {
        snprintf(b, n, "Current Input: %s", c->opts->audio_in_dev);
    } else if (c->opts->audio_in_type == 3) {
        snprintf(b, n, "Current Input: RTL-SDR dev %d", c->opts->rtl_dev_index);
    } else {
        snprintf(b, n, "Current Input: %s", name);
    }
    return b;
}

static void
switch_to_pulse(void* vctx) {
    UiCtx* c = (UiCtx*)vctx;
    snprintf(c->opts->audio_in_dev, sizeof c->opts->audio_in_dev, "%s", "pulse");
    c->opts->audio_in_type = 0;
}

#ifdef USE_RTLSDR
static void
switch_to_rtl(void* vctx) {
    UiCtx* c = (UiCtx*)vctx;
    svc_rtl_enable_input(c->opts);
}
#endif

static void
switch_to_wav(void* vctx) {
    UiCtx* c = (UiCtx*)vctx;
    char path[1024] = {0};
    if (ui_prompt_string("Enter WAV/RAW filename (or named pipe)", path, sizeof path)) {
        snprintf(c->opts->audio_in_dev, sizeof c->opts->audio_in_dev, "%s", path);
        c->opts->audio_in_type = 2; // openAudioInDevice will refine based on extension
    }
}

static void
switch_to_symbol(void* vctx) {
    UiCtx* c = (UiCtx*)vctx;
    char path[1024] = {0};
    if (ui_prompt_string("Enter symbol .bin/.raw/.sym filename", path, sizeof path)) {
        // Prefer .bin via service; else let openAudioInDevice detect .raw/.sym
        size_t len = strlen(path);
        if (len >= 4 && (strcasecmp(path + len - 4, ".bin") == 0)) {
            if (svc_open_symbol_in(c->opts, c->state, path) != 0) {
                ui_statusf("Failed to open %s", path);
            }
        } else {
            snprintf(c->opts->audio_in_dev, sizeof c->opts->audio_in_dev, "%s", path);
            // Type refined on reopen; set a sensible default
            c->opts->audio_in_type = 44; // float symbols for .raw/.sym
        }
    }
}

static void
switch_to_tcp(void* vctx) {
    io_tcp_direct_link(vctx);
}

static void
switch_to_udp(void* vctx) {
    UiCtx* c = (UiCtx*)vctx;
    // Defaults
    if (c->opts->udp_in_portno <= 0) {
        c->opts->udp_in_portno = 7355;
    }
    if (c->opts->udp_in_bindaddr[0] == '\0') {
        snprintf(c->opts->udp_in_bindaddr, sizeof c->opts->udp_in_bindaddr, "%s", "127.0.0.1");
    }
    char addr[128];
    snprintf(addr, sizeof addr, "%.*s", (int)sizeof(addr) - 1, c->opts->udp_in_bindaddr);
    if (!ui_prompt_string_prefill("Enter UDP bind address", c->opts->udp_in_bindaddr, addr, sizeof addr)) {
        return;
    }
    int port = c->opts->udp_in_portno;
    if (!ui_prompt_int_prefill("Enter UDP bind port", port, &port)) {
        return;
    }
    snprintf(c->opts->udp_in_bindaddr, sizeof c->opts->udp_in_bindaddr, "%s", addr);
    c->opts->udp_in_portno = port;
    snprintf(c->opts->audio_in_dev, sizeof c->opts->audio_in_dev, "%s", "udp");
    c->opts->audio_in_type = 6;
}

static void
io_rigctl_config(void* vctx) {
    UiCtx* c = (UiCtx*)vctx;
    // Defaults
    snprintf(c->opts->rigctlhostname, sizeof c->opts->rigctlhostname, "%s", "localhost");
    c->opts->rigctlportno = 4532;

    if (!ui_prompt_string_prefill("Enter RIGCTL Hostname", c->opts->rigctlhostname, c->opts->rigctlhostname,
                                  sizeof c->opts->rigctlhostname)) {
        c->opts->use_rigctl = 0;
        return;
    }
    int port = c->opts->rigctlportno;
    if (!ui_prompt_int_prefill("Enter RIGCTL Port Number", port, &port)) {
        c->opts->use_rigctl = 0;
        return;
    }
    c->opts->rigctlportno = port;

    if (svc_rigctl_connect(c->opts, c->opts->rigctlhostname, c->opts->rigctlportno) == 0) {
        ui_statusf("Rigctl connected: %s:%d", c->opts->rigctlhostname, c->opts->rigctlportno);
    } else {
        ui_statusf("Rigctl connect failed: %s:%d", c->opts->rigctlhostname, c->opts->rigctlportno);
    }
}

// ---- Dynamic labels for IO ----
static const char*
lbl_sym_save(void* vctx, char* b, size_t n) {
    UiCtx* c = (UiCtx*)vctx;
    if (c->opts->symbol_out_f) {
        snprintf(b, n, "Save Symbols to File [Active: %s]", c->opts->symbol_out_file);
    } else {
        snprintf(b, n, "Save Symbols to File [Inactive]");
    }
    return b;
}

static const char*
lbl_tcp(void* vctx, char* b, size_t n) {
    UiCtx* c = (UiCtx*)vctx;
    int active = (c->opts->audio_in_type == 8 && c->opts->tcp_file_in != NULL);
    if (c->opts->tcp_hostname[0] != '\0' && c->opts->tcp_portno > 0) {
        if (active) {
            snprintf(b, n, "TCP Direct Audio: %s:%d [Active]", c->opts->tcp_hostname, c->opts->tcp_portno);
        } else {
            snprintf(b, n, "TCP Direct Audio: %s:%d [Inactive]", c->opts->tcp_hostname, c->opts->tcp_portno);
        }
    } else {
        snprintf(b, n, active ? "TCP Direct Audio [Active]" : "Start TCP Direct Audio [Inactive]");
    }
    return b;
}

static const char*
lbl_rigctl(void* vctx, char* b, size_t n) {
    UiCtx* c = (UiCtx*)vctx;
    int connected = (c->opts->use_rigctl && c->opts->rigctl_sockfd != 0);
    if (c->opts->rigctlhostname[0] != '\0' && c->opts->rigctlportno > 0) {
        if (connected) {
            snprintf(b, n, "Rigctl: %s:%d [Active]", c->opts->rigctlhostname, c->opts->rigctlportno);
        } else {
            snprintf(b, n, "Rigctl: %s:%d [Inactive]", c->opts->rigctlhostname, c->opts->rigctlportno);
        }
    } else {
        snprintf(b, n, connected ? "Rigctl [Active]" : "Configure Rigctl [Inactive]");
    }
    return b;
}

static const char*
lbl_replay_last(void* vctx, char* b, size_t n) {
    UiCtx* c = (UiCtx*)vctx;
    if (c->opts->audio_in_dev[0] != '\0') {
        struct stat sb;
        if (stat(c->opts->audio_in_dev, &sb) == 0 && S_ISREG(sb.st_mode)) {
            snprintf(b, n, "Replay Last Symbol Capture [%s]", c->opts->audio_in_dev);
            return b;
        }
    }
    snprintf(b, n, "Replay Last Symbol Capture [Inactive]");
    return b;
}

static const char*
lbl_per_call_wav(void* vctx, char* b, size_t n) {
    UiCtx* c = (UiCtx*)vctx;
    if (c->opts->dmr_stereo_wav == 1 && c->opts->wav_out_f != NULL) {
        snprintf(b, n, "Save Per-Call WAV [Active]");
    } else {
        snprintf(b, n, "Save Per-Call WAV [Inactive]");
    }
    return b;
}

static const char*
lbl_stop_symbol_playback(void* vctx, char* b, size_t n) {
    UiCtx* c = (UiCtx*)vctx;
    if (c->opts->symbolfile != NULL && c->opts->audio_in_type == 4) {
        if (c->opts->audio_in_dev[0] != '\0') {
            snprintf(b, n, "Stop Symbol Playback [Active: %s]", c->opts->audio_in_dev);
        } else {
            snprintf(b, n, "Stop Symbol Playback [Active]");
        }
    } else {
        snprintf(b, n, "Stop Symbol Playback [Inactive]");
    }
    return b;
}

static const char*
lbl_stop_symbol_capture(void* vctx, char* b, size_t n) {
    UiCtx* c = (UiCtx*)vctx;
    if (c->opts->symbol_out_f) {
        if (c->opts->symbol_out_file[0] != '\0') {
            snprintf(b, n, "Stop Symbol Capture [Active: %s]", c->opts->symbol_out_file);
        } else {
            snprintf(b, n, "Stop Symbol Capture [Active]");
        }
    } else {
        snprintf(b, n, "Stop Symbol Capture [Inactive]");
    }
    return b;
}

// ---- Toggle status labels (file-scope helpers) ----
static const char*
lbl_invert_all(void* v, char* b, size_t n) {
    UiCtx* c = (UiCtx*)v;
    snprintf(b, n, "Toggle Signal Inversion [%s]", c->opts->inverted_dmr ? "Active" : "Inactive");
    return b;
}

static const char*
lbl_inv_x2(void* v, char* b, size_t n) {
    UiCtx* c = (UiCtx*)v;
    snprintf(b, n, "Invert X2-TDMA [%s]", c->opts->inverted_x2tdma ? "Active" : "Inactive");
    return b;
}

static const char*
lbl_inv_dmr(void* v, char* b, size_t n) {
    UiCtx* c = (UiCtx*)v;
    snprintf(b, n, "Invert DMR [%s]", c->opts->inverted_dmr ? "Active" : "Inactive");
    return b;
}

static const char*
lbl_inv_dpmr(void* v, char* b, size_t n) {
    UiCtx* c = (UiCtx*)v;
    snprintf(b, n, "Invert dPMR [%s]", c->opts->inverted_dpmr ? "Active" : "Inactive");
    return b;
}

static const char*
lbl_inv_m17(void* v, char* b, size_t n) {
    UiCtx* c = (UiCtx*)v;
    snprintf(b, n, "Invert M17 [%s]", c->opts->inverted_m17 ? "Active" : "Inactive");
    return b;
}

static const char*
lbl_monitor(void* v, char* b, size_t n) {
    UiCtx* c = (UiCtx*)v;
    snprintf(b, n, "Toggle Source Audio Monitor [%s]", c->opts->monitor_input_audio ? "Active" : "Inactive");
    return b;
}

static const char*
lbl_cosine(void* v, char* b, size_t n) {
    UiCtx* c = (UiCtx*)v;
    snprintf(b, n, "Toggle Cosine Filter [%s]", c->opts->use_cosine_filter ? "Active" : "Inactive");
    return b;
}

static const char*
lbl_p25_rrc(void* v, char* b, size_t n) {
    UiCtx* c = (UiCtx*)v;
    snprintf(b, n, "P25 C4FM RRC alpha=0.5 [%s]", c->opts->p25_c4fm_rrc_fixed ? "Active" : "Inactive");
    return b;
}

static const char*
lbl_p25_rrc_autoprobe(void* v, char* b, size_t n) {
    UiCtx* c = (UiCtx*)v;
    snprintf(b, n, "P25 C4FM RRC Auto-Probe [%s]", c->opts->p25_c4fm_rrc_autoprobe ? "Active" : "Inactive");
    return b;
}

static const char*
lbl_p25p2_rrc(void* v, char* b, size_t n) {
    UiCtx* c = (UiCtx*)v;
    snprintf(b, n, "P25p2 CQPSK RRC alpha=0.5 [%s]", c->opts->p25_p2_rrc_fixed ? "Active" : "Inactive");
    return b;
}

static const char*
lbl_p25p2_rrc_autoprobe(void* v, char* b, size_t n) {
    UiCtx* c = (UiCtx*)v;
#ifdef USE_RTLSDR
    int on = rtl_stream_get_p25p2_rrc_autoprobe();
#else
    int on = c->opts->p25_p2_rrc_autoprobe;
#endif
    snprintf(b, n, "P25p2 CQPSK RRC Auto-Probe [%s]", on ? "Active" : "Inactive");
    return b;
}

static const char*
lbl_toggle_payload(void* v, char* b, size_t n) {
    UiCtx* c = (UiCtx*)v;
    snprintf(b, n, "Toggle Payload Logging [%s]", c->opts->payload ? "Active" : "Inactive");
    return b;
}

static const char*
lbl_call_alert(void* v, char* b, size_t n) {
    UiCtx* c = (UiCtx*)v;
    snprintf(b, n, "Toggle Call Alert Beep [%s]", c->opts->call_alert ? "Active" : "Inactive");
    return b;
}

static const char*
lbl_crc_relax(void* v, char* b, size_t n) {
    UiCtx* c = (UiCtx*)v;
    int relaxed = (c->opts->aggressive_framesync == 0);
    snprintf(b, n, "Toggle Relaxed CRC checks [%s]", relaxed ? "Active" : "Inactive");
    return b;
}

static const char*
lbl_trunk(void* v, char* b, size_t n) {
    UiCtx* c = (UiCtx*)v;
    snprintf(b, n, "Toggle Trunking [%s]", c->opts->p25_trunk ? "Active" : "Inactive");
    return b;
}

static const char*
lbl_scan(void* v, char* b, size_t n) {
    UiCtx* c = (UiCtx*)v;
    snprintf(b, n, "Toggle Scanning Mode [%s]", c->opts->scanner_mode ? "Active" : "Inactive");
    return b;
}

static const char*
lbl_pref_cc(void* v, char* b, size_t n) {
    UiCtx* c = (UiCtx*)v;
    snprintf(b, n, "Prefer P25 CC Candidates [%s]", c->opts->p25_prefer_candidates ? "Active" : "Inactive");
    return b;
}

static const char*
lbl_lcw(void* v, char* b, size_t n) {
    UiCtx* c = (UiCtx*)v;
    snprintf(b, n, "Toggle P25 LCW Retune [%s]", c->opts->p25_lcw_retune ? "Active" : "Inactive");
    return b;
}

static const char*
lbl_p25_auto_adapt(void* v, char* b, size_t n) {
    UiCtx* c = (UiCtx*)v;
    snprintf(b, n, "P25 Auto-Adapt (beta) [%s]", (c && c->opts && c->opts->p25_auto_adapt) ? "On" : "Off");
    return b;
}

static const char*
lbl_p25_sm_basic(void* v, char* b, size_t n) {
    UiCtx* c = (UiCtx*)v;
    snprintf(b, n, "P25 Simple SM (basic) [%s]", (c && c->opts && c->opts->p25_sm_basic_mode) ? "On" : "Off");
    return b;
}

static const char*
lbl_allow(void* v, char* b, size_t n) {
    UiCtx* c = (UiCtx*)v;
    snprintf(b, n, "Toggle Allow/White List [%s]", c->opts->trunk_use_allow_list ? "Active" : "Inactive");
    return b;
}

static const char*
lbl_tune_group(void* v, char* b, size_t n) {
    UiCtx* c = (UiCtx*)v;
    snprintf(b, n, "Toggle Tune Group Calls [%s]", c->opts->trunk_tune_group_calls ? "Active" : "Inactive");
    return b;
}

static const char*
lbl_tune_priv(void* v, char* b, size_t n) {
    UiCtx* c = (UiCtx*)v;
    snprintf(b, n, "Toggle Tune Private Calls [%s]", c->opts->trunk_tune_private_calls ? "Active" : "Inactive");
    return b;
}

static const char*
lbl_tune_data(void* v, char* b, size_t n) {
    UiCtx* c = (UiCtx*)v;
    snprintf(b, n, "Toggle Tune Data Calls [%s]", c->opts->trunk_tune_data_calls ? "Active" : "Inactive");
    return b;
}

static const char*
lbl_rev_mute(void* v, char* b, size_t n) {
    UiCtx* c = (UiCtx*)v;
    snprintf(b, n, "Toggle Reverse Mute [%s]", c->opts->reverse_mute ? "Active" : "Inactive");
    return b;
}

static const char*
lbl_dmr_le(void* v, char* b, size_t n) {
    UiCtx* c = (UiCtx*)v;
    snprintf(b, n, "Toggle DMR Late Entry [%s]", c->opts->dmr_le ? "Active" : "Inactive");
    return b;
}

static const char*
lbl_slotpref(void* v, char* b, size_t n) {
    UiCtx* c = (UiCtx*)v;
    const char* now = (c->opts->slot_preference == 0) ? "1" : (c->opts->slot_preference == 1) ? "2" : "Auto";
    snprintf(b, n, "Set TDMA Slot Preference... [now %s]", now);
    return b;
}

static const char*
lbl_slots_on(void* v, char* b, size_t n) {
    UiCtx* c = (UiCtx*)v;
    const char* now = (c->opts->slot1_on && c->opts->slot2_on)
                          ? "both"
                          : (c->opts->slot1_on ? "1" : (c->opts->slot2_on ? "2" : "off"));
    snprintf(b, n, "Set TDMA Synth Slots... [now %s]", now);
    return b;
}

static const char*
lbl_muting(void* v, char* b, size_t n) {
    UiCtx* c = (UiCtx*)v;
    int dmr = (c->opts->dmr_mute_encL == 1 && c->opts->dmr_mute_encR == 1);
    int p25 = (c->opts->unmute_encrypted_p25 == 0);
    int active = (dmr && p25);
    snprintf(b, n, "Toggle Encrypted Audio Muting [%s]", active ? "Active" : "Inactive");
    return b;
}

void
ui_menu_io_options(dsd_opts* opts, dsd_state* state) {
    // Reorganized: Devices & IO (sources and immediate playback controls)
    UiCtx ctx = {opts, state};
    // Switch Input submenu (always compiled; RTL option gated by build)
    static const NcMenuItem switch_items[] = {
        {.id = "current", .label = "Current", .label_fn = lbl_current_input, .help = "Shows current input."},
        {.id = "pulse",
         .label = "Pulse Audio (mic/line)",
         .help = "Use Pulse Audio input.",
         .on_select = switch_to_pulse},
#ifdef USE_RTLSDR
        {.id = "rtl", .label = "RTL-SDR", .help = "Switch to RTL-SDR input.", .on_select = switch_to_rtl},
#endif
        {.id = "tcp",
         .label = "TCP Direct Audio...",
         .help = "Connect to PCM16LE over TCP.",
         .on_select = switch_to_tcp},
        {.id = "wav", .label = "WAV/File...", .help = "Open WAV/RAW file or named pipe.", .on_select = switch_to_wav},
        {.id = "sym",
         .label = "Symbol Capture (.bin/.raw/.sym)...",
         .help = "Replay captured symbols.",
         .on_select = switch_to_symbol},
        {.id = "udp", .label = "UDP Signal Input...", .help = "Bind UDP PCM16LE input.", .on_select = switch_to_udp},
    };
    static const NcMenuItem out_switch_items[] = {
        {.id = "current_out",
         .label = "Current Output",
         .label_fn = lbl_current_output,
         .help = "Shows the active output sink."},
        {.id = "pulse_out",
         .label = "Pulse Digital Output",
         .help = "Play decoded audio via Pulse.",
         .on_select = switch_out_pulse},
        {.id = "udp_out_set",
         .label = "UDP Audio Output...",
         .help = "Send decoded audio via UDP.",
         .on_select = switch_out_udp},
        {.id = "mute",
         .label = "Mute Output",
         .label_fn = lbl_out_mute,
         .help = "Toggle mute without changing sink.",
         .on_select = switch_out_toggle_mute},
    };

    static const NcMenuItem items[] = {
        {.id = "switch_input",
         .label = "Switch Input...",
         .help = "Change active input source.",
         .submenu = switch_items,
         .submenu_len = sizeof switch_items / sizeof switch_items[0]},
        {.id = "switch_output",
         .label = "Switch Output...",
         .help = "Change audio output sink.",
         .submenu = out_switch_items,
         .submenu_len = sizeof out_switch_items / sizeof out_switch_items[0]},
        {.id = "tcp_input",
         .label = "TCP Direct Audio",
         .label_fn = lbl_tcp,
         .help = "Connect to a remote PCM16LE source via TCP.",
         .is_enabled = io_always_on,
         .on_select = io_tcp_direct_link},
#ifdef USE_RTLSDR
        {.id = "rtl",
         .label = "RTL-SDR...",
         .help = "Configure RTL device, gain, PPM, BW, SQL.",
         .is_enabled = io_rtl_active,
         .on_select = act_rtl_opts},
#endif
        {.id = "pulse_in",
         .label = "Set Pulse Input...",
         .help = "Set Pulse input by index/name.",
         .is_enabled = io_always_on,
         .on_select = io_set_pulse_in},
        {.id = "pulse_out",
         .label = "Set Pulse Output...",
         .help = "Set Pulse output by index/name.",
         .is_enabled = io_always_on,
         .on_select = io_set_pulse_out},
        {.id = "read_sym",
         .label = "Read Symbol Capture File",
         .help = "Open an existing symbol capture for replay.",
         .is_enabled = io_always_on,
         .on_select = io_read_symbol_bin},
        {.id = "replay_last",
         .label = "Replay Last Symbol Capture",
         .label_fn = lbl_replay_last,
         .help = "Re-open the last used symbol capture file.",
         .is_enabled = io_always_on,
         .on_select = io_replay_last_symbol_bin},
        {.id = "stop_playback",
         .label = "Stop Symbol Playback",
         .label_fn = lbl_stop_symbol_playback,
         .help = "Stop replaying the symbol capture and restore input mode.",
         .is_enabled = io_always_on,
         .on_select = io_stop_symbol_playback},
        {.id = "invert",
         .label = "Toggle Signal Inversion",
         .label_fn = lbl_invert_all,
         .help = "Invert/uninvert all supported inputs.",
         .is_enabled = io_always_on,
         .on_select = act_toggle_invert},
        {.id = "inv_x2",
         .label = "Invert X2-TDMA",
         .label_fn = lbl_inv_x2,
         .help = "Toggle X2 inversion.",
         .on_select = inv_x2},
        {.id = "inv_dmr",
         .label = "Invert DMR",
         .label_fn = lbl_inv_dmr,
         .help = "Toggle DMR inversion.",
         .on_select = inv_dmr},
        {.id = "inv_dpmr",
         .label = "Invert dPMR",
         .label_fn = lbl_inv_dpmr,
         .help = "Toggle dPMR inversion.",
         .on_select = inv_dpmr},
        {.id = "inv_m17",
         .label = "Invert M17",
         .label_fn = lbl_inv_m17,
         .help = "Toggle M17 inversion.",
         .on_select = inv_m17},
        {.id = "udp_out",
         .label = "Configure UDP Output...",
         .help = "Set UDP blaster host/port and enable.",
         .on_select = io_set_udp_out},
        {.id = "gain_d", .label = "Set Digital Output Gain...", .help = "0=auto; 1..50.", .on_select = io_set_gain_dig},
        {.id = "gain_a", .label = "Set Analog Output Gain...", .help = "0..100.", .on_select = io_set_gain_ana},
        {.id = "in_vol_set",
         .label = "Set Input Volume...",
         .label_fn = lbl_input_volume,
         .help = "Scale non-RTL inputs by N (1..16).",
         .on_select = io_set_input_volume},
        {.id = "in_vol_up",
         .label = "Input Volume +1X",
         .help = "Increase non-RTL input gain.",
         .on_select = io_input_vol_up},
        {.id = "in_vol_dn",
         .label = "Input Volume -1X",
         .help = "Decrease non-RTL input gain.",
         .on_select = io_input_vol_dn},
        {.id = "monitor",
         .label = "Toggle Source Audio Monitor",
         .label_fn = lbl_monitor,
         .help = "Enable analog source monitor.",
         .on_select = io_toggle_monitor},
        {.id = "cosine",
         .label = "Toggle Cosine Filter",
         .label_fn = lbl_cosine,
         .help = "Enable/disable cosine filter.",
         .on_select = io_toggle_cosine},
        {.id = "p25_rrc",
         .label = "P25 C4FM RRC alpha=0.5",
         .label_fn = lbl_p25_rrc,
         .help = "Use fixed RRC(alpha=0.5) for P25p1 C4FM when Cosine Filter is enabled.",
         .on_select = io_toggle_p25_rrc},
        {.id = "p25_rrc_auto",
         .label = "P25 C4FM RRC Auto-Probe",
         .label_fn = lbl_p25_rrc_autoprobe,
         .help = "Probe alpha≈0.2 vs alpha=0.5 briefly and choose best.",
         .on_select = io_toggle_p25_rrc_autoprobe},
        {.id = "p25p2_rrc",
         .label = "P25p2 CQPSK RRC alpha=0.5",
         .label_fn = lbl_p25p2_rrc,
         .help = "Use fixed RRC(alpha=0.5) for P25p2 CQPSK (matched filter).",
         .on_select = io_toggle_p25p2_rrc},
        {.id = "p25p2_rrc_auto",
         .label = "P25p2 CQPSK RRC Auto-Probe",
         .label_fn = lbl_p25p2_rrc_autoprobe,
         .help = "Probe alpha≈0.2 vs alpha=0.5 briefly and choose best.",
         .on_select = io_toggle_p25p2_rrc_autoprobe},
    };
    ui_menu_run(items, sizeof items / sizeof items[0], &ctx);
}

// Logging & Capture submenu
void
ui_menu_logging_capture(dsd_opts* opts, dsd_state* state) {
    UiCtx ctx = {opts, state};
    static const NcMenuItem items[] = {
        {.id = "save_sym",
         .label = "Save Symbols to File",
         .label_fn = lbl_sym_save,
         .help = "Write raw symbols to a capture file for replay.",
         .is_enabled = io_always_on,
         .on_select = io_save_symbol_capture},
        {.id = "stop_save",
         .label = "Stop Symbol Capture",
         .label_fn = lbl_stop_symbol_capture,
         .help = "Close the current symbol capture output file.",
         .is_enabled = io_always_on,
         .on_select = io_stop_symbol_saving},
        {.id = "per_call_wav",
         .label = "Save Per-Call WAV",
         .label_fn = lbl_per_call_wav,
         .help = "Create per-call WAV files under the configured directory.",
         .is_enabled = io_always_on,
         .on_select = io_enable_per_call_wav},
        {.id = "payload",
         .label = "Toggle Payload Logging",
         .label_fn = lbl_toggle_payload,
         .help = "Toggle raw payloads to console.",
         .is_enabled = io_always_on,
         .on_select = act_toggle_payload},
        {.id = "event_on",
         .label = "Set Event Log File...",
         .help = "Append event history to a file.",
         .on_select = act_event_log_set},
        {.id = "event_off",
         .label = "Disable Event Log",
         .help = "Stop logging events to file.",
         .on_select = act_event_log_disable},
        {.id = "static_wav",
         .label = "Static WAV Output...",
         .help = "Append decoded audio to one WAV file.",
         .on_select = act_static_wav},
        {.id = "raw_wav",
         .label = "Raw Audio WAV...",
         .help = "Write raw 48k/1 input audio to WAV.",
         .on_select = act_raw_wav},
        {.id = "dsp_out",
         .label = "DSP Structured Output...",
         .help = "Write DSP structured or M17 stream to ./DSP/",
         .on_select = act_dsp_out},
        {.id = "crc_relax",
         .label = "Toggle Relaxed CRC checks",
         .label_fn = lbl_crc_relax,
         .help = "Relax CRC checks across protocols.",
         .on_select = act_crc_relax},
        {.id = "reset_eh",
         .label = "Reset Event History",
         .help = "Clear ring-buffered event history.",
         .is_enabled = io_always_on,
         .on_select = act_reset_eh},
        {.id = "call_alert",
         .label = "Toggle Call Alert Beep",
         .label_fn = lbl_call_alert,
         .help = "Audible beep on call start.",
         .is_enabled = io_always_on,
         .on_select = io_toggle_call_alert},
    };
    ui_menu_run(items, sizeof items / sizeof items[0], &ctx);
}

// Trunking & Control submenu
void
ui_menu_trunking_control(dsd_opts* opts, dsd_state* state) {
    UiCtx ctx = {opts, state};
    static const NcMenuItem items[] = {
        {.id = "trunk_on",
         .label = "Toggle Trunking",
         .label_fn = lbl_trunk,
         .help = "Enable/disable trunking features.",
         .on_select = act_trunk_toggle},
        {.id = "scan_on",
         .label = "Toggle Scanning Mode",
         .label_fn = lbl_scan,
         .help = "Enable/disable conventional scanning.",
         .on_select = act_scan_toggle},
        {.id = "prefer_cc",
         .label = "Prefer P25 CC Candidates",
         .label_fn = lbl_pref_cc,
         .help = "Prefer viable control-channel candidates during hunt.",
         .is_enabled = io_always_on,
         .on_select = io_toggle_cc_candidates},
        {.id = "lcw_retune",
         .label = "Toggle P25 LCW Retune",
         .label_fn = lbl_lcw,
         .help = "Enable LCW explicit retune.",
         .on_select = act_lcw_toggle},
        {.id = "p25_sm_basic",
         .label = "P25 Simple SM (basic)",
         .label_fn = lbl_p25_sm_basic,
         .help = "Enable simplified P25 SM (reduced safeties/post-hang gating).",
         .on_select = act_p25_sm_basic},
        {.id = "p25_auto_adapt",
         .label = "P25 Auto-Adapt (beta)",
         .label_fn = lbl_p25_auto_adapt,
         .help = "Enable/disable per-site adaptive follower timing.",
         .on_select = act_p25_auto_adapt},
        {.id = "p2params",
         .label = "Set P25 Phase 2 Parameters",
         .help = "Set WACN/SYSID/NAC manually.",
         .is_enabled = io_always_on,
         .on_select = act_p2_params},
        {.id = "rigctl",
         .label = "Rigctl",
         .label_fn = lbl_rigctl,
         .help = "Connect to a rigctl server for tuner control.",
         .is_enabled = io_always_on,
         .on_select = io_rigctl_config},
        {.id = "setmod_bw",
         .label = "Set Rigctl Setmod BW...",
         .help = "Set rigctl setmod bandwidth (Hz).",
         .on_select = act_setmod_bw},
        {.id = "chan_map",
         .label = "Import Channel Map CSV...",
         .help = "Load channel->frequency map.",
         .on_select = act_import_chan},
        {.id = "group_list",
         .label = "Import Group List CSV...",
         .help = "Load groups allow/block & labels.",
         .on_select = act_import_group},
        {.id = "allow_list",
         .label = "Toggle Allow/White List",
         .label_fn = lbl_allow,
         .help = "Use group list as allow list.",
         .on_select = act_allow_toggle},
        {.id = "tune_group",
         .label = "Toggle Tune Group Calls",
         .label_fn = lbl_tune_group,
         .help = "Enable/disable group call tuning.",
         .on_select = act_tune_group},
        {.id = "tune_priv",
         .label = "Toggle Tune Private Calls",
         .label_fn = lbl_tune_priv,
         .help = "Enable/disable private call tuning.",
         .on_select = act_tune_priv},
        {.id = "tune_data",
         .label = "Toggle Tune Data Calls",
         .label_fn = lbl_tune_data,
         .help = "Enable/disable data call tuning.",
         .on_select = act_tune_data},
        {.id = "tg_hold",
         .label = "Set TG Hold...",
         .help = "Hold on a specific TG while trunking.",
         .on_select = act_tg_hold},
        {.id = "hangtime",
         .label = "Set Hangtime (s)...",
         .help = "VC/sync loss hangtime (seconds).",
         .on_select = act_hangtime},
        {.id = "reverse_mute",
         .label = "Toggle Reverse Mute",
         .label_fn = lbl_rev_mute,
         .help = "Reverse mute behavior.",
         .on_select = act_rev_mute},
        {.id = "dmr_le",
         .label = "Toggle DMR Late Entry",
         .label_fn = lbl_dmr_le,
         .help = "Enable/disable DMR late entry.",
         .on_select = act_dmr_le},
        {.id = "slot_pref",
         .label = "Set TDMA Slot Preference...",
         .label_fn = lbl_slotpref,
         .help = "Prefer slot 1 or 2 (DMR/P25p2).",
         .on_select = act_slot_pref},
        {.id = "slots_on",
         .label = "Set TDMA Synth Slots...",
         .label_fn = lbl_slots_on,
         .help = "Bitmask: 1=slot1, 2=slot2, 3=both, 0=off.",
         .on_select = act_slots_on},
    };
    ui_menu_run(items, sizeof items / sizeof items[0], &ctx);
}

// Keys & Security submenu
static void
act_keys_submenu(void* v) {
    UiCtx* c = (UiCtx*)v;
    ui_menu_key_entry(c->opts, c->state);
}

void
ui_menu_keys_security(dsd_opts* opts, dsd_state* state) {
    UiCtx ctx = {opts, state};
    static const NcMenuItem items[] = {
        {.id = "keys",
         .label = "Manage Encryption Keys...",
         .help = "Enter or edit BP/Hytera/RC4/AES keys.",
         .on_select = act_keys_submenu},
        {.id = "keys_dec",
         .label = "Import Keys CSV (DEC)...",
         .help = "Import decimal keys CSV.",
         .on_select = act_keys_dec},
        {.id = "keys_hex",
         .label = "Import Keys CSV (HEX)...",
         .help = "Import hexadecimal keys CSV.",
         .on_select = act_keys_hex},
        {.id = "muting",
         .label = "Toggle Encrypted Audio Muting",
         .label_fn = lbl_muting,
         .help = "Toggle P25 and DMR encrypted audio muting.",
         .is_enabled = io_always_on,
         .on_select = io_toggle_mute_enc},
        {.id = "tyt_ap",
         .label = "TYT AP (PC4) Keystream...",
         .help = "Enter AP seed string.",
         .on_select = act_tyt_ap},
        {.id = "retevis_rc2",
         .label = "Retevis AP (RC2) Keystream...",
         .help = "Enter AP seed string.",
         .on_select = act_retevis_rc2},
        {.id = "tyt_ep",
         .label = "TYT EP (AES) Keystream...",
         .help = "Enter EP seed string.",
         .on_select = act_tyt_ep},
        {.id = "ken_scr",
         .label = "Kenwood DMR Scrambler...",
         .help = "Enter scrambler seed.",
         .on_select = act_ken_scr},
        {.id = "anytone_bp", .label = "Anytone BP Keystream...", .help = "Enter BP seed.", .on_select = act_anytone_bp},
        {.id = "xor_ks",
         .label = "Straight XOR Keystream...",
         .help = "Enter raw string to XOR.",
         .on_select = act_xor_ks},
    };
    ui_menu_run(items, sizeof items / sizeof items[0], &ctx);
}

#ifdef USE_RTLSDR
// Declarative DSP menu with dynamic labels
static bool
dsp_cq_on(void* v) {
    UNUSED(v);
    int cq = 0, f = 0, t = 0, a = 0;
    rtl_stream_dsp_get(&cq, &f, &t, &a);
    return cq != 0;
}

static bool
dsp_lms_on(void* v) {
    UNUSED(v);
    int l = 0, t = 0, mu = 0, st = 0, wl = 0, dfe = 0, dft = 0, mf = 0, cma = 0;
    rtl_stream_cqpsk_get(&l, &t, &mu, &st, &wl, &dfe, &dft, &mf, &cma);
    return l != 0;
}

static bool
dsp_dfe_on(void* v) {
    UNUSED(v);
    int l = 0, t = 0, mu = 0, st = 0, wl = 0, dfe = 0, dft = 0, mf = 0, cma = 0;
    rtl_stream_cqpsk_get(&l, &t, &mu, &st, &wl, &dfe, &dft, &mf, &cma);
    return dfe != 0;
}

static const char*
lbl_onoff_cq(void* v, char* b, size_t n) {
    UNUSED(v);
    int cq = 0, f = 0, t = 0, a = 0;
    rtl_stream_dsp_get(&cq, &f, &t, &a);
    snprintf(b, n, "Toggle CQPSK [%s]", cq ? "Active" : "Inactive");
    return b;
}

static const char*
lbl_onoff_fll(void* v, char* b, size_t n) {
    UNUSED(v);
    int cq = 0, f = 0, t = 0, a = 0;
    rtl_stream_dsp_get(&cq, &f, &t, &a);
    snprintf(b, n, "Toggle FLL [%s]", f ? "Active" : "Inactive");
    return b;
}

static const char*
lbl_onoff_ted(void* v, char* b, size_t n) {
    UNUSED(v);
    int cq = 0, f = 0, t = 0, a = 0;
    rtl_stream_dsp_get(&cq, &f, &t, &a);
    snprintf(b, n, "Toggle TED [%s]", t ? "Active" : "Inactive");
    return b;
}

static const char*
lbl_onoff_iqbal(void* v, char* b, size_t n) {
    UNUSED(v);
    int on = rtl_stream_get_iq_balance();
    snprintf(b, n, "Toggle IQ Balance [%s]", on ? "Active" : "Inactive");
    return b;
}

/* ---- FM AGC / Limiter / DC Block UI helpers ---- */
static const char*
lbl_fm_agc(void* v, char* b, size_t n) {
    UNUSED(v);
    int on = rtl_stream_get_fm_agc();
    snprintf(b, n, "FM AGC [%s]", on ? "On" : "Off");
    return b;
}

/* ---- FM CMA Equalizer (pre-discriminator) ---- */
static const char*
lbl_fm_cma(void* v, char* b, size_t n) {
    UNUSED(v);
    int on = rtl_stream_get_fm_cma();
    snprintf(b, n, "FM CMA Equalizer [%s]", on ? "On" : "Off");
    return b;
}

/* ---- C4FM DD Equalizer (symbol-domain) ---- */
static const char*
lbl_c4fm_dd(void* v, char* b, size_t n) {
    UNUSED(v);
    int on = rtl_stream_get_c4fm_dd_eq();
    snprintf(b, n, "C4FM DD Equalizer [%s]", on ? "On" : "Off");
    return b;
}

static void
act_toggle_c4fm_dd(void* v) {
    UNUSED(v);
    int on = rtl_stream_get_c4fm_dd_eq();
    rtl_stream_set_c4fm_dd_eq(on ? 0 : 1);
}

static const char*
lbl_c4fm_dd_params(void* v, char* b, size_t n) {
    UNUSED(v);
    int taps = 0, mu = 0;
    rtl_stream_get_c4fm_dd_eq_params(&taps, &mu);
    if (taps <= 0) {
        taps = 3;
    }
    if (mu <= 0) {
        mu = 2;
    }
    snprintf(b, n, "DD Taps/Mu: %d / %d", taps, mu);
    return b;
}

static void
act_c4fm_dd_taps_cycle(void* v) {
    UNUSED(v);
    int taps = 0, mu = 0;
    rtl_stream_get_c4fm_dd_eq_params(&taps, &mu);
    int nt;
    if (taps < 5) {
        nt = 5;
    } else if (taps < 7) {
        nt = 7;
    } else if (taps < 9) {
        nt = 9;
    } else {
        nt = 3;
    }
    rtl_stream_set_c4fm_dd_eq_params(nt, -1);
}

static void
act_c4fm_dd_mu_up(void* v) {
    UNUSED(v);
    int taps = 0, mu = 0;
    rtl_stream_get_c4fm_dd_eq_params(&taps, &mu);
    if (mu < 64) {
        mu++;
    }
    rtl_stream_set_c4fm_dd_eq_params(-1, mu);
}

static void
act_c4fm_dd_mu_dn(void* v) {
    UNUSED(v);
    int taps = 0, mu = 0;
    rtl_stream_get_c4fm_dd_eq_params(&taps, &mu);
    if (mu > 1) {
        mu--;
    }
    rtl_stream_set_c4fm_dd_eq_params(-1, mu);
}

/* ---- C4FM clock assist (EL/MM) ---- */
static const char*
lbl_c4fm_clk(void* v, char* b, size_t n) {
    UNUSED(v);
    int mode = rtl_stream_get_c4fm_clk();
    const char* s = (mode == 1) ? "EL" : (mode == 2) ? "MM" : "Off";
    snprintf(b, n, "C4FM Clock: %s (cycle)", s);
    return b;
}

static void
act_c4fm_clk_cycle(void* v) {
    UNUSED(v);
    int mode = rtl_stream_get_c4fm_clk();
    mode = (mode + 1) % 3; /* 0->1->2->0 */
    rtl_stream_set_c4fm_clk(mode);
}

static const char*
lbl_c4fm_clk_sync(void* v, char* b, size_t n) {
    UNUSED(v);
    int en = rtl_stream_get_c4fm_clk_sync();
    snprintf(b, n, "C4FM Clock While Synced [%s]", en ? "Active" : "Inactive");
    return b;
}

static void
act_c4fm_clk_sync_toggle(void* v) {
    UNUSED(v);
    int en = rtl_stream_get_c4fm_clk_sync();
    rtl_stream_set_c4fm_clk_sync(en ? 0 : 1);
}

/* ---- One-click C4FM robustness preset ---- */
static const char*
lbl_c4fm_robust(void* v, char* b, size_t n) {
    UNUSED(v);
    snprintf(b, n, "C4FM Robustness Preset (apply)");
    return b;
}

static void
act_c4fm_robust(void* v) {
    UNUSED(v);
    /* Enable DD eq with modest settings */
    rtl_stream_set_c4fm_dd_eq(1);
    rtl_stream_set_c4fm_dd_eq_params(5, 2);
    /* Enable adaptive CMA with longer span; medium strength, continuous */
    rtl_stream_set_fm_cma(1);
    rtl_stream_set_fm_cma_params(7, 2, 0);
    rtl_stream_set_fm_cma_strength(1);
    /* Ensure limiter/AGC won't fight CMA */
    rtl_stream_set_fm_limiter(0);
    rtl_stream_set_fm_agc(0);
    /* Enable TED and force it for FM/C4FM; set SPS ~10 */
    rtl_stream_toggle_ted(1);
    rtl_stream_set_ted_force(1);
    rtl_stream_set_ted_sps(10);
}

static void
act_toggle_fm_cma(void* v) {
    UNUSED(v);
    int on = rtl_stream_get_fm_cma();
    rtl_stream_set_fm_cma(on ? 0 : 1);
}

static const char*
lbl_fm_cma_taps(void* v, char* b, size_t n) {
    UNUSED(v);
    int taps = 0;
    rtl_stream_get_fm_cma_params(&taps, NULL, NULL);
    /* 1: complex gain (CMA), 3: fixed smoother, 5/7/9: adaptive symmetric FIR */
    const char* desc;
    if (taps <= 1) {
        desc = "Complex gain (no multipath mitigation)";
        taps = 1;
    } else if (taps == 3) {
        desc = "3-tap short-echo smoother";
    } else if (taps == 5) {
        desc = "5-tap adaptive symmetric FIR";
    } else if (taps == 7) {
        desc = "7-tap adaptive symmetric FIR";
    } else {
        desc = "9-tap adaptive symmetric FIR";
        taps = 9;
    }
    snprintf(b, n, "CMA Taps (1/3/5/7/9): %d  —  %s", taps, desc);
    return b;
}

static void
act_fm_cma_taps_cycle(void* v) {
    UNUSED(v);
    int taps = 0;
    rtl_stream_get_fm_cma_params(&taps, NULL, NULL);
    int nt;
    if (taps < 3) {
        nt = 3; /* 1 -> 3 */
    } else if (taps < 5) {
        nt = 5; /* 3 -> 5 */
    } else if (taps < 7) {
        nt = 7; /* 5 -> 7 */
    } else if (taps < 9) {
        nt = 9; /* 7 -> 9 */
    } else {
        nt = 1; /* 9 -> 1 */
    }
    rtl_stream_set_fm_cma_params(nt, -1, -1);
}

static const char*
lbl_fm_cma_mu(void* v, char* b, size_t n) {
    UNUSED(v);
    int mu = 0;
    rtl_stream_get_fm_cma_params(NULL, &mu, NULL);
    snprintf(b, n, "CMA mu (Q15, 1..64): %d", mu);
    return b;
}

static const char*
lbl_fm_cma_strength(void* v, char* b, size_t n) {
    UNUSED(v);
    int s = rtl_stream_get_fm_cma_strength();
    const char* name = (s == 2) ? "Strong" : (s == 1) ? "Medium" : "Light";
    snprintf(b, n, "CMA Strength: %s", name);
    return b;
}

/* Show adaptive 5-tap guard hint: adapting vs hold, and A/R counts */
static const char*
lbl_fm_cma_guard(void* v, char* b, size_t n) {
    UNUSED(v);
    int enabled = rtl_stream_get_fm_cma();
    int taps = 0, mu = 0, warm = 0;
    rtl_stream_get_fm_cma_params(&taps, &mu, &warm);
    if (!enabled || (taps != 5 && taps != 7 && taps != 9)) {
        snprintf(b, n, "CMA Adaptive: (n/a)");
        return b;
    }
    int freeze = 0, acc = 0, rej = 0;
    rtl_stream_get_fm_cma_guard(&freeze, &acc, &rej);
    if (freeze > 0) {
        snprintf(b, n, "CMA Adaptive: hold %d  |  A/R %d/%d", freeze, acc, rej);
    } else {
        snprintf(b, n, "CMA Adaptive: adapting  |  A/R %d/%d", acc, rej);
    }
    return b;
}

static void
act_fm_cma_strength_cycle(void* v) {
    UNUSED(v);
    int s = rtl_stream_get_fm_cma_strength();
    s = (s + 1) % 3;
    rtl_stream_set_fm_cma_strength(s);
}

static void
act_fm_cma_mu_up(void* v) {
    UNUSED(v);
    int taps = 0, mu = 0, warm = 0;
    rtl_stream_get_fm_cma_params(&taps, &mu, &warm);
    if (mu < 64) {
        mu++;
    }
    rtl_stream_set_fm_cma_params(-1, mu, -1);
}

static void
act_fm_cma_mu_dn(void* v) {
    UNUSED(v);
    int taps = 0, mu = 0, warm = 0;
    rtl_stream_get_fm_cma_params(&taps, &mu, &warm);
    if (mu > 1) {
        mu--;
    }
    rtl_stream_set_fm_cma_params(-1, mu, -1);
}

static const char*
lbl_fm_cma_warm(void* v, char* b, size_t n) {
    UNUSED(v);
    int warm = 0;
    rtl_stream_get_fm_cma_params(NULL, NULL, &warm);
    if (warm <= 0) {
        snprintf(b, n, "CMA Warmup (samples): 0 (continuous)");
    } else {
        snprintf(b, n, "CMA Warmup (samples): %d", warm);
    }
    return b;
}

static void
act_fm_cma_warm_up(void* v) {
    UNUSED(v);
    int warm = 0;
    rtl_stream_get_fm_cma_params(NULL, NULL, &warm);
    if (warm < 0) {
        warm = 0;
    }
    warm += 5000;
    if (warm > 200000) {
        warm = 200000;
    }
    rtl_stream_set_fm_cma_params(-1, -1, warm);
}

static void
act_fm_cma_warm_dn(void* v) {
    UNUSED(v);
    int warm = 0;
    rtl_stream_get_fm_cma_params(NULL, NULL, &warm);
    if (warm <= 0) {
        warm = 0;
    } else {
        warm -= 5000;
        if (warm < 0) {
            warm = 0;
        }
    }
    rtl_stream_set_fm_cma_params(-1, -1, warm);
}

static void
act_toggle_fm_agc(void* v) {
    UNUSED(v);
    int on = rtl_stream_get_fm_agc();
    rtl_stream_set_fm_agc(on ? 0 : 1);
}

static const char*
lbl_fm_limiter(void* v, char* b, size_t n) {
    UNUSED(v);
    int on = rtl_stream_get_fm_limiter();
    snprintf(b, n, "FM Limiter [%s]", on ? "On" : "Off");
    return b;
}

static const char*
lbl_fm_agc_auto(void* v, char* b, size_t n) {
    UNUSED(v);
    int on = rtl_stream_get_fm_agc_auto();
    snprintf(b, n, "FM AGC Auto [%s]", on ? "On" : "Off");
    return b;
}

static void
act_toggle_fm_agc_auto(void* v) {
    UNUSED(v);
    int on = rtl_stream_get_fm_agc_auto();
    rtl_stream_set_fm_agc_auto(on ? 0 : 1);
}

static void
act_toggle_fm_limiter(void* v) {
    UNUSED(v);
    int on = rtl_stream_get_fm_limiter();
    rtl_stream_set_fm_limiter(on ? 0 : 1);
}

static const char*
lbl_fm_agc_target(void* v, char* b, size_t n) {
    UNUSED(v);
    int tgt = 0;
    rtl_stream_get_fm_agc_params(&tgt, NULL, NULL, NULL);
    snprintf(b, n, "AGC Target: %d (+/-)", tgt);
    return b;
}

static void
act_fm_agc_target_up(void* v) {
    UNUSED(v);
    int tgt = 0;
    rtl_stream_get_fm_agc_params(&tgt, NULL, NULL, NULL);
    tgt += 500;
    if (tgt > 20000) {
        tgt = 20000;
    }
    rtl_stream_set_fm_agc_params(tgt, -1, -1, -1);
}

static void
act_fm_agc_target_dn(void* v) {
    UNUSED(v);
    int tgt = 0;
    rtl_stream_get_fm_agc_params(&tgt, NULL, NULL, NULL);
    tgt -= 500;
    if (tgt < 1000) {
        tgt = 1000;
    }
    rtl_stream_set_fm_agc_params(tgt, -1, -1, -1);
}

static const char*
lbl_fm_agc_min(void* v, char* b, size_t n) {
    UNUSED(v);
    int mn = 0;
    rtl_stream_get_fm_agc_params(NULL, &mn, NULL, NULL);
    snprintf(b, n, "AGC Min: %d (+/-)", mn);
    return b;
}

static void
act_fm_agc_min_up(void* v) {
    UNUSED(v);
    int mn = 0;
    rtl_stream_get_fm_agc_params(NULL, &mn, NULL, NULL);
    mn += 500;
    if (mn > 15000) {
        mn = 15000;
    }
    rtl_stream_set_fm_agc_params(-1, mn, -1, -1);
}

static void
act_fm_agc_min_dn(void* v) {
    UNUSED(v);
    int mn = 0;
    rtl_stream_get_fm_agc_params(NULL, &mn, NULL, NULL);
    mn -= 500;
    if (mn < 0) {
        mn = 0;
    }
    rtl_stream_set_fm_agc_params(-1, mn, -1, -1);
}

static const char*
lbl_fm_agc_alpha_up(void* v, char* b, size_t n) {
    UNUSED(v);
    int au = 0;
    rtl_stream_get_fm_agc_params(NULL, NULL, &au, NULL);
    int pct = (int)((au * 100 + 16384) / 32768);
    snprintf(b, n, "AGC Alpha Up: %d (Q15 ~%d%%)", au, pct);
    return b;
}

static const char*
lbl_fm_agc_alpha_down(void* v, char* b, size_t n) {
    UNUSED(v);
    int ad = 0;
    rtl_stream_get_fm_agc_params(NULL, NULL, NULL, &ad);
    int pct = (int)((ad * 100 + 16384) / 32768);
    snprintf(b, n, "AGC Alpha Down: %d (Q15 ~%d%%)", ad, pct);
    return b;
}

static void
act_fm_agc_alpha_up_up(void* v) {
    UNUSED(v);
    int au = 0;
    rtl_stream_get_fm_agc_params(NULL, NULL, &au, NULL);
    au += 1024;
    if (au > 32768) {
        au = 32768;
    }
    rtl_stream_set_fm_agc_params(-1, -1, au, -1);
}

static void
act_fm_agc_alpha_up_dn(void* v) {
    UNUSED(v);
    int au = 0;
    rtl_stream_get_fm_agc_params(NULL, NULL, &au, NULL);
    au -= 1024;
    if (au < 1) {
        au = 1;
    }
    rtl_stream_set_fm_agc_params(-1, -1, au, -1);
}

static void
act_fm_agc_alpha_down_up(void* v) {
    UNUSED(v);
    int ad = 0;
    rtl_stream_get_fm_agc_params(NULL, NULL, NULL, &ad);
    ad += 1024;
    if (ad > 32768) {
        ad = 32768;
    }
    rtl_stream_set_fm_agc_params(-1, -1, -1, ad);
}

static void
act_fm_agc_alpha_down_dn(void* v) {
    UNUSED(v);
    int ad = 0;
    rtl_stream_get_fm_agc_params(NULL, NULL, NULL, &ad);
    ad -= 1024;
    if (ad < 1) {
        ad = 1;
    }
    rtl_stream_set_fm_agc_params(-1, -1, -1, ad);
}

static const char*
lbl_iq_dc(void* v, char* b, size_t n) {
    UNUSED(v);
    int k = 0;
    int on = rtl_stream_get_iq_dc(&k);
    snprintf(b, n, "IQ DC Block [%s]", on ? "On" : "Off");
    return b;
}

static void
act_toggle_iq_dc(void* v) {
    UNUSED(v);
    int k = 0;
    int on = rtl_stream_get_iq_dc(&k);
    rtl_stream_set_iq_dc(on ? 0 : 1, -1);
}

static const char*
lbl_iq_dc_k(void* v, char* b, size_t n) {
    UNUSED(v);
    int k = 0;
    rtl_stream_get_iq_dc(&k);
    snprintf(b, n, "IQ DC Shift k: %d (+/-)", k);
    return b;
}

static void
act_iq_dc_k_up(void* v) {
    UNUSED(v);
    int k = 0;
    rtl_stream_get_iq_dc(&k);
    if (k < 15) {
        k++;
    }
    rtl_stream_set_iq_dc(-1, k);
}

static void
act_iq_dc_k_dn(void* v) {
    UNUSED(v);
    int k = 0;
    rtl_stream_get_iq_dc(&k);
    if (k > 6) {
        k--;
    }
    rtl_stream_set_iq_dc(-1, k);
}

static const char*
lbl_ted_sps(void* v, char* b, size_t n) {
    UNUSED(v);
    int sps = rtl_stream_get_ted_sps();
    snprintf(b, n, "TED SPS: %d (+1/-1)", sps);
    return b;
}

static void
act_ted_sps_up(void* v) {
    UNUSED(v);
    int sps = rtl_stream_get_ted_sps();
    if (sps < 32) {
        sps++;
    }
    rtl_stream_set_ted_sps(sps);
}

static void
act_ted_sps_dn(void* v) {
    UNUSED(v);
    int sps = rtl_stream_get_ted_sps();
    if (sps > 2) {
        sps--;
    }
    rtl_stream_set_ted_sps(sps);
}

static const char*
lbl_ted_gain(void* v, char* b, size_t n) {
    UNUSED(v);
    int g = rtl_stream_get_ted_gain();
    snprintf(b, n, "TED Gain (Q20): %d (+/-)", g);
    return b;
}

static void
act_ted_gain_up(void* v) {
    UNUSED(v);
    int g = rtl_stream_get_ted_gain();
    if (g < 512) {
        g += 8;
    }
    rtl_stream_set_ted_gain(g);
}

static void
act_ted_gain_dn(void* v) {
    UNUSED(v);
    int g = rtl_stream_get_ted_gain();
    if (g > 16) {
        g -= 8;
    }
    rtl_stream_set_ted_gain(g);
}

static void
act_toggle_iqbal(void* v) {
    UNUSED(v);
    int on = rtl_stream_get_iq_balance();
    /* If Auto-DSP is active and Manual Override is off, enable Manual Override so the user's
       choice isn't immediately overwritten by auto toggling. */
    int cq = 0, f = 0, t = 0, a = 0;
    rtl_stream_dsp_get(&cq, &f, &t, &a);
    int man = rtl_stream_get_manual_dsp();
    if (a && !man) {
        rtl_stream_set_manual_dsp(1);
    }
    rtl_stream_toggle_iq_balance(on ? 0 : 1);
}

#ifdef USE_RTLSDR
// Toggle for showing/hiding compact DSP panel in the main ncurses UI
static const char*
lbl_dsp_panel(void* v, char* b, size_t n) {
    UiCtx* c = (UiCtx*)v;
    snprintf(b, n, "Show DSP Panel [%s]", (c && c->opts && c->opts->show_dsp_panel) ? "On" : "Off");
    return b;
}

static void
act_toggle_dsp_panel(void* v) {
    UiCtx* c = (UiCtx*)v;
    if (!c || !c->opts) {
        return;
    }
    c->opts->show_dsp_panel = c->opts->show_dsp_panel ? 0 : 1;
}
#endif

static const char*
lbl_ted_force(void* v, char* b, size_t n) {
    UNUSED(v);
    int f = rtl_stream_get_ted_force();
    snprintf(b, n, "TED Force [%s]", f ? "Active" : "Inactive");
    return b;
}

static void
act_ted_force_toggle(void* v) {
    UNUSED(v);
    int f = rtl_stream_get_ted_force();
    if (!f) {
        // Enabling force: also ensure TED itself is enabled so forcing has effect.
        rtl_stream_set_ted_force(1);
        int cq = 0, fl = 0, t = 0, a = 0;
        rtl_stream_dsp_get(&cq, &fl, &t, &a);
        if (!t) {
            rtl_stream_toggle_ted(1);
        }
    } else {
        // Disabling force leaves TED enable state unchanged.
        rtl_stream_set_ted_force(0);
    }
}

static const char*
lbl_ted_bias(void* v, char* b, size_t n) {
    UNUSED(v);
    int eb = rtl_stream_ted_bias(NULL);
    snprintf(b, n, "TED Bias (EMA): %d", eb);
    return b;
}

static const char*
lbl_manual_dsp(void* v, char* b, size_t n) {
    UNUSED(v);
    int man = rtl_stream_get_manual_dsp();
    snprintf(b, n, "Manual DSP Override [%s]", man ? "Active" : "Inactive");
    return b;
}

static void
act_toggle_manual_dsp(void* v) {
    UNUSED(v);
    int man = rtl_stream_get_manual_dsp();
    rtl_stream_set_manual_dsp(man ? 0 : 1);
}

static const char*
lbl_onoff_auto(void* v, char* b, size_t n) {
    UNUSED(v);
    int cq = 0, f = 0, t = 0, a = 0;
    rtl_stream_dsp_get(&cq, &f, &t, &a);
    snprintf(b, n, "Toggle Auto-DSP [%s]", a ? "Active" : "Inactive");
    return b;
}

static const char*
lbl_onoff_lms(void* v, char* b, size_t n) {
    UNUSED(v);
    int l = 0, t = 0, mu = 0, st = 0, wl = 0, dfe = 0, dft = 0, mf = 0, cma = 0;
    rtl_stream_cqpsk_get(&l, &t, &mu, &st, &wl, &dfe, &dft, &mf, &cma);
    snprintf(b, n, "Toggle LMS [%s]", l ? "Active" : "Inactive");
    return b;
}

static const char*
lbl_onoff_mf(void* v, char* b, size_t n) {
    UNUSED(v);
    int l = 0, t = 0, mu = 0, st = 0, wl = 0, dfe = 0, dft = 0, mf = 0, cma = 0;
    rtl_stream_cqpsk_get(&l, &t, &mu, &st, &wl, &dfe, &dft, &mf, &cma);
    snprintf(b, n, "Toggle Matched Filter [%s]", mf ? "Active" : "Inactive");
    return b;
}

static const char*
lbl_toggle_rrc(void* v, char* b, size_t n) {
    UNUSED(v);
    int on = 0, a = 0, s = 0;
    rtl_stream_cqpsk_get_rrc(&on, &a, &s);
    snprintf(b, n, "Toggle RRC [%s]", on ? "Active" : "Inactive");
    return b;
}

static const char*
lbl_rrc_a_up(void* v, char* b, size_t n) {
    UNUSED(v);
    int on = 0, a = 0, s = 0;
    rtl_stream_cqpsk_get_rrc(&on, &a, &s);
    snprintf(b, n, "RRC alpha +5%% (now %d%%)", a);
    return b;
}

static const char*
lbl_rrc_a_dn(void* v, char* b, size_t n) {
    UNUSED(v);
    int on = 0, a = 0, s = 0;
    rtl_stream_cqpsk_get_rrc(&on, &a, &s);
    snprintf(b, n, "RRC alpha -5%% (now %d%%)", a);
    return b;
}

static const char*
lbl_rrc_s_up(void* v, char* b, size_t n) {
    UNUSED(v);
    int on = 0, a = 0, s = 0;
    rtl_stream_cqpsk_get_rrc(&on, &a, &s);
    snprintf(b, n, "RRC span +1 (now %d)", s);
    return b;
}

static const char*
lbl_rrc_s_dn(void* v, char* b, size_t n) {
    UNUSED(v);
    int on = 0, a = 0, s = 0;
    rtl_stream_cqpsk_get_rrc(&on, &a, &s);
    snprintf(b, n, "RRC span -1 (now %d)", s);
    return b;
}

static const char*
lbl_onoff_wl(void* v, char* b, size_t n) {
    UNUSED(v);
    int l = 0, t = 0, mu = 0, st = 0, wl = 0, dfe = 0, dft = 0, mf = 0, cma = 0;
    rtl_stream_cqpsk_get(&l, &t, &mu, &st, &wl, &dfe, &dft, &mf, &cma);
    snprintf(b, n, "Toggle WL [%s]", wl ? "Active" : "Inactive");
    return b;
}

static const char*
lbl_onoff_dfe(void* v, char* b, size_t n) {
    UNUSED(v);
    int l = 0, t = 0, mu = 0, st = 0, wl = 0, dfe = 0, dft = 0, mf = 0, cma = 0;
    rtl_stream_cqpsk_get(&l, &t, &mu, &st, &wl, &dfe, &dft, &mf, &cma);
    snprintf(b, n, "Toggle DFE [%s]", dfe ? "Active" : "Inactive");
    return b;
}

static const char*
lbl_dft_cycle(void* v, char* b, size_t n) {
    UNUSED(v);
    int l = 0, t = 0, mu = 0, st = 0, wl = 0, dfe = 0, dft = 0, mf = 0, cma = 0;
    rtl_stream_cqpsk_get(&l, &t, &mu, &st, &wl, &dfe, &dft, &mf, &cma);
    snprintf(b, n, "Cycle DFE taps: %d", dft);
    return b;
}

static const char*
lbl_eq_taps(void* v, char* b, size_t n) {
    UNUSED(v);
    int l = 0, taps = 0, mu = 0, st = 0, wl = 0, dfe = 0, dft = 0, mf = 0, cma = 0;
    rtl_stream_cqpsk_get(&l, &taps, &mu, &st, &wl, &dfe, &dft, &mf, &cma);
    snprintf(b, n, "Set EQ taps 5/7 (now %d)", taps);
    return b;
}

static const char*
lbl_onoff_dqpsk(void* v, char* b, size_t n) {
    UNUSED(v);
    int on = 0;
    rtl_stream_cqpsk_get_dqpsk(&on);
    snprintf(b, n, "Toggle DQPSK decision [%s]", on ? "Active" : "Inactive");
    return b;
}

/* ---- LSM Simple (CQPSK+RRC; Costas; EQ off) ---- */
static const char*
lbl_lsm_simple(void* v, char* b, size_t n) {
    (void)v;
    int on = dsd_neo_get_lsm_simple();
    snprintf(b, n, "LSM Simple [%s]", on ? "On" : "Off");
    return b;
}

static void
act_lsm_simple_toggle(void* v) {
    UiCtx* c = (UiCtx*)v;
    /* Persist prior DQPSK decision state across toggles */
    static int prev_dqpsk = -1;
    /* Persist prior FLL/TED states across toggles */
    static int prev_fll = -1;
    static int prev_ted_enable = -1;
    static int prev_ted_force = -1;
    /* Persist prior Manual-DSP override state across toggles */
    static int prev_manual = -1;
    int now = dsd_neo_get_lsm_simple();
    int next = now ? 0 : 1;
    dsd_neo_set_lsm_simple(next);
    if (next) {
        /* Save current DQPSK decision state so we can restore on disable */
        int dq = 0;
        rtl_stream_cqpsk_get_dqpsk(&dq);
        prev_dqpsk = dq;
        /* Save current FLL/TED states */
        int cq = 0, f = 0, t = 0, a = 0;
        rtl_stream_dsp_get(&cq, &f, &t, &a);
        prev_fll = f;
        prev_ted_enable = t;
        prev_ted_force = rtl_stream_get_ted_force();
        /* Save and force Manual-DSP override so Auto-DSP cannot fight LSM Simple */
        prev_manual = rtl_stream_get_manual_dsp();
        if (!prev_manual) {
            rtl_stream_set_manual_dsp(1);
        }
        /* Force CQPSK ON + RRC(alpha≈0.2, span≈6). Skip EQ via runtime config. */
        if (!cq) {
            rtl_stream_toggle_cqpsk(1);
        }
        /* Ensure FLL is on for one-switch lock-in */
        rtl_stream_toggle_fll(1);
        rtl_stream_cqpsk_set(-1, -1, -1, -1, -1, 0, -1, 1, -1); /* DFE off, MF on */
        rtl_stream_cqpsk_set_rrc(1, 20, 6);                     /* alpha=20%, span=6 */
        /* Enable DQPSK-aware decision */
        rtl_stream_cqpsk_set_dqpsk(1);
        /* Auto-enable TED and force it for CQPSK/FM demod path */
        rtl_stream_toggle_ted(1);
        rtl_stream_set_ted_force(1);
        /* Set a reasonable default SPS for P25p1 (4800 sym/s at 48k -> ~10) */
        rtl_stream_set_ted_sps(10);
        /* Ensure symbol sampler uses QPSK windows immediately. */
        if (c && c->state) {
            c->state->rf_mod = 1; /* QPSK */
        }
        if (c && c->opts) {
            c->opts->mod_qpsk = 1; /* reflect in UI */
        }
        /* Leave LMS state as-is; runtime will skip EQ when simple is on. */
        ui_statusf("LSM Simple: On (CQPSK+RRC; DQPSK; FLL+TED; EQ off)");
    } else {
        /* Restore prior DQPSK decision state if we saved one */
        if (prev_dqpsk != -1) {
            rtl_stream_cqpsk_set_dqpsk(prev_dqpsk);
            prev_dqpsk = -1;
        }
        /* Restore FLL/TED states if captured */
        if (prev_fll != -1) {
            rtl_stream_toggle_fll(prev_fll);
            prev_fll = -1;
        }
        if (prev_ted_enable != -1) {
            rtl_stream_toggle_ted(prev_ted_enable);
            prev_ted_enable = -1;
        }
        if (prev_ted_force != -1) {
            rtl_stream_set_ted_force(prev_ted_force);
            prev_ted_force = -1;
        }
        /* Restore prior Manual-DSP override */
        if (prev_manual != -1) {
            rtl_stream_set_manual_dsp(prev_manual);
            prev_manual = -1;
        }
        ui_statusf("LSM Simple: Off");
    }
}

static void
act_toggle_cq(void* v) {
    UiCtx* c = (UiCtx*)v;
    int cq = 0, f = 0, t = 0, a = 0;
    rtl_stream_dsp_get(&cq, &f, &t, &a);
    int next = cq ? 0 : 1;
    rtl_stream_toggle_cqpsk(next);
    /* Keep symbol sampler windowing in sync with runtime DSP path. */
    if (c && c->state) {
        c->state->rf_mod = next ? 1 : 0;
    }
    if (c && c->opts) {
        if (next) {
            c->opts->mod_qpsk = 1;
        }
    }
}

static void
act_toggle_fll(void* v) {
    UNUSED(v);
    int cq = 0, f = 0, t = 0, a = 0;
    rtl_stream_dsp_get(&cq, &f, &t, &a);
    rtl_stream_toggle_fll(f ? 0 : 1);
}

static void
act_toggle_ted(void* v) {
    UNUSED(v);
    int cq = 0, f = 0, t = 0, a = 0;
    rtl_stream_dsp_get(&cq, &f, &t, &a);
    rtl_stream_toggle_ted(t ? 0 : 1);
}

static void
act_toggle_auto(void* v) {
    UNUSED(v);
    int cq = 0, f = 0, t = 0, a = 0;
    rtl_stream_dsp_get(&cq, &f, &t, &a);
    rtl_stream_toggle_auto_dsp(a ? 0 : 1);
}

static void
act_toggle_lms(void* v) {
    UNUSED(v);
    int l = 0, taps = 0, mu = 0, st = 0, wl = 0, dfe = 0, dft = 0, mf = 0, cma = 0;
    rtl_stream_cqpsk_get(&l, &taps, &mu, &st, &wl, &dfe, &dft, &mf, &cma);
    rtl_stream_cqpsk_set(l ? 0 : 1, -1, -1, -1, -1, -1, -1, -1, -1);
}

static void
act_toggle_mf(void* v) {
    UNUSED(v);
    int l = 0, taps = 0, mu = 0, st = 0, wl = 0, dfe = 0, dft = 0, mf = 0, cma = 0;
    rtl_stream_cqpsk_get(&l, &taps, &mu, &st, &wl, &dfe, &dft, &mf, &cma);
    rtl_stream_cqpsk_set(-1, -1, -1, -1, -1, -1, -1, mf ? 0 : 1, -1);
}

static void
act_toggle_rrc(void* v) {
    UNUSED(v);
    int on = 0, a = 0, s = 0;
    rtl_stream_cqpsk_get_rrc(&on, &a, &s);
    rtl_stream_cqpsk_set_rrc(on ? 0 : 1, -1, -1);
}

static void
act_rrc_a_up(void* v) {
    UNUSED(v);
    int on = 0, a = 0, s = 0;
    rtl_stream_cqpsk_get_rrc(&on, &a, &s);
    int na = a + 5;
    if (na > 50) {
        na = 50;
    }
    rtl_stream_cqpsk_set_rrc(-1, na, -1);
}

static void
act_rrc_a_dn(void* v) {
    UNUSED(v);
    int on = 0, a = 0, s = 0;
    rtl_stream_cqpsk_get_rrc(&on, &a, &s);
    int na = a - 5;
    if (na < 5) {
        na = 5;
    }
    rtl_stream_cqpsk_set_rrc(-1, na, -1);
}

static void
act_rrc_s_up(void* v) {
    UNUSED(v);
    int on = 0, a = 0, s = 0;
    rtl_stream_cqpsk_get_rrc(&on, &a, &s);
    int ns = s + 1;
    if (ns > 16) {
        ns = 16;
    }
    rtl_stream_cqpsk_set_rrc(-1, -1, ns);
}

static void
act_rrc_s_dn(void* v) {
    UNUSED(v);
    int on = 0, a = 0, s = 0;
    rtl_stream_cqpsk_get_rrc(&on, &a, &s);
    int ns = s - 1;
    if (ns < 3) {
        ns = 3;
    }
    rtl_stream_cqpsk_set_rrc(-1, -1, ns);
}

static void
act_cma(void* v) {
    UNUSED(v);
    rtl_stream_cqpsk_set(-1, -1, -1, -1, -1, -1, -1, -1, 1500);
}

static void
act_toggle_wl(void* v) {
    UNUSED(v);
    int l = 0, taps = 0, mu = 0, st = 0, wl = 0, dfe = 0, dft = 0, mf = 0, cma = 0;
    rtl_stream_cqpsk_get(&l, &taps, &mu, &st, &wl, &dfe, &dft, &mf, &cma);
    rtl_stream_cqpsk_set(-1, -1, -1, -1, wl ? 0 : 1, -1, -1, -1, -1);
}

static void
act_toggle_dfe(void* v) {
    UNUSED(v);
    int l = 0, taps = 0, mu = 0, st = 0, wl = 0, dfe = 0, dft = 0, mf = 0, cma = 0;
    rtl_stream_cqpsk_get(&l, &taps, &mu, &st, &wl, &dfe, &dft, &mf, &cma);
    rtl_stream_cqpsk_set(-1, -1, -1, -1, -1, dfe ? 0 : 1, dft, -1, -1);
}

static void
act_cycle_dft(void* v) {
    UNUSED(v);
    int l = 0, taps = 0, mu = 0, st = 0, wl = 0, dfe = 0, dft = 0, mf = 0, cma = 0;
    rtl_stream_cqpsk_get(&l, &taps, &mu, &st, &wl, &dfe, &dft, &mf, &cma);
    int nd = (dft + 1) & 3;
    rtl_stream_cqpsk_set(-1, -1, -1, -1, -1, dfe, nd, -1, -1);
}

static void
act_taps_5_7(void* v) {
    UNUSED(v);
    int l = 0, taps = 0, mu = 0, st = 0, wl = 0, dfe = 0, dft = 0, mf = 0, cma = 0;
    rtl_stream_cqpsk_get(&l, &taps, &mu, &st, &wl, &dfe, &dft, &mf, &cma);
    int nt = (taps >= 7) ? 5 : 7;
    rtl_stream_cqpsk_set(-1, nt, -1, -1, -1, -1, -1, -1, -1);
}

static void
act_toggle_dqpsk(void* v) {
    UNUSED(v);
    int on = 0;
    rtl_stream_cqpsk_get_dqpsk(&on);
    extern void rtl_stream_cqpsk_set_dqpsk(int);
    rtl_stream_cqpsk_set_dqpsk(on ? 0 : 1);
}

#ifdef USE_RTLSDR
// ---- Auto-DSP status & config (UI) ----
static const char*
mode_to_str(int m) {
    switch (m) {
        case 2: return "Heavy";
        case 1: return "Moderate";
        default: return "Clean";
    }
}

static const char*
lbl_auto_status(void* v, char* b, size_t n) {
    (void)v;
    rtl_auto_dsp_status s = {0};
    rtl_stream_auto_dsp_get_status(&s);
    snprintf(b, n, "Auto-DSP Status [P1: %s %d%%, P2: %s]", mode_to_str(s.p25p1_mode), s.p25p1_ema_pct,
             mode_to_str(s.p25p2_mode));
    return b;
}

// Config helpers
static rtl_auto_dsp_config g_auto_cfg_cache;

static void
cfg_refresh(void) {
    rtl_stream_auto_dsp_get_config(&g_auto_cfg_cache);
}

static void
cfg_apply(void) {
    rtl_stream_auto_dsp_set_config(&g_auto_cfg_cache);
}

static const char*
lbl_p1_win(void* v, char* b, size_t n) {
    UNUSED(v);
    cfg_refresh();
    snprintf(b, n, "P25P1 Window min total: %d", g_auto_cfg_cache.p25p1_window_min_total);
    return b;
}

static const char*
lbl_p1_mod_on(void* v, char* b, size_t n) {
    UNUSED(v);
    cfg_refresh();
    snprintf(b, n, "P25P1 Moderate On %%: %d", g_auto_cfg_cache.p25p1_moderate_on_pct);
    return b;
}

static const char*
lbl_p1_mod_off(void* v, char* b, size_t n) {
    UNUSED(v);
    cfg_refresh();
    snprintf(b, n, "P25P1 Moderate Off %%: %d", g_auto_cfg_cache.p25p1_moderate_off_pct);
    return b;
}

static const char*
lbl_p1_hvy_on(void* v, char* b, size_t n) {
    UNUSED(v);
    cfg_refresh();
    snprintf(b, n, "P25P1 Heavy On %%: %d", g_auto_cfg_cache.p25p1_heavy_on_pct);
    return b;
}

static const char*
lbl_p1_hvy_off(void* v, char* b, size_t n) {
    UNUSED(v);
    cfg_refresh();
    snprintf(b, n, "P25P1 Heavy Off %%: %d", g_auto_cfg_cache.p25p1_heavy_off_pct);
    return b;
}

static const char*
lbl_p1_cool(void* v, char* b, size_t n) {
    UNUSED(v);
    cfg_refresh();
    snprintf(b, n, "P25P1 Cooldown (ms): %d", g_auto_cfg_cache.p25p1_cooldown_ms);
    return b;
}

static const char*
lbl_p2_okmin(void* v, char* b, size_t n) {
    UNUSED(v);
    cfg_refresh();
    snprintf(b, n, "P25P2 OK min: %d", g_auto_cfg_cache.p25p2_ok_min);
    return b;
}

static const char*
lbl_p2_margin_on(void* v, char* b, size_t n) {
    UNUSED(v);
    cfg_refresh();
    snprintf(b, n, "P25P2 Err margin On: %d", g_auto_cfg_cache.p25p2_err_margin_on);
    return b;
}

static const char*
lbl_p2_margin_off(void* v, char* b, size_t n) {
    UNUSED(v);
    cfg_refresh();
    snprintf(b, n, "P25P2 Err margin Off: %d", g_auto_cfg_cache.p25p2_err_margin_off);
    return b;
}

static const char*
lbl_p2_cool(void* v, char* b, size_t n) {
    UNUSED(v);
    cfg_refresh();
    snprintf(b, n, "P25P2 Cooldown (ms): %d", g_auto_cfg_cache.p25p2_cooldown_ms);
    return b;
}

static const char*
lbl_ema_alpha(void* v, char* b, size_t n) {
    UNUSED(v);
    cfg_refresh();
    int pct = (int)((g_auto_cfg_cache.ema_alpha_q15 * 100 + 16384) / 32768); // approx
    snprintf(b, n, "EMA alpha (Q15 ~%d%%): %d", pct, g_auto_cfg_cache.ema_alpha_q15);
    return b;
}

// Adjusters
static void
inc_p1_win(void* v) {
    UNUSED(v);
    cfg_refresh();
    g_auto_cfg_cache.p25p1_window_min_total += 50;
    cfg_apply();
}

static void
dec_p1_win(void* v) {
    UNUSED(v);
    cfg_refresh();
    if (g_auto_cfg_cache.p25p1_window_min_total > 50) {
        g_auto_cfg_cache.p25p1_window_min_total -= 50;
    }
    cfg_apply();
}

static void
inc_i(int* p, int d, int max) {
    int v = *p + d;
    if (v > max) {
        v = max;
    }
    *p = v;
}

static void
dec_i(int* p, int d, int min) {
    int v = *p - d;
    if (v < min) {
        v = min;
    }
    *p = v;
}

static void
inc_p1_mod_on(void* v) {
    UNUSED(v);
    cfg_refresh();
    inc_i(&g_auto_cfg_cache.p25p1_moderate_on_pct, 1, 50);
    cfg_apply();
}

static void
dec_p1_mod_on(void* v) {
    UNUSED(v);
    cfg_refresh();
    dec_i(&g_auto_cfg_cache.p25p1_moderate_on_pct, 1, 1);
    cfg_apply();
}

static void
inc_p1_mod_off(void* v) {
    UNUSED(v);
    cfg_refresh();
    inc_i(&g_auto_cfg_cache.p25p1_moderate_off_pct, 1, 50);
    cfg_apply();
}

static void
dec_p1_mod_off(void* v) {
    UNUSED(v);
    cfg_refresh();
    dec_i(&g_auto_cfg_cache.p25p1_moderate_off_pct, 1, 0);
    cfg_apply();
}

static void
inc_p1_hvy_on(void* v) {
    UNUSED(v);
    cfg_refresh();
    inc_i(&g_auto_cfg_cache.p25p1_heavy_on_pct, 1, 90);
    cfg_apply();
}

static void
dec_p1_hvy_on(void* v) {
    UNUSED(v);
    cfg_refresh();
    dec_i(&g_auto_cfg_cache.p25p1_heavy_on_pct, 1, 1);
    cfg_apply();
}

static void
inc_p1_hvy_off(void* v) {
    UNUSED(v);
    cfg_refresh();
    inc_i(&g_auto_cfg_cache.p25p1_heavy_off_pct, 1, 90);
    cfg_apply();
}

static void
dec_p1_hvy_off(void* v) {
    UNUSED(v);
    cfg_refresh();
    dec_i(&g_auto_cfg_cache.p25p1_heavy_off_pct, 1, 0);
    cfg_apply();
}

static void
inc_p1_cool(void* v) {
    UNUSED(v);
    cfg_refresh();
    g_auto_cfg_cache.p25p1_cooldown_ms += 100;
    cfg_apply();
}

static void
dec_p1_cool(void* v) {
    UNUSED(v);
    cfg_refresh();
    if (g_auto_cfg_cache.p25p1_cooldown_ms > 100) {
        g_auto_cfg_cache.p25p1_cooldown_ms -= 100;
    }
    cfg_apply();
}

static void
inc_p2_okmin(void* v) {
    UNUSED(v);
    cfg_refresh();
    inc_i(&g_auto_cfg_cache.p25p2_ok_min, 1, 50);
    cfg_apply();
}

static void
dec_p2_okmin(void* v) {
    UNUSED(v);
    cfg_refresh();
    dec_i(&g_auto_cfg_cache.p25p2_ok_min, 1, 1);
    cfg_apply();
}

static void
inc_p2_m_on(void* v) {
    UNUSED(v);
    cfg_refresh();
    inc_i(&g_auto_cfg_cache.p25p2_err_margin_on, 1, 50);
    cfg_apply();
}

static void
dec_p2_m_on(void* v) {
    UNUSED(v);
    cfg_refresh();
    dec_i(&g_auto_cfg_cache.p25p2_err_margin_on, 1, 0);
    cfg_apply();
}

static void
inc_p2_m_off(void* v) {
    UNUSED(v);
    cfg_refresh();
    inc_i(&g_auto_cfg_cache.p25p2_err_margin_off, 1, 50);
    cfg_apply();
}

static void
dec_p2_m_off(void* v) {
    UNUSED(v);
    cfg_refresh();
    dec_i(&g_auto_cfg_cache.p25p2_err_margin_off, 1, 0);
    cfg_apply();
}

static void
inc_p2_cool(void* v) {
    UNUSED(v);
    cfg_refresh();
    g_auto_cfg_cache.p25p2_cooldown_ms += 100;
    cfg_apply();
}

static void
dec_p2_cool(void* v) {
    UNUSED(v);
    cfg_refresh();
    if (g_auto_cfg_cache.p25p2_cooldown_ms > 100) {
        g_auto_cfg_cache.p25p2_cooldown_ms -= 100;
    }
    cfg_apply();
}

static void
inc_alpha(void* v) {
    UNUSED(v);
    cfg_refresh();
    inc_i(&g_auto_cfg_cache.ema_alpha_q15, 512, 32768);
    cfg_apply();
}

static void
dec_alpha(void* v) {
    UNUSED(v);
    cfg_refresh();
    dec_i(&g_auto_cfg_cache.ema_alpha_q15, 512, 1);
    cfg_apply();
}

static void
ui_menu_auto_dsp_config(dsd_opts* opts, dsd_state* state) {
    UiCtx ctx = {opts, state};
    static const NcMenuItem items[] = {
        {.id = "p1_win",
         .label = "P25P1 Window (status)",
         .label_fn = lbl_p1_win,
         .help = "Min symbols per decision window."},
        {.id = "p1_win+", .label = "P25P1 Window +50", .help = "Increase window.", .on_select = inc_p1_win},
        {.id = "p1_win-", .label = "P25P1 Window -50", .help = "Decrease window.", .on_select = dec_p1_win},
        {.id = "p1_mon",
         .label = "P25P1 Moderate On%",
         .label_fn = lbl_p1_mod_on,
         .help = "Engage moderate threshold."},
        {.id = "p1_mon+", .label = "Moderate On% +1", .on_select = inc_p1_mod_on},
        {.id = "p1_mon-", .label = "Moderate On% -1", .on_select = dec_p1_mod_on},
        {.id = "p1_moff", .label = "P25P1 Moderate Off%", .label_fn = lbl_p1_mod_off, .help = "Relax to clean."},
        {.id = "p1_moff+", .label = "Moderate Off% +1", .on_select = inc_p1_mod_off},
        {.id = "p1_moff-", .label = "Moderate Off% -1", .on_select = dec_p1_mod_off},
        {.id = "p1_hon", .label = "P25P1 Heavy On%", .label_fn = lbl_p1_hvy_on, .help = "Engage heavy threshold."},
        {.id = "p1_hon+", .label = "Heavy On% +1", .on_select = inc_p1_hvy_on},
        {.id = "p1_hon-", .label = "Heavy On% -1", .on_select = dec_p1_hvy_on},
        {.id = "p1_hoff", .label = "P25P1 Heavy Off%", .label_fn = lbl_p1_hvy_off, .help = "Relax from heavy."},
        {.id = "p1_hoff+", .label = "Heavy Off% +1", .on_select = inc_p1_hvy_off},
        {.id = "p1_hoff-", .label = "Heavy Off% -1", .on_select = dec_p1_hvy_off},
        {.id = "p1_cool",
         .label = "P25P1 Cooldown (status)",
         .label_fn = lbl_p1_cool,
         .help = "Cooldown ms between changes."},
        {.id = "p1_cool+", .label = "Cooldown +100ms", .on_select = inc_p1_cool},
        {.id = "p1_cool-", .label = "Cooldown -100ms", .on_select = dec_p1_cool},
        {.id = "p2_ok", .label = "P25P2 OK min (status)", .label_fn = lbl_p2_okmin, .help = "Min OKs to avoid heavy."},
        {.id = "p2_ok+", .label = "OK min +1", .on_select = inc_p2_okmin},
        {.id = "p2_ok-", .label = "OK min -1", .on_select = dec_p2_okmin},
        {.id = "p2_mon",
         .label = "P25P2 Err margin On",
         .label_fn = lbl_p2_margin_on,
         .help = "Err > OK + margin -> heavy."},
        {.id = "p2_mon+", .label = "Margin On +1", .on_select = inc_p2_m_on},
        {.id = "p2_mon-", .label = "Margin On -1", .on_select = dec_p2_m_on},
        {.id = "p2_moff", .label = "P25P2 Err margin Off", .label_fn = lbl_p2_margin_off, .help = "Relax heavy."},
        {.id = "p2_moff+", .label = "Margin Off +1", .on_select = inc_p2_m_off},
        {.id = "p2_moff-", .label = "Margin Off -1", .on_select = dec_p2_m_off},
        {.id = "p2_cool",
         .label = "P25P2 Cooldown (status)",
         .label_fn = lbl_p2_cool,
         .help = "Cooldown ms between changes."},
        {.id = "p2_cool+", .label = "Cooldown +100ms", .on_select = inc_p2_cool},
        {.id = "p2_cool-", .label = "Cooldown -100ms", .on_select = dec_p2_cool},
        {.id = "ema",
         .label = "EMA alpha (status)",
         .label_fn = lbl_ema_alpha,
         .help = "Smoothing constant for P25P1."},
        {.id = "ema+", .label = "EMA alpha +512", .on_select = inc_alpha},
        {.id = "ema-", .label = "EMA alpha -512", .on_select = dec_alpha},
    };
    ui_menu_run(items, sizeof items / sizeof items[0], &ctx);
}
#endif

#ifdef USE_RTLSDR
static void
act_auto_cfg(void* v) {
    UiCtx* c = (UiCtx*)v;
    ui_menu_auto_dsp_config(c->opts, c->state);
}
#endif

void
ui_menu_dsp_options(dsd_opts* opts, dsd_state* state) {
    UiCtx ctx = {opts, state};
    static const NcMenuItem items[] = {
        {.id = "hint",
         .label = "Hint: Labels show live; Manual Override pins.",
         .help = "Status rows reflect live runtime; Manual Override keeps your settings."},
#ifdef USE_RTLSDR
        {.id = "dsp_panel",
         .label = "Show DSP Panel",
         .label_fn = lbl_dsp_panel,
         .help = "Toggle compact DSP status panel in main UI.",
         .on_select = act_toggle_dsp_panel},
#endif
        {.id = "manual",
         .label = "Manual DSP Override",
         .label_fn = lbl_manual_dsp,
         .help = "When active, prevents auto on/off based on modulation.",
         .on_select = act_toggle_manual_dsp},
        {.id = "cqpsk",
         .label = "Toggle CQPSK",
         .label_fn = lbl_onoff_cq,
         .help = "Enable/disable CQPSK path (runtime may auto-toggle unless Manual is active).",
         .on_select = act_toggle_cq},
        {.id = "fll",
         .label = "Toggle FLL",
         .label_fn = lbl_onoff_fll,
         .help = "Enable/disable FLL.",
         .on_select = act_toggle_fll},
        {.id = "ted",
         .label = "Toggle TED",
         .label_fn = lbl_onoff_ted,
         .help = "Enable/disable TED.",
         .on_select = act_toggle_ted},
        {.id = "iqbal",
         .label = "Toggle IQ Balance",
         .label_fn = lbl_onoff_iqbal,
         .help = "Enable/disable mode-aware image cancellation.",
         .on_select = act_toggle_iqbal},
        {.id = "ted_sps_status",
         .label = "TED SPS (status)",
         .label_fn = lbl_ted_sps,
         .help = "Current nominal samples-per-symbol."},
        {.id = "ted_sps+",
         .label = "TED SPS +1",
         .help = "Increase nominal samples-per-symbol.",
         .on_select = act_ted_sps_up},
        {.id = "ted_sps-",
         .label = "TED SPS -1",
         .help = "Decrease nominal samples-per-symbol.",
         .on_select = act_ted_sps_dn},
        {.id = "ted_gain_status",
         .label = "TED Gain (status)",
         .label_fn = lbl_ted_gain,
         .help = "Current TED small gain (Q20)."},
        {.id = "ted_gain+", .label = "TED Gain +", .help = "Increase TED small gain.", .on_select = act_ted_gain_up},
        {.id = "ted_gain-", .label = "TED Gain -", .help = "Decrease TED small gain.", .on_select = act_ted_gain_dn},
        {.id = "ted_force",
         .label = "Toggle TED Force",
         .label_fn = lbl_ted_force,
         .help = "Force TED even for FM/C4FM paths.",
         .on_select = act_ted_force_toggle},
        {.id = "ted_bias",
         .label = "TED Bias (status)",
         .label_fn = lbl_ted_bias,
         .help = "Smoothed Gardner residual (read-only status)."},
        {.id = "c4fm_clk",
         .label = "C4FM Clock Assist",
         .label_fn = lbl_c4fm_clk,
         .help = "Cycle C4FM timing assist: Off → EL → MM.",
         .on_select = act_c4fm_clk_cycle},
        {.id = "c4fm_clk_sync",
         .label = "C4FM Clock While Synced",
         .label_fn = lbl_c4fm_clk_sync,
         .help = "Allow clock assist to remain active while synchronized.",
         .on_select = act_c4fm_clk_sync_toggle},
        {.id = "fm_agc",
         .label = "FM AGC",
         .label_fn = lbl_fm_agc,
         .help = "Toggle pre-discriminator FM AGC.",
         .on_select = act_toggle_fm_agc},
        {.id = "fm_lim",
         .label = "FM Limiter",
         .label_fn = lbl_fm_limiter,
         .help = "Toggle constant-envelope limiter.",
         .on_select = act_toggle_fm_limiter},
        {.id = "fm_agc_auto",
         .label = "FM AGC Auto",
         .label_fn = lbl_fm_agc_auto,
         .help = "Auto-tune AGC target/alphas.",
         .on_select = act_toggle_fm_agc_auto},
        {.id = "fm_tgt",
         .label = "AGC Target (status)",
         .label_fn = lbl_fm_agc_target,
         .help = "Target RMS amplitude (int16 units)."},
        {.id = "fm_tgt+", .label = "AGC Target +500", .on_select = act_fm_agc_target_up},
        {.id = "fm_tgt-", .label = "AGC Target -500", .on_select = act_fm_agc_target_dn},
        {.id = "fm_min", .label = "AGC Min (status)", .label_fn = lbl_fm_agc_min, .help = "Min RMS to engage AGC."},
        {.id = "fm_min+", .label = "AGC Min +500", .on_select = act_fm_agc_min_up},
        {.id = "fm_min-", .label = "AGC Min -500", .on_select = act_fm_agc_min_dn},
        {.id = "fm_au",
         .label = "AGC Alpha Up (status)",
         .label_fn = lbl_fm_agc_alpha_up,
         .help = "Smoothing when gain increases (Q15)."},
        {.id = "fm_au+", .label = "Alpha Up +1024", .on_select = act_fm_agc_alpha_up_up},
        {.id = "fm_au-", .label = "Alpha Up -1024", .on_select = act_fm_agc_alpha_up_dn},
        {.id = "fm_ad",
         .label = "AGC Alpha Down (status)",
         .label_fn = lbl_fm_agc_alpha_down,
         .help = "Smoothing when gain decreases (Q15)."},
        {.id = "fm_ad+", .label = "Alpha Down +1024", .on_select = act_fm_agc_alpha_down_up},
        {.id = "fm_ad-", .label = "Alpha Down -1024", .on_select = act_fm_agc_alpha_down_dn},
        {.id = "iq_dc",
         .label = "IQ DC Block",
         .label_fn = lbl_iq_dc,
         .help = "Toggle complex DC blocker.",
         .on_select = act_toggle_iq_dc},
        {.id = "iq_dck",
         .label = "IQ DC Shift k (status)",
         .label_fn = lbl_iq_dc_k,
         .help = "k in dc += (x-dc)>>k (10..14 typical)."},
        {.id = "iq_dck+", .label = "Shift k +1", .on_select = act_iq_dc_k_up},
        {.id = "iq_dck-", .label = "Shift k -1", .on_select = act_iq_dc_k_dn},
        {.id = "blanker",
         .label = "Impulse Blanker",
         .label_fn = lbl_blanker,
         .help = "Toggle pre-decimation impulse blanker.",
         .on_select = act_toggle_blanker},
        {.id = "blanker_thr",
         .label = "Blanker Thr (status)",
         .label_fn = lbl_blanker_thr,
         .help = "Magnitude threshold above mean (|I|+|Q|)."},
        {.id = "blanker_thr+", .label = "Thr +2000", .on_select = act_blanker_thr_up},
        {.id = "blanker_thr-", .label = "Thr -2000", .on_select = act_blanker_thr_dn},
        {.id = "blanker_win",
         .label = "Blanker Win (status)",
         .label_fn = lbl_blanker_win,
         .help = "Half-window in complex pairs to blank around spikes."},
        {.id = "blanker_win+", .label = "Win +1", .on_select = act_blanker_win_up},
        {.id = "blanker_win-", .label = "Win -1", .on_select = act_blanker_win_dn},
        {.id = "fm_cma",
         .label = "FM CMA Equalizer",
         .label_fn = lbl_fm_cma,
         .help = "Toggle blind CMA equalizer for FM/FSK.",
         .on_select = act_toggle_fm_cma},
        {.id = "fm_cma_t",
         .label = "CMA Taps (status)",
         .label_fn = lbl_fm_cma_taps,
         .help = "1: gain, 3: fixed smoother, 5/7/9: adaptive symmetric FIR."},
        {.id = "fm_cma_t*", .label = "Cycle CMA Taps 1/3/5/7/9", .on_select = act_fm_cma_taps_cycle},
        {.id = "fm_cma_guard",
         .label = "CMA Adaptive (status)",
         .label_fn = lbl_fm_cma_guard,
         .help = "Shows adaptive guard: adapting vs hold; accepted/rejected updates."},
        {.id = "fm_cma_mu", .label = "CMA mu (status)", .label_fn = lbl_fm_cma_mu, .help = "Step size (Q15)."},
        {.id = "fm_cma_mu+", .label = "CMA mu +1", .on_select = act_fm_cma_mu_up},
        {.id = "fm_cma_mu-", .label = "CMA mu -1", .on_select = act_fm_cma_mu_dn},
        {.id = "fm_cma_str",
         .label = "CMA Strength (status)",
         .label_fn = lbl_fm_cma_strength,
         .help = "Light ([1,4,1]/6), Medium ([1,5,1]/7), Strong ([1,6,1]/8)."},
        {.id = "fm_cma_str*", .label = "Cycle Strength Light/Medium/Strong", .on_select = act_fm_cma_strength_cycle},
        {.id = "fm_cma_w",
         .label = "CMA warmup (status)",
         .label_fn = lbl_fm_cma_warm,
         .help = "0=continuous; otherwise samples."},
        {.id = "fm_cma_w+", .label = "Warmup +5000", .on_select = act_fm_cma_warm_up},
        {.id = "fm_cma_w-", .label = "Warmup -5000", .on_select = act_fm_cma_warm_dn},
        {.id = "c4fm_robust",
         .label = "C4FM Robustness Preset",
         .label_fn = lbl_c4fm_robust,
         .help = "Applies: DD EQ (5/2), CMA (7/2, Medium), TED On/Force, SPS=10; disables FM AGC/Limiter.",
         .on_select = act_c4fm_robust},
        {.id = "c4fm_dd",
         .label = "C4FM DD Equalizer",
         .label_fn = lbl_c4fm_dd,
         .help = "Toggle symbol-domain DD equalizer for C4FM.",
         .on_select = act_toggle_c4fm_dd},
        {.id = "c4fm_dd_p",
         .label = "DD EQ (status)",
         .label_fn = lbl_c4fm_dd_params,
         .help = "Taps/Mu for C4FM DD equalizer."},
        {.id = "c4fm_dd_t*", .label = "Cycle DD Taps 3/5/7/9", .on_select = act_c4fm_dd_taps_cycle},
        {.id = "c4fm_dd_mu+", .label = "DD Mu +1", .on_select = act_c4fm_dd_mu_up},
        {.id = "c4fm_dd_mu-", .label = "DD Mu -1", .on_select = act_c4fm_dd_mu_dn},
        {.id = "auto_status",
         .label = "Auto-DSP Status",
         .label_fn = lbl_auto_status,
         .help = "Live mode and smoothed error rate."},
        {.id = "auto",
         .label = "Toggle Auto-DSP",
         .label_fn = lbl_onoff_auto,
         .help = "Enable/disable auto-DSP.",
         .on_select = act_toggle_auto},
        {.id = "auto_cfg",
         .label = "Auto-DSP Config",
         .help = "Adjust Auto-DSP thresholds and windows.",
         .on_select = act_auto_cfg},
        {.id = "lms",
         .label = "Toggle LMS",
         .label_fn = lbl_onoff_lms,
         .help = "Enable/disable LMS equalizer.",
         .is_enabled = dsp_cq_on,
         .on_select = act_toggle_lms},
        {.id = "mf",
         .label = "Toggle Matched Filter",
         .label_fn = lbl_onoff_mf,
         .help = "Enable/disable matched filter.",
         .is_enabled = dsp_cq_on,
         .on_select = act_toggle_mf},
        {.id = "lsm_simple",
         .label = "LSM Simple",
         .label_fn = lbl_lsm_simple,
         .help = "Simplified LSM (CQPSK+RRC; Costas; FLL+TED; EQ off).",
         .on_select = act_lsm_simple_toggle},
        {.id = "rrc",
         .label = "Toggle RRC",
         .label_fn = lbl_toggle_rrc,
         .help = "Enable/disable RRC matched filter.",
         .is_enabled = dsp_cq_on,
         .on_select = act_toggle_rrc},
        {.id = "rrc_a+",
         .label = "RRC alpha +5%",
         .label_fn = lbl_rrc_a_up,
         .help = "Increase RRC alpha.",
         .is_enabled = dsp_cq_on,
         .on_select = act_rrc_a_up},
        {.id = "rrc_a-",
         .label = "RRC alpha -5%",
         .label_fn = lbl_rrc_a_dn,
         .help = "Decrease RRC alpha.",
         .is_enabled = dsp_cq_on,
         .on_select = act_rrc_a_dn},
        {.id = "rrc_s+",
         .label = "RRC span +1",
         .label_fn = lbl_rrc_s_up,
         .help = "Increase RRC span.",
         .is_enabled = dsp_cq_on,
         .on_select = act_rrc_s_up},
        {.id = "rrc_s-",
         .label = "RRC span -1",
         .label_fn = lbl_rrc_s_dn,
         .help = "Decrease RRC span.",
         .is_enabled = dsp_cq_on,
         .on_select = act_rrc_s_dn},
        {.id = "cma",
         .label = "CMA Warmup Burst",
         .help = "Run CMA warmup (~1500 samples).",
         .is_enabled = dsp_cq_on,
         .on_select = act_cma},
        {.id = "wl",
         .label = "Toggle WL",
         .label_fn = lbl_onoff_wl,
         .help = "Enable/disable WL prefilter.",
         .is_enabled = dsp_lms_on,
         .on_select = act_toggle_wl},
        {.id = "dfe",
         .label = "Toggle DFE",
         .label_fn = lbl_onoff_dfe,
         .help = "Enable/disable DFE.",
         .is_enabled = dsp_lms_on,
         .on_select = act_toggle_dfe},
        {.id = "dft",
         .label = "Cycle DFE taps",
         .label_fn = lbl_dft_cycle,
         .help = "Cycle DFE taps.",
         .is_enabled = dsp_dfe_on,
         .on_select = act_cycle_dft},
        {.id = "taps",
         .label = "Set EQ taps 5/7",
         .label_fn = lbl_eq_taps,
         .help = "Toggle 5 vs 7 EQ taps.",
         .is_enabled = dsp_lms_on,
         .on_select = act_taps_5_7},
        {.id = "dqpsk",
         .label = "Toggle DQPSK decision",
         .label_fn = lbl_onoff_dqpsk,
         .help = "Toggle DQPSK decision mode.",
         .is_enabled = dsp_cq_on,
         .on_select = act_toggle_dqpsk},
    };
    ui_menu_run(items, sizeof items / sizeof items[0], &ctx);
}
#else
void
ui_menu_dsp_options(dsd_opts* opts, dsd_state* state) {
    UNUSED(opts);
    UNUSED(state);
}
#endif

// ---- Key Entry ----

static int
parse_hex_u64(const char* s, unsigned long long* out) {
    if (!s || !*s || !out) {
        return 0;
    }
    char* end = NULL;
    unsigned long long v = strtoull(s, &end, 16);
    if (!end || *end != '\0') {
        return 0;
    }
    *out = v;
    return 1;
}

static int
prompt_hex_u64(const char* title, unsigned long long* out) {
    char buf[128] = {0};
    if (!ui_prompt_string(title, buf, sizeof buf)) {
        return 0;
    }
    return parse_hex_u64(buf, out);
}

// Key Entry actions (declarative)
static void
key_basic(void* v) {
    UiCtx* c = (UiCtx*)v;
    unsigned long long vdec = 0ULL;
    c->state->payload_keyid = c->state->payload_keyidR = 0;
    c->opts->dmr_mute_encL = c->opts->dmr_mute_encR = 0;
    if (ui_prompt_int("Basic Privacy Key Number (DEC)", (int*)&vdec)) {
        if (vdec > 255ULL) {
            vdec = 255ULL;
        }
        c->state->K = vdec;
        c->state->keyloader = 0;
    }
}

static void
key_hytera(void* v) {
    UiCtx* c = (UiCtx*)v;
    unsigned long long t = 0ULL;
    c->state->payload_keyid = c->state->payload_keyidR = 0;
    c->opts->dmr_mute_encL = c->opts->dmr_mute_encR = 0;
    c->state->K1 = c->state->K2 = c->state->K3 = c->state->K4 = 0ULL;
    c->state->H = 0ULL;
    if (prompt_hex_u64("Hytera Privacy Key 1 (HEX)", &t)) {
        c->state->H = t;
        c->state->K1 = c->state->H;
    }
    if (prompt_hex_u64("Hytera Privacy Key 2 (HEX) or 0", &t)) {
        c->state->K2 = t;
    }
    if (prompt_hex_u64("Hytera Privacy Key 3 (HEX) or 0", &t)) {
        c->state->K3 = t;
    }
    if (prompt_hex_u64("Hytera Privacy Key 4 (HEX) or 0", &t)) {
        c->state->K4 = t;
    }
    c->state->keyloader = 0;
}

static void
key_scrambler(void* v) {
    UiCtx* c = (UiCtx*)v;
    unsigned long long vdec = 0ULL;
    c->state->payload_keyid = c->state->payload_keyidR = 0;
    c->opts->dmr_mute_encL = c->opts->dmr_mute_encR = 0;
    if (ui_prompt_int("NXDN/dPMR Scrambler Key (DEC)", (int*)&vdec)) {
        if (vdec > 0x7FFFULL) {
            vdec = 0x7FFFULL;
        }
        c->state->R = vdec;
        c->state->keyloader = 0;
    }
}

static void
key_force_bp(void* v) {
    UiCtx* c = (UiCtx*)v;
    c->state->M = (c->state->M == 1 || c->state->M == 0x21) ? 0 : 1;
}

static void
key_rc4des(void* v) {
    UiCtx* c = (UiCtx*)v;
    unsigned long long th = 0ULL;
    c->state->payload_keyid = c->state->payload_keyidR = 0;
    c->opts->dmr_mute_encL = c->opts->dmr_mute_encR = 0;
    if (prompt_hex_u64("RC4/DES Key (HEX)", &th)) {
        c->state->R = th;
        c->state->RR = th;
        c->state->keyloader = 0;
    }
}

static void
key_aes(void* v) {
    UiCtx* c = (UiCtx*)v;
    c->state->K1 = c->state->K2 = c->state->K3 = c->state->K4 = 0ULL;
    c->state->H = 0ULL;
    memset(c->state->A1, 0, sizeof(c->state->A1));
    memset(c->state->A2, 0, sizeof(c->state->A2));
    memset(c->state->A3, 0, sizeof(c->state->A3));
    memset(c->state->A4, 0, sizeof(c->state->A4));
    prompt_hex_u64("AES Segment 1 (HEX) or 0", &c->state->K1);
    prompt_hex_u64("AES Segment 2 (HEX) or 0", &c->state->K2);
    prompt_hex_u64("AES Segment 3 (HEX) or 0", &c->state->K3);
    prompt_hex_u64("AES Segment 4 (HEX) or 0", &c->state->K4);
    c->state->keyloader = 0;
}

void
ui_menu_key_entry(dsd_opts* opts, dsd_state* state) {
    UiCtx ctx = {opts, state};
    static const NcMenuItem items[] = {
        {.id = "basic",
         .label = "Basic Privacy (DEC)",
         .help = "Set 0..255 basic privacy key.",
         .on_select = key_basic},
        {.id = "hytera",
         .label = "Hytera Privacy (HEX)",
         .help = "Set up to 4 x 16-hex segments.",
         .on_select = key_hytera},
        {.id = "scrambler",
         .label = "NXDN/dPMR Scrambler (DEC)",
         .help = "Set 0..32767 scrambler key.",
         .on_select = key_scrambler},
        {.id = "force_bp",
         .label = "Force BP/Scr Priority",
         .help = "Toggle basic/scrambler priority.",
         .on_select = key_force_bp},
        {.id = "rc4des", .label = "RC4/DES Key (HEX)", .help = "Set RC4/DES key.", .on_select = key_rc4des},
        {.id = "aes", .label = "AES-128/256 Keys (HEX)", .help = "Set AES key segments.", .on_select = key_aes},
    };
    ui_menu_run(items, sizeof items / sizeof items[0], &ctx);
}

// ---- LRRP Options (declarative) ----
static void
lr_home(void* v) {
    UiCtx* c = (UiCtx*)v;
    if (svc_lrrp_set_home(c->opts) == 0) {
        ui_statusf("LRRP output: %s", c->opts->lrrp_out_file);
    } else {
        ui_statusf("Failed to set LRRP home output");
    }
}

static void
lr_dsdp(void* v) {
    UiCtx* c = (UiCtx*)v;
    if (svc_lrrp_set_dsdp(c->opts) == 0) {
        ui_statusf("LRRP output: %s", c->opts->lrrp_out_file);
    } else {
        ui_statusf("Failed to set LRRP DSDPlus output");
    }
}

static void
lr_custom(void* v) {
    UiCtx* c = (UiCtx*)v;
    char path[1024] = {0};
    if (ui_prompt_string("Enter LRRP output filename", path, sizeof path)) {
        if (svc_lrrp_set_custom(c->opts, path) == 0) {
            ui_statusf("LRRP output: %s", c->opts->lrrp_out_file);
        } else {
            ui_statusf("Failed to set LRRP custom output");
        }
    }
}

static void
lr_off(void* v) {
    UiCtx* c = (UiCtx*)v;
    svc_lrrp_disable(c->opts);
    ui_statusf("LRRP output disabled");
}

static const char*
lbl_lrrp_current(void* vctx, char* b, size_t n) {
    UiCtx* c = (UiCtx*)vctx;
    if (c->opts->lrrp_file_output && c->opts->lrrp_out_file[0] != '\0') {
        snprintf(b, n, "Current Output [Active: %s]", c->opts->lrrp_out_file);
    } else {
        snprintf(b, n, "Current Output [Inactive]");
    }
    return b;
}

void
ui_menu_lrrp_options(dsd_opts* opts, dsd_state* state) {
    UiCtx ctx = {opts, state};
    static const NcMenuItem items[] = {
        {.id = "current",
         .label = "Current Output",
         .label_fn = lbl_lrrp_current,
         .help = "Shows the active LRRP output target.",
         .is_enabled = io_always_on},
        {.id = "home",
         .label = "Write to ~/lrrp.txt (QGIS)",
         .help = "Standard QGIS-friendly output.",
         .on_select = lr_home},
        {.id = "dsdp",
         .label = "Write to ./DSDPlus.LRRP (LRRP.exe)",
         .help = "DSDPlus LRRP format.",
         .on_select = lr_dsdp},
        {.id = "custom", .label = "Custom Filename...", .help = "Choose a custom path.", .on_select = lr_custom},
        {.id = "disable", .label = "Disable/Stop", .help = "Disable LRRP output.", .on_select = lr_off},
    };
    ui_menu_run(items, sizeof items / sizeof items[0], &ctx);
}

// ---- Main Menu ----

// action wrappers
static void
act_mode_auto(void* v) {
    UiCtx* c = (UiCtx*)v;
    svc_mode_auto(c->opts, c->state);
}

static void
act_mode_tdma(void* v) {
    UiCtx* c = (UiCtx*)v;
    svc_mode_tdma(c->opts, c->state);
}

static void
act_mode_dstar(void* v) {
    UiCtx* c = (UiCtx*)v;
    svc_mode_dstar(c->opts, c->state);
}

static void
act_mode_m17(void* v) {
    UiCtx* c = (UiCtx*)v;
    svc_mode_m17(c->opts, c->state);
}

static void
act_mode_edacs(void* v) {
    UiCtx* c = (UiCtx*)v;
    svc_mode_edacs(c->opts, c->state);
}

static void
act_mode_p25p2(void* v) {
    UiCtx* c = (UiCtx*)v;
    svc_mode_p25p2(c->opts, c->state);
}

static void
act_mode_dpmr(void* v) {
    UiCtx* c = (UiCtx*)v;
    svc_mode_dpmr(c->opts, c->state);
}

static void
act_mode_n48(void* v) {
    UiCtx* c = (UiCtx*)v;
    svc_mode_nxdn48(c->opts, c->state);
}

static void
act_mode_n96(void* v) {
    UiCtx* c = (UiCtx*)v;
    svc_mode_nxdn96(c->opts, c->state);
}

static void
act_mode_dmr(void* v) {
    UiCtx* c = (UiCtx*)v;
    svc_mode_dmr(c->opts, c->state);
}

static void
act_mode_ysf(void* v) {
    UiCtx* c = (UiCtx*)v;
    svc_mode_ysf(c->opts, c->state);
}

static void
act_toggle_invert(void* v) {
    UiCtx* c = (UiCtx*)v;
    svc_toggle_inversion(c->opts);
}

static void
act_reset_eh(void* v) {
    UiCtx* c = (UiCtx*)v;
    svc_reset_event_history(c->state);
}

static void
act_toggle_payload(void* v) {
    UiCtx* c = (UiCtx*)v;
    svc_toggle_payload(c->opts);
}

// Generic small actions used by menus (C, no lambdas!)
static void
act_event_log_set(void* v) {
    UiCtx* c = (UiCtx*)v;
    char path[1024] = {0};
    if (ui_prompt_string_prefill("Event log filename", c->opts->event_out_file, path, sizeof path)) {
        if (svc_set_event_log(c->opts, path) == 0) {
            ui_statusf("Event log: %s", path);
        }
    }
}

static void
act_event_log_disable(void* v) {
    svc_disable_event_log(((UiCtx*)v)->opts);
}

static void
act_static_wav(void* v) {
    UiCtx* c = (UiCtx*)v;
    char path[1024] = {0};
    if (ui_prompt_string_prefill("Static WAV filename", c->opts->wav_out_file, path, sizeof path)) {
        if (svc_open_static_wav(c->opts, c->state, path) == 0) {
            ui_statusf("Static WAV: %s", path);
        }
    }
}

static void
act_raw_wav(void* v) {
    UiCtx* c = (UiCtx*)v;
    char path[1024] = {0};
    if (ui_prompt_string_prefill("Raw WAV filename", c->opts->wav_out_file_raw, path, sizeof path)) {
        if (svc_open_raw_wav(c->opts, c->state, path) == 0) {
            ui_statusf("Raw WAV: %s", path);
        }
    }
}

static void
act_dsp_out(void* v) {
    UiCtx* c = (UiCtx*)v;
    char name[256] = {0};
    if (ui_prompt_string_prefill("DSP output base filename", c->opts->dsp_out_file, name, sizeof name)) {
        if (svc_set_dsp_output_file(c->opts, name) == 0) {
            ui_statusf("DSP out: %s", c->opts->dsp_out_file);
        }
    }
}

static void
act_crc_relax(void* v) {
    svc_toggle_crc_relax(((UiCtx*)v)->opts);
}

static void
act_trunk_toggle(void* v) {
    svc_toggle_trunking(((UiCtx*)v)->opts);
}

static void
act_scan_toggle(void* v) {
    svc_toggle_scanner(((UiCtx*)v)->opts);
}

static void
act_lcw_toggle(void* v) {
    svc_toggle_lcw_retune(((UiCtx*)v)->opts);
}

static void
act_p25_auto_adapt(void* v) {
    UiCtx* c = (UiCtx*)v;
    svc_toggle_p25_auto_adapt(c->opts);
    ui_statusf("P25 Auto-Adapt: %s", c->opts->p25_auto_adapt ? "On" : "Off");
}

static void
act_p25_sm_basic(void* v) {
    UiCtx* c = (UiCtx*)v;
    if (!c || !c->opts) {
        return;
    }
    c->opts->p25_sm_basic_mode = c->opts->p25_sm_basic_mode ? 0 : 1;
    if (c->opts->p25_sm_basic_mode) {
        setenv("DSD_NEO_P25_SM_BASIC", "1", 1);
        ui_statusf("P25 Simple SM: On");
        fprintf(stderr, "\n P25 SM basic mode enabled (UI).\n");
    } else {
        setenv("DSD_NEO_P25_SM_BASIC", "0", 1);
        setenv("DSD_NEO_P25_SM_NO_SAFETY", "0", 1);
        ui_statusf("P25 Simple SM: Off");
        fprintf(stderr, "\n P25 SM basic mode disabled (UI).\n");
    }
}

static void
act_setmod_bw(void* v) {
    UiCtx* c = (UiCtx*)v;
    int bw = c->opts->setmod_bw;
    if (ui_prompt_int_prefill("Setmod BW (Hz)", bw, &bw)) {
        svc_set_rigctl_setmod_bw(c->opts, bw);
    }
}

static void
act_import_chan(void* v) {
    UiCtx* c = (UiCtx*)v;
    char p[1024] = {0};
    if (ui_prompt_string("Channel map CSV", p, sizeof p)) {
        svc_import_channel_map(c->opts, c->state, p);
    }
}

static void
act_import_group(void* v) {
    UiCtx* c = (UiCtx*)v;
    char p[1024] = {0};
    if (ui_prompt_string("Group list CSV", p, sizeof p)) {
        svc_import_group_list(c->opts, c->state, p);
    }
}

static void
act_allow_toggle(void* v) {
    UiCtx* c = (UiCtx*)v;
    c->opts->trunk_use_allow_list = !c->opts->trunk_use_allow_list;
}

static void
act_tune_group(void* v) {
    svc_toggle_tune_group(((UiCtx*)v)->opts);
}

static void
act_tune_priv(void* v) {
    svc_toggle_tune_private(((UiCtx*)v)->opts);
}

static void
act_tune_data(void* v) {
    svc_toggle_tune_data(((UiCtx*)v)->opts);
}

static void
act_tg_hold(void* v) {
    UiCtx* c = (UiCtx*)v;
    int tg = (int)c->state->tg_hold;
    if (ui_prompt_int_prefill("TG Hold", tg, &tg)) {
        svc_set_tg_hold(c->state, (unsigned)tg);
    }
}

static void
act_hangtime(void* v) {
    UiCtx* c = (UiCtx*)v;
    double s = c->opts->trunk_hangtime;
    if (ui_prompt_double_prefill("Hangtime seconds", s, &s)) {
        svc_set_hangtime(c->opts, s);
    }
}

static void
act_rev_mute(void* v) {
    svc_toggle_reverse_mute(((UiCtx*)v)->opts);
}

static void
act_dmr_le(void* v) {
    svc_toggle_dmr_le(((UiCtx*)v)->opts);
}

static void
act_slot_pref(void* v) {
    UiCtx* c = (UiCtx*)v;
    int p = c->opts->slot_preference + 1;
    if (ui_prompt_int_prefill("Slot 1 or 2", p, &p)) {
        if (p < 1) {
            p = 1;
        }
        if (p > 2) {
            p = 2;
        }
        svc_set_slot_pref(c->opts, p - 1);
    }
}

static void
act_slots_on(void* v) {
    UiCtx* c = (UiCtx*)v;
    int m = (c->opts->slot1_on ? 1 : 0) | (c->opts->slot2_on ? 2 : 0);
    if (ui_prompt_int_prefill("Slots mask (0..3)", m, &m)) {
        svc_set_slots_onoff(c->opts, m);
    }
}

static void
act_keys_dec(void* v) {
    UiCtx* c = (UiCtx*)v;
    char p[1024] = {0};
    if (ui_prompt_string("Keys CSV (DEC)", p, sizeof p)) {
        svc_import_keys_dec(c->opts, c->state, p);
    }
}

static void
act_keys_hex(void* v) {
    UiCtx* c = (UiCtx*)v;
    char p[1024] = {0};
    if (ui_prompt_string("Keys CSV (HEX)", p, sizeof p)) {
        svc_import_keys_hex(c->opts, c->state, p);
    }
}

static void
act_tyt_ap(void* v) {
    UiCtx* c = (UiCtx*)v;
    char s[256] = {0};
    if (ui_prompt_string("TYT AP string", s, sizeof s)) {
        tyt_ap_pc4_keystream_creation(c->state, s);
    }
}

static void
act_retevis_rc2(void* v) {
    UiCtx* c = (UiCtx*)v;
    char s[256] = {0};
    if (ui_prompt_string("Retevis AP string", s, sizeof s)) {
        retevis_rc2_keystream_creation(c->state, s);
    }
}

static void
act_tyt_ep(void* v) {
    UiCtx* c = (UiCtx*)v;
    char s[256] = {0};
    if (ui_prompt_string("TYT EP string", s, sizeof s)) {
        tyt_ep_aes_keystream_creation(c->state, s);
    }
}

static void
act_ken_scr(void* v) {
    UiCtx* c = (UiCtx*)v;
    char s[256] = {0};
    if (ui_prompt_string("Kenwood scrambler", s, sizeof s)) {
        ken_dmr_scrambler_keystream_creation(c->state, s);
    }
}

static void
act_anytone_bp(void* v) {
    UiCtx* c = (UiCtx*)v;
    char s[256] = {0};
    if (ui_prompt_string("Anytone BP", s, sizeof s)) {
        anytone_bp_keystream_creation(c->state, s);
    }
}

static void
act_xor_ks(void* v) {
    UiCtx* c = (UiCtx*)v;
    char s[256] = {0};
    if (ui_prompt_string("XOR keystream", s, sizeof s)) {
        straight_mod_xor_keystream_creation(c->state, s);
    }
}
#ifdef USE_RTLSDR
static void
act_rtl_opts(void* v) {
    UiCtx* c = (UiCtx*)v;
    ui_menu_rtl_options(c->opts, c->state);
}
#endif

static __attribute__((unused)) void
act_key_entry(void* v) {
    UiCtx* c = (UiCtx*)v;
    ui_menu_key_entry(c->opts, c->state);
}

static __attribute__((unused)) void
act_io_opts(void* v) {
    UiCtx* c = (UiCtx*)v;
    ui_menu_io_options(c->opts, c->state);
}

static void
act_devices_io(void* v) {
    UiCtx* c = (UiCtx*)v;
    ui_menu_io_options(c->opts, c->state);
}

static void
act_logging_capture_menu(void* v) {
    UiCtx* c = (UiCtx*)v;
    ui_menu_logging_capture(c->opts, c->state);
}

static void
act_trunk_ctrl_menu(void* v) {
    UiCtx* c = (UiCtx*)v;
    ui_menu_trunking_control(c->opts, c->state);
}

static void
act_keys_sec_menu(void* v) {
    UiCtx* c = (UiCtx*)v;
    ui_menu_keys_security(c->opts, c->state);
}

static void
act_dsp_opts(void* v) {
    UiCtx* c = (UiCtx*)v;
    ui_menu_dsp_options(c->opts, c->state);
}

// ---- UI Display Options ----
static const char*
lbl_ui_p25_metrics(void* v, char* b, size_t n) {
    UiCtx* c = (UiCtx*)v;
    snprintf(b, n, "Show P25 Metrics [%s]", (c && c->opts && c->opts->show_p25_metrics) ? "On" : "Off");
    return b;
}

static void
act_toggle_ui_p25_metrics(void* v) {
    UiCtx* c = (UiCtx*)v;
    if (!c || !c->opts) {
        return;
    }
    c->opts->show_p25_metrics = c->opts->show_p25_metrics ? 0 : 1;
}

static const char*
lbl_ui_p25_affil(void* v, char* b, size_t n) {
    UiCtx* c = (UiCtx*)v;
    snprintf(b, n, "Show P25 Affiliations [%s]", (c && c->opts && c->opts->show_p25_affiliations) ? "On" : "Off");
    return b;
}

static void
act_toggle_ui_p25_affil(void* v) {
    UiCtx* c = (UiCtx*)v;
    if (!c || !c->opts) {
        return;
    }
    c->opts->show_p25_affiliations = c->opts->show_p25_affiliations ? 0 : 1;
}

static const char*
lbl_ui_p25_ga(void* v, char* b, size_t n) {
    UiCtx* c = (UiCtx*)v;
    snprintf(b, n, "Show P25 Group Affiliation [%s]",
             (c && c->opts && c->opts->show_p25_group_affiliations) ? "On" : "Off");
    return b;
}

static void
act_toggle_ui_p25_ga(void* v) {
    UiCtx* c = (UiCtx*)v;
    if (!c || !c->opts) {
        return;
    }
    c->opts->show_p25_group_affiliations = c->opts->show_p25_group_affiliations ? 0 : 1;
}

static const char*
lbl_ui_p25_neighbors(void* v, char* b, size_t n) {
    UiCtx* c = (UiCtx*)v;
    snprintf(b, n, "Show P25 Neighbors [%s]", (c && c->opts && c->opts->show_p25_neighbors) ? "On" : "Off");
    return b;
}

static void
act_toggle_ui_p25_neighbors(void* v) {
    UiCtx* c = (UiCtx*)v;
    if (!c || !c->opts) {
        return;
    }
    c->opts->show_p25_neighbors = c->opts->show_p25_neighbors ? 0 : 1;
}

static const char*
lbl_ui_p25_iden(void* v, char* b, size_t n) {
    UiCtx* c = (UiCtx*)v;
    snprintf(b, n, "Show P25 IDEN Plan [%s]", (c && c->opts && c->opts->show_p25_iden_plan) ? "On" : "Off");
    return b;
}

static void
act_toggle_ui_p25_iden(void* v) {
    UiCtx* c = (UiCtx*)v;
    if (!c || !c->opts) {
        return;
    }
    c->opts->show_p25_iden_plan = c->opts->show_p25_iden_plan ? 0 : 1;
}

static const char*
lbl_ui_p25_ccc(void* v, char* b, size_t n) {
    UiCtx* c = (UiCtx*)v;
    snprintf(b, n, "Show P25 CC Candidates [%s]", (c && c->opts && c->opts->show_p25_cc_candidates) ? "On" : "Off");
    return b;
}

static void
act_toggle_ui_p25_ccc(void* v) {
    UiCtx* c = (UiCtx*)v;
    if (!c || !c->opts) {
        return;
    }
    c->opts->show_p25_cc_candidates = c->opts->show_p25_cc_candidates ? 0 : 1;
}

static const char*
lbl_ui_channels(void* v, char* b, size_t n) {
    UiCtx* c = (UiCtx*)v;
    snprintf(b, n, "Show Channels [%s]", (c && c->opts && c->opts->show_channels) ? "On" : "Off");
    return b;
}

static void
act_toggle_ui_channels(void* v) {
    UiCtx* c = (UiCtx*)v;
    if (!c || !c->opts) {
        return;
    }
    c->opts->show_channels = c->opts->show_channels ? 0 : 1;
}

static void
act_ui_display(void* v) {
    UiCtx* c = (UiCtx*)v;
    static const NcMenuItem items[] = {
        {.id = "p25m",
         .label_fn = lbl_ui_p25_metrics,
         .help = "Toggle P25 Metrics section.",
         .on_select = act_toggle_ui_p25_metrics},
        {.id = "p25aff",
         .label_fn = lbl_ui_p25_affil,
         .help = "Toggle P25 Affiliations section (RID list).",
         .on_select = act_toggle_ui_p25_affil},
        {.id = "p25ga",
         .label_fn = lbl_ui_p25_ga,
         .help = "Toggle P25 Group Affiliation section (RID↔TG).",
         .on_select = act_toggle_ui_p25_ga},
        {.id = "p25nb",
         .label_fn = lbl_ui_p25_neighbors,
         .help = "Toggle P25 Neighbors section (adjacent/candidate freqs).",
         .on_select = act_toggle_ui_p25_neighbors},
        {.id = "p25iden",
         .label_fn = lbl_ui_p25_iden,
         .help = "Toggle P25 IDEN Plan table.",
         .on_select = act_toggle_ui_p25_iden},
        {.id = "p25ccc",
         .label_fn = lbl_ui_p25_ccc,
         .help = "Toggle P25 CC Candidates list.",
         .on_select = act_toggle_ui_p25_ccc},
        {.id = "chans",
         .label_fn = lbl_ui_channels,
         .help = "Toggle Channels section.",
         .on_select = act_toggle_ui_channels},
    };
    ui_menu_run(items, sizeof items / sizeof items[0], c);
}

static void
act_lrrp_opts(void* v) {
    UiCtx* c = (UiCtx*)v;
    ui_menu_lrrp_options(c->opts, c->state);
}

static void
act_p2_params(void* v) {
    UiCtx* c = (UiCtx*)v;
    unsigned long long w = 0, s = 0, n = 0;
    char buf[64];
    snprintf(buf, sizeof buf, "%llX", (unsigned long long)c->state->p2_wacn);
    if (ui_prompt_string_prefill("Enter Phase 2 WACN (HEX)", buf, buf, sizeof buf) && parse_hex_u64(buf, &w)) {}
    snprintf(buf, sizeof buf, "%llX", (unsigned long long)c->state->p2_sysid);
    if (ui_prompt_string_prefill("Enter Phase 2 SYSID (HEX)", buf, buf, sizeof buf) && parse_hex_u64(buf, &s)) {}
    snprintf(buf, sizeof buf, "%llX", (unsigned long long)c->state->p2_cc);
    if (ui_prompt_string_prefill("Enter Phase 2 NAC/CC (HEX)", buf, buf, sizeof buf) && parse_hex_u64(buf, &n)) {}
    svc_set_p2_params(c->state, w, s, n);
}

static void
act_exit(void* v) {
    (void)v;
    exitflag = 1;
}

void
ui_menu_main(dsd_opts* opts, dsd_state* state) {
    UiCtx ctx = {opts, state};
    static const NcMenuItem decode_items[] = {
        {.id = "auto", .label = "Auto", .help = "Auto-detect: P25p1, P25p2, DMR, YSF.", .on_select = act_mode_auto},
        {.id = "tdma", .label = "TDMA", .help = "TDMA focus: P25p1, P25p2, DMR.", .on_select = act_mode_tdma},
        {.id = "p25p2", .label = "P25 Phase 2", .help = "P25 Phase 2 control or voice.", .on_select = act_mode_p25p2},
        {.id = "dmr", .label = "DMR", .help = "Switch to DMR (stereo).", .on_select = act_mode_dmr},
        {.id = "ysf", .label = "YSF", .help = "Switch to Yaesu System Fusion.", .on_select = act_mode_ysf},
        {.id = "dstar", .label = "D-STAR", .help = "Switch to D-STAR demodulation.", .on_select = act_mode_dstar},
        {.id = "m17", .label = "M17", .help = "Switch to M17 demodulation.", .on_select = act_mode_m17},
        {.id = "edacs", .label = "EDACS / ProVoice", .help = "EDACS/ProVoice (GFSK).", .on_select = act_mode_edacs},
        {.id = "n48", .label = "NXDN 48", .help = "Switch to NXDN 48.", .on_select = act_mode_n48},
        {.id = "n96", .label = "NXDN 96", .help = "Switch to NXDN 96.", .on_select = act_mode_n96},
        {.id = "dpmr", .label = "dPMR", .help = "Switch to dPMR demodulation.", .on_select = act_mode_dpmr},
    };

    static const NcMenuItem items[] = {
        {.id = "decode",
         .label = "Decode...",
         .help = "Select decode mode.",
         .submenu = decode_items,
         .submenu_len = sizeof decode_items / sizeof decode_items[0]},
        {.id = "devices_io",
         .label = "Devices & IO",
         .help = "TCP, symbol replay, inversion.",
         .on_select = act_devices_io},
        {.id = "logging",
         .label = "Logging & Capture",
         .help = "Symbols, WAV, payloads, alerts, history.",
         .on_select = act_logging_capture_menu},
        {.id = "trunk_ctrl",
         .label = "Trunking & Control",
         .help = "P25 CC prefs, Phase 2 params, rigctl.",
         .on_select = act_trunk_ctrl_menu},
        {.id = "keys_sec",
         .label = "Keys & Security",
         .help = "Manage keys and encrypted audio muting.",
         .on_select = act_keys_sec_menu},
        {.id = "dsp",
         .label = "DSP Options",
         .help = "RTL-SDR DSP toggles and tuning.",
         .is_enabled = io_rtl_active,
         .on_select = act_dsp_opts},
        {.id = "ui_display", .label = "UI Display", .help = "Toggle on-screen sections.", .on_select = act_ui_display},
        {.id = "lrrp", .label = "LRRP Output", .help = "Configure LRRP file output.", .on_select = act_lrrp_opts},
        {.id = "exit", .label = "Exit DSD-neo", .help = "Quit the application.", .on_select = act_exit},
    };
    ui_menu_run(items, sizeof items / sizeof items[0], &ctx);
}

/* Blanker UI handlers implementation */
#ifdef USE_RTLSDR
static const char*
lbl_blanker(void* v, char* b, size_t n) {
    UNUSED(v);
    int thr = 0, win = 0;
    int on = rtl_stream_get_blanker(&thr, &win);
    snprintf(b, n, "Impulse Blanker: %s", on ? "On" : "Off");
    return b;
}

static const char*
lbl_blanker_thr(void* v, char* b, size_t n) {
    UNUSED(v);
    int thr = 0;
    rtl_stream_get_blanker(&thr, NULL);
    snprintf(b, n, "Blanker Thr: %d", thr);
    return b;
}

static const char*
lbl_blanker_win(void* v, char* b, size_t n) {
    UNUSED(v);
    int win = 0;
    rtl_stream_get_blanker(NULL, &win);
    snprintf(b, n, "Blanker Win: %d", win);
    return b;
}

static void
act_toggle_blanker(void* v) {
    UNUSED(v);
    int thr = 0, win = 0;
    int on = rtl_stream_get_blanker(&thr, &win);
    rtl_stream_set_blanker(on ? 0 : 1, -1, -1);
}

static void
act_blanker_thr_up(void* v) {
    UNUSED(v);
    int thr = 0;
    rtl_stream_get_blanker(&thr, NULL);
    thr += 2000;
    if (thr > 60000) {
        thr = 60000;
    }
    rtl_stream_set_blanker(-1, thr, -1);
}

static void
act_blanker_thr_dn(void* v) {
    UNUSED(v);
    int thr = 0;
    rtl_stream_get_blanker(&thr, NULL);
    thr -= 2000;
    if (thr < 0) {
        thr = 0;
    }
    rtl_stream_set_blanker(-1, thr, -1);
}

static void
act_blanker_win_up(void* v) {
    UNUSED(v);
    int win = 0;
    rtl_stream_get_blanker(NULL, &win);
    win += 1;
    if (win > 16) {
        win = 16;
    }
    rtl_stream_set_blanker(-1, -1, win);
}

static void
act_blanker_win_dn(void* v) {
    UNUSED(v);
    int win = 0;
    rtl_stream_get_blanker(NULL, &win);
    win -= 1;
    if (win < 0) {
        win = 0;
    }
    rtl_stream_set_blanker(-1, -1, win);
}
#endif
