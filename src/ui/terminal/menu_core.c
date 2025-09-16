// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/ui/menu_core.h>

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

#include <dsd-neo/ui/menu_services.h>

#ifdef USE_RTLSDR
#include <dsd-neo/io/rtl_stream_c.h>
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
    vsnprintf(g_status_msg, sizeof g_status_msg, fmt, ap);
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

static void
ui_draw_menu(WINDOW* menu_win, const NcMenuItem* items, size_t n, int hi, void* ctx) {
    int x = 2;
    int y = 1;
    werase(menu_win);
    box(menu_win, 0, 0);
    int mh = 0, mw = 0;
    getmaxyx(menu_win, mh, mw);
    for (size_t i = 0; i < n; i++) {
        if ((int)i == hi) {
            wattron(menu_win, A_REVERSE);
        }
        if (!ui_is_enabled(&items[i], ctx)) {
            wattron(menu_win, A_DIM);
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
        wattroff(menu_win, A_REVERSE | A_DIM);
    }
    // footer help
    mvwprintw(menu_win, mh - 3, x, "Arrows: move  Enter: select  h: help  q: back");
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
    WINDOW* hw = ui_make_window(h, w, 3, 6);
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
    int height = (int)(n + 6);
    if (height < 10) {
        height = 10;
    }
    int width = 50;
    // Redraw underlying stdscr so base application remains visible beneath menu
    redrawwin(stdscr);
    refresh();
    WINDOW* menu_win = ui_make_window(height, width, 1, 2);
    keypad(menu_win, TRUE);

    int hi = 0;
    while (1) {
        ui_draw_menu(menu_win, items, n, hi, ctx);
        int c = wgetch(menu_win);
        if (c == KEY_UP) {
            do {
                hi = (hi - 1 + (int)n) % (int)n;
            } while (!ui_is_enabled(&items[hi], ctx));
        } else if (c == KEY_DOWN) {
            do {
                hi = (hi + 1) % (int)n;
            } while (!ui_is_enabled(&items[hi], ctx));
        } else if (c == 'h' || c == 'H') {
            ui_show_help(&items[hi]);
        } else if (c == 'q' || c == 'Q') {
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
ui_prompt_common(const char* title, char* buf, size_t cap) {
    if (!buf || cap == 0) {
        return 0;
    }
    buf[0] = '\0';
    size_t len = 0;
    int h = 8, w = (int)(strlen(title) + 16);
    if (w < 54) {
        w = 54;
    }
    WINDOW* win = ui_make_window(h, w, 4, 4);
    keypad(win, TRUE);
    noecho();
    curs_set(1);
    mvwprintw(win, 1, 2, "%s", title);
    mvwprintw(win, 3, 2, "> ");
    mvwprintw(win, h - 2, 2, "Enter=OK  Esc=Cancel");
    wmove(win, 3, 4);
    wrefresh(win);

    while (1) {
        int ch = wgetch(win);
        if (ch == 27) { // ESC
            len = 0;
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
ui_prompt_string(const char* title, char* out, size_t out_cap) {
    return ui_prompt_common(title, out, out_cap);
}

int
ui_prompt_int(const char* title, int* out) {
    char tmp[64] = {0};
    if (!ui_prompt_common(title, tmp, sizeof tmp)) {
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
    if (!ui_prompt_common(title, tmp, sizeof tmp)) {
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
    WINDOW* win = ui_make_window(h, w, 5, 5);
    mvwprintw(win, 1, 2, "%s", title);
    mvwprintw(win, 3, 2, "y = Yes, n = No, Esc = Cancel");
    wrefresh(win);
    int res = 0;
    while (1) {
        int c = wgetch(win);
        if (c == 'y' || c == 'Y') {
            res = 1;
            break;
        }
        if (c == 'n' || c == 'N' || c == 27) {
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
        fprintf(stderr, "\n P25: Prefer CC Candidates: ON\n");
    } else {
        fprintf(stderr, "\n P25: Prefer CC Candidates: OFF\n");
    }
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

static void
io_tcp_direct_link(void* vctx) {
    UiCtx* c = (UiCtx*)vctx;
    // Defaults
    snprintf(c->opts->tcp_hostname, sizeof c->opts->tcp_hostname, "%s", "localhost");
    c->opts->tcp_portno = 7355;

    if (!ui_prompt_string("Enter TCP Direct Link Hostname (default: localhost)", c->opts->tcp_hostname,
                          sizeof c->opts->tcp_hostname)) {
        return;
    }
    int port = c->opts->tcp_portno;
    if (!ui_prompt_int("Enter TCP Direct Link Port Number (default: 7355)", &port)) {
        return;
    }
    c->opts->tcp_portno = port;

    if (svc_tcp_connect_audio(c->opts, c->opts->tcp_hostname, c->opts->tcp_portno) == 0) {
        ui_statusf("TCP connected: %s:%d", c->opts->tcp_hostname, c->opts->tcp_portno);
    } else {
        ui_statusf("TCP connect failed: %s:%d", c->opts->tcp_hostname, c->opts->tcp_portno);
    }
}

static void
io_rigctl_config(void* vctx) {
    UiCtx* c = (UiCtx*)vctx;
    // Defaults
    snprintf(c->opts->rigctlhostname, sizeof c->opts->rigctlhostname, "%s", "localhost");
    c->opts->rigctlportno = 4532;

    if (!ui_prompt_string("Enter RIGCTL Hostname (default: localhost)", c->opts->rigctlhostname,
                          sizeof c->opts->rigctlhostname)) {
        c->opts->use_rigctl = 0;
        return;
    }
    int port = c->opts->rigctlportno;
    if (!ui_prompt_int("Enter RIGCTL Port Number (default: 4532)", &port)) {
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

void
ui_menu_io_options(dsd_opts* opts, dsd_state* state) {
    // Reorganized: Devices & IO (sources and immediate playback controls)
    UiCtx ctx = {opts, state};
    static const NcMenuItem items[] = {
        {.id = "tcp_input",
         .label = "TCP Direct Audio",
         .label_fn = lbl_tcp,
         .help = "Connect to a remote PCM16LE source via TCP.",
         .is_enabled = io_always_on,
         .on_select = io_tcp_direct_link},
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
         .help = "Invert/uninvert all supported inputs.",
         .is_enabled = io_always_on,
         .on_select = act_toggle_invert},
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
         .help = "Toggle raw payloads to console.",
         .is_enabled = io_always_on,
         .on_select = act_toggle_payload},
        {.id = "reset_eh",
         .label = "Reset Event History",
         .help = "Clear ring-buffered event history.",
         .is_enabled = io_always_on,
         .on_select = act_reset_eh},
        {.id = "call_alert",
         .label = "Toggle Call Alert Beep",
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
        {.id = "prefer_cc",
         .label = "Prefer P25 CC Candidates",
         .help = "Prefer viable control-channel candidates during hunt.",
         .is_enabled = io_always_on,
         .on_select = io_toggle_cc_candidates},
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
        {.id = "muting",
         .label = "Toggle Encrypted Audio Muting",
         .help = "Toggle P25 and DMR encrypted audio muting.",
         .is_enabled = io_always_on,
         .on_select = io_toggle_mute_enc},
    };
    ui_menu_run(items, sizeof items / sizeof items[0], &ctx);
}

#ifdef USE_RTLSDR
// Declarative DSP menu with dynamic labels
static bool
dsp_cq_on(void* v) {
    int cq = 0, f = 0, t = 0, a = 0;
    rtl_stream_dsp_get(&cq, &f, &t, &a);
    return cq != 0;
}

static bool
dsp_lms_on(void* v) {
    int l = 0, t = 0, mu = 0, st = 0, wl = 0, dfe = 0, dft = 0, mf = 0, cma = 0;
    rtl_stream_cqpsk_get(&l, &t, &mu, &st, &wl, &dfe, &dft, &mf, &cma);
    return l != 0;
}

static bool
dsp_dfe_on(void* v) {
    int l = 0, t = 0, mu = 0, st = 0, wl = 0, dfe = 0, dft = 0, mf = 0, cma = 0;
    rtl_stream_cqpsk_get(&l, &t, &mu, &st, &wl, &dfe, &dft, &mf, &cma);
    return dfe != 0;
}

static const char*
lbl_onoff_cq(void* v, char* b, size_t n) {
    int cq = 0, f = 0, t = 0, a = 0;
    rtl_stream_dsp_get(&cq, &f, &t, &a);
    snprintf(b, n, "Toggle CQPSK [%s]", cq ? "Active" : "Inactive");
    return b;
}

static const char*
lbl_onoff_fll(void* v, char* b, size_t n) {
    int cq = 0, f = 0, t = 0, a = 0;
    rtl_stream_dsp_get(&cq, &f, &t, &a);
    snprintf(b, n, "Toggle FLL [%s]", f ? "Active" : "Inactive");
    return b;
}

static const char*
lbl_onoff_ted(void* v, char* b, size_t n) {
    int cq = 0, f = 0, t = 0, a = 0;
    rtl_stream_dsp_get(&cq, &f, &t, &a);
    snprintf(b, n, "Toggle TED [%s]", t ? "Active" : "Inactive");
    return b;
}

static const char*
lbl_manual_dsp(void* v, char* b, size_t n) {
    int man = rtl_stream_get_manual_dsp();
    snprintf(b, n, "Manual DSP Override [%s]", man ? "Active" : "Inactive");
    return b;
}

static void
act_toggle_manual_dsp(void* v) {
    int man = rtl_stream_get_manual_dsp();
    rtl_stream_set_manual_dsp(man ? 0 : 1);
}

static const char*
lbl_onoff_auto(void* v, char* b, size_t n) {
    int cq = 0, f = 0, t = 0, a = 0;
    rtl_stream_dsp_get(&cq, &f, &t, &a);
    snprintf(b, n, "Toggle Auto-DSP [%s]", a ? "Active" : "Inactive");
    return b;
}

static const char*
lbl_onoff_lms(void* v, char* b, size_t n) {
    int l = 0, t = 0, mu = 0, st = 0, wl = 0, dfe = 0, dft = 0, mf = 0, cma = 0;
    rtl_stream_cqpsk_get(&l, &t, &mu, &st, &wl, &dfe, &dft, &mf, &cma);
    snprintf(b, n, "Toggle LMS [%s]", l ? "Active" : "Inactive");
    return b;
}

static const char*
lbl_onoff_mf(void* v, char* b, size_t n) {
    int l = 0, t = 0, mu = 0, st = 0, wl = 0, dfe = 0, dft = 0, mf = 0, cma = 0;
    rtl_stream_cqpsk_get(&l, &t, &mu, &st, &wl, &dfe, &dft, &mf, &cma);
    snprintf(b, n, "Toggle Matched Filter [%s]", mf ? "Active" : "Inactive");
    return b;
}

static const char*
lbl_toggle_rrc(void* v, char* b, size_t n) {
    int on = 0, a = 0, s = 0;
    rtl_stream_cqpsk_get_rrc(&on, &a, &s);
    snprintf(b, n, "Toggle RRC [%s]", on ? "Active" : "Inactive");
    return b;
}

static const char*
lbl_rrc_a_up(void* v, char* b, size_t n) {
    int on = 0, a = 0, s = 0;
    rtl_stream_cqpsk_get_rrc(&on, &a, &s);
    snprintf(b, n, "RRC alpha +5%% (now %d%%)", a);
    return b;
}

static const char*
lbl_rrc_a_dn(void* v, char* b, size_t n) {
    int on = 0, a = 0, s = 0;
    rtl_stream_cqpsk_get_rrc(&on, &a, &s);
    snprintf(b, n, "RRC alpha -5%% (now %d%%)", a);
    return b;
}

static const char*
lbl_rrc_s_up(void* v, char* b, size_t n) {
    int on = 0, a = 0, s = 0;
    rtl_stream_cqpsk_get_rrc(&on, &a, &s);
    snprintf(b, n, "RRC span +1 (now %d)", s);
    return b;
}

static const char*
lbl_rrc_s_dn(void* v, char* b, size_t n) {
    int on = 0, a = 0, s = 0;
    rtl_stream_cqpsk_get_rrc(&on, &a, &s);
    snprintf(b, n, "RRC span -1 (now %d)", s);
    return b;
}

static const char*
lbl_onoff_wl(void* v, char* b, size_t n) {
    int l = 0, t = 0, mu = 0, st = 0, wl = 0, dfe = 0, dft = 0, mf = 0, cma = 0;
    rtl_stream_cqpsk_get(&l, &t, &mu, &st, &wl, &dfe, &dft, &mf, &cma);
    snprintf(b, n, "Toggle WL [%s]", wl ? "Active" : "Inactive");
    return b;
}

static const char*
lbl_onoff_dfe(void* v, char* b, size_t n) {
    int l = 0, t = 0, mu = 0, st = 0, wl = 0, dfe = 0, dft = 0, mf = 0, cma = 0;
    rtl_stream_cqpsk_get(&l, &t, &mu, &st, &wl, &dfe, &dft, &mf, &cma);
    snprintf(b, n, "Toggle DFE [%s]", dfe ? "Active" : "Inactive");
    return b;
}

static const char*
lbl_dft_cycle(void* v, char* b, size_t n) {
    int l = 0, t = 0, mu = 0, st = 0, wl = 0, dfe = 0, dft = 0, mf = 0, cma = 0;
    rtl_stream_cqpsk_get(&l, &t, &mu, &st, &wl, &dfe, &dft, &mf, &cma);
    snprintf(b, n, "Cycle DFE taps: %d", dft);
    return b;
}

static const char*
lbl_eq_taps(void* v, char* b, size_t n) {
    int l = 0, taps = 0, mu = 0, st = 0, wl = 0, dfe = 0, dft = 0, mf = 0, cma = 0;
    rtl_stream_cqpsk_get(&l, &taps, &mu, &st, &wl, &dfe, &dft, &mf, &cma);
    snprintf(b, n, "Set EQ taps 5/7 (now %d)", taps);
    return b;
}

static const char*
lbl_onoff_dqpsk(void* v, char* b, size_t n) {
    int on = 0;
    rtl_stream_cqpsk_get_dqpsk(&on);
    snprintf(b, n, "Toggle DQPSK decision [%s]", on ? "Active" : "Inactive");
    return b;
}

static void
act_toggle_cq(void* v) {
    int cq = 0, f = 0, t = 0, a = 0;
    rtl_stream_dsp_get(&cq, &f, &t, &a);
    rtl_stream_toggle_cqpsk(cq ? 0 : 1);
}

static void
act_toggle_fll(void* v) {
    int cq = 0, f = 0, t = 0, a = 0;
    rtl_stream_dsp_get(&cq, &f, &t, &a);
    rtl_stream_toggle_fll(f ? 0 : 1);
}

static void
act_toggle_ted(void* v) {
    int cq = 0, f = 0, t = 0, a = 0;
    rtl_stream_dsp_get(&cq, &f, &t, &a);
    rtl_stream_toggle_ted(t ? 0 : 1);
}

static void
act_toggle_auto(void* v) {
    int cq = 0, f = 0, t = 0, a = 0;
    rtl_stream_dsp_get(&cq, &f, &t, &a);
    rtl_stream_toggle_auto_dsp(a ? 0 : 1);
}

static void
act_toggle_lms(void* v) {
    int l = 0, taps = 0, mu = 0, st = 0, wl = 0, dfe = 0, dft = 0, mf = 0, cma = 0;
    rtl_stream_cqpsk_get(&l, &taps, &mu, &st, &wl, &dfe, &dft, &mf, &cma);
    rtl_stream_cqpsk_set(l ? 0 : 1, -1, -1, -1, -1, -1, -1, -1, -1);
}

static void
act_toggle_mf(void* v) {
    int l = 0, taps = 0, mu = 0, st = 0, wl = 0, dfe = 0, dft = 0, mf = 0, cma = 0;
    rtl_stream_cqpsk_get(&l, &taps, &mu, &st, &wl, &dfe, &dft, &mf, &cma);
    rtl_stream_cqpsk_set(-1, -1, -1, -1, -1, -1, -1, mf ? 0 : 1, -1);
}

static void
act_toggle_rrc(void* v) {
    int on = 0, a = 0, s = 0;
    rtl_stream_cqpsk_get_rrc(&on, &a, &s);
    rtl_stream_cqpsk_set_rrc(on ? 0 : 1, -1, -1);
}

static void
act_rrc_a_up(void* v) {
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
    rtl_stream_cqpsk_set(-1, -1, -1, -1, -1, -1, -1, -1, 1500);
}

static void
act_toggle_wl(void* v) {
    int l = 0, taps = 0, mu = 0, st = 0, wl = 0, dfe = 0, dft = 0, mf = 0, cma = 0;
    rtl_stream_cqpsk_get(&l, &taps, &mu, &st, &wl, &dfe, &dft, &mf, &cma);
    rtl_stream_cqpsk_set(-1, -1, -1, -1, wl ? 0 : 1, -1, -1, -1, -1);
}

static void
act_toggle_dfe(void* v) {
    int l = 0, taps = 0, mu = 0, st = 0, wl = 0, dfe = 0, dft = 0, mf = 0, cma = 0;
    rtl_stream_cqpsk_get(&l, &taps, &mu, &st, &wl, &dfe, &dft, &mf, &cma);
    rtl_stream_cqpsk_set(-1, -1, -1, -1, -1, dfe ? 0 : 1, dft, -1, -1);
}

static void
act_cycle_dft(void* v) {
    int l = 0, taps = 0, mu = 0, st = 0, wl = 0, dfe = 0, dft = 0, mf = 0, cma = 0;
    rtl_stream_cqpsk_get(&l, &taps, &mu, &st, &wl, &dfe, &dft, &mf, &cma);
    int nd = (dft + 1) & 3;
    rtl_stream_cqpsk_set(-1, -1, -1, -1, -1, dfe, nd, -1, -1);
}

static void
act_taps_5_7(void* v) {
    int l = 0, taps = 0, mu = 0, st = 0, wl = 0, dfe = 0, dft = 0, mf = 0, cma = 0;
    rtl_stream_cqpsk_get(&l, &taps, &mu, &st, &wl, &dfe, &dft, &mf, &cma);
    int nt = (taps >= 7) ? 5 : 7;
    rtl_stream_cqpsk_set(-1, nt, -1, -1, -1, -1, -1, -1, -1);
}

static void
act_toggle_dqpsk(void* v) {
    int on = 0;
    rtl_stream_cqpsk_get_dqpsk(&on);
    extern void rtl_stream_cqpsk_set_dqpsk(int);
    rtl_stream_cqpsk_set_dqpsk(on ? 0 : 1);
}

void
ui_menu_dsp_options(dsd_opts* opts, dsd_state* state) {
    UiCtx ctx = {opts, state};
    static const NcMenuItem items[] = {
        {.id = "hint",
         .label = "Hint: Labels show live; Manual Override pins.",
         .help = "Status rows reflect live runtime; Manual Override keeps your settings."},
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
        {.id = "auto",
         .label = "Toggle Auto-DSP",
         .label_fn = lbl_onoff_auto,
         .help = "Enable/disable auto-DSP.",
         .on_select = act_toggle_auto},
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

static void
act_key_entry(void* v) {
    UiCtx* c = (UiCtx*)v;
    ui_menu_key_entry(c->opts, c->state);
}

static void
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
    if (ui_prompt_string("Enter Phase 2 WACN (HEX)", buf, sizeof buf) && parse_hex_u64(buf, &w)) {}
    if (ui_prompt_string("Enter Phase 2 SYSID (HEX)", buf, sizeof buf) && parse_hex_u64(buf, &s)) {}
    if (ui_prompt_string("Enter Phase 2 NAC/CC (HEX)", buf, sizeof buf) && parse_hex_u64(buf, &n)) {}
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
        {.id = "dsp", .label = "DSP Options", .help = "RTL-SDR DSP toggles and tuning.", .on_select = act_dsp_opts},
        {.id = "lrrp", .label = "LRRP Output", .help = "Configure LRRP file output.", .on_select = act_lrrp_opts},
        {.id = "exit", .label = "Exit DSD-neo", .help = "Quit the application.", .on_select = act_exit},
    };
    ui_menu_run(items, sizeof items / sizeof items[0], &ctx);
}
