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
#include <strings.h>

#if defined(__SSE__) || defined(__SSE2__)
#include <xmmintrin.h>
#endif

#ifdef USE_RTLSDR
#include <dsd-neo/io/rtl_stream_c.h>
#endif

/* Forward declarations for new P25p2 RRC UI handlers */
static const char* lbl_p25p2_rrc_autoprobe(void* v, char* b, size_t n);
static void io_toggle_p25p2_rrc_autoprobe(void* vctx);
// Forward decl for hex parser used by async callbacks
static int parse_hex_u64(const char* s, unsigned long long* out);

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

// Shared UI context for menu callbacks
typedef struct {
    dsd_opts* opts;
    dsd_state* state;
} UiCtx;

// Help overlay handled via nonblocking ui_help_open/ui_help_close

// forward declarations for actions referenced from multiple menus
static void act_toggle_invert(void* v);
static void act_toggle_payload(void* v);
static void act_reset_eh(void* v);
static void act_p2_params(void* v);
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
static void act_p25_enc_lockout(void* v);
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
// Prototypes for DSP C4FM clock assist controls defined later
static const char* lbl_c4fm_clk(void* v, char* b, size_t n);
static void act_c4fm_clk_cycle(void* v);
static const char* lbl_c4fm_clk_sync(void* v, char* b, size_t n);
static void act_c4fm_clk_sync_toggle(void* v);
#ifdef USE_RTLSDR
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

// -------------------- Nonblocking overlay driver --------------------

typedef struct {
    const NcMenuItem* items;
    size_t n;
    int hi;
    WINDOW* win;
    int w, h;
    int y, x;
} UiMenuFrame;

static int g_overlay_open = 0;
static UiMenuFrame g_stack[8];
static int g_depth = 0;
static UiCtx g_ctx_overlay = {0};

// Transient generic chooser (string list) overlay
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

// Forward decls for chooser completion handlers
static void chooser_done_pulse_out(void* u, int sel);
static void chooser_done_pulse_in(void* u, int sel);

// ---- Prompt overlays (highest priority) ----
typedef struct {
    int active;
    const char* title;
    WINDOW* win;
    // string mode fields
    char* buf;
    size_t cap;
    size_t len;
    void (*on_done_str)(void* user, const char* text); // NULL text indicates cancel/empty
    void* user;
} UiPrompt;

static UiPrompt g_prompt = {0};

static void
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
    memset(&g_prompt, 0, sizeof(g_prompt));
}

static void
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

// Convenience typed wrappers
typedef struct {
    void (*cb)(void*, int, int);
    void* user;
} PromptIntCtx;

typedef struct {
    void (*cb)(void*, int, double);
    void* user;
} PromptDblCtx;

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

static void
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

static void
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

// ---- Async prompt action callbacks and contexts ----
typedef struct {
    UiCtx* c;
    char host[256];
    int port;
} UdpOutCtx;

typedef struct {
    UiCtx* c;
} TcpWavSymCtx; // reuse for simple one-shot string

typedef struct {
    UiCtx* c;
    char host[256];
    int port;
} TcpLinkCtx;

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

// Simple string path setters
static void
cb_event_log_set(void* v, const char* path) {
    UiCtx* c = (UiCtx*)v;
    if (!c) {
        return;
    }
    if (path && *path) {
        if (svc_set_event_log(c->opts, path) == 0) {
            ui_statusf("Event log: %s", path);
        }
    }
}

static void
cb_static_wav(void* v, const char* path) {
    UiCtx* c = (UiCtx*)v;
    if (!c) {
        return;
    }
    if (path && *path) {
        if (svc_open_static_wav(c->opts, c->state, path) == 0) {
            ui_statusf("Static WAV: %s", path);
        }
    }
}

static void
cb_raw_wav(void* v, const char* path) {
    UiCtx* c = (UiCtx*)v;
    if (!c) {
        return;
    }
    if (path && *path) {
        if (svc_open_raw_wav(c->opts, c->state, path) == 0) {
            ui_statusf("Raw WAV: %s", path);
        }
    }
}

static void
cb_dsp_out(void* v, const char* name) {
    UiCtx* c = (UiCtx*)v;
    if (!c) {
        return;
    }
    if (name && *name) {
        if (svc_set_dsp_output_file(c->opts, name) == 0) {
            ui_statusf("DSP out: %s", c->opts->dsp_out_file);
        }
    }
}

// Generic file imports
static void
cb_import_chan(void* v, const char* p) {
    UiCtx* c = (UiCtx*)v;
    if (!c) {
        return;
    }
    if (p && *p) {
        svc_import_channel_map(c->opts, c->state, p);
    }
}

static void
cb_import_group(void* v, const char* p) {
    UiCtx* c = (UiCtx*)v;
    if (!c) {
        return;
    }
    if (p && *p) {
        svc_import_group_list(c->opts, c->state, p);
    }
}

static void
cb_keys_dec(void* v, const char* p) {
    UiCtx* c = (UiCtx*)v;
    if (!c) {
        return;
    }
    if (p && *p) {
        svc_import_keys_dec(c->opts, c->state, p);
    }
}

static void
cb_keys_hex(void* v, const char* p) {
    UiCtx* c = (UiCtx*)v;
    if (!c) {
        return;
    }
    if (p && *p) {
        svc_import_keys_hex(c->opts, c->state, p);
    }
}

// Small typed setters
static void
cb_setmod_bw(void* v, int ok, int bw) {
    UiCtx* c = (UiCtx*)v;
    if (!c) {
        return;
    }
    if (ok) {
        svc_set_rigctl_setmod_bw(c->opts, bw);
    }
}

static void
cb_tg_hold(void* v, int ok, int tg) {
    UiCtx* c = (UiCtx*)v;
    if (!c) {
        return;
    }
    if (ok) {
        svc_set_tg_hold(c->state, (unsigned)tg);
    }
}

static void
cb_hangtime(void* v, int ok, double s) {
    UiCtx* c = (UiCtx*)v;
    if (!c) {
        return;
    }
    if (ok) {
        svc_set_hangtime(c->opts, s);
    }
}

