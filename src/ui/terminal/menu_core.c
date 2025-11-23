// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/runtime/unicode.h>
#include <dsd-neo/ui/keymap.h>
#include <dsd-neo/ui/menu_core.h>

#include <ctype.h>
#include <dsd-neo/core/dsd.h>
#include <dsd-neo/ui/ui_prims.h>
#include <ncurses.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <dsd-neo/runtime/config.h>
#include <dsd-neo/ui/menu_defs.h>
#include <dsd-neo/ui/menu_services.h>
#include <dsd-neo/ui/ui_async.h>
#include <dsd-neo/ui/ui_cmd.h>
#include <strings.h>

#if defined(__SSE__) || defined(__SSE2__)
#include <xmmintrin.h>
#endif

#ifdef USE_RTLSDR
#include <dsd-neo/io/rtl_stream_c.h>
#endif
#include <dsd-neo/ui/ui_dsp_cmd.h>

// Forward decl for hex parser used by async callbacks
static int parse_hex_u64(const char* s, unsigned long long* out);

#ifndef UNUSED
#define UNUSED(x) (void)(x)
#endif

// Internal helpers
// window helpers and transient status moved to ui_prims

// Forward declare submenu visibility helper so ui_is_enabled can use it
static int ui_submenu_has_visible(const NcMenuItem* items, size_t n, void* ctx);

static int
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

// Returns 1 if any item in a submenu is visible/enabled under the given ctx
static int
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

// Shared UI context for menu callbacks
typedef struct UiCtx {
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
static void act_p25_enc_lockout(void* v);
static void act_config_load(void* v);
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
static void act_config_save_default(void* v);
static void act_config_save_as(void* v);
// Prototypes for DSP C4FM clock assist controls defined later
static const char* lbl_c4fm_clk(void* v, char* b, size_t n);
static void act_c4fm_clk_cycle(void* v);
static const char* lbl_c4fm_clk_sync(void* v, char* b, size_t n);
static void act_c4fm_clk_sync_toggle(void* v);

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
    char sline[256];
    if (ui_status_peek(sline, sizeof sline, now)) {
        // clear line then print
        mvwhline(menu_win, mh - 2, 1, ' ', mw - 2);
        mvwprintw(menu_win, mh - 2, x, "Status: %s", sline);
    } else {
        ui_status_clear_if_expired(now);
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
        ui_post_cmd(UI_CMD_EVENT_LOG_SET, path, strlen(path) + 1);
        ui_statusf("Event log set requested");
    }
}

static void
cb_static_wav(void* v, const char* path) {
    UiCtx* c = (UiCtx*)v;
    if (!c) {
        return;
    }
    if (path && *path) {
        ui_post_cmd(UI_CMD_WAV_STATIC_OPEN, path, strlen(path) + 1);
        ui_statusf("Static WAV open requested");
    }
}

static void
cb_raw_wav(void* v, const char* path) {
    UiCtx* c = (UiCtx*)v;
    if (!c) {
        return;
    }
    if (path && *path) {
        ui_post_cmd(UI_CMD_WAV_RAW_OPEN, path, strlen(path) + 1);
        ui_statusf("Raw WAV open requested");
    }
}

static void
cb_dsp_out(void* v, const char* name) {
    UiCtx* c = (UiCtx*)v;
    if (!c) {
        return;
    }
    if (name && *name) {
        ui_post_cmd(UI_CMD_DSP_OUT_SET, name, strlen(name) + 1);
        ui_statusf("DSP output set requested");
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
        ui_post_cmd(UI_CMD_IMPORT_CHANNEL_MAP, p, strlen(p) + 1);
        ui_statusf("Import channel map requested");
    }
}

static void
cb_import_group(void* v, const char* p) {
    UiCtx* c = (UiCtx*)v;
    if (!c) {
        return;
    }
    if (p && *p) {
        ui_post_cmd(UI_CMD_IMPORT_GROUP_LIST, p, strlen(p) + 1);
        ui_statusf("Import group list requested");
    }
}

static void
cb_keys_dec(void* v, const char* p) {
    UiCtx* c = (UiCtx*)v;
    if (!c) {
        return;
    }
    if (p && *p) {
        ui_post_cmd(UI_CMD_IMPORT_KEYS_DEC, p, strlen(p) + 1);
        ui_statusf("Import keys (DEC) requested");
    }
}

static void
cb_keys_hex(void* v, const char* p) {
    UiCtx* c = (UiCtx*)v;
    if (!c) {
        return;
    }
    if (p && *p) {
        ui_post_cmd(UI_CMD_IMPORT_KEYS_HEX, p, strlen(p) + 1);
        ui_statusf("Import keys (HEX) requested");
    }
}

// Config save helpers
static void
cb_config_load(void* v, const char* path) {
    UiCtx* c = (UiCtx*)v;
    if (!c) {
        return;
    }
    if (!path || !*path) {
        ui_statusf("Config load canceled");
        return;
    }

    dsdneoUserConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    if (dsd_user_config_load(path, &cfg) != 0) {
        ui_statusf("Failed to load config from %s", path);
        return;
    }

    ui_post_cmd(UI_CMD_CONFIG_APPLY, &cfg, sizeof cfg);
    ui_statusf("Config loaded from %s", path);
}

static void
cb_config_save_as(void* v, const char* path) {
    UiCtx* c = (UiCtx*)v;
    if (!c) {
        return;
    }
    if (!path || !*path) {
        ui_statusf("Config save canceled");
        return;
    }
    dsdneoUserConfig cfg;
    dsd_snapshot_opts_to_user_config(c->opts, c->state, &cfg);
    if (dsd_user_config_save_atomic(path, &cfg) == 0) {
        ui_statusf("Config saved to %s", path);
    } else {
        ui_statusf("Failed to save config to %s", path);
    }
}

static void
act_config_save_default(void* v) {
    UiCtx* c = (UiCtx*)v;
    if (!c) {
        return;
    }
    const char* path = dsd_user_config_default_path();
    if (!path || !*path) {
        ui_statusf("No default config path; nothing saved");
        return;
    }
    dsdneoUserConfig cfg;
    dsd_snapshot_opts_to_user_config(c->opts, c->state, &cfg);
    if (dsd_user_config_save_atomic(path, &cfg) == 0) {
        ui_statusf("Config saved to %s", path);
    } else {
        ui_statusf("Failed to save config to %s", path);
    }
}

static void
act_config_save_as(void* v) {
    const char* def = dsd_user_config_default_path();
    ui_prompt_open_string_async("Save config to path", (def && *def) ? def : "", 512, cb_config_save_as, v);
}

static void
act_config_load(void* v) {
    const char* def = dsd_user_config_default_path();
    ui_prompt_open_string_async("Load config from path", (def && *def) ? def : "", 512, cb_config_load, v);
}

// Small typed setters
static void
cb_setmod_bw(void* v, int ok, int bw) {
    UiCtx* c = (UiCtx*)v;
    if (!c) {
        return;
    }
    if (ok) {
        int32_t hz = bw;
        ui_post_cmd(UI_CMD_RIGCTL_SET_MOD_BW, &hz, sizeof hz);
    }
}

static void
cb_tg_hold(void* v, int ok, int tg) {
    UiCtx* c = (UiCtx*)v;
    if (!c) {
        return;
    }
    if (ok) {
        uint32_t t = (unsigned)tg;
        ui_post_cmd(UI_CMD_TG_HOLD_SET, &t, sizeof t);
    }
}

static void
cb_hangtime(void* v, int ok, double s) {
    UiCtx* c = (UiCtx*)v;
    if (!c) {
        return;
    }
    if (ok) {
        double d = s;
        ui_post_cmd(UI_CMD_HANGTIME_SET, &d, sizeof d);
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
        int32_t pref01 = p - 1;
        ui_post_cmd(UI_CMD_SLOT_PREF_SET, &pref01, sizeof pref01);
    }
}

static void
cb_slots_on(void* v, int ok, int m) {
    UiCtx* c = (UiCtx*)v;
    if (!c) {
        return;
    }
    if (ok) {
        int32_t mask = m;
        ui_post_cmd(UI_CMD_SLOTS_ONOFF_SET, &mask, sizeof mask);
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
        ui_post_cmd(UI_CMD_KEY_TYT_AP_SET, s, strlen(s) + 1);
        ui_statusf("TYT AP keystream set requested");
    }
}

static void
cb_retevis_rc2(void* v, const char* s) {
    UiCtx* c = (UiCtx*)v;
    if (!c) {
        return;
    }
    if (s && *s) {
        ui_post_cmd(UI_CMD_KEY_RETEVIS_RC2_SET, s, strlen(s) + 1);
        ui_statusf("Retevis AP keystream set requested");
    }
}

static void
cb_tyt_ep(void* v, const char* s) {
    UiCtx* c = (UiCtx*)v;
    if (!c) {
        return;
    }
    if (s && *s) {
        ui_post_cmd(UI_CMD_KEY_TYT_EP_SET, s, strlen(s) + 1);
        ui_statusf("TYT EP keystream set requested");
    }
}

static void
cb_ken_scr(void* v, const char* s) {
    UiCtx* c = (UiCtx*)v;
    if (!c) {
        return;
    }
    if (s && *s) {
        ui_post_cmd(UI_CMD_KEY_KEN_SCR_SET, s, strlen(s) + 1);
        ui_statusf("Kenwood scrambler keystream set requested");
    }
}

static void
cb_anytone_bp(void* v, const char* s) {
    UiCtx* c = (UiCtx*)v;
    if (!c) {
        return;
    }
    if (s && *s) {
        ui_post_cmd(UI_CMD_KEY_ANYTONE_BP_SET, s, strlen(s) + 1);
        ui_statusf("Anytone BP keystream set requested");
    }
}

static void
cb_xor_ks(void* v, const char* s) {
    UiCtx* c = (UiCtx*)v;
    if (!c) {
        return;
    }
    if (s && *s) {
        ui_post_cmd(UI_CMD_KEY_XOR_SET, s, strlen(s) + 1);
        ui_statusf("XOR keystream set requested");
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
        uint32_t k = (uint32_t)vdec;
        ui_post_cmd(UI_CMD_KEY_BASIC_SET, &k, sizeof k);
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
        uint32_t r = (uint32_t)vdec;
        ui_post_cmd(UI_CMD_KEY_SCRAMBLER_SET, &r, sizeof r);
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
            uint64_t r = th;
            ui_post_cmd(UI_CMD_KEY_RC4DES_SET, &r, sizeof r);
        }
    }
}

// Hytera/AES multi-step
typedef struct {
    UiCtx* c;
    int step;
    unsigned long long H, K1, K2, K3, K4;
} HyCtx;