static void
cb_slot_pref(void* v, int ok, int p) {
    UiCtx* c = (UiCtx*)v;
    if (!c) {
        return;
    }
    if (ok) {
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
cb_slots_on(void* v, int ok, int m) {
    UiCtx* c = (UiCtx*)v;
    if (!c) {
        return;
    }
    if (ok) {
        svc_set_slots_onoff(c->opts, m);
    }
}

// Keystream helpers
static void
cb_tyt_ap(void* v, const char* s) {
    UiCtx* c = (UiCtx*)v;
    if (!c) {
        return;
    }
    if (s && *s) {
        char tmp[256];
        snprintf(tmp, sizeof tmp, "%s", s);
        tyt_ap_pc4_keystream_creation(c->state, tmp);
    }
}

static void
cb_retevis_rc2(void* v, const char* s) {
    UiCtx* c = (UiCtx*)v;
    if (!c) {
        return;
    }
    if (s && *s) {
        char tmp[256];
        snprintf(tmp, sizeof tmp, "%s", s);
        retevis_rc2_keystream_creation(c->state, tmp);
    }
}

static void
cb_tyt_ep(void* v, const char* s) {
    UiCtx* c = (UiCtx*)v;
    if (!c) {
        return;
    }
    if (s && *s) {
        char tmp[256];
        snprintf(tmp, sizeof tmp, "%s", s);
        tyt_ep_aes_keystream_creation(c->state, tmp);
    }
}

static void
cb_ken_scr(void* v, const char* s) {
    UiCtx* c = (UiCtx*)v;
    if (!c) {
        return;
    }
    if (s && *s) {
        char tmp[256];
        snprintf(tmp, sizeof tmp, "%s", s);
        ken_dmr_scrambler_keystream_creation(c->state, tmp);
    }
}

static void
cb_anytone_bp(void* v, const char* s) {
    UiCtx* c = (UiCtx*)v;
    if (!c) {
        return;
    }
    if (s && *s) {
        char tmp[256];
        snprintf(tmp, sizeof tmp, "%s", s);
        anytone_bp_keystream_creation(c->state, tmp);
    }
}

static void
cb_xor_ks(void* v, const char* s) {
    UiCtx* c = (UiCtx*)v;
    if (!c) {
        return;
    }
    if (s && *s) {
        char tmp[256];
        snprintf(tmp, sizeof tmp, "%s", s);
        straight_mod_xor_keystream_creation(c->state, tmp);
    }
}

// Key entry typed
static void
cb_key_basic(void* v, int ok, int val) {
    UiCtx* c = (UiCtx*)v;
    if (!c) {
        return;
    }
    if (ok) {
        unsigned long long vdec = val;
        if (vdec > 255ULL) {
            vdec = 255ULL;
        }
        c->state->K = vdec;
        c->state->keyloader = 0;
        c->state->payload_keyid = c->state->payload_keyidR = 0;
        c->opts->dmr_mute_encL = c->opts->dmr_mute_encR = 0;
    }
}

static void
cb_key_scrambler(void* v, int ok, int val) {
    UiCtx* c = (UiCtx*)v;
    if (!c) {
        return;
    }
    if (ok) {
        unsigned long long vdec = val;
        if (vdec > 0x7FFFULL) {
            vdec = 0x7FFFULL;
        }
        c->state->R = vdec;
        c->state->keyloader = 0;
        c->state->payload_keyid = c->state->payload_keyidR = 0;
        c->opts->dmr_mute_encL = c->opts->dmr_mute_encR = 0;
    }
}

static void
cb_key_rc4des(void* v, const char* text) {
    UiCtx* c = (UiCtx*)v;
    if (!c) {
        return;
    }
    if (text && *text) {
        unsigned long long th = 0ULL;
        if (parse_hex_u64(text, &th)) {
            c->state->R = th;
            c->state->RR = th;
            c->state->keyloader = 0;
            c->state->payload_keyid = c->state->payload_keyidR = 0;
            c->opts->dmr_mute_encL = c->opts->dmr_mute_encR = 0;
        }
    }
}

// Hytera/AES multi-step
typedef struct {
    UiCtx* c;
    int step;
} HyCtx;

static void
cb_hytera_step(void* u, const char* text) {
    HyCtx* hc = (HyCtx*)u;
    if (!hc) {
        return;
    }
    unsigned long long t = 0ULL;
    if (text && *text) {
        if (parse_hex_u64(text, &t)) {
            if (hc->step == 0) {
                hc->c->state->H = t;
                hc->c->state->K1 = t;
            } else if (hc->step == 1) {
                hc->c->state->K2 = t;
            } else if (hc->step == 2) {
                hc->c->state->K3 = t;
            } else if (hc->step == 3) {
                hc->c->state->K4 = t;
            }
        }
    }
    hc->step++;
    if (hc->step == 1) {
        ui_prompt_open_string_async("Hytera Privacy Key 2 (HEX) or 0", NULL, 128, cb_hytera_step, hc);
        return;
    }
    if (hc->step == 2) {
        ui_prompt_open_string_async("Hytera Privacy Key 3 (HEX) or 0", NULL, 128, cb_hytera_step, hc);
        return;
    }
    if (hc->step == 3) {
        ui_prompt_open_string_async("Hytera Privacy Key 4 (HEX) or 0", NULL, 128, cb_hytera_step, hc);
        return;
    }
    hc->c->state->keyloader = 0;
    free(hc);
}

typedef struct {
    UiCtx* c;
    int step;
} AesCtx;

static void
cb_aes_step(void* u, const char* text) {
    AesCtx* ac = (AesCtx*)u;
    if (!ac) {
        return;
    }
    unsigned long long t = 0ULL;
    if (text && *text && parse_hex_u64(text, &t)) {
        if (ac->step == 0) {
            ac->c->state->K1 = t;
        } else if (ac->step == 1) {
            ac->c->state->K2 = t;
        } else if (ac->step == 2) {
            ac->c->state->K3 = t;
        } else if (ac->step == 3) {
            ac->c->state->K4 = t;
        }
    }
    ac->step++;
    if (ac->step == 1) {
        ui_prompt_open_string_async("AES Segment 2 (HEX) or 0", NULL, 128, cb_aes_step, ac);
        return;
    }
    if (ac->step == 2) {
        ui_prompt_open_string_async("AES Segment 3 (HEX) or 0", NULL, 128, cb_aes_step, ac);
        return;
    }
    if (ac->step == 3) {
        ui_prompt_open_string_async("AES Segment 4 (HEX) or 0", NULL, 128, cb_aes_step, ac);
        return;
    }
    ac->c->state->keyloader = 0;
    free(ac);
}

// LRRP custom
static void
cb_lr_custom(void* v, const char* path) {
    UiCtx* c = (UiCtx*)v;
    if (!c) {
        return;
    }
    if (path && *path) {
        if (svc_lrrp_set_custom(c->opts, path) == 0) {
            ui_statusf("LRRP output: %s", c->opts->lrrp_out_file);
        } else {
            ui_statusf("Failed to set LRRP custom output");
        }
    }
}

// P25 Phase 2 params chain
static void
cb_p2_step(void* u, const char* text) {
    P2Ctx* pc = (P2Ctx*)u;
    if (!pc) {
        return;
    }
    unsigned long long t = 0ULL;
    if (text && *text) {
        parse_hex_u64(text, &t);
    }
    if (pc->step == 0) {
        pc->w = t;
    } else if (pc->step == 1) {
        pc->s = t;
    } else if (pc->step == 2) {
        pc->n = t;
    }
    pc->step++;
    char pre[64];
    if (pc->step == 1) {
        snprintf(pre, sizeof pre, "%llX", (unsigned long long)pc->c->state->p2_sysid);
        ui_prompt_open_string_async("Enter Phase 2 SYSID (HEX)", pre, sizeof pre, cb_p2_step, pc);
        return;
    }
    if (pc->step == 2) {
        snprintf(pre, sizeof pre, "%llX", (unsigned long long)pc->c->state->p2_cc);
        ui_prompt_open_string_async("Enter Phase 2 NAC/CC (HEX)", pre, sizeof pre, cb_p2_step, pc);
        return;
    }
    svc_set_p2_params(pc->c->state, pc->w, pc->s, pc->n);
    free(pc);
}

// Save symbol capture
static void
cb_io_save_symbol_capture(void* v, const char* path) {
    UiCtx* c = (UiCtx*)v;
    if (!c) {
        return;
    }
    if (path && *path) {
        if (svc_open_symbol_out(c->opts, c->state, path) == 0) {
            ui_statusf("Symbol capture: %s", c->opts->symbol_out_file);
        } else {
            ui_statusf("Failed to open symbol capture");
        }
    }
}

// Read symbol bin
static void
cb_io_read_symbol_bin(void* v, const char* path) {
    UiCtx* c = (UiCtx*)v;
    if (!c) {
        return;
    }
    if (path && *path) {
        if (svc_open_symbol_in(c->opts, c->state, path) == 0) {
            ui_statusf("Symbol input: %s", path);
        } else {
            ui_statusf("Failed to open: %s", path);
        }
    }
}

// UDP out host->port chain
static void
cb_udp_out_port(void* u, int ok, int port) {
    UdpOutCtx* ctx = (UdpOutCtx*)u;
    if (!ctx) {
        return;
    }
    if (!ok) {
        free(ctx);
        return;
    }
    ctx->port = port;
    if (svc_udp_output_config(ctx->c->opts, ctx->c->state, ctx->host, ctx->port) == 0) {
        ui_statusf("UDP out: %s:%d", ctx->host, ctx->port);
    } else {
        ui_statusf("UDP out failed");
    }
    free(ctx);
}

static void
cb_udp_out_host(void* u, const char* host) {
    UdpOutCtx* ctx = (UdpOutCtx*)u;
    if (!ctx) {
        return;
    }
    if (!host || !*host) {
        free(ctx);
        return;
    }
    snprintf(ctx->host, sizeof ctx->host, "%s", host);
    int port_default = ctx->c->opts->udp_portno > 0 ? ctx->c->opts->udp_portno : 23456;
    ui_prompt_open_int_async("UDP blaster port", port_default, cb_udp_out_port, ctx);
}

// Gain setters
static void
cb_gain_dig(void* u, int ok, double g) {
    UiCtx* c = (UiCtx*)u;
    if (!c) {
        return;
    }
    if (ok) {
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
cb_gain_ana(void* u, int ok, double g) {
    UiCtx* c = (UiCtx*)u;
    if (!c) {
        return;
    }
    if (ok) {
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
cb_input_vol(void* u, int ok, int m) {
    UiCtx* c = (UiCtx*)u;
    if (!c) {
        return;
    }
    if (ok) {
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

// RTL typed callbacks reuse RtlCtx with specific one-offs
static void
cb_rtl_dev(void* u, int ok, int i) {
    UiCtx* c = (UiCtx*)u;
    if (!c) {
        return;
    }
    if (ok) {
        svc_rtl_set_dev_index(c->opts, i);
    }
}

static void
cb_rtl_freq(void* u, int ok, int f) {
    UiCtx* c = (UiCtx*)u;
    if (!c) {
        return;
    }
    if (ok) {
        svc_rtl_set_freq(c->opts, (uint32_t)f);
    }
}

static void
cb_rtl_gain(void* u, int ok, int g) {
    UiCtx* c = (UiCtx*)u;
    if (!c) {
        return;
    }
    if (ok) {
        svc_rtl_set_gain(c->opts, g);
    }
}

static void
cb_rtl_ppm(void* u, int ok, int p) {
    UiCtx* c = (UiCtx*)u;
    if (!c) {
        return;
    }
    if (ok) {
        svc_rtl_set_ppm(c->opts, p);
    }
}

static void
cb_rtl_bw(void* u, int ok, int bw) {
    UiCtx* c = (UiCtx*)u;
    if (!c) {
        return;
    }
    if (ok) {
        svc_rtl_set_bandwidth(c->opts, bw);
    }
}

static void
cb_rtl_sql(void* u, int ok, double dB) {
    UiCtx* c = (UiCtx*)u;
    if (!c) {
        return;
    }
    if (ok) {
        svc_rtl_set_sql_db(c->opts, dB);
    }
}

static void
cb_rtl_vol(void* u, int ok, int m) {
    UiCtx* c = (UiCtx*)u;
    if (!c) {
        return;
    }
    if (ok) {
        svc_rtl_set_volume_mult(c->opts, m);
    }
}

// WAV/SYM
static void
cb_switch_to_wav(void* v, const char* path) {
    UiCtx* c = (UiCtx*)v;
    if (!c) {
        return;
    }
    if (path && *path) {
        snprintf(c->opts->audio_in_dev, sizeof c->opts->audio_in_dev, "%s", path);
        c->opts->audio_in_type = 2;
    }
}

static void
cb_switch_to_symbol(void* v, const char* path) {
    UiCtx* c = (UiCtx*)v;
    if (!c) {
        return;
    }
    if (path && *path) {
        size_t len = strlen(path);
        if (len >= 4 && strcasecmp(path + len - 4, ".bin") == 0) {
            if (svc_open_symbol_in(c->opts, c->state, path) != 0) {
                ui_statusf("Failed to open %s", path);
            }
        } else {
            snprintf(c->opts->audio_in_dev, sizeof c->opts->audio_in_dev, "%s", path);
            c->opts->audio_in_type = 44;
        }
    }
}

// TCP Direct Link chain
static void
cb_tcp_port(void* u, int ok, int port) {
    TcpLinkCtx* ctx = (TcpLinkCtx*)u;
    if (!ctx) {
        return;
    }
    if (!ok) {
        free(ctx);
        return;
    }
    ctx->port = port;
    snprintf(ctx->c->opts->tcp_hostname, sizeof ctx->c->opts->tcp_hostname, "%s", ctx->host);
    ctx->c->opts->tcp_portno = ctx->port;
    if (svc_tcp_connect_audio(ctx->c->opts, ctx->c->opts->tcp_hostname, ctx->c->opts->tcp_portno) == 0) {
        ui_statusf("TCP connected: %s:%d", ctx->c->opts->tcp_hostname, ctx->c->opts->tcp_portno);
    } else {
        ui_statusf("TCP connect failed: %s:%d", ctx->c->opts->tcp_hostname, ctx->c->opts->tcp_portno);
    }
    free(ctx);
}

static void
cb_tcp_host(void* u, const char* host) {
    TcpLinkCtx* ctx = (TcpLinkCtx*)u;
    if (!ctx) {
        return;
    }
    if (!host || !*host) {
        free(ctx);
        return;
    }
    snprintf(ctx->host, sizeof ctx->host, "%s", host);
    int defp = ctx->c->opts->tcp_portno > 0 ? ctx->c->opts->tcp_portno : 7355;
    ui_prompt_open_int_async("Enter TCP Direct Link Port Number", defp, cb_tcp_port, ctx);
}

// UDP input chain
static void
cb_udp_in_port(void* u, int ok, int port) {
    UdpInCtx* ctx = (UdpInCtx*)u;
    if (!ctx) {
        return;
    }
    if (!ok) {
        free(ctx);
        return;
    }
    ctx->port = port;
    snprintf(ctx->c->opts->udp_in_bindaddr, sizeof ctx->c->opts->udp_in_bindaddr, "%s", ctx->addr);
    ctx->c->opts->udp_in_portno = ctx->port;
    snprintf(ctx->c->opts->audio_in_dev, sizeof ctx->c->opts->audio_in_dev, "%s", "udp");
    ctx->c->opts->audio_in_type = 6;
    free(ctx);
}

static void
cb_udp_in_addr(void* u, const char* addr) {
    UdpInCtx* ctx = (UdpInCtx*)u;
    if (!ctx) {
        return;
    }
    if (!addr || !*addr) {
        free(ctx);
        return;
    }
    snprintf(ctx->addr, sizeof ctx->addr, "%s", addr);
    int defp = ctx->c->opts->udp_in_portno > 0 ? ctx->c->opts->udp_in_portno : 7355;
    ui_prompt_open_int_async("Enter UDP bind port", defp, cb_udp_in_port, ctx);
}

// RIGCTL chain
static void
cb_rig_port(void* u, int ok, int port) {
    RigCtx* ctx = (RigCtx*)u;
    if (!ctx) {
        return;
    }
    if (!ok) {
        ctx->c->opts->use_rigctl = 0;
        free(ctx);
        return;
    }
    ctx->port = port;
    snprintf(ctx->c->opts->rigctlhostname, sizeof ctx->c->opts->rigctlhostname, "%s", ctx->host);
    ctx->c->opts->rigctlportno = ctx->port;
    if (svc_rigctl_connect(ctx->c->opts, ctx->c->opts->rigctlhostname, ctx->c->opts->rigctlportno) == 0) {
        ui_statusf("Rigctl connected: %s:%d", ctx->c->opts->rigctlhostname, ctx->c->opts->rigctlportno);
    } else {
        ui_statusf("Rigctl connect failed: %s:%d", ctx->c->opts->rigctlhostname, ctx->c->opts->rigctlportno);
    }
    free(ctx);
}

static void
cb_rig_host(void* u, const char* host) {
    RigCtx* ctx = (RigCtx*)u;
    if (!ctx) {
        return;
    }
    if (!host || !*host) {
        ctx->c->opts->use_rigctl = 0;
        free(ctx);
        return;
    }
    snprintf(ctx->host, sizeof ctx->host, "%s", host);
    int defp = ctx->c->opts->rigctlportno > 0 ? ctx->c->opts->rigctlportno : 4532;
    ui_prompt_open_int_async("Enter RIGCTL Port Number", defp, cb_rig_port, ctx);
}

// Transient Help overlay (press any key)
typedef struct {
    int active;
    const char* text;
    WINDOW* win;
} UiHelp;

static UiHelp g_help = {0};

static void
ui_help_open(const char* help) {
    if (!help || !*help) {
        return;
    }
    g_help.active = 1;
    g_help.text = help;
    if (g_help.win) {
        delwin(g_help.win);
        g_help.win = NULL;
    }
}

// -------------------- Advanced/Env helpers --------------------

// Common helpers to get/set numeric env values with defaults.
static int
env_get_int(const char* name, int defv) {
    const char* v = getenv(name);
    return (v && *v) ? atoi(v) : defv;
}

static double
env_get_double(const char* name, double defv) {
    const char* v = getenv(name);
    return (v && *v) ? atof(v) : defv;
}

static void
env_set_int(const char* name, int v) {
    char buf[64];
    snprintf(buf, sizeof buf, "%d", v);
    setenv(name, buf, 1);
}

static void
env_set_double(const char* name, double v) {
    char buf[64];
    // limit precision just for display sanity
    snprintf(buf, sizeof buf, "%.6g", v);
    setenv(name, buf, 1);
}

// After changing env-backed runtime config, re-parse to apply immediately.
static void
env_reparse_runtime_cfg(dsd_opts* opts) {
    dsd_neo_config_init(opts);
}

// FTZ/DAZ toggle (SSE)
static void
act_toggle_ftz_daz(void* v) {
    UiCtx* c = (UiCtx*)v;
    (void)c;
#if defined(__SSE__) || defined(__SSE2__)
    int on = 0;
    const char* e = getenv("DSD_NEO_FTZ_DAZ");
    on = (e && *e && *e != '0' && *e != 'f' && *e != 'F' && *e != 'n' && *e != 'N');
    on = on ? 0 : 1; // flip
    setenv("DSD_NEO_FTZ_DAZ", on ? "1" : "0", 1);
    unsigned int mxcsr = _mm_getcsr();
    if (on) {
        mxcsr |= (1u << 15) | (1u << 6);
    } else {
        mxcsr &= ~((1u << 15) | (1u << 6));
    }
    _mm_setcsr(mxcsr);
#else
    // no-op on non-SSE builds
#endif
}

static const char*
lbl_ftz_daz(void* v, char* b, size_t n) {
    (void)v;
#if defined(__SSE__) || defined(__SSE2__)
    const char* e = getenv("DSD_NEO_FTZ_DAZ");
    int on = (e && *e && *e != '0' && *e != 'f' && *e != 'F' && *e != 'n' && *e != 'N');
    snprintf(b, n, "SSE FTZ/DAZ: %s", on ? "On" : "Off");
    return b;
#else
    snprintf(b, n, "SSE FTZ/DAZ: Unavailable");
    return b;
#endif
}

// Low input-level warning threshold (dBFS)
static void
cb_input_warn(void* v, int ok, double thr) {
    UiCtx* c = (UiCtx*)v;
    if (!c) {
        return;
    }
    if (!ok) {
        return;
    }
    if (thr < -200.0) {
        thr = -200.0;
    }
    if (thr > 0.0) {
        thr = 0.0;
    }
    c->opts->input_warn_db = thr;
    env_set_double("DSD_NEO_INPUT_WARN_DB", thr);
}

static const char*
lbl_input_warn(void* v, char* b, size_t n) {
    UiCtx* c = (UiCtx*)v;
    double thr = c ? c->opts->input_warn_db : env_get_double("DSD_NEO_INPUT_WARN_DB", -40.0);
    snprintf(b, n, "Low Input Warning: %.1f dBFS", thr);
    return b;
}

static void
act_set_input_warn(void* v) {
    UiCtx* c = (UiCtx*)v;
    double thr = c ? c->opts->input_warn_db : env_get_double("DSD_NEO_INPUT_WARN_DB", -40.0);
    ui_prompt_open_double_async("Low input warning threshold (dBFS)", thr, cb_input_warn, c);
}

// P25 adaptive follower numeric settings (env-driven at runtime)
typedef struct {
    UiCtx* c;
    const char* name;
} P25NumCtx;

static void
cb_set_p25_num(void* u, int ok, double val) {
    P25NumCtx* pc = (P25NumCtx*)u;
    if (!pc) {
        return;
    }
    if (ok) {
        env_set_double(pc->name, val);
        // sync select mirrored opts fields when present
        if (pc->c && pc->c->opts) {
            if (strcmp(pc->name, "DSD_NEO_P25_VC_GRACE") == 0) {
                pc->c->opts->p25_vc_grace_s = val;
            } else if (strcmp(pc->name, "DSD_NEO_P25_MIN_FOLLOW_DWELL") == 0) {
                pc->c->opts->p25_min_follow_dwell_s = val;
            } else if (strcmp(pc->name, "DSD_NEO_P25_GRANT_VOICE_TO") == 0) {
                pc->c->opts->p25_grant_voice_to_s = val;
            } else if (strcmp(pc->name, "DSD_NEO_P25_RETUNE_BACKOFF") == 0) {
                pc->c->opts->p25_retune_backoff_s = val;
            } else if (strcmp(pc->name, "DSD_NEO_P25_FORCE_RELEASE_EXTRA") == 0) {
                pc->c->opts->p25_force_release_extra_s = val;
            } else if (strcmp(pc->name, "DSD_NEO_P25_FORCE_RELEASE_MARGIN") == 0) {
                pc->c->opts->p25_force_release_margin_s = val;
            } else if (strcmp(pc->name, "DSD_NEO_P25P1_ERR_HOLD_PCT") == 0) {
                pc->c->opts->p25_p1_err_hold_pct = val;
            } else if (strcmp(pc->name, "DSD_NEO_P25P1_ERR_HOLD_S") == 0) {
                pc->c->opts->p25_p1_err_hold_s = val;
            }
        }
    }
    free(pc);
}

static void
act_prompt_p25_num(void* v, const char* env_name, const char* title, double defv) {
    UiCtx* c = (UiCtx*)v;
    P25NumCtx* pc = (P25NumCtx*)calloc(1, sizeof(P25NumCtx));
    if (!pc) {
        return;
    }
    pc->c = c;
    pc->name = env_name;
    ui_prompt_open_double_async(title, defv, cb_set_p25_num, pc);
}

static const char*
lbl_p25_num(void* v, char* b, size_t n, const char* env_name, const char* fmt, double defv) {
    (void)v;
    double val = env_get_double(env_name, defv);
    snprintf(b, n, fmt, val);
    return b;
}

static void
act_set_p25_vc_grace(void* v) {
    act_prompt_p25_num(v, "DSD_NEO_P25_VC_GRACE", "P25: VC grace seconds", env_get_double("DSD_NEO_P25_VC_GRACE", 0));
}

static const char*
lbl_p25_vc_grace(void* v, char* b, size_t n) {
    return lbl_p25_num(v, b, n, "DSD_NEO_P25_VC_GRACE", "P25: VC grace (s): %.3f", 0.0);
}

static void
act_set_p25_min_follow(void* v) {
    act_prompt_p25_num(v, "DSD_NEO_P25_MIN_FOLLOW_DWELL", "P25: Min follow dwell (s)",
                       env_get_double("DSD_NEO_P25_MIN_FOLLOW_DWELL", 0));
}

static const char*
lbl_p25_min_follow(void* v, char* b, size_t n) {
    return lbl_p25_num(v, b, n, "DSD_NEO_P25_MIN_FOLLOW_DWELL", "P25: Min follow dwell (s): %.3f", 0.0);
}

static void
act_set_p25_grant_voice(void* v) {
    act_prompt_p25_num(v, "DSD_NEO_P25_GRANT_VOICE_TO", "P25: Grant->Voice timeout (s)",
                       env_get_double("DSD_NEO_P25_GRANT_VOICE_TO", 0));
}

static const char*
lbl_p25_grant_voice(void* v, char* b, size_t n) {
    return lbl_p25_num(v, b, n, "DSD_NEO_P25_GRANT_VOICE_TO", "P25: Grant->Voice timeout (s): %.3f", 0.0);
}

static void
act_set_p25_retune_backoff(void* v) {
    act_prompt_p25_num(v, "DSD_NEO_P25_RETUNE_BACKOFF", "P25: Retune backoff (s)",
                       env_get_double("DSD_NEO_P25_RETUNE_BACKOFF", 0));
}

static const char*
lbl_p25_retune_backoff(void* v, char* b, size_t n) {
    return lbl_p25_num(v, b, n, "DSD_NEO_P25_RETUNE_BACKOFF", "P25: Retune backoff (s): %.3f", 0.0);
}

static void
act_set_p25_cc_grace(void* v) {
    act_prompt_p25_num(v, "DSD_NEO_P25_CC_GRACE", "P25: CC hunt grace (s)", env_get_double("DSD_NEO_P25_CC_GRACE", 0));
}

static const char*
lbl_p25_cc_grace(void* v, char* b, size_t n) {
    return lbl_p25_num(v, b, n, "DSD_NEO_P25_CC_GRACE", "P25: CC hunt grace (s): %.3f", 0.0);
}

static void
act_set_p25_force_extra(void* v) {
    act_prompt_p25_num(v, "DSD_NEO_P25_FORCE_RELEASE_EXTRA", "P25: Safety-net extra (s)",
                       env_get_double("DSD_NEO_P25_FORCE_RELEASE_EXTRA", 0));
}

static const char*
lbl_p25_force_extra(void* v, char* b, size_t n) {
    return lbl_p25_num(v, b, n, "DSD_NEO_P25_FORCE_RELEASE_EXTRA", "P25: Force release extra (s): %.3f", 0.0);
}

static void
act_set_p25_force_margin(void* v) {
    act_prompt_p25_num(v, "DSD_NEO_P25_FORCE_RELEASE_MARGIN", "P25: Safety-net margin (s)",
                       env_get_double("DSD_NEO_P25_FORCE_RELEASE_MARGIN", 0));
}

static const char*
lbl_p25_force_margin(void* v, char* b, size_t n) {
    return lbl_p25_num(v, b, n, "DSD_NEO_P25_FORCE_RELEASE_MARGIN", "P25: Force release margin (s): %.3f", 0.0);
}

static void
act_set_p25_p1_err_pct(void* v) {
    act_prompt_p25_num(v, "DSD_NEO_P25P1_ERR_HOLD_PCT", "P25p1: Error-hold percent",
                       env_get_double("DSD_NEO_P25P1_ERR_HOLD_PCT", 0));
}

static const char*
lbl_p25_p1_err_pct(void* v, char* b, size_t n) {
    return lbl_p25_num(v, b, n, "DSD_NEO_P25P1_ERR_HOLD_PCT", "P25p1: Err-hold pct: %.1f%%", 0.0);
}

static void
act_set_p25_p1_err_sec(void* v) {
    act_prompt_p25_num(v, "DSD_NEO_P25P1_ERR_HOLD_S", "P25p1: Error-hold seconds",
                       env_get_double("DSD_NEO_P25P1_ERR_HOLD_S", 0));
}

static const char*
lbl_p25_p1_err_sec(void* v, char* b, size_t n) {
    return lbl_p25_num(v, b, n, "DSD_NEO_P25P1_ERR_HOLD_S", "P25p1: Err-hold sec: %.3f", 0.0);
}

// C4FM clock assist control is already provided by RTL stream layer

// Deemphasis and audio LPF (config-backed)
static const char*
lbl_deemph(void* v, char* b, size_t n) {
    (void)v;
    const dsdneoRuntimeConfig* cfg = dsd_neo_get_config();
    const char* s = "Unset";
    if (cfg) {
        switch (cfg->deemph_mode) {
            case DSD_NEO_DEEMPH_OFF: s = "Off"; break;
            case DSD_NEO_DEEMPH_50: s = "50"; break;
            case DSD_NEO_DEEMPH_75: s = "75"; break;
            case DSD_NEO_DEEMPH_NFM: s = "NFM"; break;
            default: s = "Unset"; break;
        }
    }
    snprintf(b, n, "Deemphasis: %s", s);
    return b;
}

static void
act_deemph_cycle(void* v) {
    UiCtx* c = (UiCtx*)v;
    const dsdneoRuntimeConfig* cfg = dsd_neo_get_config();
    int mode = cfg ? cfg->deemph_mode : DSD_NEO_DEEMPH_UNSET;
    mode = (mode + 1) % 5; // cycle through UNSET->OFF->50->75->NFM->UNSET
    switch (mode) {
        case DSD_NEO_DEEMPH_UNSET: setenv("DSD_NEO_DEEMPH", "", 1); break;
        case DSD_NEO_DEEMPH_OFF: setenv("DSD_NEO_DEEMPH", "off", 1); break;
        case DSD_NEO_DEEMPH_50: setenv("DSD_NEO_DEEMPH", "50", 1); break;
        case DSD_NEO_DEEMPH_75: setenv("DSD_NEO_DEEMPH", "75", 1); break;
        case DSD_NEO_DEEMPH_NFM: setenv("DSD_NEO_DEEMPH", "nfm", 1); break;
        default: break;
    }
    env_reparse_runtime_cfg(c ? c->opts : NULL);
}

static const char*
lbl_audio_lpf(void* v, char* b, size_t n) {
    (void)v;
    const dsdneoRuntimeConfig* cfg = dsd_neo_get_config();
    if (cfg && cfg->audio_lpf_is_set && !cfg->audio_lpf_disable && cfg->audio_lpf_cutoff_hz > 0) {
        snprintf(b, n, "Audio LPF: %d Hz", cfg->audio_lpf_cutoff_hz);
    } else {
        snprintf(b, n, "Audio LPF: Off");
    }
    return b;
}

static void
cb_audio_lpf(void* v, int ok, int hz) {
    UiCtx* c = (UiCtx*)v;
    if (!c || !ok) {
        return;
    }
    if (hz <= 0) {
        setenv("DSD_NEO_AUDIO_LPF", "off", 1);
    } else {
        env_set_int("DSD_NEO_AUDIO_LPF", hz);
    }
    env_reparse_runtime_cfg(c->opts);
}

static void
act_set_audio_lpf(void* v) {
    const dsdneoRuntimeConfig* cfg = dsd_neo_get_config();
    int def = (cfg && cfg->audio_lpf_is_set && !cfg->audio_lpf_disable) ? cfg->audio_lpf_cutoff_hz : 0;
    ui_prompt_open_int_async("Audio LPF cutoff Hz (0=off)", def, cb_audio_lpf, v);
}

static const char*
lbl_window_freeze(void* v, char* b, size_t n) {
    (void)v;
    const dsdneoRuntimeConfig* cfg = dsd_neo_get_config();
    int on = (cfg && cfg->window_freeze_is_set) ? cfg->window_freeze : 0;
    snprintf(b, n, "Freeze Symbol Window: %s", on ? "On" : "Off");
    return b;
}

static void
act_window_freeze_toggle(void* v) {
    UiCtx* c = (UiCtx*)v;
    const dsdneoRuntimeConfig* cfg = dsd_neo_get_config();
    int on = (cfg && cfg->window_freeze_is_set) ? cfg->window_freeze : 0;
    setenv("DSD_NEO_WINDOW_FREEZE", on ? "0" : "1", 1);
    env_reparse_runtime_cfg(c ? c->opts : NULL);
}

// RTL/TCP networking/Auto-PPM
static const char*
lbl_auto_ppm_snr(void* v, char* b, size_t n) {
    (void)v;
    double d = env_get_double("DSD_NEO_AUTO_PPM_SNR_DB", 18.0);
    snprintf(b, n, "Auto-PPM SNR threshold: %.1f dB", d);
    return b;
}

static void
cb_auto_ppm_snr(void* v, int ok, double d) {
    (void)v;
    if (!ok) {
        return;
    }
    env_set_double("DSD_NEO_AUTO_PPM_SNR_DB", d);
}

static const char*
lbl_auto_ppm_pwr(void* v, char* b, size_t n) {
    (void)v;
    double d = env_get_double("DSD_NEO_AUTO_PPM_PWR_DB", -10.0);
    snprintf(b, n, "Auto-PPM Min power: %.1f dB", d);
    return b;
}

static void
cb_auto_ppm_pwr(void* v, int ok, double d) {
    (void)v;
    if (ok) {
        env_set_double("DSD_NEO_AUTO_PPM_PWR_DB", d);
    }
}

static const char*
lbl_auto_ppm_zeroppm(void* v, char* b, size_t n) {
    (void)v;
    int p = env_get_int("DSD_NEO_AUTO_PPM_ZEROLOCK_PPM", 3);
    snprintf(b, n, "Auto-PPM Zero-lock PPM: %d", p);
    return b;
}

static void
cb_auto_ppm_zeroppm(void* v, int ok, int p) {
    (void)v;
    if (ok) {
        env_set_int("DSD_NEO_AUTO_PPM_ZEROLOCK_PPM", p);
    }
}

static const char*
lbl_auto_ppm_zerohz(void* v, char* b, size_t n) {
    (void)v;
    int h = env_get_int("DSD_NEO_AUTO_PPM_ZEROLOCK_HZ", 1500);
    snprintf(b, n, "Auto-PPM Zero-lock Hz: %d", h);
    return b;
}

static void
cb_auto_ppm_zerohz(void* v, int ok, int h) {
    (void)v;
    if (ok) {
        env_set_int("DSD_NEO_AUTO_PPM_ZEROLOCK_HZ", h);
    }
}

static const char*
lbl_auto_ppm_freeze(void* v, char* b, size_t n) {
    (void)v;
    const char* e = getenv("DSD_NEO_AUTO_PPM_FREEZE");
    int on = (e && *e && *e != '0');
    snprintf(b, n, "Auto-PPM Freeze: %s", on ? "On" : "Off");
    return b;
}

static void
act_auto_ppm_freeze(void* v) {
    (void)v;
    const char* e = getenv("DSD_NEO_AUTO_PPM_FREEZE");
    int on = (e && *e && *e != '0');
    setenv("DSD_NEO_AUTO_PPM_FREEZE", on ? "0" : "1", 1);
}

static const char*
lbl_tcp_prebuf(void* v, char* b, size_t n) {
    (void)v;
    int ms = env_get_int("DSD_NEO_TCP_PREBUF_MS", 30);
    snprintf(b, n, "RTL-TCP Prebuffer: %d ms", ms);
    return b;
}

static void
cb_tcp_prebuf(void* v, int ok, int ms) {
    UiCtx* c = (UiCtx*)v;
    if (!ok) {
        return;
    }
    env_set_int("DSD_NEO_TCP_PREBUF_MS", ms);
    if (c && c->opts && c->opts->audio_in_type == 3) {
        (void)svc_rtl_restart(c->opts);
    }
}

static const char*
lbl_tcp_rcvbuf(void* v, char* b, size_t n) {
    (void)v;
    int sz = env_get_int("DSD_NEO_TCP_RCVBUF", 0);
    if (sz > 0) {
        snprintf(b, n, "RTL-TCP SO_RCVBUF: %d bytes", sz);
    } else {
        snprintf(b, n, "RTL-TCP SO_RCVBUF: system default");
    }
    return b;
}

static void
cb_tcp_rcvbuf(void* v, int ok, int sz) {
    UiCtx* c = (UiCtx*)v;
    if (!ok) {
        return;
    }
    if (sz <= 0) {
        setenv("DSD_NEO_TCP_RCVBUF", "", 1);
    } else {
        env_set_int("DSD_NEO_TCP_RCVBUF", sz);
    }
    if (c && c->opts && c->opts->audio_in_type == 3) {
        (void)svc_rtl_restart(c->opts);
    }
}

static const char*
lbl_tcp_rcvtimeo(void* v, char* b, size_t n) {
    (void)v;
    int ms = env_get_int("DSD_NEO_TCP_RCVTIMEO", 0);
    if (ms > 0) {
        snprintf(b, n, "RTL-TCP SO_RCVTIMEO: %d ms", ms);
    } else {
        snprintf(b, n, "RTL-TCP SO_RCVTIMEO: off");
    }
    return b;
}

static void
cb_tcp_rcvtimeo(void* v, int ok, int ms) {
    UiCtx* c = (UiCtx*)v;
    if (!ok) {
        return;
    }
    if (ms <= 0) {
        setenv("DSD_NEO_TCP_RCVTIMEO", "", 1);
    } else {
        env_set_int("DSD_NEO_TCP_RCVTIMEO", ms);
    }
    if (c && c->opts && c->opts->audio_in_type == 3) {
        (void)svc_rtl_restart(c->opts);
    }
}

static const char*
lbl_tcp_waitall(void* v, char* b, size_t n) {
    (void)v;
    const char* e = getenv("DSD_NEO_TCP_WAITALL");
    int on = (e && *e && *e != '0');
    snprintf(b, n, "RTL-TCP MSG_WAITALL: %s", on ? "On" : "Off");
    return b;
}

static void
act_tcp_waitall(void* v) {
    UiCtx* c = (UiCtx*)v;
    const char* e = getenv("DSD_NEO_TCP_WAITALL");
    int on = (e && *e && *e != '0');
    setenv("DSD_NEO_TCP_WAITALL", on ? "0" : "1", 1);
    if (c && c->opts && c->opts->audio_in_type == 3) {
        (void)svc_rtl_restart(c->opts);
    }
}

// Runtime scheduling and threads
static const char*
lbl_rt_sched(void* v, char* b, size_t n) {
    (void)v;
    const char* e = getenv("DSD_NEO_RT_SCHED");
    int on = (e && *e && *e != '0');
    snprintf(b, n, "Realtime Scheduling: %s", on ? "On" : "Off");
    return b;
}

static void
act_rt_sched(void* v) {
    (void)v;
    const char* e = getenv("DSD_NEO_RT_SCHED");
    int on = (e && *e && *e != '0');
    setenv("DSD_NEO_RT_SCHED", on ? "0" : "1", 1);
}

static const char*
lbl_mt(void* v, char* b, size_t n) {
    (void)v;
    const dsdneoRuntimeConfig* cfg = dsd_neo_get_config();
    int on = (cfg && cfg->mt_is_set) ? cfg->mt_enable : 0;
    snprintf(b, n, "Intra-block MT: %s", on ? "On" : "Off");
    return b;
}

static void
act_mt(void* v) {
    UiCtx* c = (UiCtx*)v;
    const dsdneoRuntimeConfig* cfg = dsd_neo_get_config();
    int on = (cfg && cfg->mt_is_set) ? cfg->mt_enable : 0;
    setenv("DSD_NEO_MT", on ? "0" : "1", 1);
    env_reparse_runtime_cfg(c ? c->opts : NULL);
}

// Generic editor for DSD_NEO_* env var
typedef struct {
    UiCtx* c;
    char name[64];
} EnvEditCtx;

static void
cb_env_edit_value(void* u, const char* val) {
    EnvEditCtx* ec = (EnvEditCtx*)u;
    if (!ec) {
        return;
    }
    if (val && *val) {
        setenv(ec->name, val, 1);
        // Apply to runtime config as appropriate
        env_reparse_runtime_cfg(ec->c ? ec->c->opts : NULL);
    }
    free(ec);
}

static void
cb_env_edit_name(void* u, const char* name) {
    EnvEditCtx* ec = (EnvEditCtx*)u;
    if (!ec) {
        return;
    }
    if (!name || !*name) {
        free(ec);
        return;
    }
    // Require DSD_NEO_ prefix for safety
    if (strncasecmp(name, "DSD_NEO_", 8) != 0) {
        free(ec);
        return;
    }
    snprintf(ec->name, sizeof ec->name, "%s", name);
    const char* cur = getenv(ec->name);
    ui_prompt_open_string_async("Enter value (empty to clear)", cur ? cur : "", 256, cb_env_edit_value, ec);
}

static void
act_env_editor(void* v) {
    EnvEditCtx* ec = (EnvEditCtx*)calloc(1, sizeof(EnvEditCtx));
    if (!ec) {
        return;
    }
    ec->c = (UiCtx*)v;
    ui_prompt_open_string_async("Enter DSD_NEO_* variable name", "DSD_NEO_", 128, cb_env_edit_name, ec);
}

static void
ui_help_close(void) {
    if (g_help.win) {
        delwin(g_help.win);
        g_help.win = NULL;
    }
    memset(&g_help, 0, sizeof(g_help));
}

static void
ui_chooser_start(const char* title, const char* const* items, int count, void (*on_done)(void*, int), void* user) {
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

static void
ui_chooser_close(void) {
    if (g_chooser.win) {
        delwin(g_chooser.win);
        g_chooser.win = NULL;
    }
    memset(&g_chooser, 0, sizeof(g_chooser));
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

static int
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

static void
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
    width += 2; // borders
    int height = vis + 6;
    if (height < 8) {
        height = 8;
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
        if (height < 7) {
            height = 7;
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

static void
ui_overlay_ensure_window(UiMenuFrame* f) {
    if (!f) {
        return;
    }
    if (!f->win) {
        f->win = ui_make_window(f->h, f->w, f->y, f->x);
        keypad(f->win, TRUE);
        wtimeout(f->win, 0);
    }
}

// If the desired geometry for the current frame has changed since the window
// was created, drop and recreate it on the next ensure pass. This allows the
// nonblocking menu to grow/shrink immediately when item visibility flips.
static void
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

// Forward declare a helper that exposes main menu items for async open
void ui_menu_get_main_items(const NcMenuItem** out_items, size_t* out_n, UiCtx* ctx);

void
ui_menu_open_async(dsd_opts* opts, dsd_state* state) {
    // Initialize overlay context and push root menu
    g_ctx_overlay.opts = opts;
    g_ctx_overlay.state = state;
    const NcMenuItem* items = NULL;
    size_t n = 0;
    ui_menu_get_main_items(&items, &n, &g_ctx_overlay);
    if (!items || n == 0) {
        return;
    }
    g_overlay_open = 1;
    g_depth = 1;
    memset(g_stack, 0, sizeof(g_stack));
    g_stack[0].items = items;
    g_stack[0].n = n;
    g_stack[0].hi = 0;
    ui_overlay_layout(&g_stack[0], &g_ctx_overlay);
}

int
ui_menu_is_open(void) {
    return g_overlay_open;
}

static int
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

int
ui_menu_handle_key(int ch, dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    if (!g_overlay_open || g_depth <= 0) {
        return 0;
    }
    // Prompt has highest priority
    if (g_prompt.active) {
        if (ch == KEY_RESIZE) {
            if (g_prompt.win) {
                delwin(g_prompt.win);
                g_prompt.win = NULL;
            }
            return 1;
        }
        if (ch == ERR) {
            return 1;
        }
        if (ch == DSD_KEY_ESC || ch == 'q' || ch == 'Q') {
            if (g_prompt.on_done_str) {
                void (*cb)(void*, const char*) = g_prompt.on_done_str;
                void* up = g_prompt.user;
                g_prompt.on_done_str = NULL; // prevent close_all() from calling again
                cb(up, NULL);
            }
            ui_prompt_close_all();
            return 1;
        }
        if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
            if (g_prompt.len > 0) {
                g_prompt.buf[--g_prompt.len] = '\0';
            }
            return 1;
        }
        if (ch == 10 || ch == KEY_ENTER || ch == '\r') {
            if (g_prompt.on_done_str) {
                void (*cb)(void*, const char*) = g_prompt.on_done_str;
                void* up = g_prompt.user;
                g_prompt.on_done_str = NULL; // prevent close_all() from calling again
                if (g_prompt.len == 0) {
                    cb(up, NULL);
                } else {
                    cb(up, g_prompt.buf);
                }
            }
            ui_prompt_close_all();
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
    // Help has next priority
    if (g_help.active) {
        if (ch != ERR) {
            ui_help_close();
        }
        return 1;
    }
    // Help has highest priority
    if (g_help.active) {
        if (ch != ERR) {
            ui_help_close();
        }
        return 1;
    }
    // Chooser has priority when active
    if (g_chooser.active) {
        if (ch == ERR) {
            return 1;
        }
        if (ch == KEY_RESIZE) {
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
            ui_chooser_close();
            return 1;
        }
        if (ch == 10 || ch == KEY_ENTER || ch == '\r') {
            void (*cb)(void*, int) = g_chooser.on_done;
            void* userp = g_chooser.user;
            int sel = g_chooser.sel;
            ui_chooser_close();
            if (cb) {
                cb(userp, sel);
            }
            return 1;
        }
        return 1;
    }
    UiMenuFrame* f = &g_stack[g_depth - 1];
    if (!f->items || f->n == 0) {
        ui_overlay_close_all();
        return 1;
    }
    if (ch == KEY_RESIZE) {
        // Recompute layout and recreate window on next tick
        if (f->win) {
            delwin(f->win);
            f->win = NULL;
        }
        ui_overlay_layout(f, &g_ctx_overlay);
        return 1;
    }
    if (ch == ERR) {
        return 0;
    }
    if (ch == KEY_UP) {
        f->hi = ui_next_enabled(f->items, f->n, &g_ctx_overlay, f->hi, -1);
        return 1;
    }
    if (ch == KEY_DOWN) {
        f->hi = ui_next_enabled(f->items, f->n, &g_ctx_overlay, f->hi, +1);
        return 1;
    }
    if (ch == 'h' || ch == 'H') {
        const NcMenuItem* it = &f->items[f->hi];
        if (ui_is_enabled(it, &g_ctx_overlay) && it->help && *it->help) {
            ui_help_open(it->help);
        }
        return 1;
    }
    if (ch == DSD_KEY_ESC || ch == 'q' || ch == 'Q') {
        // Pop submenu or close root
        if (g_depth > 1) {
            UiMenuFrame* cur = &g_stack[g_depth - 1];
            if (cur->win) {
                delwin(cur->win);
                cur->win = NULL;
            }
            g_depth--;
        } else {
            ui_overlay_close_all();
        }
        return 1;
    }
    if (ch == 10 || ch == KEY_ENTER || ch == '\r') {
        const NcMenuItem* it = &f->items[f->hi];
        if (!ui_is_enabled(it, &g_ctx_overlay)) {
            return 1;
        }
        if (it->submenu && it->submenu_len > 0) {
            if (g_depth < (int)(sizeof g_stack / sizeof g_stack[0])) {
                UiMenuFrame* nf = &g_stack[g_depth++];
                memset(nf, 0, sizeof(*nf));
                nf->items = it->submenu;
                nf->n = it->submenu_len;
                nf->hi = 0;
                ui_overlay_layout(nf, &g_ctx_overlay);
            }
        }
        if (it->on_select) {
            it->on_select(&g_ctx_overlay);
            if (exitflag) {
                // Let caller exit soon
                ui_overlay_close_all();
                return 1;
            }
            // After a toggle or action, visible items may have changed.
            // Ensure the highlight points at a visible item and recompute size.
            UiMenuFrame* cf = &g_stack[g_depth - 1];
            if (!ui_is_enabled(&cf->items[cf->hi], &g_ctx_overlay)) {
                cf->hi = ui_next_enabled(cf->items, cf->n, &g_ctx_overlay, cf->hi, +1);
            }
            ui_overlay_layout(cf, &g_ctx_overlay);
            ui_overlay_recreate_if_needed(cf);
        }
        if (!it->on_select && (!it->submenu || it->submenu_len == 0) && it->help && *it->help) {
            ui_help_open(it->help);
        }
        return 1;
    }
    return 0;
}

void
ui_menu_tick(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    if (!g_overlay_open || g_depth <= 0) {
        return;
    }
    // Render Prompt overlay (highest priority)
    if (g_prompt.active) {
        const char* title = g_prompt.title ? g_prompt.title : "Input";
        int h = 8;
        int w = (int)strlen(title) + 16;
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
        if (!g_prompt.win) {
            g_prompt.win = ui_make_window(h, w, py, px);
            wtimeout(g_prompt.win, 0);
        }
        WINDOW* win = g_prompt.win;
        werase(win);
        box(win, 0, 0);
        mvwprintw(win, 1, 2, "%s", title);
        mvwprintw(win, 3, 2, "> %s", g_prompt.buf ? g_prompt.buf : "");
        mvwprintw(win, h - 2, 2, "Enter=OK  Esc/q=Cancel");
        wrefresh(win);
        return;
    }
    // Render Help overlay if active
    if (g_help.active) {
        const char* t = g_help.text ? g_help.text : "";
        int h = 8;
        int w = (int)strlen(t) + 6;
        if (w < 40) {
            w = 40;
        }
        int scr_h = 0, scr_w = 0;
        getmaxyx(stdscr, scr_h, scr_w);
        if (w > scr_w - 2) {
            w = scr_w - 2;
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
            wtimeout(g_help.win, 0);
        }
        WINDOW* hw = g_help.win;
        werase(hw);
        box(hw, 0, 0);
        mvwprintw(hw, 1, 2, "Help:");
        mvwprintw(hw, 3, 2, "%s", t);
        mvwprintw(hw, h - 2, 2, "Press any key to continue...");
        wrefresh(hw);
        return;
    }
    // Render chooser if active
    if (g_chooser.active) {
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
        if (!g_chooser.win) {
            g_chooser.win = ui_make_window(h, w, wy, wx);
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
        wrefresh(win);
        return;
    }
    UiMenuFrame* f = &g_stack[g_depth - 1];
    // Ensure window exists with up-to-date geometry
    ui_overlay_layout(f, &g_ctx_overlay);
    ui_overlay_recreate_if_needed(f);
    ui_overlay_ensure_window(f);
    ui_draw_menu(f->win, f->items, f->n, f->hi, &g_ctx_overlay);
}

// ---- IO submenu ----

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
    ui_prompt_open_string_async("Enter Symbol Capture Filename", NULL, 1024, cb_io_save_symbol_capture, c);
}

static void
io_read_symbol_bin(void* vctx) {
    UiCtx* c = (UiCtx*)vctx;
    ui_prompt_open_string_async("Enter Symbol Capture Filename", NULL, 1024, cb_io_read_symbol_bin, c);
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

static void
io_set_pulse_out(void* vctx) {
    UiCtx* c = (UiCtx*)vctx;
    pa_devicelist_t outs[16];
    pa_devicelist_t ins[16];
    if (pa_get_devicelist(ins, outs) < 0) {
        ui_statusf("Failed to get Pulse device list");
        return;
    }
    int n = 0;
    // Allocate dynamic arrays to persist until chooser completes
    const char** labels = (const char**)calloc(16, sizeof(char*));
    const char** names = (const char**)calloc(16, sizeof(char*));
    char** bufs = (char**)calloc(16, sizeof(char*));
    if (!labels || !names || !bufs) {
        free(labels);
        free(names);
        free(bufs);
        ui_statusf("Out of memory");
        return;
    }
    for (int i = 0; i < 16; i++) {
        if (!outs[i].initialized) {
            break;
        }
        bufs[n] = (char*)calloc(768, sizeof(char));
        if (!bufs[n]) {
            continue;
        }
        int name_len = (int)strnlen(outs[i].name, 511);
        int desc_len = (int)strnlen(outs[i].description, 255);
        snprintf(bufs[n], 768, "[%d] %.*s %s %.*s", outs[i].index, name_len, outs[i].name,
                 dsd_unicode_or_ascii("—", "-"), desc_len, outs[i].description);
        labels[n] = bufs[n];
        names[n] = strdup(outs[i].name);
        n++;
    }
    if (n == 0) {
        free(labels);
        free(names);
        free(bufs);
        ui_statusf("No Pulse outputs found");
        return;
    }

    typedef struct {
        UiCtx* c;
        const char** labels;
        const char** names;
        char** bufs;
        int n;
    } PulseSelCtx;

    PulseSelCtx* pctx = (PulseSelCtx*)calloc(1, sizeof(PulseSelCtx));
    if (!pctx) {
        for (int i = 0; i < n; i++) {
            free((void*)names[i]);
            free(bufs[i]);
        }
        free(labels);
        free(names);
        free(bufs);
        ui_statusf("Out of memory");
        return;
    }
    pctx->c = c;
    pctx->labels = labels;
    pctx->names = names;
    pctx->bufs = bufs;
    pctx->n = n;
    ui_chooser_start("Select Pulse Output", labels, n, chooser_done_pulse_out, pctx);
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
    int n = 0;
    const char** labels = (const char**)calloc(16, sizeof(char*));
    const char** names = (const char**)calloc(16, sizeof(char*));
    char** bufs = (char**)calloc(16, sizeof(char*));
    if (!labels || !names || !bufs) {
        free(labels);
        free(names);
        free(bufs);
        ui_statusf("Out of memory");
        return;
    }
    for (int i = 0; i < 16; i++) {
        if (!ins[i].initialized) {
            break;
        }
        bufs[n] = (char*)calloc(768, sizeof(char));
        if (!bufs[n]) {
            continue;
        }
        int name_len2 = (int)strnlen(ins[i].name, 511);
        int desc_len2 = (int)strnlen(ins[i].description, 255);
        snprintf(bufs[n], 768, "[%d] %.*s %s %.*s", ins[i].index, name_len2, ins[i].name,
                 dsd_unicode_or_ascii("—", "-"), desc_len2, ins[i].description);
        labels[n] = bufs[n];
        names[n] = strdup(ins[i].name);
        n++;
    }
    if (n == 0) {
        free(labels);
        free(names);
        free(bufs);
        ui_statusf("No Pulse inputs found");
        return;
    }

    typedef struct {
        UiCtx* c;
        const char** labels;
        const char** names;
        char** bufs;
        int n;
    } PulseInSelCtx;

    PulseInSelCtx* pctx = (PulseInSelCtx*)calloc(1, sizeof(PulseInSelCtx));
    if (!pctx) {
        for (int i = 0; i < n; i++) {
            free((void*)names[i]);
            free(bufs[i]);
        }
        free(labels);
        free(names);
        free(bufs);
        ui_statusf("Out of memory");
        return;
    }
    pctx->c = c;
    pctx->labels = labels;
    pctx->names = names;
    pctx->bufs = bufs;
    pctx->n = n;
    ui_chooser_start("Select Pulse Input", labels, n, chooser_done_pulse_in, pctx);
}

static void
chooser_free_lists(const char** names, char** bufs, int n, const char** labels) {
    for (int i = 0; i < n; i++) {
        if (names) {
            free((void*)names[i]);
        }
        if (bufs) {
            free(bufs[i]);
        }
    }
    if (labels) {
        free((void*)labels);
    }
    if (names) {
        free((void*)names);
    }
    if (bufs) {
        free((void*)bufs);
    }
}

static void
chooser_done_pulse_out(void* u, int sel) {
    typedef struct {
        UiCtx* c;
        const char** labels;
        const char** names;
        char** bufs;
        int n;
    } PulseSelCtx;

    PulseSelCtx* pc = (PulseSelCtx*)u;
    if (pc) {
        if (sel >= 0 && sel < pc->n) {
            svc_set_pulse_output(pc->c->opts, pc->names[sel]);
            ui_statusf("Pulse out: %s", pc->names[sel]);
        }
        chooser_free_lists(pc->names, pc->bufs, pc->n, pc->labels);
        free(pc);
    }
}

static void
chooser_done_pulse_in(void* u, int sel) {
    typedef struct {
        UiCtx* c;
        const char** labels;
        const char** names;
        char** bufs;
        int n;
    } PulseInSelCtx;

    PulseInSelCtx* pc = (PulseInSelCtx*)u;
    if (pc) {
        if (sel >= 0 && sel < pc->n) {
            svc_set_pulse_input(pc->c->opts, pc->names[sel]);
            ui_statusf("Pulse in: %s", pc->names[sel]);
        }
        chooser_free_lists(pc->names, pc->bufs, pc->n, pc->labels);
        free(pc);
    }
}

static void
io_set_udp_out(void* vctx) {
    UiCtx* c = (UiCtx*)vctx;
    UdpOutCtx* u = (UdpOutCtx*)calloc(1, sizeof(UdpOutCtx));
    if (!u) {
        return;
    }
    u->c = c;
    const char* src = c->opts->udp_hostname[0] ? c->opts->udp_hostname : "127.0.0.1";
    snprintf(u->host, sizeof u->host, "%.*s", (int)sizeof(u->host) - 1, src);
    ui_prompt_open_string_async("UDP blaster host", u->host, sizeof u->host, cb_udp_out_host, u);
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
            size_t prefix = strlen("Current Output: Pulse []") - 2; /* exclude %s */
            int m = (n > prefix) ? (int)(n - prefix) : 0;
            snprintf(b, n, "Current Output: Pulse [%.*s]", m, c->opts->pa_output_idx);
        } else {
            snprintf(b, n, "Current Output: Pulse [default]");
        }
    } else if (c->opts->audio_out_type == 8) {
        int m = (n > 32) ? (int)(n - 32) : 0; /* leave room for prefix and port */
        snprintf(b, n, "Current Output: UDP %.*s:%d", m, c->opts->udp_hostname, c->opts->udp_portno);
    } else if (c->opts->audio_out_type == 2 || c->opts->audio_out_type == 5) {
        int m = (n > 24) ? (int)(n - 24) : 0; /* room for suffix */
        snprintf(b, n, "Current Output: %.*s (%s)", m, c->opts->audio_out_dev, name);
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
    ui_prompt_open_double_async("Digital output gain (0=auto; 1..50)", c->opts->audio_gain, cb_gain_dig, c);
}

static void
io_set_gain_ana(void* vctx) {
    UiCtx* c = (UiCtx*)vctx;
    ui_prompt_open_double_async("Analog output gain (0..100)", c->opts->audio_gainA, cb_gain_ana, c);
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
    ui_prompt_open_int_async("Input Volume Multiplier (1..16)", m, cb_input_vol, c);
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
    ui_prompt_open_int_async("Device index", c->opts->rtl_dev_index, cb_rtl_dev, c);
}

static void
rtl_set_freq(void* v) {
    UiCtx* c = (UiCtx*)v;
    ui_prompt_open_int_async("Frequency (Hz)", (int)c->opts->rtlsdr_center_freq, cb_rtl_freq, c);
}

static void
rtl_set_gain(void* v) {
    UiCtx* c = (UiCtx*)v;
    ui_prompt_open_int_async("Gain (0=AGC, 0..49)", c->opts->rtl_gain_value, cb_rtl_gain, c);
}

static void
rtl_set_ppm(void* v) {
    UiCtx* c = (UiCtx*)v;
    ui_prompt_open_int_async("PPM error (-200..200)", c->opts->rtlsdr_ppm_error, cb_rtl_ppm, c);
}

static void
rtl_set_bw(void* v) {
    UiCtx* c = (UiCtx*)v;
    ui_prompt_open_int_async("Bandwidth kHz (4,6,8,12,16,24)", c->opts->rtl_bandwidth, cb_rtl_bw, c);
}

static void
rtl_set_sql(void* v) {
    UiCtx* c = (UiCtx*)v;
    ui_prompt_open_double_async("Squelch (dB, negative)", pwr_to_dB(c->opts->rtl_squelch_level), cb_rtl_sql, c);
}

static void
rtl_set_vol(void* v) {
    UiCtx* c = (UiCtx*)v;
    ui_prompt_open_int_async("Volume multiplier (0..3)", c->opts->rtl_volume_multiplier, cb_rtl_vol, c);
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

// RTL-SDR grouped submenus
static const NcMenuItem RTL_CTL_ITEMS[] = {
    {.id = "enable", .label = "Enable RTL-SDR Input", .help = "Switch input to RTL-SDR.", .on_select = rtl_enable},
    {.id = "restart",
     .label = "Restart RTL Stream",
     .help = "Apply config by restarting the stream.",
     .on_select = rtl_restart},
    {.id = "dev", .label = "Set Device Index...", .help = "Select RTL device index.", .on_select = rtl_set_dev},
};

static const NcMenuItem RTL_RF_ITEMS[] = {
    {.id = "freq", .label = "Set Frequency (Hz)...", .help = "Set center frequency in Hz.", .on_select = rtl_set_freq},
    {.id = "gain", .label = "Set Gain...", .help = "0=AGC; else driver gain units.", .on_select = rtl_set_gain},
    {.id = "ppm", .label = "Set PPM error...", .help = "-200..200.", .on_select = rtl_set_ppm},
    {.id = "bw", .label = "Set Bandwidth (kHz)...", .help = "4,6,8,12,16,24.", .on_select = rtl_set_bw},
    {.id = "sql", .label = "Set Squelch (dB)...", .help = "More negative -> tighter.", .on_select = rtl_set_sql},
    {.id = "vol", .label = "Set Volume Multiplier...", .help = "0..3 sample scaler.", .on_select = rtl_set_vol},
};

static const NcMenuItem RTL_CAL_ITEMS[] = {
    {.id = "auto_ppm",
     .label = "Auto-PPM (Spectrum)",
     .label_fn = lbl_rtl_auto_ppm,
     .help = "Enable/disable spectrum-based auto PPM tracking.",
     .on_select = rtl_toggle_auto_ppm},
    {.id = "tuner_autogain",
     .label = "Tuner Autogain",
     .label_fn = lbl_rtl_tuner_autogain,
     .help = "Enable/disable supervisory tuner autogain.",
     .on_select = rtl_toggle_tuner_autogain},
    {.id = "bias",
     .label = "Toggle Bias Tee",
     .label_fn = lbl_rtl_bias,
     .help = "Enable/disable 5V bias tee (USB or rtl_tcp).",
     .on_select = rtl_toggle_bias},
    {.id = "rtltcp_autotune",
     .label = "RTL-TCP Adaptive Networking",
     .label_fn = lbl_rtl_rtltcp_autotune,
     .help = "Enable/disable adaptive buffering for rtl_tcp.",
     .on_select = rtl_toggle_rtltcp_autotune},
};

static const NcMenuItem RTL_MENU_ITEMS[] = {
    {.id = "ctl",
     .label = "Control...",
     .help = "Stream control and device select.",
     .submenu = RTL_CTL_ITEMS,
     .submenu_len = sizeof RTL_CTL_ITEMS / sizeof RTL_CTL_ITEMS[0]},
    {.id = "rf",
     .label = "RF & IF Tuning...",
     .help = "RF center/gain, BW, squelch, volume.",
     .submenu = RTL_RF_ITEMS,
     .submenu_len = sizeof RTL_RF_ITEMS / sizeof RTL_RF_ITEMS[0]},
    {.id = "cal",
     .label = "Calibration & Helpers...",
     .help = "Auto-PPM, autogain, bias tee, RTL-TCP.",
     .submenu = RTL_CAL_ITEMS,
     .submenu_len = sizeof RTL_CAL_ITEMS / sizeof RTL_CAL_ITEMS[0]},
};
#endif

static void
io_tcp_direct_link(void* vctx) {
    UiCtx* c = (UiCtx*)vctx;
    TcpLinkCtx* u = (TcpLinkCtx*)calloc(1, sizeof(TcpLinkCtx));
    if (!u) {
        return;
    }
    u->c = c;
    const char* defh = c->opts->tcp_hostname[0] ? c->opts->tcp_hostname : "localhost";
    snprintf(u->host, sizeof u->host, "%.*s", (int)sizeof(u->host) - 1, defh);
    ui_prompt_open_string_async("Enter TCP Direct Link Hostname", u->host, sizeof u->host, cb_tcp_host, u);
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
        int m = (n > 32) ? (int)(n - 32) : 0;
        snprintf(b, n, "Current Input: TCP %.*s:%d", m, c->opts->tcp_hostname, c->opts->tcp_portno);
    } else if (c->opts->audio_in_type == 6) {
        const char* addr = c->opts->udp_in_bindaddr[0] ? c->opts->udp_in_bindaddr : "127.0.0.1";
        int m = (n > 32) ? (int)(n - 32) : 0;
        snprintf(b, n, "Current Input: UDP %.*s:%d", m, addr, c->opts->udp_in_portno);
    } else if (c->opts->audio_in_type == 2 || c->opts->audio_in_type == 4 || c->opts->audio_in_type == 44) {
        int m = (n > 18) ? (int)(n - 18) : 0;
        snprintf(b, n, "Current Input: %.*s", m, c->opts->audio_in_dev);
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
    ui_prompt_open_string_async("Enter WAV/RAW filename (or named pipe)", NULL, 1024, cb_switch_to_wav, c);
}

static void
switch_to_symbol(void* vctx) {
    UiCtx* c = (UiCtx*)vctx;
    ui_prompt_open_string_async("Enter symbol .bin/.raw/.sym filename", NULL, 1024, cb_switch_to_symbol, c);
}

static void
switch_to_tcp(void* vctx) {
    io_tcp_direct_link(vctx);
}

static void
switch_to_udp(void* vctx) {
    UiCtx* c = (UiCtx*)vctx;
    UdpInCtx* u = (UdpInCtx*)calloc(1, sizeof(UdpInCtx));
    if (!u) {
        return;
    }
    u->c = c;
    const char* defa = c->opts->udp_in_bindaddr[0] ? c->opts->udp_in_bindaddr : "127.0.0.1";
    snprintf(u->addr, sizeof u->addr, "%.*s", (int)sizeof(u->addr) - 1, defa);
    ui_prompt_open_string_async("Enter UDP bind address", u->addr, sizeof u->addr, cb_udp_in_addr, u);
}

static void
io_rigctl_config(void* vctx) {
    UiCtx* c = (UiCtx*)vctx;
    RigCtx* u = (RigCtx*)calloc(1, sizeof(RigCtx));
    if (!u) {
        return;
    }
    u->c = c;
    const char* defh = c->opts->rigctlhostname[0] ? c->opts->rigctlhostname : "localhost";
    snprintf(u->host, sizeof u->host, "%.*s", (int)sizeof(u->host) - 1, defh);
    ui_prompt_open_string_async("Enter RIGCTL Hostname", u->host, sizeof u->host, cb_rig_host, u);
}

// ---- Dynamic labels for IO ----
static const char*
lbl_sym_save(void* vctx, char* b, size_t n) {
    UiCtx* c = (UiCtx*)vctx;
    if (c->opts->symbol_out_f) {
        size_t prefix = strlen("Save Symbols to File [Active: ]") - 2; /* exclude %s */
        int m = (n > prefix) ? (int)(n - prefix) : 0;
        snprintf(b, n, "Save Symbols to File [Active: %.*s]", m, c->opts->symbol_out_file);
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
        int m = (n > 32) ? (int)(n - 32) : 0;
        if (active) {
            snprintf(b, n, "TCP Direct Audio: %.*s:%d [Active]", m, c->opts->tcp_hostname, c->opts->tcp_portno);
        } else {
            snprintf(b, n, "TCP Direct Audio: %.*s:%d [Inactive]", m, c->opts->tcp_hostname, c->opts->tcp_portno);
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
        int m = (n > 24) ? (int)(n - 24) : 0;
        if (connected) {
            snprintf(b, n, "Rigctl: %.*s:%d [Active]", m, c->opts->rigctlhostname, c->opts->rigctlportno);
        } else {
            snprintf(b, n, "Rigctl: %.*s:%d [Inactive]", m, c->opts->rigctlhostname, c->opts->rigctlportno);
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
    (void)c;
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
lbl_p25_enc_lockout(void* v, char* b, size_t n) {
    UiCtx* c = (UiCtx*)v;
    int on = (c && c->opts) ? ((c->opts->trunk_tune_enc_calls == 0) ? 1 : 0) : 0;
    snprintf(b, n, "P25 Encrypted Call Lockout [%s]", on ? "On" : "Off");
    return b;
}

static void
act_p25_enc_lockout(void* v) {
    UiCtx* c = (UiCtx*)v;
    if (!c || !c->opts) {
        return;
    }
    c->opts->trunk_tune_enc_calls = c->opts->trunk_tune_enc_calls ? 0 : 1;
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

// Logging & Capture submenu

// Trunking & Control submenu

// Keys & Security submenu

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

#endif /* end of USE_RTLSDR block started at 2881 (DSP labels/actions) */

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

/* Nonblocking Auto-DSP config submenu */
static const NcMenuItem AUTO_DSP_CFG_ITEMS[] = {
    {.id = "p1_win",
     .label = "P25P1 Window (status)",
     .label_fn = lbl_p1_win,
     .help = "Min symbols per decision window."},
    {.id = "p1_win+", .label = "P25P1 Window +50", .help = "Increase window.", .on_select = inc_p1_win},
    {.id = "p1_win-", .label = "P25P1 Window -50", .help = "Decrease window.", .on_select = dec_p1_win},
    {.id = "p1_mon", .label = "P25P1 Moderate On%", .label_fn = lbl_p1_mod_on, .help = "Engage moderate threshold."},
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
    {.id = "ema", .label = "EMA alpha (status)", .label_fn = lbl_ema_alpha, .help = "Smoothing constant for P25P1."},
    {.id = "ema+", .label = "EMA alpha +512", .on_select = inc_alpha},
    {.id = "ema-", .label = "EMA alpha -512", .on_select = dec_alpha},
};
#endif

#ifdef USE_RTLSDR
#endif

/*
void ui_menu_dsp_options(dsd_opts* opts, dsd_state* state) {
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
void ui_menu_dsp_options(dsd_opts* opts, dsd_state* state) { UNUSED(opts); UNUSED(state); }
#endif
*/

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

// Key Entry actions (declarative)
static void
key_basic(void* v) {
    UiCtx* c = (UiCtx*)v;
    c->state->payload_keyid = c->state->payload_keyidR = 0;
    c->opts->dmr_mute_encL = c->opts->dmr_mute_encR = 0;
    ui_prompt_open_int_async("Basic Privacy Key Number (DEC)", 0, cb_key_basic, c);
}

static void
key_hytera(void* v) {
    UiCtx* c = (UiCtx*)v;
    c->state->payload_keyid = c->state->payload_keyidR = 0;
    c->opts->dmr_mute_encL = c->opts->dmr_mute_encR = 0;
    c->state->K1 = c->state->K2 = c->state->K3 = c->state->K4 = 0ULL;
    c->state->H = 0ULL;
    HyCtx* hc = (HyCtx*)calloc(1, sizeof(HyCtx));
    if (!hc) {
        return;
    }
    hc->c = c;
    hc->step = 0;
    ui_prompt_open_string_async("Hytera Privacy Key 1 (HEX)", NULL, 128, cb_hytera_step, hc);
}

static void
key_scrambler(void* v) {
    UiCtx* c = (UiCtx*)v;
    c->state->payload_keyid = c->state->payload_keyidR = 0;
    c->opts->dmr_mute_encL = c->opts->dmr_mute_encR = 0;
    ui_prompt_open_int_async("NXDN/dPMR Scrambler Key (DEC)", 0, cb_key_scrambler, c);
}

static void
key_force_bp(void* v) {
    UiCtx* c = (UiCtx*)v;
    c->state->M = (c->state->M == 1 || c->state->M == 0x21) ? 0 : 1;
}

static void
key_rc4des(void* v) {
    UiCtx* c = (UiCtx*)v;
    c->state->payload_keyid = c->state->payload_keyidR = 0;
    c->opts->dmr_mute_encL = c->opts->dmr_mute_encR = 0;
    ui_prompt_open_string_async("RC4/DES Key (HEX)", NULL, 128, cb_key_rc4des, c);
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
    AesCtx* ac = (AesCtx*)calloc(1, sizeof(AesCtx));
    if (!ac) {
        return;
    }
    ac->c = c;
    ac->step = 0;
    ui_prompt_open_string_async("AES Segment 1 (HEX) or 0", NULL, 128, cb_aes_step, ac);
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
    ui_prompt_open_string_async("Enter LRRP output filename", NULL, 1024, cb_lr_custom, c);
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

// ---- Main Menu ----

// action wrappers

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
    ui_prompt_open_string_async("Event log filename", c->opts->event_out_file, 1024, cb_event_log_set, c);
}

static void
act_event_log_disable(void* v) {
    svc_disable_event_log(((UiCtx*)v)->opts);
}

static void
act_static_wav(void* v) {
    UiCtx* c = (UiCtx*)v;
    ui_prompt_open_string_async("Static WAV filename", c->opts->wav_out_file, 1024, cb_static_wav, c);
}

static void
act_raw_wav(void* v) {
    UiCtx* c = (UiCtx*)v;
    ui_prompt_open_string_async("Raw WAV filename", c->opts->wav_out_file_raw, 1024, cb_raw_wav, c);
}

static void
act_dsp_out(void* v) {
    UiCtx* c = (UiCtx*)v;
    ui_prompt_open_string_async("DSP output base filename", c->opts->dsp_out_file, 256, cb_dsp_out, c);
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
    ui_prompt_open_int_async("Setmod BW (Hz)", c->opts->setmod_bw, cb_setmod_bw, c);
}

static void
act_import_chan(void* v) {
    UiCtx* c = (UiCtx*)v;
    ui_prompt_open_string_async("Channel map CSV", NULL, 1024, cb_import_chan, c);
}

static void
act_import_group(void* v) {
    UiCtx* c = (UiCtx*)v;
    ui_prompt_open_string_async("Group list CSV", NULL, 1024, cb_import_group, c);
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
    ui_prompt_open_int_async("TG Hold", (int)c->state->tg_hold, cb_tg_hold, c);
}

static void
act_hangtime(void* v) {
    UiCtx* c = (UiCtx*)v;
    ui_prompt_open_double_async("Hangtime seconds", c->opts->trunk_hangtime, cb_hangtime, c);
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
    ui_prompt_open_int_async("Slot 1 or 2", c->opts->slot_preference + 1, cb_slot_pref, c);
}

static void
act_slots_on(void* v) {
    UiCtx* c = (UiCtx*)v;
    int m = (c->opts->slot1_on ? 1 : 0) | (c->opts->slot2_on ? 2 : 0);
    ui_prompt_open_int_async("Slots mask (0..3)", m, cb_slots_on, c);
}

static void
act_keys_dec(void* v) {
    UiCtx* c = (UiCtx*)v;
    ui_prompt_open_string_async("Keys CSV (DEC)", NULL, 1024, cb_keys_dec, c);
}

static void
act_keys_hex(void* v) {
    UiCtx* c = (UiCtx*)v;
    ui_prompt_open_string_async("Keys CSV (HEX)", NULL, 1024, cb_keys_hex, c);
}

static void
act_tyt_ap(void* v) {
    UiCtx* c = (UiCtx*)v;
    ui_prompt_open_string_async("TYT AP string", NULL, 256, cb_tyt_ap, c);
}

static void
act_retevis_rc2(void* v) {
    UiCtx* c = (UiCtx*)v;
    ui_prompt_open_string_async("Retevis AP string", NULL, 256, cb_retevis_rc2, c);
}

static void
act_tyt_ep(void* v) {
    UiCtx* c = (UiCtx*)v;
    ui_prompt_open_string_async("TYT EP string", NULL, 256, cb_tyt_ep, c);
}

static void
act_ken_scr(void* v) {
    UiCtx* c = (UiCtx*)v;
    ui_prompt_open_string_async("Kenwood scrambler", NULL, 256, cb_ken_scr, c);
}

static void
act_anytone_bp(void* v) {
    UiCtx* c = (UiCtx*)v;
    ui_prompt_open_string_async("Anytone BP", NULL, 256, cb_anytone_bp, c);
}

static void
act_xor_ks(void* v) {
    UiCtx* c = (UiCtx*)v;
    ui_prompt_open_string_async("XOR keystream", NULL, 256, cb_xor_ks, c);
}
#ifdef USE_RTLSDR
#endif

// M17 encoder user data (CAN/DST/SRC)
typedef struct {
    UiCtx* c;
} M17Ctx;

static const char*
lbl_m17_user_data(void* v, char* b, size_t n) {
    UiCtx* c = (UiCtx*)v;
    const char* s = (c && c->state && c->state->m17dat[0]) ? c->state->m17dat : "<unset>";
    int m = (int)n - 18;
    if (m < 0) {
        m = 0;
    }
    snprintf(b, n, "M17 Encoder User Data: %.*s", m, s);
    return b;
}

static void
cb_m17_user_data(void* u, const char* text) {
    M17Ctx* mc = (M17Ctx*)u;
    if (mc && mc->c && mc->c->state && text) {
        strncpy(mc->c->state->m17dat, text, 49);
        mc->c->state->m17dat[49] = '\0';
    }
    free(mc);
}

static void
act_m17_user_data(void* v) {
    UiCtx* c = (UiCtx*)v;
    const char* pre = (c && c->state && c->state->m17dat[0]) ? c->state->m17dat : "";
    M17Ctx* mc = (M17Ctx*)calloc(1, sizeof(M17Ctx));
    if (!mc) {
        return;
    }
    mc->c = c;
    ui_prompt_open_string_async("Enter M17 User Data (CAN,DST,SRC)", pre, 128, cb_m17_user_data, mc);
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
act_p2_params(void* v) {
    UiCtx* c = (UiCtx*)v;
    P2Ctx* pc = (P2Ctx*)calloc(1, sizeof(P2Ctx));
    if (!pc) {
        return;
    }
    pc->c = c;
    pc->step = 0;
    pc->w = pc->s = pc->n = 0ULL;
    char pre[64];
    snprintf(pre, sizeof pre, "%llX", (unsigned long long)c->state->p2_wacn);
    ui_prompt_open_string_async("Enter Phase 2 WACN (HEX)", pre, sizeof pre, cb_p2_step, pc);
}

static void
act_exit(void* v) {
    (void)v;
    exitflag = 1;
}

// ---- Declarative submenu tables for nonblocking overlay ----
// Devices & IO
#ifdef USE_RTLSDR
extern const NcMenuItem RTL_MENU_ITEMS[]; // defined above in this file
#endif
static const NcMenuItem IO_SWITCH_INPUT_ITEMS[] = {
    {.id = "current", .label = "Current", .label_fn = lbl_current_input, .help = "Shows current input."},
    {.id = "pulse", .label = "Pulse Audio (mic/line)", .help = "Use Pulse Audio input.", .on_select = switch_to_pulse},
#ifdef USE_RTLSDR
    {.id = "rtl", .label = "RTL-SDR", .help = "Switch to RTL-SDR input.", .on_select = switch_to_rtl},
#endif
    {.id = "tcp", .label = "TCP Direct Audio...", .help = "Connect to PCM16LE over TCP.", .on_select = switch_to_tcp},
    {.id = "wav", .label = "WAV/File...", .help = "Open WAV/RAW file or named pipe.", .on_select = switch_to_wav},
    {.id = "sym",
     .label = "Symbol Capture (.bin/.raw/.sym)...",
     .help = "Replay captured symbols.",
     .on_select = switch_to_symbol},
    {.id = "udp", .label = "UDP Signal Input...", .help = "Bind UDP PCM16LE input.", .on_select = switch_to_udp},
};

static const NcMenuItem IO_SWITCH_OUTPUT_ITEMS[] = {
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

// IO grouped submenus
static const NcMenuItem IO_INPUT_ITEMS[] = {
    {.id = "switch_input",
     .label = "Switch Input...",
     .help = "Change active input source.",
     .submenu = IO_SWITCH_INPUT_ITEMS,
     .submenu_len = sizeof IO_SWITCH_INPUT_ITEMS / sizeof IO_SWITCH_INPUT_ITEMS[0]},
#ifdef USE_RTLSDR
    {.id = "rtl",
     .label = "RTL-SDR...",
     .help = "Configure RTL device, gain, PPM, BW, SQL.",
     .is_enabled = io_rtl_active,
     .submenu = RTL_MENU_ITEMS,
     .submenu_len = sizeof RTL_MENU_ITEMS / sizeof RTL_MENU_ITEMS[0]},
#endif
    {.id = "pulse_in",
     .label = "Set Pulse Input...",
     .help = "Set Pulse input by index/name.",
     .is_enabled = io_always_on,
     .on_select = io_set_pulse_in},
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
};

static const NcMenuItem IO_OUTPUT_ITEMS2[] = {
    {.id = "switch_output",
     .label = "Switch Output...",
     .help = "Change audio output sink.",
     .submenu = IO_SWITCH_OUTPUT_ITEMS,
     .submenu_len = sizeof IO_SWITCH_OUTPUT_ITEMS / sizeof IO_SWITCH_OUTPUT_ITEMS[0]},
    {.id = "pulse_out",
     .label = "Set Pulse Output...",
     .help = "Set Pulse output by index/name.",
     .is_enabled = io_always_on,
     .on_select = io_set_pulse_out},
    {.id = "udp_out",
     .label = "Configure UDP Output...",
     .help = "Set UDP blaster host/port and enable.",
     .on_select = io_set_udp_out},
};

static const NcMenuItem IO_LEVELS_ITEMS[] = {
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
    {.id = "input_warn",
     .label = "Low Input Warning (dBFS)",
     .label_fn = lbl_input_warn,
     .help = "Warn if input magnitude below threshold.",
     .on_select = act_set_input_warn},
};

static const NcMenuItem IO_INV_ITEMS[] = {
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
};

static const NcMenuItem IO_FILTER_ITEMS[] = {
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

static const NcMenuItem IO_MENU_ITEMS[] = {
    {.id = "inputs",
     .label = "Inputs...",
     .help = "Select and configure inputs.",
     .submenu = IO_INPUT_ITEMS,
     .submenu_len = sizeof IO_INPUT_ITEMS / sizeof IO_INPUT_ITEMS[0]},
    {.id = "outputs",
     .label = "Outputs...",
     .help = "Audio sinks and UDP output.",
     .submenu = IO_OUTPUT_ITEMS2,
     .submenu_len = sizeof IO_OUTPUT_ITEMS2 / sizeof IO_OUTPUT_ITEMS2[0]},
    {.id = "levels",
     .label = "Levels & Monitor...",
     .help = "Gains, input volume, monitor.",
     .submenu = IO_LEVELS_ITEMS,
     .submenu_len = sizeof IO_LEVELS_ITEMS / sizeof IO_LEVELS_ITEMS[0]},
    {.id = "invert",
     .label = "Inversion...",
     .help = "Per‑protocol inversion toggles.",
     .submenu = IO_INV_ITEMS,
     .submenu_len = sizeof IO_INV_ITEMS / sizeof IO_INV_ITEMS[0]},
    {.id = "filters",
     .label = "Filters...",
     .help = "Cosine and fixed/probed RRC presets.",
     .submenu = IO_FILTER_ITEMS,
     .submenu_len = sizeof IO_FILTER_ITEMS / sizeof IO_FILTER_ITEMS[0]},
};

// Logging & Capture
static const NcMenuItem LOGGING_CAPTURE_ITEMS[] = {
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
};

static const NcMenuItem LOGGING_LOG_ITEMS[] = {
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

static const NcMenuItem LOGGING_MENU_ITEMS[] = {
    {.id = "capture",
     .label = "Capture...",
     .help = "Symbol/audio capture and structured output.",
     .submenu = LOGGING_CAPTURE_ITEMS,
     .submenu_len = sizeof LOGGING_CAPTURE_ITEMS / sizeof LOGGING_CAPTURE_ITEMS[0]},
    {.id = "logging",
     .label = "Logging...",
     .help = "Event/payload logging and housekeeping.",
     .submenu = LOGGING_LOG_ITEMS,
     .submenu_len = sizeof LOGGING_LOG_ITEMS / sizeof LOGGING_LOG_ITEMS[0]},
};

// Trunking & Control
static const NcMenuItem TRUNK_MODES_ITEMS[] = {
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
};

static const NcMenuItem TRUNK_P25_ITEMS[] = {
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
    {.id = "p25_enc",
     .label = "P25 Encrypted Call Lockout",
     .label_fn = lbl_p25_enc_lockout,
     .help = "Do not tune encrypted calls when On.",
     .on_select = act_p25_enc_lockout},
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
};

static const NcMenuItem TRUNK_RIG_ITEMS[] = {
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
};

static const NcMenuItem TRUNK_LISTS_ITEMS[] = {
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
};

static const NcMenuItem TRUNK_TDMA_ITEMS[] = {
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

static const NcMenuItem TRUNK_MENU_ITEMS[] = {
    {.id = "modes",
     .label = "Modes...",
     .help = "Enable trunking or conventional scanning.",
     .submenu = TRUNK_MODES_ITEMS,
     .submenu_len = sizeof TRUNK_MODES_ITEMS / sizeof TRUNK_MODES_ITEMS[0]},
    {.id = "p25",
     .label = "P25 Options...",
     .help = "Control-channel hunting and follower behavior.",
     .submenu = TRUNK_P25_ITEMS,
     .submenu_len = sizeof TRUNK_P25_ITEMS / sizeof TRUNK_P25_ITEMS[0]},
    {.id = "rig",
     .label = "Rig Control...",
     .help = "External rig control settings.",
     .submenu = TRUNK_RIG_ITEMS,
     .submenu_len = sizeof TRUNK_RIG_ITEMS / sizeof TRUNK_RIG_ITEMS[0]},
    {.id = "lists",
     .label = "Lists & Filters...",
     .help = "Channel maps, groups, and tuning filters.",
     .submenu = TRUNK_LISTS_ITEMS,
     .submenu_len = sizeof TRUNK_LISTS_ITEMS / sizeof TRUNK_LISTS_ITEMS[0]},
    {.id = "tdma",
     .label = "DMR/TDMA...",
     .help = "TDMA slot controls and DMR late entry.",
     .submenu = TRUNK_TDMA_ITEMS,
     .submenu_len = sizeof TRUNK_TDMA_ITEMS / sizeof TRUNK_TDMA_ITEMS[0]},
};

// Keys & Security
static const NcMenuItem KEYS_ENTRY_ITEMS[] = {
    {.id = "basic", .label = "Basic Privacy (DEC)", .help = "Set 0..255 basic privacy key.", .on_select = key_basic},
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

static const NcMenuItem KEYS_MANAGE_ITEMS[] = {
    {.id = "keys",
     .label = "Manage Encryption Keys...",
     .help = "Enter or edit BP/Hytera/RC4/AES keys.",
     .submenu = KEYS_ENTRY_ITEMS,
     .submenu_len = sizeof KEYS_ENTRY_ITEMS / sizeof KEYS_ENTRY_ITEMS[0]},
    {.id = "muting",
     .label = "Toggle Encrypted Audio Muting",
     .label_fn = lbl_muting,
     .help = "Toggle P25 and DMR encrypted audio muting.",
     .is_enabled = io_always_on,
     .on_select = io_toggle_mute_enc},
};

static const NcMenuItem KEYS_IMPORT_ITEMS[] = {
    {.id = "keys_dec",
     .label = "Import Keys CSV (DEC)...",
     .help = "Import decimal keys CSV.",
     .on_select = act_keys_dec},
    {.id = "keys_hex",
     .label = "Import Keys CSV (HEX)...",
     .help = "Import hexadecimal keys CSV.",
     .on_select = act_keys_hex},
};

static const NcMenuItem KEYS_KS_ITEMS[] = {
    {.id = "tyt_ap", .label = "TYT AP (PC4) Keystream...", .help = "Enter AP seed string.", .on_select = act_tyt_ap},
    {.id = "retevis_rc2",
     .label = "Retevis AP (RC2) Keystream...",
     .help = "Enter AP seed string.",
     .on_select = act_retevis_rc2},
    {.id = "tyt_ep", .label = "TYT EP (AES) Keystream...", .help = "Enter EP seed string.", .on_select = act_tyt_ep},
    {.id = "ken_scr", .label = "Kenwood DMR Scrambler...", .help = "Enter scrambler seed.", .on_select = act_ken_scr},
    {.id = "anytone_bp", .label = "Anytone BP Keystream...", .help = "Enter BP seed.", .on_select = act_anytone_bp},
    {.id = "xor_ks", .label = "Straight XOR Keystream...", .help = "Enter raw string to XOR.", .on_select = act_xor_ks},
};

static const NcMenuItem KEYS_M17_ITEMS[] = {
    {.id = "m17_ud",
     .label = "M17 Encoder User Data...",
     .label_fn = lbl_m17_user_data,
     .help = "Set M17 encoder CAN/DST/SRC user data.",
     .on_select = act_m17_user_data},
};

static const NcMenuItem KEYS_MENU_ITEMS[] = {
    {.id = "manage",
     .label = "Manage...",
     .help = "Enter/edit keys and priorities.",
     .submenu = KEYS_MANAGE_ITEMS,
     .submenu_len = sizeof KEYS_MANAGE_ITEMS / sizeof KEYS_MANAGE_ITEMS[0]},
    {.id = "import",
     .label = "Import...",
     .help = "Import key CSV files.",
     .submenu = KEYS_IMPORT_ITEMS,
     .submenu_len = sizeof KEYS_IMPORT_ITEMS / sizeof KEYS_IMPORT_ITEMS[0]},
    {.id = "ks",
     .label = "Keystreams...",
     .help = "Radio/vendor-specific derivations.",
     .submenu = KEYS_KS_ITEMS,
     .submenu_len = sizeof KEYS_KS_ITEMS / sizeof KEYS_KS_ITEMS[0]},
    {.id = "m17",
     .label = "M17 Encoder...",
     .help = "Set M17 encoder IDs/user data.",
     .submenu = KEYS_M17_ITEMS,
     .submenu_len = sizeof KEYS_M17_ITEMS / sizeof KEYS_M17_ITEMS[0]},
};

// UI Display
static const NcMenuItem UI_DISPLAY_P25_ITEMS[] = {
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
};

static const NcMenuItem UI_DISPLAY_GENERAL_ITEMS[] = {
    {.id = "chans",
     .label_fn = lbl_ui_channels,
     .help = "Toggle Channels section.",
     .on_select = act_toggle_ui_channels},
};

static const NcMenuItem UI_DISPLAY_MENU_ITEMS[] = {
    {.id = "p25",
     .label = "P25 Sections...",
     .help = "Toggle P25-related on-screen sections.",
     .submenu = UI_DISPLAY_P25_ITEMS,
     .submenu_len = sizeof UI_DISPLAY_P25_ITEMS / sizeof UI_DISPLAY_P25_ITEMS[0]},
    {.id = "general",
     .label = "General...",
     .help = "Other UI sections.",
     .submenu = UI_DISPLAY_GENERAL_ITEMS,
     .submenu_len = sizeof UI_DISPLAY_GENERAL_ITEMS / sizeof UI_DISPLAY_GENERAL_ITEMS[0]},
};

// LRRP
static const NcMenuItem LRRP_STATUS_ITEMS[] = {
    {.id = "current",
     .label = "Current Output",
     .label_fn = lbl_lrrp_current,
     .help = "Shows the active LRRP output target.",
     .is_enabled = io_always_on},
};

static const NcMenuItem LRRP_DEST_ITEMS[] = {
    {.id = "home",
     .label = "Write to ~/lrrp.txt (QGIS)",
     .help = "Standard QGIS-friendly output.",
     .on_select = lr_home},
    {.id = "dsdp", .label = "Write to ./DSDPlus.LRRP (LRRP.exe)", .help = "DSDPlus LRRP format.", .on_select = lr_dsdp},
    {.id = "custom", .label = "Custom Filename...", .help = "Choose a custom path.", .on_select = lr_custom},
    {.id = "disable", .label = "Disable/Stop", .help = "Disable LRRP output.", .on_select = lr_off},
};

static const NcMenuItem LRRP_MENU_ITEMS[] = {
    {.id = "status",
     .label = "Status...",
     .help = "Shows current output target.",
     .submenu = LRRP_STATUS_ITEMS,
     .submenu_len = sizeof LRRP_STATUS_ITEMS / sizeof LRRP_STATUS_ITEMS[0]},
    {.id = "dest",
     .label = "Destination...",
     .help = "Choose LRRP output file path.",
     .submenu = LRRP_DEST_ITEMS,
     .submenu_len = sizeof LRRP_DEST_ITEMS / sizeof LRRP_DEST_ITEMS[0]},
};

// DSP
#ifdef USE_RTLSDR
// Submenus for DSP groups
static const NcMenuItem DSP_OVERVIEW_ITEMS[] = {
    {.id = "status",
     .label = "Show DSP Panel",
     .label_fn = lbl_dsp_panel,
     .help = "Toggle compact DSP status panel in main UI.",
     .on_select = act_toggle_dsp_panel},
    {.id = "man",
     .label = "Manual DSP Override",
     .label_fn = lbl_manual_dsp,
     .help = "Pin manual control; disables auto on/off by modulation.",
     .on_select = act_toggle_manual_dsp},
    {.id = "auto",
     .label = "Toggle Auto-DSP",
     .label_fn = lbl_onoff_auto,
     .help = "Enable/disable auto-DSP.",
     .on_select = act_toggle_auto},
    {.id = "auto_status",
     .label = "Auto-DSP Status",
     .label_fn = lbl_auto_status,
     .help = "Current Auto-DSP mode and P25 metrics."},
    {.id = "auto_cfg",
     .label = "Auto-DSP Config...",
     .help = "Adjust Auto-DSP thresholds and windows.",
     .submenu = AUTO_DSP_CFG_ITEMS,
     .submenu_len = sizeof AUTO_DSP_CFG_ITEMS / sizeof AUTO_DSP_CFG_ITEMS[0]},
};

static const NcMenuItem DSP_PATH_ITEMS[] = {
    {.id = "cqpsk",
     .label = "Toggle CQPSK",
     .label_fn = lbl_onoff_cq,
     .help = "Enable/disable CQPSK path.",
     .on_select = act_toggle_cq},
    {.id = "dqpsk",
     .label = "DQPSK Decision",
     .label_fn = lbl_onoff_dqpsk,
     .help = "Toggle DQPSK decision stage.",
     .is_enabled = dsp_cq_on,
     .on_select = act_toggle_dqpsk},
    {.id = "fll",
     .label = "Toggle FLL",
     .label_fn = lbl_onoff_fll,
     .help = "Enable/disable frequency-locked loop.",
     .on_select = act_toggle_fll},
    {.id = "ted",
     .label = "Timing Error (TED)",
     .label_fn = lbl_onoff_ted,
     .help = "Toggle TED (symbol timing).",
     .on_select = act_toggle_ted},
    {.id = "ted_force",
     .label = "TED Force",
     .label_fn = lbl_ted_force,
     .help = "Force TED even for FM/C4FM paths.",
     .on_select = act_ted_force_toggle},
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
};

static const NcMenuItem DSP_FILTER_ITEMS[] = {
    {.id = "rrc",
     .label = "RRC Filter",
     .label_fn = lbl_toggle_rrc,
     .help = "Toggle Root-Raised-Cosine matched filter.",
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
    {.id = "mf",
     .label = "Matched Filter (legacy)",
     .label_fn = lbl_onoff_mf,
     .help = "Toggle RX matched filter stage.",
     .is_enabled = dsp_cq_on,
     .on_select = act_toggle_mf},
    {.id = "lms",
     .label = "LMS Equalizer",
     .label_fn = lbl_onoff_lms,
     .help = "Toggle LMS equalizer.",
     .is_enabled = dsp_cq_on,
     .on_select = act_toggle_lms},
    {.id = "wl",
     .label = "WL Enhancement",
     .label_fn = lbl_onoff_wl,
     .help = "Toggle WL enhancement (CQPSK).",
     .is_enabled = dsp_cq_on,
     .on_select = act_toggle_wl},
    {.id = "dfe",
     .label = "Decision-Feedback EQ",
     .label_fn = lbl_onoff_dfe,
     .help = "Toggle DFE (CQPSK).",
     .is_enabled = dsp_cq_on,
     .on_select = act_toggle_dfe},
    {.id = "dft",
     .label = "Cycle DFE taps",
     .label_fn = lbl_dft_cycle,
     .help = "Cycle DFE tap count/mode.",
     .is_enabled = dsp_cq_on,
     .on_select = act_cycle_dft},
    {.id = "eq_taps",
     .label = "Set EQ taps 5/7",
     .label_fn = lbl_eq_taps,
     .help = "Toggle 5 vs 7 taps for EQ.",
     .is_enabled = dsp_cq_on,
     .on_select = act_taps_5_7},
    {.id = "c4fm_dd",
     .label = "C4FM DD Equalizer",
     .label_fn = lbl_c4fm_dd,
     .help = "Toggle symbol-domain decision-directed EQ.",
     .on_select = act_toggle_c4fm_dd},
    {.id = "c4fm_dd_params",
     .label = "DD Taps/Mu (status)",
     .label_fn = lbl_c4fm_dd_params,
     .help = "Current DD EQ taps and mu."},
    {.id = "c4fm_dd_taps",
     .label = "DD Taps cycle",
     .help = "Cycle DD EQ taps 3/5/7/9.",
     .on_select = act_c4fm_dd_taps_cycle},
    {.id = "c4fm_dd_mu+", .label = "DD mu +1", .help = "Increase DD mu.", .on_select = act_c4fm_dd_mu_up},
    {.id = "c4fm_dd_mu-", .label = "DD mu -1", .help = "Decrease DD mu.", .on_select = act_c4fm_dd_mu_dn},
    {.id = "cma",
     .label = "FM CMA Equalizer",
     .label_fn = lbl_fm_cma,
     .help = "Toggle pre-discriminator CMA equalizer.",
     .on_select = act_toggle_fm_cma},
    {.id = "cma_taps",
     .label = "CMA Taps (1/3/5/7/9)",
     .label_fn = lbl_fm_cma_taps,
     .help = "Cycle CMA taps.",
     .on_select = act_fm_cma_taps_cycle},
    {.id = "cma_mu", .label = "CMA mu (status)", .label_fn = lbl_fm_cma_mu, .help = "Step size (Q15)."},
    {.id = "cma_mu+", .label = "CMA mu +1", .help = "Increase mu.", .on_select = act_fm_cma_mu_up},
    {.id = "cma_mu-", .label = "CMA mu -1", .help = "Decrease mu.", .on_select = act_fm_cma_mu_dn},
    {.id = "cma_s",
     .label = "CMA Strength",
     .label_fn = lbl_fm_cma_strength,
     .help = "Cycle strength L/M/S.",
     .on_select = act_fm_cma_strength_cycle},
    {.id = "cma_guard",
     .label = "CMA Adaptive (status)",
     .label_fn = lbl_fm_cma_guard,
     .help = "Adapting/hold with accept/reject counts."},
    {.id = "cma_warm",
     .label = "CMA Warmup (status)",
     .label_fn = lbl_fm_cma_warm,
     .help = "Samples to hold before adapting (0=continuous)."},
    {.id = "cma_warm+", .label = "Warmup +5k", .help = "Increase warmup.", .on_select = act_fm_cma_warm_up},
    {.id = "cma_warm-", .label = "Warmup -5k", .help = "Decrease warmup.", .on_select = act_fm_cma_warm_dn},
};

static const NcMenuItem DSP_IQ_ITEMS[] = {
    {.id = "iqb",
     .label = "IQ Balance",
     .label_fn = lbl_onoff_iqbal,
     .help = "Toggle IQ imbalance compensation.",
     .on_select = act_toggle_iqbal},
    {.id = "iq_dc",
     .label = "IQ DC Block",
     .label_fn = lbl_iq_dc,
     .help = "Toggle complex DC blocker.",
     .on_select = act_toggle_iq_dc},
    {.id = "iq_dck",
     .label = "IQ DC Shift k (status)",
     .label_fn = lbl_iq_dc_k,
     .help = "k in dc += (x-dc)>>k (6..15)."},
    {.id = "iq_dck+", .label = "Shift k +1", .help = "Increase k.", .on_select = act_iq_dc_k_up},
    {.id = "iq_dck-", .label = "Shift k -1", .help = "Decrease k.", .on_select = act_iq_dc_k_dn},
};

static const NcMenuItem DSP_AGC_ITEMS[] = {
    {.id = "fm_agc",
     .label = "FM AGC",
     .label_fn = lbl_fm_agc,
     .help = "Toggle pre-discriminator FM AGC.",
     .on_select = act_toggle_fm_agc},
    {.id = "fm_agc_auto",
     .label = "FM AGC Auto",
     .label_fn = lbl_fm_agc_auto,
     .help = "Auto-tune AGC target/alphas.",
     .on_select = act_toggle_fm_agc_auto},
    {.id = "fm_lim",
     .label = "FM Limiter",
     .label_fn = lbl_fm_limiter,
     .help = "Toggle constant-envelope limiter.",
     .on_select = act_toggle_fm_limiter},
    {.id = "fm_tgt",
     .label = "AGC Target (status)",
     .label_fn = lbl_fm_agc_target,
     .help = "Target RMS amplitude (int16)."},
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
};

static const NcMenuItem DSP_TED_ITEMS[] = {
    {.id = "ted_sps", .label = "TED SPS (status)", .label_fn = lbl_ted_sps, .help = "Nominal samples-per-symbol."},
    {.id = "ted_sps+", .label = "TED SPS +1", .help = "Increase TED SPS.", .on_select = act_ted_sps_up},
    {.id = "ted_sps-", .label = "TED SPS -1", .help = "Decrease TED SPS.", .on_select = act_ted_sps_dn},
    {.id = "ted_gain_status", .label = "TED Gain (status)", .label_fn = lbl_ted_gain, .help = "TED small gain (Q20)."},
    {.id = "ted_gain+", .label = "TED Gain +", .help = "Increase TED small gain.", .on_select = act_ted_gain_up},
    {.id = "ted_gain-", .label = "TED Gain -", .help = "Decrease TED small gain.", .on_select = act_ted_gain_dn},
    {.id = "ted_bias",
     .label = "TED Bias (status)",
     .label_fn = lbl_ted_bias,
     .help = "Smoothed Gardner residual (read-only)."},
};

static const NcMenuItem DSP_BLANKER_ITEMS[] = {
    {.id = "blanker",
     .label = "Impulse Blanker",
     .label_fn = lbl_blanker,
     .help = "Toggle impulse blanker.",
     .on_select = act_toggle_blanker},
    {.id = "blanker_thr",
     .label = "Blanker Thr (status)",
     .label_fn = lbl_blanker_thr,
     .help = "Set blanker threshold."},
    {.id = "blanker_thr+", .label = "Blanker Thr +2k", .on_select = act_blanker_thr_up},
    {.id = "blanker_thr-", .label = "Blanker Thr -2k", .on_select = act_blanker_thr_dn},
    {.id = "blanker_win",
     .label = "Blanker Win (status)",
     .label_fn = lbl_blanker_win,
     .help = "Set blanker window (samples)."},
    {.id = "blanker_win+", .label = "Blanker Win +1", .on_select = act_blanker_win_up},
    {.id = "blanker_win-", .label = "Blanker Win -1", .on_select = act_blanker_win_dn},
};

static const NcMenuItem DSP_MENU_ITEMS[] = {
    {.id = "overview",
     .label = "Overview...",
     .help = "Global toggles and status.",
     .submenu = DSP_OVERVIEW_ITEMS,
     .submenu_len = sizeof DSP_OVERVIEW_ITEMS / sizeof DSP_OVERVIEW_ITEMS[0]},
    {.id = "path",
     .label = "Signal Path & Timing...",
     .help = "Demod path selection and timing assists.",
     .submenu = DSP_PATH_ITEMS,
     .submenu_len = sizeof DSP_PATH_ITEMS / sizeof DSP_PATH_ITEMS[0]},
    {.id = "filters",
     .label = "Filtering & Equalizers...",
     .help = "RRC/MF/LMS/DFE, C4FM DD EQ, FM CMA.",
     .submenu = DSP_FILTER_ITEMS,
     .submenu_len = sizeof DSP_FILTER_ITEMS / sizeof DSP_FILTER_ITEMS[0]},
    {.id = "iq",
     .label = "IQ & Front-End...",
     .help = "IQ balance and DC blocker.",
     .submenu = DSP_IQ_ITEMS,
     .submenu_len = sizeof DSP_IQ_ITEMS / sizeof DSP_IQ_ITEMS[0]},
    {.id = "agc",
     .label = "AGC & Limiter...",
     .help = "FM AGC, limiter, and parameters.",
     .submenu = DSP_AGC_ITEMS,
     .submenu_len = sizeof DSP_AGC_ITEMS / sizeof DSP_AGC_ITEMS[0]},
    {.id = "ted",
     .label = "TED Controls...",
     .help = "Timing recovery parameters.",
     .submenu = DSP_TED_ITEMS,
     .submenu_len = sizeof DSP_TED_ITEMS / sizeof DSP_TED_ITEMS[0]},
    {.id = "blanker",
     .label = "Impulse Blanker...",
     .help = "Impulse blanker threshold and window.",
     .submenu = DSP_BLANKER_ITEMS,
     .submenu_len = sizeof DSP_BLANKER_ITEMS / sizeof DSP_BLANKER_ITEMS[0]},
};
#endif

// ---- Advanced & Env submenus ----
static const NcMenuItem P25_FOLLOW_ITEMS[] = {
    {.id = "p25_vc_grace",
     .label = "P25: VC grace (s)",
     .label_fn = lbl_p25_vc_grace,
     .help = "Seconds after VC tune before eligible to return to CC.",
     .on_select = act_set_p25_vc_grace},
    {.id = "p25_min_follow",
     .label = "P25: Min follow dwell (s)",
     .label_fn = lbl_p25_min_follow,
     .help = "Minimum follow dwell after first voice.",
     .on_select = act_set_p25_min_follow},
    {.id = "p25_grant_voice",
     .label = "P25: Grant->Voice timeout (s)",
     .label_fn = lbl_p25_grant_voice,
     .help = "Max seconds from grant to voice before return.",
     .on_select = act_set_p25_grant_voice},
    {.id = "p25_retune_backoff",
     .label = "P25: Retune backoff (s)",
     .label_fn = lbl_p25_retune_backoff,
     .help = "Block immediate re-tune to same VC for N seconds.",
     .on_select = act_set_p25_retune_backoff},
    {.id = "p25_cc_grace",
     .label = "P25: CC hunt grace (s)",
     .label_fn = lbl_p25_cc_grace,
     .help = "Grace period for CC candidate transitions.",
     .on_select = act_set_p25_cc_grace},
    {.id = "p25_force_extra",
     .label = "P25: Safety-net extra (s)",
     .label_fn = lbl_p25_force_extra,
     .help = "Extra seconds beyond hangtime before force-release.",
     .on_select = act_set_p25_force_extra},
    {.id = "p25_force_margin",
     .label = "P25: Safety-net margin (s)",
     .label_fn = lbl_p25_force_margin,
     .help = "Hard margin seconds beyond extra.",
     .on_select = act_set_p25_force_margin},
    {.id = "p25p1_err_pct",
     .label = "P25p1: Err-hold %%",
     .label_fn = lbl_p25_p1_err_pct,
     .help = "IMBE error %% threshold to extend hang.",
     .on_select = act_set_p25_p1_err_pct},
    {.id = "p25p1_err_s",
     .label = "P25p1: Err-hold seconds",
     .label_fn = lbl_p25_p1_err_sec,
     .help = "Additional seconds to hold when threshold exceeded.",
     .on_select = act_set_p25_p1_err_sec},
};

static const NcMenuItem DSP_ADV_ITEMS[] = {
    {.id = "clk_assist",
     .label = "C4FM Clock Assist",
     .label_fn = lbl_c4fm_clk,
     .help = "Cycle C4FM clock assist: Off/EL/MM.",
     .on_select = act_c4fm_clk_cycle},
    {.id = "clk_sync",
     .label = "C4FM Clock Assist while Synced",
     .label_fn = lbl_c4fm_clk_sync,
     .help = "Allow clock assist to run while voice is synced.",
     .on_select = act_c4fm_clk_sync_toggle},
    {.id = "deemph",
     .label = "Deemphasis",
     .label_fn = lbl_deemph,
     .help = "Cycle deemphasis: Unset/Off/50/75/NFM.",
     .on_select = act_deemph_cycle},
    {.id = "audio_lpf",
     .label = "Audio LPF cutoff...",
     .label_fn = lbl_audio_lpf,
     .help = "Set post-demod LPF cutoff in Hz (0=off).",
     .on_select = act_set_audio_lpf},
    {.id = "win_freeze",
     .label = "Freeze Symbol Window",
     .label_fn = lbl_window_freeze,
     .help = "Freeze window selection and disable auto-centering.",
     .on_select = act_window_freeze_toggle},
    {.id = "ftz_daz",
     .label = "SSE FTZ/DAZ",
     .label_fn = lbl_ftz_daz,
     .help = "Toggle Flush-To-Zero / Denormals-Are-Zero (x86 SSE).",
     .on_select = act_toggle_ftz_daz},
};

/* (placeholder removed; see RTL_TCP_ADV_ITEMS_REAL below) */

// Provide explicit wrappers for prompts (avoid function pointer type mismatch)
static void
act_auto_ppm_snr_prompt(void* v) {
    double d = env_get_double("DSD_NEO_AUTO_PPM_SNR_DB", 18.0);
    ui_prompt_open_double_async("Auto-PPM SNR threshold (dB)", d, cb_auto_ppm_snr, v);
}

static void
act_auto_ppm_pwr_prompt(void* v) {
    double d = env_get_double("DSD_NEO_AUTO_PPM_PWR_DB", -10.0);
    ui_prompt_open_double_async("Auto-PPM min power (dB)", d, cb_auto_ppm_pwr, v);
}

static void
act_auto_ppm_zeroppm_prompt(void* v) {
    int p = env_get_int("DSD_NEO_AUTO_PPM_ZEROLOCK_PPM", 3);
    ui_prompt_open_int_async("Auto-PPM zero-lock PPM", p, cb_auto_ppm_zeroppm, v);
}

static void
act_auto_ppm_zerohz_prompt(void* v) {
    int h = env_get_int("DSD_NEO_AUTO_PPM_ZEROLOCK_HZ", 1500);
    ui_prompt_open_int_async("Auto-PPM zero-lock Hz", h, cb_auto_ppm_zerohz, v);
}

static void
act_tcp_prebuf_prompt(void* v) {
    int ms = env_get_int("DSD_NEO_TCP_PREBUF_MS", 30);
    ui_prompt_open_int_async("RTL-TCP prebuffer (ms)", ms, cb_tcp_prebuf, v);
}

static void
act_tcp_rcvbuf_prompt(void* v) {
    int sz = env_get_int("DSD_NEO_TCP_RCVBUF", 0);
    ui_prompt_open_int_async("RTL-TCP SO_RCVBUF (0=default)", sz, cb_tcp_rcvbuf, v);
}

static void
act_tcp_rcvtimeo_prompt(void* v) {
    int ms = env_get_int("DSD_NEO_TCP_RCVTIMEO", 0);
    ui_prompt_open_int_async("RTL-TCP SO_RCVTIMEO (ms; 0=off)", ms, cb_tcp_rcvtimeo, v);
}

static const NcMenuItem RTL_TCP_ADV_ITEMS_REAL[] = {
    {.id = "ap_snr",
     .label = "Auto-PPM SNR threshold...",
     .label_fn = lbl_auto_ppm_snr,
     .help = "Minimum SNR to allow spectrum-based PPM tracking.",
     .on_select = act_auto_ppm_snr_prompt},
    {.id = "ap_pwr",
     .label = "Auto-PPM Min power...",
     .label_fn = lbl_auto_ppm_pwr,
     .help = "Minimum spectral power to track PPM.",
     .on_select = act_auto_ppm_pwr_prompt},
    {.id = "ap_zero_ppm",
     .label = "Auto-PPM Zero-lock PPM...",
     .label_fn = lbl_auto_ppm_zeroppm,
     .help = "Snap to PPM=0 when within threshold.",
     .on_select = act_auto_ppm_zeroppm_prompt},
    {.id = "ap_zero_hz",
     .label = "Auto-PPM Zero-lock Hz...",
     .label_fn = lbl_auto_ppm_zerohz,
     .help = "Snap to PPM=0 when within frequency threshold.",
     .on_select = act_auto_ppm_zerohz_prompt},
    {.id = "ap_freeze",
     .label = "Auto-PPM Freeze",
     .label_fn = lbl_auto_ppm_freeze,
     .help = "Temporarily freeze auto-PPM updates.",
     .on_select = act_auto_ppm_freeze},
    {.id = "tcp_prebuf",
     .label = "RTL-TCP Prebuffer (ms)...",
     .label_fn = lbl_tcp_prebuf,
     .help = "Internal prebuffering to absorb jitter.",
     .on_select = act_tcp_prebuf_prompt},
    {.id = "tcp_rcvbuf",
     .label = "RTL-TCP SO_RCVBUF...",
     .label_fn = lbl_tcp_rcvbuf,
     .help = "Socket receive buffer size (bytes).",
     .on_select = act_tcp_rcvbuf_prompt},
    {.id = "tcp_rcvtimeo",
     .label = "RTL-TCP SO_RCVTIMEO...",
     .label_fn = lbl_tcp_rcvtimeo,
     .help = "Socket receive timeout (ms).",
     .on_select = act_tcp_rcvtimeo_prompt},
    {.id = "tcp_waitall",
     .label = "RTL-TCP MSG_WAITALL",
     .label_fn = lbl_tcp_waitall,
     .help = "Enable recv() MSG_WAITALL for full-block reads.",
     .on_select = act_tcp_waitall},
};

static const NcMenuItem RUNTIME_ADV_ITEMS[] = {
    {.id = "rt_sched",
     .label = "Realtime Scheduling",
     .label_fn = lbl_rt_sched,
     .help = "Best-effort realtime threads (requires privileges).",
     .on_select = act_rt_sched},
    {.id = "mt",
     .label = "Intra-block Multithreading",
     .label_fn = lbl_mt,
     .help = "Enable light worker-pool for hot loops.",
     .on_select = act_mt},
};

static const NcMenuItem ENV_EDITOR_ITEMS[] = {
    {.id = "edit",
     .label = "Set DSD_NEO_* Variable...",
     .help = "Edit any DSD_NEO_* environment variable.",
     .on_select = act_env_editor},
};

static const NcMenuItem ADV_MENU_ITEMS[] = {
    {.id = "p25_follow",
     .label = "P25 Follower Tuning",
     .help = "Adjust P25 SM/follower timing parameters.",
     .submenu = P25_FOLLOW_ITEMS,
     .submenu_len = sizeof P25_FOLLOW_ITEMS / sizeof P25_FOLLOW_ITEMS[0]},
    {.id = "dsp_adv",
     .label = "DSP Advanced",
     .help = "Clock assist, deemph, LPF, window freeze, FTZ/DAZ.",
     .submenu = DSP_ADV_ITEMS,
     .submenu_len = sizeof DSP_ADV_ITEMS / sizeof DSP_ADV_ITEMS[0]},
    {.id = "rtl_tcp_adv",
     .label = "RTL/TCP Advanced",
     .help = "Auto-PPM thresholds and RTL-TCP socket tuning.",
     .submenu = RTL_TCP_ADV_ITEMS_REAL,
     .submenu_len = sizeof RTL_TCP_ADV_ITEMS_REAL / sizeof RTL_TCP_ADV_ITEMS_REAL[0]},
    {.id = "runtime",
     .label = "Runtime & Threads",
     .help = "Realtime scheduling and light MT.",
     .submenu = RUNTIME_ADV_ITEMS,
     .submenu_len = sizeof RUNTIME_ADV_ITEMS / sizeof RUNTIME_ADV_ITEMS[0]},
    {.id = "env_editor",
     .label = "Environment Editor",
     .help = "Set any DSD_NEO_* variable.",
     .submenu = ENV_EDITOR_ITEMS,
     .submenu_len = sizeof ENV_EDITOR_ITEMS / sizeof ENV_EDITOR_ITEMS[0]},
};

void
ui_menu_get_main_items(const NcMenuItem** out_items, size_t* out_n, UiCtx* ctx) {
    UiCtx* c = ctx;
    static const NcMenuItem items[] = {
        {.id = "devices_io",
         .label = "Devices & IO",
         .help = "TCP, symbol replay, inversion.",
         .submenu = IO_MENU_ITEMS,
         .submenu_len = sizeof IO_MENU_ITEMS / sizeof IO_MENU_ITEMS[0]},
        {.id = "logging",
         .label = "Logging & Capture",
         .help = "Symbols, WAV, payloads, alerts, history.",
         .submenu = LOGGING_MENU_ITEMS,
         .submenu_len = sizeof LOGGING_MENU_ITEMS / sizeof LOGGING_MENU_ITEMS[0]},
        {.id = "trunk_ctrl",
         .label = "Trunking & Control",
         .help = "P25 CC prefs, Phase 2 params, rigctl.",
         .submenu = TRUNK_MENU_ITEMS,
         .submenu_len = sizeof TRUNK_MENU_ITEMS / sizeof TRUNK_MENU_ITEMS[0]},
        {.id = "keys_sec",
         .label = "Keys & Security",
         .help = "Manage keys and encrypted audio muting.",
         .submenu = KEYS_MENU_ITEMS,
         .submenu_len = sizeof KEYS_MENU_ITEMS / sizeof KEYS_MENU_ITEMS[0]},
        {.id = "dsp",
         .label = "DSP Options",
         .help = "RTL-SDR DSP toggles and tuning.",
         .is_enabled = io_rtl_active,
#ifdef USE_RTLSDR
         .submenu = DSP_MENU_ITEMS,
         .submenu_len = sizeof DSP_MENU_ITEMS / sizeof DSP_MENU_ITEMS[0]},
#else
         .submenu = NULL,
         .submenu_len = 0},
#endif
        {.id = "ui_display",
         .label = "UI Display",
         .help = "Toggle on-screen sections.",
         .submenu = UI_DISPLAY_MENU_ITEMS,
         .submenu_len = sizeof UI_DISPLAY_MENU_ITEMS / sizeof UI_DISPLAY_MENU_ITEMS[0]},
        {.id = "lrrp",
         .label = "LRRP Output",
         .help = "Configure LRRP file output.",
         .submenu = LRRP_MENU_ITEMS,
         .submenu_len = sizeof LRRP_MENU_ITEMS / sizeof LRRP_MENU_ITEMS[0]},
        {.id = "advanced",
         .label = "Advanced & Env",
         .help = "P25 follower, DSP advanced, RTL/TCP, env editor.",
         .submenu = ADV_MENU_ITEMS,
         .submenu_len = sizeof ADV_MENU_ITEMS / sizeof ADV_MENU_ITEMS[0]},
        {.id = "exit", .label = "Exit DSD-neo", .help = "Quit the application.", .on_select = act_exit},
    };
    (void)c; // context used by callbacks; arrays are static so safe to expose
    if (out_items) {
        *out_items = items;
    }
    if (out_n) {
        *out_n = sizeof items / sizeof items[0];
    }
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