static void
cb_hytera_step(void* u, const char* text) {
    HyCtx* hc = (HyCtx*)u;
    if (!hc) {
        return;
    }
    unsigned long long t = 0ULL;
    if (text && *text && parse_hex_u64(text, &t)) {
        if (hc->step == 0) {
            hc->H = t;
            hc->K1 = t;
        } else if (hc->step == 1) {
            hc->K2 = t;
        } else if (hc->step == 2) {
            hc->K3 = t;
        } else if (hc->step == 3) {
            hc->K4 = t;
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

    struct {
        uint64_t H, K1, K2, K3, K4;
    } p = {hc->H, hc->K1, hc->K2, hc->K3, hc->K4};

    ui_post_cmd(UI_CMD_KEY_HYTERA_SET, &p, sizeof p);
    free(hc);
}

typedef struct {
    UiCtx* c;
    int step;
    unsigned long long K1, K2, K3, K4;
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
            ac->K1 = t;
        } else if (ac->step == 1) {
            ac->K2 = t;
        } else if (ac->step == 2) {
            ac->K3 = t;
        } else if (ac->step == 3) {
            ac->K4 = t;
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

    struct {
        uint64_t K1, K2, K3, K4;
    } p = {ac->K1, ac->K2, ac->K3, ac->K4};

    ui_post_cmd(UI_CMD_KEY_AES_SET, &p, sizeof p);
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
        ui_post_cmd(UI_CMD_LRRP_SET_CUSTOM, path, strlen(path) + 1);
        ui_statusf("LRRP custom output requested");
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

    struct {
        uint64_t w;
        uint64_t s;
        uint64_t n;
    } p = {pc->w, pc->s, pc->n};

    ui_post_cmd(UI_CMD_P25_P2_PARAMS_SET, &p, sizeof p);
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
        ui_post_cmd(UI_CMD_SYMCAP_OPEN, path, strlen(path) + 1);
        ui_statusf("Symbol capture open requested");
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
        ui_post_cmd(UI_CMD_SYMBOL_IN_OPEN, path, strlen(path) + 1);
        ui_statusf("Symbol input open requested");
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

    struct {
        char host[256];
        int32_t port;
    } payload = {0};

    snprintf(payload.host, sizeof payload.host, "%s", ctx->host);
    payload.port = port;
    ui_post_cmd(UI_CMD_UDP_OUT_CFG, &payload, sizeof payload);
    ui_statusf("UDP out requested: %s:%d", ctx->host, ctx->port);
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
        int32_t v = (int32_t)g;
        ui_post_cmd(UI_CMD_GAIN_SET, &v, sizeof v);
        ui_statusf("Digital gain set requested to %.1f", g);
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
        int32_t v = (int32_t)g;
        ui_post_cmd(UI_CMD_AGAIN_SET, &v, sizeof v);
        ui_statusf("Analog gain set requested to %.1f", g);
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
        int32_t v = m;
        ui_post_cmd(UI_CMD_INPUT_VOL_SET, &v, sizeof v);
        ui_statusf("Input Volume set requested to %dX", m);
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
        int32_t v = i;
        ui_post_cmd(UI_CMD_RTL_SET_DEV, &v, sizeof v);
    }
}

static void
cb_rtl_freq(void* u, int ok, int f) {
    UiCtx* c = (UiCtx*)u;
    if (!c) {
        return;
    }
    if (ok) {
        int32_t v = f;
        ui_post_cmd(UI_CMD_RTL_SET_FREQ, &v, sizeof v);
    }
}

static void
cb_rtl_gain(void* u, int ok, int g) {
    UiCtx* c = (UiCtx*)u;
    if (!c) {
        return;
    }
    if (ok) {
        int32_t v = g;
        ui_post_cmd(UI_CMD_RTL_SET_GAIN, &v, sizeof v);
    }
}

static void
cb_rtl_ppm(void* u, int ok, int p) {
    UiCtx* c = (UiCtx*)u;
    if (!c) {
        return;
    }
    if (ok) {
        int32_t v = p;
        ui_post_cmd(UI_CMD_RTL_SET_PPM, &v, sizeof v);
    }
}

static void
cb_rtl_bw(void* u, int ok, int bw) {
    UiCtx* c = (UiCtx*)u;
    if (!c) {
        return;
    }
    if (ok) {
        int32_t v = bw;
        ui_post_cmd(UI_CMD_RTL_SET_BW, &v, sizeof v);
    }
}

static void
cb_rtl_sql(void* u, int ok, double dB) {
    UiCtx* c = (UiCtx*)u;
    if (!c) {
        return;
    }
    if (ok) {
        double v = dB;
        ui_post_cmd(UI_CMD_RTL_SET_SQL_DB, &v, sizeof v);
    }
}

static void
cb_rtl_vol(void* u, int ok, int m) {
    UiCtx* c = (UiCtx*)u;
    if (!c) {
        return;
    }
    if (ok) {
        int32_t v = m;
        ui_post_cmd(UI_CMD_RTL_SET_VOL_MULT, &v, sizeof v);
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
        ui_post_cmd(UI_CMD_INPUT_WAV_SET, path, strlen(path) + 1);
        ui_statusf("WAV input requested: %s", path);
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
            ui_post_cmd(UI_CMD_SYMBOL_IN_OPEN, path, strlen(path) + 1);
            ui_statusf("Symbol input open requested");
        } else {
            ui_post_cmd(UI_CMD_INPUT_SYM_STREAM_SET, path, strlen(path) + 1);
            ui_statusf("Symbol stream input requested");
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

    struct {
        char host[256];
        int32_t port;
    } payload = {0};

    snprintf(payload.host, sizeof payload.host, "%s", ctx->host);
    payload.port = ctx->port;
    ui_post_cmd(UI_CMD_TCP_CONNECT_AUDIO_CFG, &payload, sizeof payload);
    ui_statusf("TCP connect requested: %s:%d", ctx->host, ctx->port);
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

    struct {
        char bind[256];
        int32_t port;
    } payload = {0};

    snprintf(payload.bind, sizeof payload.bind, "%s", ctx->addr);
    payload.port = ctx->port;
    ui_post_cmd(UI_CMD_UDP_INPUT_CFG, &payload, sizeof payload);
    ui_statusf("UDP input set requested: %s:%d", ctx->addr, ctx->port);
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
        free(ctx);
        return;
    }
    ctx->port = port;

    struct {
        char host[256];
        int32_t port;
    } payload = {0};

    snprintf(payload.host, sizeof payload.host, "%s", ctx->host);
    payload.port = ctx->port;
    ui_post_cmd(UI_CMD_RIGCTL_CONNECT_CFG, &payload, sizeof payload);
    ui_statusf("Rigctl connect requested: %s:%d", ctx->host, ctx->port);
    free(ctx);
}

static void
cb_rig_host(void* u, const char* host) {
    RigCtx* ctx = (RigCtx*)u;
    if (!ctx) {
        return;
    }
    if (!host || !*host) {
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
    ui_post_cmd(UI_CMD_INPUT_WARN_DB_SET, &thr, sizeof thr);
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
    // Align UI default with algorithm/README: 6 dB
    double d = env_get_double("DSD_NEO_AUTO_PPM_SNR_DB", 6.0);
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
    // Align UI default with algorithm/README: -80 dB
    double d = env_get_double("DSD_NEO_AUTO_PPM_PWR_DB", -80.0);
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
    // Align UI default with algorithm/README: 0.6 PPM
    double p = env_get_double("DSD_NEO_AUTO_PPM_ZEROLOCK_PPM", 0.6);
    snprintf(b, n, "Auto-PPM Zero-lock PPM: %.2f", p);
    return b;
}

static void
cb_auto_ppm_zeroppm(void* v, int ok, double p) {
    (void)v;
    if (ok) {
        env_set_double("DSD_NEO_AUTO_PPM_ZEROLOCK_PPM", p);
    }
}

static const char*
lbl_auto_ppm_zerohz(void* v, char* b, size_t n) {
    (void)v;
    // Align UI default with algorithm/README: 60 Hz
    int h = env_get_int("DSD_NEO_AUTO_PPM_ZEROLOCK_HZ", 60);
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
        ui_post_cmd(UI_CMD_RTL_RESTART, NULL, 0);
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
        ui_post_cmd(UI_CMD_RTL_RESTART, NULL, 0);
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
        ui_post_cmd(UI_CMD_RTL_RESTART, NULL, 0);
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
        ui_post_cmd(UI_CMD_RTL_RESTART, NULL, 0);
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

// main menu item provider declared in menu_defs.h

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
    // If no default config exists yet, provide a small hint so users starting
    // from CLI arguments discover the Config menu for saving defaults.
    const char* cfg_path = dsd_user_config_default_path();
    if (cfg_path && *cfg_path) {
        struct stat st;
        if (stat(cfg_path, &st) != 0) {
            ui_statusf("No default config; use Config menu to save to %s", cfg_path);
        }
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
bool
io_rtl_active(void* ctx) {
    UiCtx* c = (UiCtx*)ctx;
    if (!c || !c->opts) {
        return false;
    }
    return (c->opts->audio_in_type == 3);
}

static void
io_toggle_mute_enc(void* vctx) {
    (void)vctx;
    ui_post_cmd(UI_CMD_ALL_MUTES_TOGGLE, NULL, 0);
}

static void
io_toggle_call_alert(void* vctx) {
    (void)vctx;
    ui_post_cmd(UI_CMD_CALL_ALERT_TOGGLE, NULL, 0);
}

static void
io_toggle_cc_candidates(void* vctx) {
    (void)vctx;
    ui_post_cmd(UI_CMD_P25_CC_CAND_TOGGLE, NULL, 0);
}

static void
io_enable_per_call_wav(void* vctx) {
    UiCtx* c = (UiCtx*)vctx;
    if (c->opts->dmr_stereo_wav == 1 && c->opts->wav_out_f != NULL) {
        ui_post_cmd(UI_CMD_WAV_STOP, NULL, 0);
        ui_statusf("Per-call WAV stop requested");
    } else {
        ui_post_cmd(UI_CMD_WAV_START, NULL, 0);
        ui_statusf("Per-call WAV start requested");
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
    (void)vctx;
    ui_post_cmd(UI_CMD_REPLAY_LAST, NULL, 0);
    ui_statusf("Replay last requested");
}

static void
io_stop_symbol_playback(void* vctx) {
    (void)vctx;
    ui_post_cmd(UI_CMD_STOP_PLAYBACK, NULL, 0);
    ui_statusf("Stop playback requested");
}

static void
io_stop_symbol_saving(void* vctx) {
    (void)vctx;
    ui_post_cmd(UI_CMD_SYMCAP_STOP, NULL, 0);
    ui_statusf("Stop symbol capture requested");
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
            const char* name = pc->names[sel];
            ui_post_cmd(UI_CMD_PULSE_OUT_SET, name, strlen(name) + 1);
            ui_statusf("Pulse out requested: %s", name);
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
            const char* name = pc->names[sel];
            ui_post_cmd(UI_CMD_PULSE_IN_SET, name, strlen(name) + 1);
            ui_statusf("Pulse in requested: %s", name);
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
    ui_post_cmd(UI_CMD_PULSE_OUT_SET, idx, strlen(idx) + 1);
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
    (void)vctx;
    ui_post_cmd(UI_CMD_TOGGLE_MUTE, NULL, 0);
    ui_statusf("Output mute toggle requested");
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
    (void)vctx;
    ui_post_cmd(UI_CMD_INPUT_MONITOR_TOGGLE, NULL, 0);
}

static void
io_toggle_cosine(void* vctx) {
    (void)vctx;
    ui_post_cmd(UI_CMD_COSINE_FILTER_TOGGLE, NULL, 0);
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
    int32_t v = m;
    ui_post_cmd(UI_CMD_INPUT_VOL_SET, &v, sizeof v);
    ui_statusf("Input Volume requested: %dX", m);
}

static void
io_input_vol_dn(void* vctx) {
    UiCtx* c = (UiCtx*)vctx;
    int m = c->opts->input_volume_multiplier;
    if (m > 1) {
        m--;
    }
    int32_t v = m;
    ui_post_cmd(UI_CMD_INPUT_VOL_SET, &v, sizeof v);
    ui_statusf("Input Volume requested: %dX", m);
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
inv_x2(void* v) {
    (void)v;
    ui_post_cmd(UI_CMD_INV_X2_TOGGLE, NULL, 0);
}

static void
inv_dmr(void* v) {
    (void)v;
    ui_post_cmd(UI_CMD_INV_DMR_TOGGLE, NULL, 0);
}

static void
inv_dpmr(void* v) {
    (void)v;
    ui_post_cmd(UI_CMD_INV_DPMR_TOGGLE, NULL, 0);
}

static void
inv_m17(void* v) {
    (void)v;
    ui_post_cmd(UI_CMD_INV_M17_TOGGLE, NULL, 0);
}

#ifdef USE_RTLSDR
// ---- RTL-SDR submenu ----
static void
rtl_enable(void* v) {
    (void)v;
    ui_post_cmd(UI_CMD_RTL_ENABLE_INPUT, NULL, 0);
}

static void
rtl_restart(void* v) {
    (void)v;
    ui_post_cmd(UI_CMD_RTL_RESTART, NULL, 0);
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
    ui_prompt_open_int_async("DSP Bandwidth kHz (4,6,8,12,16,24)", c->opts->rtl_dsp_bw_khz, cb_rtl_bw, c);
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
    int32_t on = c->opts->rtl_bias_tee ? 0 : 1;
    ui_post_cmd(UI_CMD_RTL_SET_BIAS_TEE, &on, sizeof on);
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
    int32_t on = c->opts->rtltcp_autotune ? 0 : 1;
    ui_post_cmd(UI_CMD_RTLTCP_SET_AUTOTUNE, &on, sizeof on);
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
    int32_t on = c->opts->rtl_auto_ppm ? 0 : 1;
    ui_post_cmd(UI_CMD_RTL_SET_AUTO_PPM, &on, sizeof on);
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
        UiDspPayload p = {.op = UI_DSP_OP_TUNER_AUTOGAIN_TOGGLE};
        ui_post_cmd(UI_CMD_DSP_OP, &p, sizeof p);
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
    {.id = "bw", .label = "Set DSP Bandwidth (kHz)...", .help = "4,6,8,12,16,24.", .on_select = rtl_set_bw},
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
    {.id = "rtl.ctl",
     .label = "Control...",
     .help = "Stream control and device select.",
     .submenu = RTL_CTL_ITEMS,
     .submenu_len = sizeof RTL_CTL_ITEMS / sizeof RTL_CTL_ITEMS[0]},
    {.id = "rtl.rf",
     .label = "RF & IF Tuning...",
     .help = "RF center/gain, BW, squelch, volume.",
     .submenu = RTL_RF_ITEMS,
     .submenu_len = sizeof RTL_RF_ITEMS / sizeof RTL_RF_ITEMS[0]},
    {.id = "rtl.cal",
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
    (void)c;
    ui_post_cmd(UI_CMD_INPUT_SET_PULSE, NULL, 0);
    ui_statusf("Pulse input requested");
}

#ifdef USE_RTLSDR
static void
switch_to_rtl(void* vctx) {
    UiCtx* c = (UiCtx*)vctx;
    (void)c;
    ui_post_cmd(UI_CMD_RTL_ENABLE_INPUT, NULL, 0);
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
    (void)v;
    ui_post_cmd(UI_CMD_TRUNK_ENC_TOGGLE, NULL, 0);
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
#include <assert.h>

static bool
dsp_cq_on(void* v) {
    UNUSED(v);
    int cq = 0, f = 0, t = 0;
    rtl_stream_dsp_get(&cq, &f, &t);
    return cq != 0;
}

// ---- Modulation-aware capability helpers ----
// rf_mod convention (from CLI): 0=C4FM/FM family, 1=QPSK/CQPSK, 2=GFSK/2-level FSK
static int
ui_current_mod(const void* v) {
    const UiCtx* c = (const UiCtx*)v;
    int mod = -1;

    // Honor CLI-locked demod selection when present
    if (c && c->opts && c->opts->mod_cli_lock) {
        if (c->opts->mod_qpsk) {
            mod = 1;
        } else if (c->opts->mod_gfsk) {
            mod = 2;
        } else {
            mod = 0;
        }
    }

    // Prefer live state when available (any valid rf_mod)
    if (mod < 0 && c && c->state) {
        int rf = c->state->rf_mod;
        if (rf >= 0 && rf <= 2) {
            mod = rf;
        }
    }

    // Snap to the active DSP path: CQPSK toggle always means QPSK path
    int cq = 0;
    rtl_stream_dsp_get(&cq, NULL, NULL);
    if (cq) {
        mod = 1;
    }

    // Fallback: default to FM/C4FM family (or GFSK when hinted)
    if (mod < 0) {
        mod = 0;
    }
    return mod;
}

static bool
is_mod_qpsk(void* v) {
    return ui_current_mod(v) == 1;
}

static bool
is_mod_c4fm(void* v) {
    return ui_current_mod(v) == 0;
}

static bool
is_mod_gfsk(void* v) {
    return ui_current_mod(v) == 2;
}

static bool
is_mod_fm(void* v) {
    int m = ui_current_mod(v);
    return m == 0 || m == 2;
}

static bool
is_not_qpsk(void* v) {
    return !is_mod_qpsk(v);
}

// Policy: FLL allowed on FM/FSK and CQPSK paths
static bool
is_fll_allowed(void* v) {
    return is_mod_qpsk(v) || is_mod_fm(v);
}

// Policy: TED controls are relevant for CQPSK and FM/FSK family paths
static bool
is_ted_allowed(void* v) {
    return is_mod_qpsk(v) || is_mod_fm(v);
}

static const char*
lbl_onoff_cq(void* v, char* b, size_t n) {
    UNUSED(v);
    int cq = 0, f = 0, t = 0;
    rtl_stream_dsp_get(&cq, &f, &t);
    snprintf(b, n, "Toggle CQPSK [%s]", cq ? "Active" : "Inactive");
    return b;
}

static const char*
lbl_onoff_fll(void* v, char* b, size_t n) {
    UNUSED(v);
    int cq = 0, f = 0, t = 0;
    rtl_stream_dsp_get(&cq, &f, &t);
    snprintf(b, n, "Toggle FLL [%s]", f ? "Active" : "Inactive");
    return b;
}

static const char*
lbl_onoff_ted(void* v, char* b, size_t n) {
    UNUSED(v);
    int cq = 0, f = 0, t = 0;
    rtl_stream_dsp_get(&cq, &f, &t);
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

/* ---- C4FM DD Equalizer (symbol-domain) ---- */
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
    UiDspPayload p = {.op = UI_DSP_OP_C4FM_CLK_CYCLE};
    ui_post_cmd(UI_CMD_DSP_OP, &p, sizeof p);
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
    UiDspPayload p = {.op = UI_DSP_OP_C4FM_CLK_SYNC_TOGGLE};
    ui_post_cmd(UI_CMD_DSP_OP, &p, sizeof p);
}

static void
act_toggle_fm_agc(void* v) {
    UNUSED(v);
    UiDspPayload p = {.op = UI_DSP_OP_FM_AGC_TOGGLE};
    ui_post_cmd(UI_CMD_DSP_OP, &p, sizeof p);
}

static const char*
lbl_fm_limiter(void* v, char* b, size_t n) {
    UNUSED(v);
    int on = rtl_stream_get_fm_limiter();
    snprintf(b, n, "FM Limiter [%s]", on ? "On" : "Off");
    return b;
}

static void
act_toggle_fm_limiter(void* v) {
    UNUSED(v);
    UiDspPayload p = {.op = UI_DSP_OP_FM_LIMITER_TOGGLE};
    ui_post_cmd(UI_CMD_DSP_OP, &p, sizeof p);
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
    UiDspPayload p = {.op = UI_DSP_OP_FM_AGC_TARGET_DELTA, .a = +500};
    ui_post_cmd(UI_CMD_DSP_OP, &p, sizeof p);
}

static void
act_fm_agc_target_dn(void* v) {
    UNUSED(v);
    UiDspPayload p = {.op = UI_DSP_OP_FM_AGC_TARGET_DELTA, .a = -500};
    ui_post_cmd(UI_CMD_DSP_OP, &p, sizeof p);
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
    UiDspPayload p = {.op = UI_DSP_OP_FM_AGC_ATTACK_DELTA, .a = -1024};
    ui_post_cmd(UI_CMD_DSP_OP, &p, sizeof p);
}

static void
act_fm_agc_alpha_down_up(void* v) {
    UNUSED(v);
    UiDspPayload p = {.op = UI_DSP_OP_FM_AGC_DECAY_DELTA, .a = +1024};
    ui_post_cmd(UI_CMD_DSP_OP, &p, sizeof p);
}

static void
act_fm_agc_alpha_down_dn(void* v) {
    UNUSED(v);
    UiDspPayload p = {.op = UI_DSP_OP_FM_AGC_DECAY_DELTA, .a = -1024};
    ui_post_cmd(UI_CMD_DSP_OP, &p, sizeof p);
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
    UiDspPayload p = {.op = UI_DSP_OP_IQ_DC_TOGGLE};
    ui_post_cmd(UI_CMD_DSP_OP, &p, sizeof p);
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
    UiDspPayload p = {.op = UI_DSP_OP_IQ_DC_K_DELTA, .a = +1};
    ui_post_cmd(UI_CMD_DSP_OP, &p, sizeof p);
}

static void
act_iq_dc_k_dn(void* v) {
    UNUSED(v);
    UiDspPayload p = {.op = UI_DSP_OP_IQ_DC_K_DELTA, .a = -1};
    ui_post_cmd(UI_CMD_DSP_OP, &p, sizeof p);
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
    if (sps < 64) {
        sps++;
    }
    UiDspPayload p = {.op = UI_DSP_OP_TED_SPS_SET, .a = sps};
    ui_post_cmd(UI_CMD_DSP_OP, &p, sizeof p);
}

static void
act_ted_sps_dn(void* v) {
    UNUSED(v);
    int sps = rtl_stream_get_ted_sps();
    if (sps > 2) {
        sps--;
    }
    UiDspPayload p = {.op = UI_DSP_OP_TED_SPS_SET, .a = sps};
    ui_post_cmd(UI_CMD_DSP_OP, &p, sizeof p);
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
    UiDspPayload p = {.op = UI_DSP_OP_TED_GAIN_SET, .a = g};
    ui_post_cmd(UI_CMD_DSP_OP, &p, sizeof p);
}

static void
act_ted_gain_dn(void* v) {
    UNUSED(v);
    int g = rtl_stream_get_ted_gain();
    if (g > 16) {
        g -= 8;
    }
    UiDspPayload p = {.op = UI_DSP_OP_TED_GAIN_SET, .a = g};
    ui_post_cmd(UI_CMD_DSP_OP, &p, sizeof p);
}

static void
act_toggle_iqbal(void* v) {
    UNUSED(v);
    UiDspPayload p = {.op = UI_DSP_OP_TOGGLE_IQBAL};
    ui_post_cmd(UI_CMD_DSP_OP, &p, sizeof p);
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
    (void)v;
    ui_post_cmd(UI_CMD_UI_SHOW_DSP_PANEL_TOGGLE, NULL, 0);
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
        int cq = 0, fl = 0, t = 0;
        rtl_stream_dsp_get(&cq, &fl, &t);
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
lbl_onoff_mf(void* v, char* b, size_t n) {
    UNUSED(v);
    int mf = 0;
    rtl_stream_cqpsk_get(&mf);
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

/* ---- CQPSK acquisition-only FLL (pre-Costas) ---- */
static const char*
lbl_cqpsk_acq_fll(void* v, char* b, size_t n) {
    UNUSED(v);
#ifdef USE_RTLSDR
    int on = rtl_stream_get_cqpsk_acq_fll();
    snprintf(b, n, "CQPSK Acquisition FLL [%s]", on ? "On" : "Off");
#else
    snprintf(b, n, "CQPSK Acquisition FLL [N/A]");
#endif
    return b;
}

static void
act_toggle_cqpsk_acq_fll(void* v) {
    UNUSED(v);
    UiDspPayload p = {.op = UI_DSP_OP_CQPSK_ACQ_FLL_TOGGLE};
    ui_post_cmd(UI_CMD_DSP_OP, &p, sizeof p);
}

static void
act_toggle_cq(void* v) {
    (void)v;
    UiDspPayload p = {.op = UI_DSP_OP_TOGGLE_CQ};
    ui_post_cmd(UI_CMD_DSP_OP, &p, sizeof p);
}

static void
act_toggle_fll(void* v) {
    UNUSED(v);
    UiDspPayload p = {.op = UI_DSP_OP_TOGGLE_FLL};
    ui_post_cmd(UI_CMD_DSP_OP, &p, sizeof p);
}

static void
act_toggle_ted(void* v) {
    UNUSED(v);
    UiDspPayload p = {.op = UI_DSP_OP_TOGGLE_TED};
    ui_post_cmd(UI_CMD_DSP_OP, &p, sizeof p);
}

static void
act_toggle_mf(void* v) {
    UNUSED(v);
    UiDspPayload p = {.op = UI_DSP_OP_TOGGLE_MF};
    ui_post_cmd(UI_CMD_DSP_OP, &p, sizeof p);
}

static void
act_toggle_rrc(void* v) {
    UNUSED(v);
    UiDspPayload p = {.op = UI_DSP_OP_TOGGLE_RRC};
    ui_post_cmd(UI_CMD_DSP_OP, &p, sizeof p);
}

static void
act_rrc_a_up(void* v) {
    UNUSED(v);
    UiDspPayload p = {.op = UI_DSP_OP_RRC_ALPHA_DELTA, .a = +5};
    ui_post_cmd(UI_CMD_DSP_OP, &p, sizeof p);
}

static void
act_rrc_a_dn(void* v) {
    UNUSED(v);
    UiDspPayload p = {.op = UI_DSP_OP_RRC_ALPHA_DELTA, .a = -5};
    ui_post_cmd(UI_CMD_DSP_OP, &p, sizeof p);
}

static void
act_rrc_s_up(void* v) {
    UNUSED(v);
    UiDspPayload p = {.op = UI_DSP_OP_RRC_SPAN_DELTA, .a = +1};
    ui_post_cmd(UI_CMD_DSP_OP, &p, sizeof p);
}

static void
act_rrc_s_dn(void* v) {
    UNUSED(v);
    UiDspPayload p = {.op = UI_DSP_OP_RRC_SPAN_DELTA, .a = -1};
    ui_post_cmd(UI_CMD_DSP_OP, &p, sizeof p);
}

#endif /* end of USE_RTLSDR block started at 2881 (DSP labels/actions) */

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
    ui_prompt_open_int_async("Basic Privacy Key Number (DEC)", 0, cb_key_basic, c);
}

static void
key_hytera(void* v) {
    UiCtx* c = (UiCtx*)v;
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
    ui_prompt_open_int_async("NXDN/dPMR Scrambler Key (DEC)", 0, cb_key_scrambler, c);
}

static void
key_force_bp(void* v) {
    (void)v;
    ui_post_cmd(UI_CMD_FORCE_PRIV_TOGGLE, NULL, 0);
}

static void
key_rc4des(void* v) {
    UiCtx* c = (UiCtx*)v;
    ui_prompt_open_string_async("RC4/DES Key (HEX)", NULL, 128, cb_key_rc4des, c);
}

static void
key_aes(void* v) {
    UiCtx* c = (UiCtx*)v;
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
    (void)c;
    ui_post_cmd(UI_CMD_LRRP_SET_HOME, NULL, 0);
    ui_statusf("LRRP set home requested");
}

static void
lr_dsdp(void* v) {
    UiCtx* c = (UiCtx*)v;
    (void)c;
    ui_post_cmd(UI_CMD_LRRP_SET_DSDP, NULL, 0);
    ui_statusf("LRRP set DSDPlus requested");
}

static void
lr_custom(void* v) {
    UiCtx* c = (UiCtx*)v;
    ui_prompt_open_string_async("Enter LRRP output filename", NULL, 1024, cb_lr_custom, c);
}

static void
lr_off(void* v) {
    (void)v;
    ui_post_cmd(UI_CMD_LRRP_DISABLE, NULL, 0);
    ui_statusf("LRRP disable requested");
}

static const char*
lbl_lrrp_current(void* vctx, char* b, size_t n) {
    UiCtx* c = (UiCtx*)vctx;
    if (c->opts->lrrp_file_output && c->opts->lrrp_out_file[0] != '\0') {
        snprintf(b, n, "LRRP Output [Active: %s]", c->opts->lrrp_out_file);
    } else {
        snprintf(b, n, "LRRP Output [Inactive]");
    }
    return b;
}

// ---- Main Menu ----

// action wrappers

static void
act_toggle_invert(void* v) {
    UiCtx* c = (UiCtx*)v;
    (void)c;
    ui_post_cmd(UI_CMD_INVERT_TOGGLE, NULL, 0);
}

static void
act_reset_eh(void* v) {
    (void)v;
    ui_post_cmd(UI_CMD_EH_RESET, NULL, 0);
}

static void
act_toggle_payload(void* v) {
    (void)v;
    ui_post_cmd(UI_CMD_PAYLOAD_TOGGLE, NULL, 0);
}

// Generic small actions used by menus (C, no lambdas!)
static void
act_event_log_set(void* v) {
    UiCtx* c = (UiCtx*)v;
    ui_prompt_open_string_async("Event log filename", c->opts->event_out_file, 1024, cb_event_log_set, c);
}

static void
act_event_log_disable(void* v) {
    (void)v;
    ui_post_cmd(UI_CMD_EVENT_LOG_DISABLE, NULL, 0);
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
    (void)v;
    ui_post_cmd(UI_CMD_CRC_RELAX_TOGGLE, NULL, 0);
}

static void
act_trunk_toggle(void* v) {
    (void)v;
    ui_post_cmd(UI_CMD_TRUNK_TOGGLE, NULL, 0);
    ui_statusf("Trunking toggle requested...");
}

static void
act_scan_toggle(void* v) {
    (void)v;
    ui_post_cmd(UI_CMD_SCANNER_TOGGLE, NULL, 0);
    ui_statusf("Scanner toggle requested...");
}

static void
act_lcw_toggle(void* v) {
    (void)v;
    ui_post_cmd(UI_CMD_LCW_RETUNE_TOGGLE, NULL, 0);
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
    (void)v;
    ui_post_cmd(UI_CMD_TRUNK_WLIST_TOGGLE, NULL, 0);
}

static void
act_tune_group(void* v) {
    (void)v;
    // Toggle group-call tuning (was incorrectly wired to allow/whitelist toggle)
    ui_post_cmd(UI_CMD_TRUNK_GROUP_TOGGLE, NULL, 0);
}

static void
act_tune_priv(void* v) {
    (void)v;
    ui_post_cmd(UI_CMD_TRUNK_PRIV_TOGGLE, NULL, 0);
}

static void
act_tune_data(void* v) {
    (void)v;
    ui_post_cmd(UI_CMD_TRUNK_DATA_TOGGLE, NULL, 0);
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
    (void)v;
    ui_post_cmd(UI_CMD_REVERSE_MUTE_TOGGLE, NULL, 0);
}

static void
act_dmr_le(void* v) {
    (void)v;
    ui_post_cmd(UI_CMD_DMR_LE_TOGGLE, NULL, 0);
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
    if (mc && mc->c && text && *text) {
        ui_post_cmd(UI_CMD_M17_USER_DATA_SET, text, strlen(text) + 1);
        ui_statusf("M17 user data set requested");
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
    (void)v;
    ui_post_cmd(UI_CMD_UI_SHOW_P25_METRICS_TOGGLE, NULL, 0);
}

static const char*
lbl_ui_p25_affil(void* v, char* b, size_t n) {
    UiCtx* c = (UiCtx*)v;
    snprintf(b, n, "Show P25 Affiliations [%s]", (c && c->opts && c->opts->show_p25_affiliations) ? "On" : "Off");
    return b;
}

static void
act_toggle_ui_p25_affil(void* v) {
    (void)v;
    ui_post_cmd(UI_CMD_UI_SHOW_P25_AFFIL_TOGGLE, NULL, 0);
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
    (void)v;
    ui_post_cmd(UI_CMD_P25_GA_TOGGLE, NULL, 0);
}

static const char*
lbl_ui_p25_neighbors(void* v, char* b, size_t n) {
    UiCtx* c = (UiCtx*)v;
    snprintf(b, n, "Show P25 Neighbors [%s]", (c && c->opts && c->opts->show_p25_neighbors) ? "On" : "Off");
    return b;
}

static void
act_toggle_ui_p25_neighbors(void* v) {
    (void)v;
    ui_post_cmd(UI_CMD_UI_SHOW_P25_NEIGHBORS_TOGGLE, NULL, 0);
}

static const char*
lbl_ui_p25_iden(void* v, char* b, size_t n) {
    UiCtx* c = (UiCtx*)v;
    snprintf(b, n, "Show P25 IDEN Plan [%s]", (c && c->opts && c->opts->show_p25_iden_plan) ? "On" : "Off");
    return b;
}

static void
act_toggle_ui_p25_iden(void* v) {
    (void)v;
    ui_post_cmd(UI_CMD_UI_SHOW_P25_IDEN_TOGGLE, NULL, 0);
}

static const char*
lbl_ui_p25_ccc(void* v, char* b, size_t n) {
    UiCtx* c = (UiCtx*)v;
    snprintf(b, n, "Show P25 CC Candidates [%s]", (c && c->opts && c->opts->show_p25_cc_candidates) ? "On" : "Off");
    return b;
}

static void
act_toggle_ui_p25_ccc(void* v) {
    (void)v;
    ui_post_cmd(UI_CMD_UI_SHOW_P25_CCC_TOGGLE, NULL, 0);
}

static const char*
lbl_ui_channels(void* v, char* b, size_t n) {
    UiCtx* c = (UiCtx*)v;
    snprintf(b, n, "Show Channels [%s]", (c && c->opts && c->opts->show_channels) ? "On" : "Off");
    return b;
}

static void
act_toggle_ui_channels(void* v) {
    (void)v;
    ui_post_cmd(UI_CMD_UI_SHOW_CHANNELS_TOGGLE, NULL, 0);
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

void
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
    {.id = "switch.current", .label = "Current", .label_fn = lbl_current_input, .help = "Shows current input."},
    {.id = "switch.pulse",
     .label = "Pulse Audio (mic/line)",
     .help = "Use Pulse Audio input.",
     .on_select = switch_to_pulse},
#ifdef USE_RTLSDR
    {.id = "switch.rtl", .label = "RTL-SDR", .help = "Switch to RTL-SDR input.", .on_select = switch_to_rtl},
#endif
    {.id = "switch.tcp",
     .label = "TCP Direct Audio...",
     .help = "Connect to PCM16LE over TCP.",
     .on_select = switch_to_tcp},
    {.id = "switch.wav",
     .label = "WAV/File...",
     .help = "Open WAV/RAW file or named pipe.",
     .on_select = switch_to_wav},
    {.id = "switch.sym",
     .label = "Symbol Capture (.bin/.raw/.sym)...",
     .help = "Replay captured symbols.",
     .on_select = switch_to_symbol},
    {.id = "switch.udp", .label = "UDP Signal Input...", .help = "Bind UDP PCM16LE input.", .on_select = switch_to_udp},
};

static const NcMenuItem IO_SWITCH_OUTPUT_ITEMS[] = {
    {.id = "switch.current_out",
     .label = "Current Output",
     .label_fn = lbl_current_output,
     .help = "Shows the active output sink."},
    {.id = "switch.pulse_out",
     .label = "Pulse Digital Output",
     .help = "Play decoded audio via Pulse.",
     .on_select = switch_out_pulse},
    {.id = "switch.udp_out",
     .label = "UDP Audio Output...",
     .help = "Send decoded audio via UDP.",
     .on_select = switch_out_udp},
    {.id = "switch.mute",
     .label = "Mute Output",
     .label_fn = lbl_out_mute,
     .help = "Toggle mute without changing sink.",
     .on_select = switch_out_toggle_mute},
};

// IO grouped submenus
static const NcMenuItem IO_INPUT_ITEMS[] = {
    {.id = "io.switch_input",
     .label = "Switch Input...",
     .help = "Change active input source.",
     .submenu = IO_SWITCH_INPUT_ITEMS,
     .submenu_len = sizeof IO_SWITCH_INPUT_ITEMS / sizeof IO_SWITCH_INPUT_ITEMS[0]},
#ifdef USE_RTLSDR
    {.id = "io.rtl",
     .label = "RTL-SDR...",
     .help = "Configure RTL device, gain, PPM, BW, SQL.",
     .is_enabled = io_rtl_active,
     .submenu = RTL_MENU_ITEMS,
     .submenu_len = sizeof RTL_MENU_ITEMS / sizeof RTL_MENU_ITEMS[0]},
#endif
    {.id = "io.pulse_in",
     .label = "Set Pulse Input...",
     .help = "Set Pulse input by index/name.",
     .is_enabled = io_always_on,
     .on_select = io_set_pulse_in},
    {.id = "io.tcp_input",
     .label = "TCP Direct Audio",
     .label_fn = lbl_tcp,
     .help = "Connect to a remote PCM16LE source via TCP.",
     .is_enabled = io_always_on,
     .on_select = io_tcp_direct_link},
    {.id = "io.read_sym",
     .label = "Read Symbol Capture File",
     .help = "Open an existing symbol capture for replay.",
     .is_enabled = io_always_on,
     .on_select = io_read_symbol_bin},
    {.id = "io.replay_last",
     .label = "Replay Last Symbol Capture",
     .label_fn = lbl_replay_last,
     .help = "Re-open the last used symbol capture file.",
     .is_enabled = io_always_on,
     .on_select = io_replay_last_symbol_bin},
    {.id = "io.stop_playback",
     .label = "Stop Symbol Playback",
     .label_fn = lbl_stop_symbol_playback,
     .help = "Stop replaying the symbol capture and restore input mode.",
     .is_enabled = io_always_on,
     .on_select = io_stop_symbol_playback},
};

static const NcMenuItem IO_OUTPUT_ITEMS2[] = {
    {.id = "io.switch_output",
     .label = "Switch Output...",
     .help = "Change audio output sink.",
     .submenu = IO_SWITCH_OUTPUT_ITEMS,
     .submenu_len = sizeof IO_SWITCH_OUTPUT_ITEMS / sizeof IO_SWITCH_OUTPUT_ITEMS[0]},
    {.id = "io.pulse_out",
     .label = "Set Pulse Output...",
     .help = "Set Pulse output by index/name.",
     .is_enabled = io_always_on,
     .on_select = io_set_pulse_out},
    {.id = "io.udp_out",
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
};

const NcMenuItem IO_MENU_ITEMS[] = {
    {.id = "io.inputs",
     .label = "Inputs...",
     .help = "Select and configure inputs.",
     .submenu = IO_INPUT_ITEMS,
     .submenu_len = sizeof IO_INPUT_ITEMS / sizeof IO_INPUT_ITEMS[0]},
    {.id = "io.outputs",
     .label = "Outputs...",
     .help = "Audio sinks and UDP output.",
     .submenu = IO_OUTPUT_ITEMS2,
     .submenu_len = sizeof IO_OUTPUT_ITEMS2 / sizeof IO_OUTPUT_ITEMS2[0]},
    {.id = "io.levels",
     .label = "Levels & Monitor...",
     .help = "Gains, input volume, monitor.",
     .submenu = IO_LEVELS_ITEMS,
     .submenu_len = sizeof IO_LEVELS_ITEMS / sizeof IO_LEVELS_ITEMS[0]},
    {.id = "io.invert",
     .label = "Inversion...",
     .help = "Per‑protocol inversion toggles.",
     .submenu = IO_INV_ITEMS,
     .submenu_len = sizeof IO_INV_ITEMS / sizeof IO_INV_ITEMS[0]},
    {.id = "io.filters",
     .label = "Filters...",
     .help = "Cosine and fixed RRC presets.",
     .submenu = IO_FILTER_ITEMS,
     .submenu_len = sizeof IO_FILTER_ITEMS / sizeof IO_FILTER_ITEMS[0]},
};
const size_t IO_MENU_ITEMS_LEN = sizeof IO_MENU_ITEMS / sizeof IO_MENU_ITEMS[0];

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

const NcMenuItem LOGGING_MENU_ITEMS[] = {
    {.id = "log.capture",
     .label = "Capture...",
     .help = "Symbol/audio capture and structured output.",
     .submenu = LOGGING_CAPTURE_ITEMS,
     .submenu_len = sizeof LOGGING_CAPTURE_ITEMS / sizeof LOGGING_CAPTURE_ITEMS[0]},
    {.id = "log.logging",
     .label = "Logging...",
     .help = "Event/payload logging and housekeeping.",
     .submenu = LOGGING_LOG_ITEMS,
     .submenu_len = sizeof LOGGING_LOG_ITEMS / sizeof LOGGING_LOG_ITEMS[0]},
};
const size_t LOGGING_MENU_ITEMS_LEN = sizeof LOGGING_MENU_ITEMS / sizeof LOGGING_MENU_ITEMS[0];

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

    {.id = "p25_enc",
     .label = "P25 Encrypted Call Lockout",
     .label_fn = lbl_p25_enc_lockout,
     .help = "Do not tune encrypted calls when On.",
     .on_select = act_p25_enc_lockout},
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

const NcMenuItem TRUNK_MENU_ITEMS[] = {
    {.id = "trunk.modes",
     .label = "Modes...",
     .help = "Enable trunking or conventional scanning.",
     .submenu = TRUNK_MODES_ITEMS,
     .submenu_len = sizeof TRUNK_MODES_ITEMS / sizeof TRUNK_MODES_ITEMS[0]},
    {.id = "trunk.p25",
     .label = "P25 Options...",
     .help = "Control-channel hunting and follower behavior.",
     .submenu = TRUNK_P25_ITEMS,
     .submenu_len = sizeof TRUNK_P25_ITEMS / sizeof TRUNK_P25_ITEMS[0]},
    {.id = "trunk.rig",
     .label = "Rig Control...",
     .help = "External rig control settings.",
     .submenu = TRUNK_RIG_ITEMS,
     .submenu_len = sizeof TRUNK_RIG_ITEMS / sizeof TRUNK_RIG_ITEMS[0]},
    {.id = "trunk.lists",
     .label = "Lists & Filters...",
     .help = "Channel maps, groups, and tuning filters.",
     .submenu = TRUNK_LISTS_ITEMS,
     .submenu_len = sizeof TRUNK_LISTS_ITEMS / sizeof TRUNK_LISTS_ITEMS[0]},
    {.id = "trunk.tdma",
     .label = "DMR/TDMA...",
     .help = "TDMA slot controls and DMR late entry.",
     .submenu = TRUNK_TDMA_ITEMS,
     .submenu_len = sizeof TRUNK_TDMA_ITEMS / sizeof TRUNK_TDMA_ITEMS[0]},
};
const size_t TRUNK_MENU_ITEMS_LEN = sizeof TRUNK_MENU_ITEMS / sizeof TRUNK_MENU_ITEMS[0];

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

const NcMenuItem KEYS_MENU_ITEMS[] = {
    {.id = "keys.manage",
     .label = "Manage...",
     .help = "Enter/edit keys and priorities.",
     .submenu = KEYS_MANAGE_ITEMS,
     .submenu_len = sizeof KEYS_MANAGE_ITEMS / sizeof KEYS_MANAGE_ITEMS[0]},
    {.id = "keys.import",
     .label = "Import...",
     .help = "Import key CSV files.",
     .submenu = KEYS_IMPORT_ITEMS,
     .submenu_len = sizeof KEYS_IMPORT_ITEMS / sizeof KEYS_IMPORT_ITEMS[0]},
    {.id = "keys.ks",
     .label = "Keystreams...",
     .help = "Radio/vendor-specific derivations.",
     .submenu = KEYS_KS_ITEMS,
     .submenu_len = sizeof KEYS_KS_ITEMS / sizeof KEYS_KS_ITEMS[0]},
    {.id = "keys.m17",
     .label = "M17 Encoder...",
     .help = "Set M17 encoder IDs/user data.",
     .submenu = KEYS_M17_ITEMS,
     .submenu_len = sizeof KEYS_M17_ITEMS / sizeof KEYS_M17_ITEMS[0]},
};
const size_t KEYS_MENU_ITEMS_LEN = sizeof KEYS_MENU_ITEMS / sizeof KEYS_MENU_ITEMS[0];

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

const NcMenuItem UI_DISPLAY_MENU_ITEMS[] = {
    {.id = "ui.p25",
     .label = "P25 Sections...",
     .help = "Toggle P25-related on-screen sections.",
     .submenu = UI_DISPLAY_P25_ITEMS,
     .submenu_len = sizeof UI_DISPLAY_P25_ITEMS / sizeof UI_DISPLAY_P25_ITEMS[0]},
    {.id = "ui.general",
     .label = "General...",
     .help = "Other UI sections.",
     .submenu = UI_DISPLAY_GENERAL_ITEMS,
     .submenu_len = sizeof UI_DISPLAY_GENERAL_ITEMS / sizeof UI_DISPLAY_GENERAL_ITEMS[0]},
};
const size_t UI_DISPLAY_MENU_ITEMS_LEN = sizeof UI_DISPLAY_MENU_ITEMS / sizeof UI_DISPLAY_MENU_ITEMS[0];

// LRRP
static const NcMenuItem LRRP_STATUS_ITEMS[] = {
    {.id = "lrrp.status",
     .label = "LRRP Output",
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

const NcMenuItem LRRP_MENU_ITEMS[] = {
    {.id = "lrrp.status_menu",
     .label = "Status...",
     .help = "Shows current output target.",
     .submenu = LRRP_STATUS_ITEMS,
     .submenu_len = sizeof LRRP_STATUS_ITEMS / sizeof LRRP_STATUS_ITEMS[0]},
    {.id = "lrrp.dest",
     .label = "Destination...",
     .help = "Choose LRRP output file path.",
     .submenu = LRRP_DEST_ITEMS,
     .submenu_len = sizeof LRRP_DEST_ITEMS / sizeof LRRP_DEST_ITEMS[0]},
};
const size_t LRRP_MENU_ITEMS_LEN = sizeof LRRP_MENU_ITEMS / sizeof LRRP_MENU_ITEMS[0];

// DSP
#ifdef USE_RTLSDR
// Submenus for DSP groups
// Forward declare submenu visibility predicates
static bool dsp_filters_any(void* v);
static bool dsp_agc_any(void* v);
static bool dsp_ted_any(void* v);
static const NcMenuItem DSP_OVERVIEW_ITEMS[] = {
    {.id = "dsp.status",
     .label = "Show DSP Panel",
     .label_fn = lbl_dsp_panel,
     .help = "Toggle compact DSP status panel in main UI.",
     .on_select = act_toggle_dsp_panel},

};

static const NcMenuItem DSP_PATH_ITEMS[] = {
    {.id = "cqpsk",
     .label = "Toggle CQPSK",
     .label_fn = lbl_onoff_cq,
     .help = "Enable/disable CQPSK path.",
     .on_select = act_toggle_cq},
    {.id = "fll",
     .label = "Toggle FLL",
     .label_fn = lbl_onoff_fll,
     .help = "Enable/disable frequency-locked loop.",
     .is_enabled = is_fll_allowed,
     .on_select = act_toggle_fll},
    {.id = "cq_acq_fll",
     .label = "CQPSK Acquisition FLL",
     .label_fn = lbl_cqpsk_acq_fll,
     .help = "Pre-Costas pull-in FLL for CQPSK; auto-disables on lock.",
     .is_enabled = is_mod_qpsk,
     .on_select = act_toggle_cqpsk_acq_fll},
    {.id = "ted",
     .label = "Timing Error (TED)",
     .label_fn = lbl_onoff_ted,
     .help = "Toggle TED (symbol timing).",
     .is_enabled = is_ted_allowed,
     .on_select = act_toggle_ted},
    {.id = "ted_force",
     .label = "TED Force",
     .label_fn = lbl_ted_force,
     .help = "Force TED even for FM/C4FM/GFSK paths.",
     .is_enabled = is_mod_fm,
     .on_select = act_ted_force_toggle},
    {.id = "c4fm_clk",
     .label = "C4FM Clock Assist",
     .label_fn = lbl_c4fm_clk,
     .help = "Cycle C4FM timing assist: Off → EL → MM.",
     .is_enabled = is_mod_c4fm,
     .on_select = act_c4fm_clk_cycle},
    {.id = "c4fm_clk_sync",
     .label = "C4FM Clock While Synced",
     .label_fn = lbl_c4fm_clk_sync,
     .help = "Allow clock assist to remain active while synchronized.",
     .is_enabled = is_mod_c4fm,
     .on_select = act_c4fm_clk_sync_toggle},
};

static const NcMenuItem DSP_FILTER_ITEMS[] = {
    {.id = "rrc",
     .label = "RRC Filter",
     .label_fn = lbl_toggle_rrc,
     .help = "Toggle RRC used by Matched Filter (requires MF enabled).",
     .is_enabled = is_mod_qpsk,
     .on_select = act_toggle_rrc},
    {.id = "rrc_a+",
     .label = "RRC alpha +5%",
     .label_fn = lbl_rrc_a_up,
     .help = "Increase RRC alpha.",
     .is_enabled = is_mod_qpsk,
     .on_select = act_rrc_a_up},
    {.id = "rrc_a-",
     .label = "RRC alpha -5%",
     .label_fn = lbl_rrc_a_dn,
     .help = "Decrease RRC alpha.",
     .is_enabled = is_mod_qpsk,
     .on_select = act_rrc_a_dn},
    {.id = "rrc_s+",
     .label = "RRC span +1",
     .label_fn = lbl_rrc_s_up,
     .help = "Increase RRC span.",
     .is_enabled = is_mod_qpsk,
     .on_select = act_rrc_s_up},
    {.id = "rrc_s-",
     .label = "RRC span -1",
     .label_fn = lbl_rrc_s_dn,
     .help = "Decrease RRC span.",
     .is_enabled = is_mod_qpsk,
     .on_select = act_rrc_s_dn},
    {.id = "mf",
     .label = "Matched Filter (pre-Costas)",
     .label_fn = lbl_onoff_mf,
     .help = "Pre-Costas matched filter; uses RRC when enabled, else 5-tap fallback.",
     .is_enabled = is_mod_qpsk,
     .on_select = act_toggle_mf},
};

// Visible only when at least one filter/EQ item applies
static bool
dsp_filters_any(void* v) {
    return ui_submenu_has_visible(DSP_FILTER_ITEMS, sizeof DSP_FILTER_ITEMS / sizeof DSP_FILTER_ITEMS[0], v) ? true
                                                                                                             : false;
}

static const NcMenuItem DSP_IQ_ITEMS[] = {
    {.id = "iqb",
     .label = "IQ Balance",
     .label_fn = lbl_onoff_iqbal,
     .help = "Toggle IQ imbalance compensation.",
     .is_enabled = is_not_qpsk,
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
     .is_enabled = is_mod_fm,
     .on_select = act_toggle_fm_agc},

    {.id = "fm_lim",
     .label = "FM Limiter",
     .label_fn = lbl_fm_limiter,
     .help = "Toggle constant-envelope limiter.",
     .is_enabled = is_mod_fm,
     .on_select = act_toggle_fm_limiter},
    {.id = "fm_tgt",
     .label = "AGC Target (status)",
     .label_fn = lbl_fm_agc_target,
     .help = "Target RMS amplitude (int16).",
     .is_enabled = is_mod_fm},
    {.id = "fm_tgt+", .label = "AGC Target +500", .is_enabled = is_mod_fm, .on_select = act_fm_agc_target_up},
    {.id = "fm_tgt-", .label = "AGC Target -500", .is_enabled = is_mod_fm, .on_select = act_fm_agc_target_dn},
    {.id = "fm_min",
     .label = "AGC Min (status)",
     .label_fn = lbl_fm_agc_min,
     .help = "Min RMS to engage AGC.",
     .is_enabled = is_mod_fm},
    {.id = "fm_min+", .label = "AGC Min +500", .is_enabled = is_mod_fm, .on_select = act_fm_agc_min_up},
    {.id = "fm_min-", .label = "AGC Min -500", .is_enabled = is_mod_fm, .on_select = act_fm_agc_min_dn},
    {.id = "fm_au",
     .label = "AGC Alpha Up (status)",
     .label_fn = lbl_fm_agc_alpha_up,
     .help = "Smoothing when gain increases (Q15).",
     .is_enabled = is_mod_fm},
    {.id = "fm_au+", .label = "Alpha Up +1024", .is_enabled = is_mod_fm, .on_select = act_fm_agc_alpha_up_up},
    {.id = "fm_au-", .label = "Alpha Up -1024", .is_enabled = is_mod_fm, .on_select = act_fm_agc_alpha_up_dn},
    {.id = "fm_ad",
     .label = "AGC Alpha Down (status)",
     .label_fn = lbl_fm_agc_alpha_down,
     .help = "Smoothing when gain decreases (Q15).",
     .is_enabled = is_mod_fm},
    {.id = "fm_ad+", .label = "Alpha Down +1024", .is_enabled = is_mod_fm, .on_select = act_fm_agc_alpha_down_up},
    {.id = "fm_ad-", .label = "Alpha Down -1024", .is_enabled = is_mod_fm, .on_select = act_fm_agc_alpha_down_dn},
};

static bool
dsp_agc_any(void* v) {
    return ui_submenu_has_visible(DSP_AGC_ITEMS, sizeof DSP_AGC_ITEMS / sizeof DSP_AGC_ITEMS[0], v) ? true : false;
}

static const NcMenuItem DSP_TED_ITEMS[] = {
    {.id = "ted_sps",
     .label = "TED SPS (status)",
     .label_fn = lbl_ted_sps,
     .help = "Nominal samples-per-symbol.",
     .is_enabled = is_ted_allowed},
    {.id = "ted_sps+",
     .label = "TED SPS +1",
     .help = "Increase TED SPS.",
     .is_enabled = is_ted_allowed,
     .on_select = act_ted_sps_up},
    {.id = "ted_sps-",
     .label = "TED SPS -1",
     .help = "Decrease TED SPS.",
     .is_enabled = is_ted_allowed,
     .on_select = act_ted_sps_dn},
    {.id = "ted_gain_status",
     .label = "TED Gain (status)",
     .label_fn = lbl_ted_gain,
     .help = "TED small gain (Q20).",
     .is_enabled = is_ted_allowed},
    {.id = "ted_gain+",
     .label = "TED Gain +",
     .help = "Increase TED small gain.",
     .is_enabled = is_ted_allowed,
     .on_select = act_ted_gain_up},
    {.id = "ted_gain-",
     .label = "TED Gain -",
     .help = "Decrease TED small gain.",
     .is_enabled = is_ted_allowed,
     .on_select = act_ted_gain_dn},
    {.id = "ted_bias",
     .label = "TED Bias (status)",
     .label_fn = lbl_ted_bias,
     .help = "Smoothed Gardner residual (read-only).",
     .is_enabled = is_ted_allowed},
};

static bool
dsp_ted_any(void* v) {
    return ui_submenu_has_visible(DSP_TED_ITEMS, sizeof DSP_TED_ITEMS / sizeof DSP_TED_ITEMS[0], v) ? true : false;
}

const NcMenuItem DSP_MENU_ITEMS[] = {
    {.id = "dsp.overview",
     .label = "Overview...",
     .help = "Global toggles and status.",
     .submenu = DSP_OVERVIEW_ITEMS,
     .submenu_len = sizeof DSP_OVERVIEW_ITEMS / sizeof DSP_OVERVIEW_ITEMS[0]},
    {.id = "dsp.path",
     .label = "Signal Path & Timing...",
     .help = "Demod path selection and timing assists.",
     .submenu = DSP_PATH_ITEMS,
     .submenu_len = sizeof DSP_PATH_ITEMS / sizeof DSP_PATH_ITEMS[0]},
    {.id = "dsp.filters",
     .label = "Filtering & Equalizers...",
     .help = "RRC/MF.",
     .submenu = DSP_FILTER_ITEMS,
     .submenu_len = sizeof DSP_FILTER_ITEMS / sizeof DSP_FILTER_ITEMS[0],
     .is_enabled = dsp_filters_any},
    {.id = "dsp.iq",
     .label = "IQ & Front-End...",
     .help = "IQ balance and DC blocker.",
     .submenu = DSP_IQ_ITEMS,
     .submenu_len = sizeof DSP_IQ_ITEMS / sizeof DSP_IQ_ITEMS[0]},
    {.id = "dsp.agc",
     .label = "AGC & Limiter...",
     .help = "FM AGC, limiter, and parameters.",
     .submenu = DSP_AGC_ITEMS,
     .submenu_len = sizeof DSP_AGC_ITEMS / sizeof DSP_AGC_ITEMS[0],
     .is_enabled = dsp_agc_any},
    {.id = "dsp.ted",
     .label = "TED Controls...",
     .help = "Timing recovery parameters.",
     .submenu = DSP_TED_ITEMS,
     .submenu_len = sizeof DSP_TED_ITEMS / sizeof DSP_TED_ITEMS[0],
     .is_enabled = dsp_ted_any},
};
const size_t DSP_MENU_ITEMS_LEN = sizeof DSP_MENU_ITEMS / sizeof DSP_MENU_ITEMS[0];
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

// Provide explicit wrappers for prompts (avoid function pointer type mismatch)
static void
act_auto_ppm_snr_prompt(void* v) {
    double d = env_get_double("DSD_NEO_AUTO_PPM_SNR_DB", 6.0);
    ui_prompt_open_double_async("Auto-PPM SNR threshold (dB)", d, cb_auto_ppm_snr, v);
}

static void
act_auto_ppm_pwr_prompt(void* v) {
    double d = env_get_double("DSD_NEO_AUTO_PPM_PWR_DB", -80.0);
    ui_prompt_open_double_async("Auto-PPM min power (dB)", d, cb_auto_ppm_pwr, v);
}

static void
act_auto_ppm_zeroppm_prompt(void* v) {
    double p = env_get_double("DSD_NEO_AUTO_PPM_ZEROLOCK_PPM", 0.6);
    ui_prompt_open_double_async("Auto-PPM zero-lock PPM", p, cb_auto_ppm_zeroppm, v);
}

static void
act_auto_ppm_zerohz_prompt(void* v) {
    int h = env_get_int("DSD_NEO_AUTO_PPM_ZEROLOCK_HZ", 60);
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
    {.id = "env.edit",
     .label = "Set DSD_NEO_* Variable...",
     .help = "Edit any DSD_NEO_* environment variable.",
     .on_select = act_env_editor},
};

const NcMenuItem CONFIG_MENU_ITEMS[] = {
    {.id = "cfg.save_default",
     .label = "Save Config (Default)",
     .help = "Save current settings to the default config path.",
     .on_select = act_config_save_default},
    {.id = "cfg.load",
     .label = "Load Config...",
     .help = "Load settings from a config file into this session.",
     .on_select = act_config_load},
    {.id = "cfg.save_as",
     .label = "Save Config As...",
     .help = "Choose a path and save current settings.",
     .on_select = act_config_save_as},
};
const size_t CONFIG_MENU_ITEMS_LEN = sizeof CONFIG_MENU_ITEMS / sizeof CONFIG_MENU_ITEMS[0];

const NcMenuItem ADV_MENU_ITEMS[] = {
    {.id = "p25_follow",
     .label = "P25 Follower Tuning",
     .help = "Adjust P25 SM/follower timing parameters.",
     .submenu = P25_FOLLOW_ITEMS,
     .submenu_len = sizeof P25_FOLLOW_ITEMS / sizeof P25_FOLLOW_ITEMS[0]},
    {.id = "dsp_adv",
     .label = "DSP Advanced",
     .help = "Deemph, LPF, window freeze, FTZ/DAZ.",
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
const size_t ADV_MENU_ITEMS_LEN = sizeof ADV_MENU_ITEMS / sizeof ADV_MENU_ITEMS[0];

// main menu items now provided by src/ui/terminal/menus/menu_defs.c
