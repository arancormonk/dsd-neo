// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Dynamic label generators and visibility predicates for menu items.
 */

#include "menu_labels.h"
#include <dsd-neo/app_control/frontend.h>
#include <dsd-neo/app_control/history.h>
#include <dsd-neo/core/enc_lockout.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/io/tcp_input.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/decode_mode.h>
#include <dsd-neo/runtime/radioreference.h>
#include <dsd-neo/runtime/scan_mode.h>
#include <stdint.h>
#include <string.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "dsd-neo/platform/sockets.h"
#include "dsd-neo/runtime/call_alert.h"
#include "menu_env.h"
#include "menu_internal.h"
#include "ui_key_status.h"

static const char*
onoff(int on) {
    return on ? "On" : "Off";
}

// ---- Visibility/predicate functions ----

static int
menu_audio_in_is_soapy(const dsd_opts* opts) {
    const char* dev = opts ? opts->audio_in_dev : NULL;
    if (!dev) {
        return 0;
    }
    return (strcmp(dev, "soapy") == 0) || (strncmp(dev, "soapy:", 6) == 0);
}

bool
io_rtl_active(const void* ctx) {
    const UiCtx* c = (const UiCtx*)ctx;
    if (!c || !c->opts) {
        return false;
    }
    return (c->opts->audio_in_type == AUDIO_IN_RTL);
}

bool
trunk_enabled(const void* ctx) {
    const UiCtx* c = (const UiCtx*)ctx;
    if (!c || !c->opts) {
        return false;
    }
    /* A conventional --trunk-scan target parks with trunk_enable off, but Next channel still
       has somewhere to go: the next target. */
    return c->opts->trunk_enable == 1 || c->opts->scanner_mode == 1 || c->opts->trunk_scan_enabled == 1;
}

/* Hold and avoid act on a rotation: the -Y scan list or the --trunk-scan target list. Plain
   -T follows one system and has nothing to hold or avoid at channel scope. */
bool
scan_rotation_active(const void* ctx) {
    const UiCtx* c = (const UiCtx*)ctx;
    if (!c || !c->opts) {
        return false;
    }
    return c->opts->scanner_mode == 1 || c->opts->trunk_scan_enabled == 1;
}

bool
provoice_active(const void* ctx) {
    const UiCtx* c = (const UiCtx*)ctx;
    return c && c->opts && c->opts->frame_provoice == 1;
}

/* The commands behind the constellation and eye sub-options refuse to do anything
   unless their parent view is up (DSD_APP_CMD_CONST_NORM_TOGGLE,
   DSD_APP_CMD_EYE_UNICODE_TOGGLE, DSD_APP_CMD_EYE_COLOR_TOGGLE all test it). Without
   these gates the rows are selectable and Enter is a silent no-op. */
bool
const_view_active(const void* ctx) {
    const UiCtx* c = (const UiCtx*)ctx;
    return c && c->opts && c->opts->frontend_display.constellation == 1;
}

bool
eye_view_active(const void* ctx) {
    const UiCtx* c = (const UiCtx*)ctx;
    return c && c->opts && c->opts->frontend_display.eye_view == 1;
}

bool
rr_feature_available(const void* ctx) {
    (void)ctx;
    return dsd_rr_available() != 0;
}

bool
rr_key_prompt_offered(const void* ctx) {
    (void)ctx;
    const char* key = dsd_rr_builtin_app_key();
    if (key == NULL) {
        return true;
    }
    return key[0] == '\0';
}

bool
rr_imports_available(const void* ctx) {
    /* Reports whether an imports directory RESOLVES - whether XDG_CONFIG_HOME /
       HOME (POSIX) or APPDATA (Windows) gave a config root - deliberately not
       whether any import exists. Predicates run on every menu render (up to
       15 FPS), and listing the directory plus reading a sidecar per entry at
       that rate is filesystem traffic for no benefit; the empty case is
       reported once, on activation, by rr_panel_open_library(). */
    (void)ctx;
    const char* dir = dsd_user_imports_dir();
    if (dir == NULL) {
        return false;
    }
    return dir[0] != '\0';
}

/* The RadioReference rows sit inside the Channels & groups submenu, so each
   carries the feature gate itself rather than a parent row doing it. */
bool
rr_imports_available_and_feature(const void* ctx) {
    return rr_feature_available(ctx) && rr_imports_available(ctx);
}

bool
rr_key_prompt_offered_and_feature(const void* ctx) {
    return rr_feature_available(ctx) && rr_key_prompt_offered(ctx);
}

#ifdef USE_RADIO
static dsd_frontend_metrics
menu_frontend_metrics(const void* v) {
    (void)v;
    dsd_frontend_metrics metrics;
    (void)dsd_app_frontend_get_metrics(&metrics);
    return metrics;
}

static bool
rtl_fsk_symbol_output_active_for_ui(const void* v) {
    dsd_frontend_metrics metrics = menu_frontend_metrics(v);
    return metrics.output_kind == DSD_FRONTEND_RTL_OUTPUT_FSK_DISCRIMINATOR;
}

int
ui_current_mod(const void* v) {
    const UiCtx* c = (const UiCtx*)v;
    int mod = -1;

    // Honor an explicitly locked demod selection when present
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
    dsd_frontend_metrics metrics = menu_frontend_metrics(v);
    if (metrics.cqpsk_enable) {
        mod = 1;
    }

    // Fallback: default to the 4-level FSK family (or GFSK when hinted)
    if (mod < 0) {
        mod = 0;
    }
    return mod;
}

bool
is_mod_qpsk(const void* v) {
    return ui_current_mod(v) == 1;
}

bool
is_not_qpsk(const void* v) {
    return !is_mod_qpsk(v);
}

bool
is_ted_allowed(const void* v) {
    return !rtl_fsk_symbol_output_active_for_ui(v) && is_mod_qpsk(v);
}
#endif

// ---- Decoder labels ----

const char*
lbl_decode_mode(const void* v, char* b, size_t n) {
    const UiCtx* c = (const UiCtx*)v;
    dsdneoUserDecodeMode mode = (c && c->opts) ? dsd_scan_mode_configured_preset(c->opts, c->state) : DSDCFG_MODE_AUTO;
    /* "..." because the row opens a picker; without it the grammar promises a toggle. */
    const dsd_scan_mode active = c ? dsd_scan_mode_active(c->state) : DSD_SCAN_MODE_INHERIT;
    if (active != DSD_SCAN_MODE_INHERIT) {
        DSD_SNPRINTF(b, n, "Mode... [%s; scan %s]", dsd_decode_mode_display_name(mode), dsd_scan_mode_name(active));
    } else {
        DSD_SNPRINTF(b, n, "Mode... [%s]", dsd_decode_mode_display_name(mode));
    }
    return b;
}

const char*
lbl_modulation(const void* v, char* b, size_t n) {
    const UiCtx* c = (const UiCtx*)v;
    int mod = -1;
    if (c && c->state && c->state->rf_mod >= 0 && c->state->rf_mod <= 2) {
        mod = c->state->rf_mod;
    } else if (c && c->opts) {
        mod = dsd_opts_modulation(c->opts);
    }
    const char* name = (mod == 1) ? "QPSK" : ((mod == 2) ? "GFSK" : "C4FM");
    DSD_SNPRINTF(b, n, "Modulation [%s]", name);
    return b;
}

const char*
lbl_p25p2_mod_lock(const void* v, char* b, size_t n) {
    const UiCtx* c = (const UiCtx*)v;
    const char* s = "Off";
    /* Which modulation the lock pinned, read from the modulation itself. Reading
       opts->mod_p25p2_c4fm instead reported QPSK forever: ui_handle_mod_p2_toggle()
       -- the only thing this row and its 'M' hotkey run -- clears that flag on every
       press and expresses the choice through mod_qpsk/rf_mod. The flag is a CLI-only
       spelling of "P25p2 C4FM at 6000 sps", so it still counts as a lock. */
    if (c && c->opts && (c->opts->mod_p25p2_profile_lock || c->opts->mod_p25p2_c4fm)) {
        int qpsk = (c->opts->mod_qpsk != 0);
        if (c->state && c->state->rf_mod >= 0 && c->state->rf_mod <= 2) {
            qpsk = (c->state->rf_mod == 1);
        }
        if (c->opts->mod_p25p2_c4fm) {
            qpsk = 0;
        }
        s = qpsk ? "QPSK" : "C4FM";
    }
    DSD_SNPRINTF(b, n, "P25 Phase 2 modulation lock [%s]", s);
    return b;
}

const char*
lbl_lpf(const void* v, char* b, size_t n) {
    const UiCtx* c = (const UiCtx*)v;
    DSD_SNPRINTF(b, n, "Low-pass filter [%s]", onoff(c && c->opts && c->opts->use_lpf));
    return b;
}

const char*
lbl_hpf(const void* v, char* b, size_t n) {
    const UiCtx* c = (const UiCtx*)v;
    DSD_SNPRINTF(b, n, "High-pass filter [%s]", onoff(c && c->opts && c->opts->use_hpf));
    return b;
}

const char*
lbl_pbf(const void* v, char* b, size_t n) {
    const UiCtx* c = (const UiCtx*)v;
    DSD_SNPRINTF(b, n, "Pulse-shaping band-pass [%s]", onoff(c && c->opts && c->opts->use_pbf));
    return b;
}

const char*
lbl_hpf_d(const void* v, char* b, size_t n) {
    const UiCtx* c = (const UiCtx*)v;
    DSD_SNPRINTF(b, n, "Digital high-pass filter [%s]", onoff(c && c->opts && c->opts->use_hpf_d));
    return b;
}

const char*
lbl_crc_relax(const void* v, char* b, size_t n) {
    const UiCtx* c = (const UiCtx*)v;
    int relaxed = (c->opts->aggressive_framesync == 0);
    DSD_SNPRINTF(b, n, "Relaxed CRC checks [%s]", onoff(relaxed));
    return b;
}

const char*
lbl_dmr_le(const void* v, char* b, size_t n) {
    const UiCtx* c = (const UiCtx*)v;
    DSD_SNPRINTF(b, n, "DMR late entry [%s]", onoff(c->opts->dmr_le));
    return b;
}

const char*
lbl_slot1(const void* v, char* b, size_t n) {
    const UiCtx* c = (const UiCtx*)v;
    DSD_SNPRINTF(b, n, "Slot 1 audio [%s]", onoff(c && c->opts && c->opts->slot1_on));
    return b;
}

const char*
lbl_slot2(const void* v, char* b, size_t n) {
    const UiCtx* c = (const UiCtx*)v;
    DSD_SNPRINTF(b, n, "Slot 2 audio [%s]", onoff(c && c->opts && c->opts->slot2_on));
    return b;
}

const char*
lbl_slotpref(const void* v, char* b, size_t n) {
    const UiCtx* c = (const UiCtx*)v;
    const char* now = (c->opts->slot_preference == 0) ? "1" : (c->opts->slot_preference == 1) ? "2" : "Auto";
    DSD_SNPRINTF(b, n, "Slot preference... [%s]", now);
    return b;
}

const char*
lbl_provoice_esk(const void* v, char* b, size_t n) {
    const UiCtx* c = (const UiCtx*)v;
    DSD_SNPRINTF(b, n, "ProVoice ESK mask [%s]", onoff(c && c->state && c->state->esk_mask != 0));
    return b;
}

const char*
lbl_provoice_mode(const void* v, char* b, size_t n) {
    const UiCtx* c = (const UiCtx*)v;
    DSD_SNPRINTF(b, n, "ProVoice EA mode [%s]", onoff(c && c->state && c->state->ea_mode != 0));
    return b;
}

const char*
lbl_invert_all(const void* v, char* b, size_t n) {
    const UiCtx* c = (const UiCtx*)v;
    DSD_SNPRINTF(b, n, "Invert signal [%s]", onoff(c->opts->inverted_dmr));
    return b;
}

const char*
lbl_inv_x2(const void* v, char* b, size_t n) {
    const UiCtx* c = (const UiCtx*)v;
    DSD_SNPRINTF(b, n, "Invert X2-TDMA [%s]", onoff(c->opts->inverted_x2tdma));
    return b;
}

const char*
lbl_inv_dmr(const void* v, char* b, size_t n) {
    const UiCtx* c = (const UiCtx*)v;
    DSD_SNPRINTF(b, n, "Invert DMR [%s]", onoff(c->opts->inverted_dmr));
    return b;
}

const char*
lbl_inv_dpmr(const void* v, char* b, size_t n) {
    const UiCtx* c = (const UiCtx*)v;
    DSD_SNPRINTF(b, n, "Invert dPMR [%s]", onoff(c->opts->inverted_dpmr));
    return b;
}

const char*
lbl_inv_m17(const void* v, char* b, size_t n) {
    const UiCtx* c = (const UiCtx*)v;
    DSD_SNPRINTF(b, n, "Invert M17 [%s]", onoff(c->opts->inverted_m17));
    return b;
}

const char*
lbl_m17_user_data(const void* v, char* b, size_t n) {
    const UiCtx* c = (const UiCtx*)v;
    const char* s = (c && c->state && c->state->m17dat[0]) ? c->state->m17dat : "<unset>";
    int m = (int)n - 28;
    if (m < 0) {
        m = 0;
    }
    DSD_SNPRINTF(b, n, "M17 encoder user data... [%.*s]", m, s);
    return b;
}

// ---- Trunking labels ----

const char*
lbl_trunk(const void* v, char* b, size_t n) {
    const UiCtx* c = (const UiCtx*)v;
    DSD_SNPRINTF(b, n, "Trunking [%s]", onoff(c->opts->trunk_enable));
    return b;
}

const char*
lbl_scan(const void* v, char* b, size_t n) {
    const UiCtx* c = (const UiCtx*)v;
    DSD_SNPRINTF(b, n, "Conventional scanning [%s]", onoff(c->opts->scanner_mode));
    return b;
}

const char*
lbl_lcw(const void* v, char* b, size_t n) {
    const UiCtx* c = (const UiCtx*)v;
    DSD_SNPRINTF(b, n, "LCW explicit retune [%s]", onoff(c->opts->p25_lcw_retune));
    return b;
}

const char*
lbl_allow(const void* v, char* b, size_t n) {
    const UiCtx* c = (const UiCtx*)v;
    DSD_SNPRINTF(b, n, "Allow-list mode [%s]", onoff(c->opts->trunk_use_allow_list));
    return b;
}

const char*
lbl_tune_group(const void* v, char* b, size_t n) {
    const UiCtx* c = (const UiCtx*)v;
    DSD_SNPRINTF(b, n, "Group calls [%s]", onoff(c->opts->trunk_tune_group_calls));
    return b;
}

const char*
lbl_tune_priv(const void* v, char* b, size_t n) {
    const UiCtx* c = (const UiCtx*)v;
    DSD_SNPRINTF(b, n, "Private calls [%s]", onoff(c->opts->trunk_tune_private_calls));
    return b;
}

const char*
lbl_tune_data(const void* v, char* b, size_t n) {
    const UiCtx* c = (const UiCtx*)v;
    DSD_SNPRINTF(b, n, "Data calls [%s]", onoff(c->opts->trunk_tune_data_calls));
    return b;
}

const char*
lbl_tg_hold(const void* v, char* b, size_t n) {
    const UiCtx* c = (const UiCtx*)v;
    const uint32_t tg = (c && c->state) ? c->state->tg_hold : 0U;
    if (tg != 0U) {
        DSD_SNPRINTF(b, n, "Talkgroup hold... [%u]", (unsigned)tg);
    } else {
        DSD_SNPRINTF(b, n, "Talkgroup hold... [none]");
    }
    return b;
}

const char*
lbl_hangtime(const void* v, char* b, size_t n) {
    const UiCtx* c = (const UiCtx*)v;
    const double s = (c && c->opts) ? (double)c->opts->trunk_hangtime : 0.0;
    DSD_SNPRINTF(b, n, "Hangtime... [%.1f s]", s);
    return b;
}

const char*
lbl_scan_voice_only(const void* v, char* b, size_t n) {
    const UiCtx* c = (const UiCtx*)v;
    DSD_SNPRINTF(b, n, "Voice-only scan [%s]", onoff(c && c->opts && c->opts->scan_voice_only));
    return b;
}

const char*
lbl_scan_voice_qualify(const void* v, char* b, size_t n) {
    const UiCtx* c = (const UiCtx*)v;
    const int ms = (c && c->opts) ? c->opts->scan_voice_qualify_ms : 0;
    DSD_SNPRINTF(b, n, "Voice qualify... [%d ms]", ms);
    return b;
}

const char*
lbl_scan_voice_hold(const void* v, char* b, size_t n) {
    const UiCtx* c = (const UiCtx*)v;
    const int ms = (c && c->opts) ? c->opts->scan_voice_hold_ms : 0;
    DSD_SNPRINTF(b, n, "Voice hold... [%d ms]", ms);
    return b;
}

const char*
lbl_rev_mute(const void* v, char* b, size_t n) {
    const UiCtx* c = (const UiCtx*)v;
    DSD_SNPRINTF(b, n, "Reverse mute [%s]", onoff(c->opts->reverse_mute));
    return b;
}

const char*
lbl_pref_cc(const void* v, char* b, size_t n) {
    const UiCtx* c = (const UiCtx*)v;
    DSD_SNPRINTF(b, n, "Prefer CC candidates [%s]", onoff(c->opts->p25_prefer_candidates));
    return b;
}

const char*
lbl_rigctl(const void* vctx, char* b, size_t n) {
    const UiCtx* c = (const UiCtx*)vctx;
    int connected = (c->opts->use_rigctl && c->opts->rigctl_sockfd != DSD_INVALID_SOCKET);
    if (c->opts->rigctlhostname[0] != '\0' && c->opts->rigctlportno > 0) {
        int m = (n > 24) ? (int)(n - 24) : 0;
        DSD_SNPRINTF(b, n, "Rigctl: %.*s:%d [%s]", m, c->opts->rigctlhostname, c->opts->rigctlportno, onoff(connected));
    } else {
        DSD_SNPRINTF(b, n, connected ? "Rigctl [On]" : "Rigctl... [Off]");
    }
    return b;
}

const char*
lbl_rr_account(const void* v, char* b, size_t n) {
    const UiCtx* c = (const UiCtx*)v;
    const char* user = (c && c->opts && c->opts->rr_username[0] != '\0') ? c->opts->rr_username : "(not set)";
    int m = (n > 32) ? (int)(n - 32) : 0;
    DSD_SNPRINTF(b, n, "RadioReference username... [%.*s]", m, user);
    return b;
}

static double
lbl_p25_num(const void* v, const char* env_name, double defv) {
    (void)v;
    return env_get_double(env_name, defv);
}

const char*
lbl_p25_vc_grace(const void* v, char* b, size_t n) {
    DSD_SNPRINTF(b, n, "VC grace... [%.3f s]", lbl_p25_num(v, "DSD_NEO_P25_VC_GRACE", 0.0));
    return b;
}

const char*
lbl_p25_min_follow(const void* v, char* b, size_t n) {
    DSD_SNPRINTF(b, n, "Minimum follow dwell... [%.3f s]", lbl_p25_num(v, "DSD_NEO_P25_MIN_FOLLOW_DWELL", 0.0));
    return b;
}

const char*
lbl_p25_grant_voice(const void* v, char* b, size_t n) {
    DSD_SNPRINTF(b, n, "Grant-to-voice timeout... [%.3f s]", lbl_p25_num(v, "DSD_NEO_P25_GRANT_VOICE_TO", 0.0));
    return b;
}

const char*
lbl_p25_cc_grace(const void* v, char* b, size_t n) {
    DSD_SNPRINTF(b, n, "CC hunt grace... [%.3f s]", lbl_p25_num(v, "DSD_NEO_P25_CC_GRACE", 0.0));
    return b;
}

const char*
lbl_p25_force_extra(const void* v, char* b, size_t n) {
    DSD_SNPRINTF(b, n, "Safety-net extra... [%.3f s]", lbl_p25_num(v, "DSD_NEO_P25_FORCE_RELEASE_EXTRA", 0.0));
    return b;
}

const char*
lbl_p25_force_margin(const void* v, char* b, size_t n) {
    DSD_SNPRINTF(b, n, "Safety-net margin... [%.3f s]", lbl_p25_num(v, "DSD_NEO_P25_FORCE_RELEASE_MARGIN", 0.0));
    return b;
}

const char*
lbl_p25_p1_err_pct(const void* v, char* b, size_t n) {
    DSD_SNPRINTF(b, n, "Phase 1 error hold... [%.1f%%]", lbl_p25_num(v, "DSD_NEO_P25P1_ERR_HOLD_PCT", 0.0));
    return b;
}

const char*
lbl_p25_p1_err_sec(const void* v, char* b, size_t n) {
    DSD_SNPRINTF(b, n, "Phase 1 error hold time... [%.3f s]", lbl_p25_num(v, "DSD_NEO_P25P1_ERR_HOLD_S", 0.0));
    return b;
}

// ---- Encryption labels ----

const char*
lbl_muting(const void* v, char* b, size_t n) {
    const UiCtx* c = (const UiCtx*)v;
    int dmr = (c->opts->dmr_mute_encL == 1 && c->opts->dmr_mute_encR == 1);
    int p25 = (c->opts->unmute_encrypted_p25 == 0);
    DSD_SNPRINTF(b, n, "Mute encrypted audio [%s]", onoff(dmr && p25));
    return b;
}

const char*
lbl_p25_enc_lockout(const void* v, char* b, size_t n) {
    const UiCtx* c = (const UiCtx*)v;
    int on = (c && c->opts) ? ((c->opts->trunk_tune_enc_calls == 0) ? 1 : 0) : 0;
    DSD_SNPRINTF(b, n, "Lock out encrypted calls [%s]", onoff(on));
    return b;
}

const char*
lbl_scan_hold(const void* v, char* b, size_t n) {
    const UiCtx* c = (const UiCtx*)v;
    int on = 0;
    if (c && c->opts && c->state) {
        on = (c->opts->trunk_scan_enabled == 1) ? (c->state->trunk_scan_hold != 0) : (c->state->lcn_scan_hold != 0);
    }
    DSD_SNPRINTF(b, n, "Scan hold [%s]", onoff(on));
    return b;
}

const char*
lbl_scan_avoid_clear(const void* v, char* b, size_t n) {
    const UiCtx* c = (const UiCtx*)v;
    unsigned count = 0;
    if (c && c->opts && c->state) {
        count = (c->opts->trunk_scan_enabled == 1) ? c->state->trunk_scan_avoided_count : c->state->lcn_avoid_count;
    }
    DSD_SNPRINTF(b, n, "Clear avoids [%u]", count);
    return b;
}

const char*
lbl_enc_lockout_clear(const void* v, char* b, size_t n) {
    const UiCtx* c = (const UiCtx*)v;
    const int count = (c && c->state) ? dsd_enc_lockout_active_count(c->state) : 0;
    DSD_SNPRINTF(b, n, "Clear lockouts [%d]", count);
    return b;
}

const char*
lbl_key_force_bp(const void* v, char* b, size_t n) {
    const UiCtx* c = (const UiCtx*)v;
    const int on = (c && c->state && c->state->M == 1);
    DSD_SNPRINTF(b, n, "Force basic/scrambler key [%s]", onoff(on));
    return b;
}

const char*
lbl_key_force_rc4(const void* v, char* b, size_t n) {
    const UiCtx* c = (const UiCtx*)v;
    const int on = (c && c->state && c->state->M == 0x21);
    DSD_SNPRINTF(b, n, "Force RC4 key [%s]", onoff(on));
    return b;
}

const char*
lbl_key_hytera(const void* v, char* b, size_t n) {
    const UiCtx* c = (const UiCtx*)v;
    if (!c || !c->state) {
        DSD_SNPRINTF(b, n, "Hytera privacy key (hex)...");
        return b;
    }
    const dsd_state* s = c->state;
    const int loaded = (s->H != 0ULL && s->tyt_bp == 0);
    if (!loaded) {
        DSD_SNPRINTF(b, n, "Hytera privacy key (hex)...");
        return b;
    }
    const unsigned int segment_count = ui_hytera_key_segment_count(s);
    const char* kind = (segment_count == 1U) ? "40-bit" : ((segment_count == 2U) ? "128-bit" : "256-bit");
    DSD_SNPRINTF(b, n, "Hytera privacy key (hex)... [%s]", kind);
    return b;
}

// ---- Input / Audio labels ----

const char*
lbl_current_output(const void* vctx, char* b, size_t n) {
    const UiCtx* c = (const UiCtx*)vctx;
    const char* name;
    switch (c->opts->audio_out_type) {
        case 0: name = "Pulse Digital"; break;
        case 8: name = "UDP"; break;
        default: name = "?"; break;
    }
    if (c->opts->audio_out_type == 0) {
        if (c->opts->pa_output_idx[0]) {
            size_t prefix = strlen("Output: Pulse []") - 2; /* exclude %s */
            int m = (n > prefix) ? (int)(n - prefix) : 0;
            DSD_SNPRINTF(b, n, "Output: Pulse [%.*s]", m, c->opts->pa_output_idx);
        } else {
            DSD_SNPRINTF(b, n, "Output: Pulse [default]");
        }
    } else if (c->opts->audio_out_type == 8) {
        int m = (n > 24) ? (int)(n - 24) : 0; /* leave room for prefix and port */
        DSD_SNPRINTF(b, n, "Output: UDP %.*s:%d", m, c->opts->udp_hostname, c->opts->udp_portno);
    } else {
        DSD_SNPRINTF(b, n, "Output: %s", name);
    }
    return b;
}

static const char*
lbl_current_input_tcp(const dsd_opts* opts, char* b, size_t n) {
    int m = (n > 24) ? (int)(n - 24) : 0;
    DSD_SNPRINTF(b, n, "Source: TCP %.*s:%d", m, opts->tcp_hostname, opts->tcp_portno);
    return b;
}

static const char*
lbl_current_input_udp(const dsd_opts* opts, char* b, size_t n) {
    const char* addr = opts->udp_in_bindaddr[0] ? opts->udp_in_bindaddr : "127.0.0.1";
    int m = (n > 24) ? (int)(n - 24) : 0;
    DSD_SNPRINTF(b, n, "Source: UDP %.*s:%d", m, addr, opts->udp_in_portno);
    return b;
}

static const char*
lbl_current_input_file_like(const dsd_opts* opts, char* b, size_t n) {
    int m = (n > 10) ? (int)(n - 10) : 0;
    DSD_SNPRINTF(b, n, "Source: %.*s", m, opts->audio_in_dev);
    return b;
}

static const char*
lbl_current_input_rtl(const dsd_opts* opts, char* b, size_t n) {
    if (menu_audio_in_is_soapy(opts)) {
        if (strncmp(opts->audio_in_dev, "soapy:", 6) == 0 && opts->audio_in_dev[6] != '\0') {
            size_t prefix = strlen("Source: SoapySDR []") - 2;
            int m = (n > prefix) ? (int)(n - prefix) : 0;
            DSD_SNPRINTF(b, n, "Source: SoapySDR [%.*s]", m, opts->audio_in_dev + 6);
        } else {
            DSD_SNPRINTF(b, n, "Source: SoapySDR");
        }
        return b;
    }

    DSD_SNPRINTF(b, n, "Source: RTL-SDR dev %d", opts->rtl_dev_index);
    return b;
}

const char*
lbl_current_input(const void* vctx, char* b, size_t n) {
    const UiCtx* c = (const UiCtx*)vctx;
    if (c == NULL || c->opts == NULL) {
        DSD_SNPRINTF(b, n, "Source: ?");
        return b;
    }

    const dsd_opts* opts = c->opts;
    switch (opts->audio_in_type) {
        case AUDIO_IN_TCP: return lbl_current_input_tcp(opts, b, n);
        case AUDIO_IN_UDP: return lbl_current_input_udp(opts, b, n);
        case AUDIO_IN_WAV:
        case AUDIO_IN_SYMBOL_BIN:
        case AUDIO_IN_SYMBOL_FLT: return lbl_current_input_file_like(opts, b, n);
        case AUDIO_IN_RTL: return lbl_current_input_rtl(opts, b, n);
        case AUDIO_IN_PULSE: DSD_SNPRINTF(b, n, "Source: Pulse"); return b;
        case AUDIO_IN_STDIN: DSD_SNPRINTF(b, n, "Source: STDIN"); return b;
        default: DSD_SNPRINTF(b, n, "Source: ?"); return b;
    }
}

const char*
lbl_input_volume(const void* vctx, char* b, size_t n) {
    const UiCtx* c = (const UiCtx*)vctx;
    int m = c->opts->input_volume_multiplier;
    if (m < 1) {
        m = 1;
    }
    DSD_SNPRINTF(b, n, "Input volume... [%dX]", m);
    return b;
}

const char*
lbl_input_warn(const void* v, char* b, size_t n) {
    const UiCtx* c = (const UiCtx*)v;
    double thr = c ? c->opts->input_warn_db : env_get_double("DSD_NEO_INPUT_WARN_DB", -40.0);
    DSD_SNPRINTF(b, n, "Low-input warning... [%.1f dBFS]", thr);
    return b;
}

const char*
lbl_tcp(const void* vctx, char* b, size_t n) {
    const UiCtx* c = (const UiCtx*)vctx;
    int active = (c->opts->audio_in_type == AUDIO_IN_TCP && tcp_input_is_valid(c->opts->tcp_in_ctx));
    if (c->opts->tcp_hostname[0] != '\0' && c->opts->tcp_portno > 0) {
        int m = (n > 28) ? (int)(n - 28) : 0;
        DSD_SNPRINTF(b, n, "TCP audio: %.*s:%d [%s]", m, c->opts->tcp_hostname, c->opts->tcp_portno, onoff(active));
    } else {
        DSD_SNPRINTF(b, n, active ? "TCP audio [On]" : "TCP audio... [Off]");
    }
    return b;
}

const char*
lbl_out_mute(const void* vctx, char* b, size_t n) {
    const UiCtx* c = (const UiCtx*)vctx;
    DSD_SNPRINTF(b, n, "Mute [%s]", onoff(c->opts->audio_out == 0));
    return b;
}

const char*
lbl_gain_dig(const void* v, char* b, size_t n) {
    const UiCtx* c = (const UiCtx*)v;
    const float g = (c && c->opts) ? c->opts->audio_gain : 0.0f;
    if (g <= 0.0f) {
        DSD_SNPRINTF(b, n, "Digital gain... [auto]");
    } else {
        DSD_SNPRINTF(b, n, "Digital gain... [%d]", (int)g);
    }
    return b;
}

const char*
lbl_gain_ana(const void* v, char* b, size_t n) {
    const UiCtx* c = (const UiCtx*)v;
    const float g = (c && c->opts) ? c->opts->audio_gainA : 0.0f;
    DSD_SNPRINTF(b, n, "Analog gain... [%d]", (int)g);
    return b;
}

const char*
lbl_monitor(const void* v, char* b, size_t n) {
    const UiCtx* c = (const UiCtx*)v;
    DSD_SNPRINTF(b, n, "Source audio monitor [%s]", onoff(c->opts->monitor_input_audio));
    return b;
}

const char*
lbl_cosine(const void* v, char* b, size_t n) {
    const UiCtx* c = (const UiCtx*)v;
    DSD_SNPRINTF(b, n, "Cosine filter [%s]", onoff(c->opts->use_cosine_filter));
    return b;
}

const char*
lbl_deemph(const void* v, char* b, size_t n) {
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
    DSD_SNPRINTF(b, n, "Deemphasis [%s]", s);
    return b;
}

const char*
lbl_audio_lpf(const void* v, char* b, size_t n) {
    (void)v;
    const dsdneoRuntimeConfig* cfg = dsd_neo_get_config();
    if (cfg && cfg->audio_lpf_is_set && !cfg->audio_lpf_disable && cfg->audio_lpf_cutoff_hz > 0) {
        DSD_SNPRINTF(b, n, "Audio low-pass... [%d Hz]", cfg->audio_lpf_cutoff_hz);
    } else {
        DSD_SNPRINTF(b, n, "Audio low-pass... [Off]");
    }
    return b;
}

const char*
lbl_call_alert(const void* v, char* b, size_t n) {
    const UiCtx* c = (const UiCtx*)v;
    DSD_SNPRINTF(b, n, "Call alert beep [%s]", onoff(c->opts->call_alert));
    return b;
}

static const char*
call_alert_events_name(uint8_t events) {
    switch (events & DSD_CALL_ALERT_EVENT_ALL) {
        case DSD_CALL_ALERT_EVENT_VOICE_START: return "Start";
        case DSD_CALL_ALERT_EVENT_VOICE_END: return "End";
        case DSD_CALL_ALERT_EVENT_DATA: return "Data";
        case DSD_CALL_ALERT_EVENT_VOICE_START | DSD_CALL_ALERT_EVENT_VOICE_END: return "Start+End";
        case DSD_CALL_ALERT_EVENT_VOICE_START | DSD_CALL_ALERT_EVENT_DATA: return "Start+Data";
        case DSD_CALL_ALERT_EVENT_VOICE_END | DSD_CALL_ALERT_EVENT_DATA: return "End+Data";
        case DSD_CALL_ALERT_EVENT_ALL: return "All";
        default: return "Off";
    }
}

const char*
lbl_call_alert_events(const void* v, char* b, size_t n) {
    const UiCtx* c = (const UiCtx*)v;
    uint8_t events = dsd_call_alert_effective_events(c->opts->call_alert, c->opts->call_alert_events);
    DSD_SNPRINTF(b, n, "Alert on... [%s]", call_alert_events_name(events));
    return b;
}

// ---- Recording & logs labels ----

const char*
lbl_sym_save(const void* vctx, char* b, size_t n) {
    const UiCtx* c = (const UiCtx*)vctx;
    if (c->opts->symbol_out_f) {
        size_t prefix = strlen("Record symbols... []") - 2; /* exclude %s */
        int m = (n > prefix) ? (int)(n - prefix) : 0;
        DSD_SNPRINTF(b, n, "Record symbols... [%.*s]", m, c->opts->symbol_out_file);
    } else {
        DSD_SNPRINTF(b, n, "Record symbols... [Off]");
    }
    return b;
}

const char*
lbl_stop_symbol_capture(const void* vctx, char* b, size_t n) {
    const UiCtx* c = (const UiCtx*)vctx;
    if (c->opts->symbol_out_f) {
        if (c->opts->symbol_out_file[0] != '\0') {
            DSD_SNPRINTF(b, n, "Stop recording [%s]", c->opts->symbol_out_file);
        } else {
            DSD_SNPRINTF(b, n, "Stop recording [On]");
        }
    } else {
        DSD_SNPRINTF(b, n, "Stop recording [Off]");
    }
    return b;
}

const char*
lbl_replay_last(const void* vctx, char* b, size_t n) {
    const UiCtx* c = (const UiCtx*)vctx;
    if (c->opts->audio_in_dev[0] != '\0') {
        dsd_stat_t sb;
        if (dsd_stat_path(c->opts->audio_in_dev, &sb) == 0) {
            DSD_SNPRINTF(b, n, "Replay last capture [%s]", c->opts->audio_in_dev);
            return b;
        }
    }
    DSD_SNPRINTF(b, n, "Replay last capture [none]");
    return b;
}

const char*
lbl_stop_symbol_playback(const void* vctx, char* b, size_t n) {
    const UiCtx* c = (const UiCtx*)vctx;
    if (c->opts->symbolfile != NULL && c->opts->audio_in_type == AUDIO_IN_SYMBOL_BIN) {
        if (c->opts->audio_in_dev[0] != '\0') {
            DSD_SNPRINTF(b, n, "Stop replay [%s]", c->opts->audio_in_dev);
        } else {
            DSD_SNPRINTF(b, n, "Stop replay [On]");
        }
    } else {
        DSD_SNPRINTF(b, n, "Stop replay [Off]");
    }
    return b;
}

const char*
lbl_per_call_wav(const void* vctx, char* b, size_t n) {
    const UiCtx* c = (const UiCtx*)vctx;
    const int on = (c->opts->dmr_stereo_wav == 1 && c->opts->wav_out_f != NULL);
    DSD_SNPRINTF(b, n, "Per-call WAV [%s]", onoff(on));
    return b;
}

const char*
lbl_event_log(const void* v, char* b, size_t n) {
    const UiCtx* c = (const UiCtx*)v;
    if (c && c->opts && c->opts->event_out_file[0] != '\0') {
        int m = (n > 22) ? (int)(n - 22) : 0;
        DSD_SNPRINTF(b, n, "Event log file... [%.*s]", m, c->opts->event_out_file);
    } else {
        DSD_SNPRINTF(b, n, "Event log file... [off]");
    }
    return b;
}

const char*
lbl_toggle_payload(const void* v, char* b, size_t n) {
    const UiCtx* c = (const UiCtx*)v;
    DSD_SNPRINTF(b, n, "Payload logging to console [%s]", onoff(c->opts->payload));
    return b;
}

const char*
lbl_lrrp_current(const void* vctx, char* b, size_t n) {
    const UiCtx* c = (const UiCtx*)vctx;
    if (c->opts->lrrp_file_output && c->opts->lrrp_out_file[0] != '\0') {
        DSD_SNPRINTF(b, n, "LRRP output: %s", c->opts->lrrp_out_file);
    } else {
        DSD_SNPRINTF(b, n, "LRRP output: off");
    }
    return b;
}

// ---- Display labels ----

const char*
lbl_ui_compact(const void* v, char* b, size_t n) {
    const UiCtx* c = (const UiCtx*)v;
    DSD_SNPRINTF(b, n, "Compact view [%s]", onoff(c && c->opts && c->opts->frontend_terminal_display.terminal_compact));
    return b;
}

const char*
lbl_ui_channels(const void* v, char* b, size_t n) {
    const UiCtx* c = (const UiCtx*)v;
    DSD_SNPRINTF(b, n, "Channels [%s]", onoff(c && c->opts && c->opts->frontend_display.show_channels));
    return b;
}

const char*
lbl_ui_p25_metrics(const void* v, char* b, size_t n) {
    const UiCtx* c = (const UiCtx*)v;
    DSD_SNPRINTF(b, n, "P25 metrics [%s]", onoff(c && c->opts && c->opts->frontend_display.show_p25_metrics));
    return b;
}

const char*
lbl_ui_p25_affil(const void* v, char* b, size_t n) {
    const UiCtx* c = (const UiCtx*)v;
    DSD_SNPRINTF(b, n, "P25 affiliations [%s]", onoff(c && c->opts && c->opts->frontend_display.show_p25_affiliations));
    return b;
}

const char*
lbl_ui_p25_ga(const void* v, char* b, size_t n) {
    const UiCtx* c = (const UiCtx*)v;
    DSD_SNPRINTF(b, n, "P25 group affiliation [%s]",
                 onoff(c && c->opts && c->opts->frontend_display.show_p25_group_affiliations));
    return b;
}

const char*
lbl_ui_p25_neighbors(const void* v, char* b, size_t n) {
    const UiCtx* c = (const UiCtx*)v;
    DSD_SNPRINTF(b, n, "P25 neighbors [%s]", onoff(c && c->opts && c->opts->frontend_display.show_p25_neighbors));
    return b;
}

const char*
lbl_ui_p25_iden(const void* v, char* b, size_t n) {
    const UiCtx* c = (const UiCtx*)v;
    DSD_SNPRINTF(b, n, "P25 IDEN plan [%s]", onoff(c && c->opts && c->opts->frontend_display.show_p25_iden_plan));
    return b;
}

const char*
lbl_ui_p25_ccc(const void* v, char* b, size_t n) {
    const UiCtx* c = (const UiCtx*)v;
    DSD_SNPRINTF(b, n, "P25 CC candidates [%s]",
                 onoff(c && c->opts && c->opts->frontend_display.show_p25_cc_candidates));
    return b;
}

const char*
lbl_ui_p25_callsign(const void* v, char* b, size_t n) {
    const UiCtx* c = (const UiCtx*)v;
    DSD_SNPRINTF(b, n, "P25 callsign decode [%s]",
                 onoff(c && c->opts && c->opts->frontend_display.show_p25_callsign_decode));
    return b;
}

const char*
lbl_vis_const(const void* v, char* b, size_t n) {
    const UiCtx* c = (const UiCtx*)v;
    DSD_SNPRINTF(b, n, "Constellation [%s]", onoff(c && c->opts && c->opts->frontend_display.constellation));
    return b;
}

const char*
lbl_vis_const_norm(const void* v, char* b, size_t n) {
    const UiCtx* c = (const UiCtx*)v;
    /* Two normalizations, not on/off: 0 is radial p99, which is the default and is
       still normalizing. "Off" said the opposite, and disagreed with the main screen
       ("Norm: radial/unit") and the visualizer's own legend. */
    const int unit = (c && c->opts && c->opts->frontend_display.const_norm_mode) ? 1 : 0;
    DSD_SNPRINTF(b, n, "Constellation normalization [%s]", unit ? "Unit circle" : "Radial");
    return b;
}

const char*
lbl_vis_eye(const void* v, char* b, size_t n) {
    const UiCtx* c = (const UiCtx*)v;
    DSD_SNPRINTF(b, n, "Eye diagram [%s]", onoff(c && c->opts && c->opts->frontend_display.eye_view));
    return b;
}

const char*
lbl_vis_eye_unicode(const void* v, char* b, size_t n) {
    const UiCtx* c = (const UiCtx*)v;
    DSD_SNPRINTF(b, n, "Eye diagram Unicode [%s]",
                 onoff(c && c->opts && c->opts->frontend_terminal_display.eye_unicode));
    return b;
}

const char*
lbl_vis_eye_color(const void* v, char* b, size_t n) {
    const UiCtx* c = (const UiCtx*)v;
    DSD_SNPRINTF(b, n, "Eye diagram color [%s]", onoff(c && c->opts && c->opts->frontend_terminal_display.eye_color));
    return b;
}

const char*
lbl_vis_fsk(const void* v, char* b, size_t n) {
    const UiCtx* c = (const UiCtx*)v;
    DSD_SNPRINTF(b, n, "FSK histogram [%s]", onoff(c && c->opts && c->opts->frontend_display.fsk_hist_view));
    return b;
}

const char*
lbl_vis_spectrum(const void* v, char* b, size_t n) {
    const UiCtx* c = (const UiCtx*)v;
    DSD_SNPRINTF(b, n, "Spectrum analyzer [%s]", onoff(c && c->opts && c->opts->frontend_display.spectrum_view));
    return b;
}

const char*
lbl_history_mode(const void* v, char* b, size_t n) {
    (void)v;
    const int mode = dsd_app_frontend_history_get_mode();
    const char* s = (mode == 1) ? "Short" : ((mode == 2) ? "Long" : "Off");
    DSD_SNPRINTF(b, n, "Mode [%s]", s);
    return b;
}

const char*
lbl_history_slot(const void* v, char* b, size_t n) {
    const UiCtx* c = (const UiCtx*)v;
    /* Three states, not two: ui_handle_eh_toggle_slot() cycles 0 -> 1 -> 2 -> 0 and
       the session starts on 2. Naming 2 "1" made the merged view read exactly like
       slot 1, so cycling off it looked as if the row had done nothing. Matches what
       the main screen's history header prints. */
    const unsigned slot = (c && c->state) ? (unsigned)c->state->eh_slot : 2U;
    const char* s = (slot == 0U) ? "1" : ((slot == 1U) ? "2" : "1+2");
    DSD_SNPRINTF(b, n, "Slot [%s]", s);
    return b;
}

// ---- Advanced labels ----

const char*
lbl_ftz_daz(const void* v, char* b, size_t n) {
    (void)v;
#if defined(__SSE__) || defined(__SSE2__)
    const dsdneoRuntimeConfig* cfg = dsd_neo_get_config();
    int on = (cfg && cfg->ftz_daz_enable) ? 1 : 0;
    DSD_SNPRINTF(b, n, "SSE FTZ/DAZ [%s]", onoff(on));
    return b;
#else
    DSD_SNPRINTF(b, n, "SSE FTZ/DAZ [Unavailable]");
    return b;
#endif
}

const char*
lbl_window_freeze(const void* v, char* b, size_t n) {
    (void)v;
    const dsdneoRuntimeConfig* cfg = dsd_neo_get_config();
    int on = (cfg && cfg->window_freeze_is_set) ? cfg->window_freeze : 0;
    DSD_SNPRINTF(b, n, "Freeze symbol window [%s]", onoff(on));
    return b;
}

const char*
lbl_rt_sched(const void* v, char* b, size_t n) {
    (void)v;
    const dsdneoRuntimeConfig* cfg = dsd_neo_get_config();
    int on = (cfg && cfg->rt_sched_enable) ? 1 : 0;
    DSD_SNPRINTF(b, n, "Realtime scheduling [%s]", onoff(on));
    return b;
}

const char*
lbl_mt(const void* v, char* b, size_t n) {
    (void)v;
    const dsdneoRuntimeConfig* cfg = dsd_neo_get_config();
    int on = (cfg && cfg->mt_is_set) ? cfg->mt_enable : 0;
    DSD_SNPRINTF(b, n, "Intra-block multithreading [%s]", onoff(on));
    return b;
}

const char*
lbl_auto_ppm_snr(const void* v, char* b, size_t n) {
    (void)v;
    double d = env_get_double("DSD_NEO_AUTO_PPM_SNR_DB", 6.0);
    DSD_SNPRINTF(b, n, "Auto-PPM SNR threshold... [%.1f dB]", d);
    return b;
}

const char*
lbl_auto_ppm_pwr(const void* v, char* b, size_t n) {
    (void)v;
    double d = env_get_double("DSD_NEO_AUTO_PPM_PWR_DB", -80.0);
    DSD_SNPRINTF(b, n, "Auto-PPM minimum power... [%.1f dB]", d);
    return b;
}

const char*
lbl_auto_ppm_zeroppm(const void* v, char* b, size_t n) {
    (void)v;
    double p = env_get_double("DSD_NEO_AUTO_PPM_ZEROLOCK_PPM", 0.6);
    DSD_SNPRINTF(b, n, "Auto-PPM zero-lock PPM... [%.2f]", p);
    return b;
}

const char*
lbl_auto_ppm_zerohz(const void* v, char* b, size_t n) {
    (void)v;
    int h = env_get_int("DSD_NEO_AUTO_PPM_ZEROLOCK_HZ", 60);
    DSD_SNPRINTF(b, n, "Auto-PPM zero-lock Hz... [%d]", h);
    return b;
}

const char*
lbl_auto_ppm_freeze(const void* v, char* b, size_t n) {
    (void)v;
    const dsdneoRuntimeConfig* cfg = dsd_neo_get_config();
    int on = (cfg && cfg->auto_ppm_freeze_enable) ? 1 : 0;
    DSD_SNPRINTF(b, n, "Auto-PPM freeze [%s]", onoff(on));
    return b;
}

const char*
lbl_tcp_prebuf(const void* v, char* b, size_t n) {
    (void)v;
    int ms = env_get_int("DSD_NEO_TCP_PREBUF_MS", 30);
    DSD_SNPRINTF(b, n, "rtl_tcp prebuffer... [%d ms]", ms);
    return b;
}

const char*
lbl_tcp_rcvbuf(const void* v, char* b, size_t n) {
    (void)v;
    int sz = env_get_int("DSD_NEO_TCP_RCVBUF", 0);
    if (sz > 0) {
        DSD_SNPRINTF(b, n, "rtl_tcp SO_RCVBUF... [%d bytes]", sz);
    } else {
        DSD_SNPRINTF(b, n, "rtl_tcp SO_RCVBUF... [system default]");
    }
    return b;
}

const char*
lbl_tcp_rcvtimeo(const void* v, char* b, size_t n) {
    (void)v;
    int ms = env_get_int("DSD_NEO_TCP_RCVTIMEO", 0);
    if (ms > 0) {
        DSD_SNPRINTF(b, n, "rtl_tcp SO_RCVTIMEO... [%d ms]", ms);
    } else {
        DSD_SNPRINTF(b, n, "rtl_tcp SO_RCVTIMEO... [Off]");
    }
    return b;
}

const char*
lbl_tcp_waitall(const void* v, char* b, size_t n) {
    (void)v;
    const dsdneoRuntimeConfig* cfg = dsd_neo_get_config();
    int on = (cfg && cfg->tcp_waitall_enable) ? 1 : 0;
    DSD_SNPRINTF(b, n, "rtl_tcp MSG_WAITALL [%s]", onoff(on));
    return b;
}

// ---- RTL-SDR and DSP labels (USE_RADIO only) ----

#ifdef USE_RADIO

const char*
lbl_rtl_freq(const void* v, char* b, size_t n) {
    const UiCtx* c = (const UiCtx*)v;
    const double mhz = ((c && c->opts) ? (double)c->opts->rtlsdr_center_freq : 0.0) / 1e6;
    DSD_SNPRINTF(b, n, "Frequency... [%.6f MHz]", mhz);
    return b;
}

const char*
lbl_rtl_gain(const void* v, char* b, size_t n) {
    const UiCtx* c = (const UiCtx*)v;
    const int gain = (c && c->opts) ? c->opts->rtl_gain_value : 0;
    if (gain == 0) {
        DSD_SNPRINTF(b, n, "Gain... [AGC]");
    } else {
        DSD_SNPRINTF(b, n, "Gain... [%d]", gain);
    }
    return b;
}

const char*
lbl_rtl_ppm(const void* v, char* b, size_t n) {
    const UiCtx* c = (const UiCtx*)v;
    DSD_SNPRINTF(b, n, "PPM correction... [%d]", (c && c->opts) ? c->opts->rtlsdr_ppm_error : 0);
    return b;
}

const char*
lbl_rtl_bw(const void* v, char* b, size_t n) {
    const UiCtx* c = (const UiCtx*)v;
    DSD_SNPRINTF(b, n, "Bandwidth... [%d kHz]", (c && c->opts) ? c->opts->rtl_dsp_bw_khz : 0);
    return b;
}

const char*
lbl_rtl_vol(const void* v, char* b, size_t n) {
    const UiCtx* c = (const UiCtx*)v;
    DSD_SNPRINTF(b, n, "Volume multiplier... [%d]", (c && c->opts) ? c->opts->rtl_volume_multiplier : 0);
    return b;
}

const char*
lbl_rtl_bias(const void* v, char* b, size_t n) {
    const UiCtx* c = (const UiCtx*)v;
    DSD_SNPRINTF(b, n, "Bias tee [%s]", onoff(c->opts->rtl_bias_tee));
    return b;
}

const char*
lbl_rtl_rtltcp_autotune(const void* v, char* b, size_t n) {
    const UiCtx* c = (const UiCtx*)v;
    DSD_SNPRINTF(b, n, "rtl_tcp adaptive buffering [%s]", onoff(c->opts->rtltcp_autotune));
    return b;
}

const char*
lbl_rtl_auto_ppm(const void* v, char* b, size_t n) {
    const UiCtx* c = (const UiCtx*)v;
    int on = (c && c->opts && c->opts->rtl_auto_ppm) ? 1 : 0;
    if (c && c->state && c->state->rtl_ctx) {
        dsd_frontend_metrics metrics = menu_frontend_metrics(v);
        on = metrics.auto_ppm_enabled;
    }
    DSD_SNPRINTF(b, n, "Auto-PPM [%s]", onoff(on));
    return b;
}

const char*
lbl_rtl_tuner_autogain(const void* v, char* b, size_t n) {
    const UiCtx* c = (const UiCtx*)v;
    const dsdneoRuntimeConfig* cfg = dsd_neo_get_config();
    int on = (cfg && cfg->tuner_autogain_enable) ? 1 : 0;
    if (c && c->state && c->state->rtl_ctx) {
        dsd_frontend_metrics metrics = menu_frontend_metrics(v);
        on = metrics.tuner_autogain;
    }
    DSD_SNPRINTF(b, n, "Tuner autogain [%s]", onoff(on));
    return b;
}

const char*
lbl_onoff_cq(const void* v, char* b, size_t n) {
    dsd_frontend_metrics metrics = menu_frontend_metrics(v);
    DSD_SNPRINTF(b, n, "CQPSK path [%s]", onoff(metrics.cqpsk_enable));
    return b;
}

const char*
lbl_onoff_iqbal(const void* v, char* b, size_t n) {
    dsd_frontend_metrics metrics = menu_frontend_metrics(v);
    DSD_SNPRINTF(b, n, "IQ balance [%s]", onoff(metrics.iq_balance));
    return b;
}

const char*
lbl_iq_dc(const void* v, char* b, size_t n) {
    dsd_frontend_metrics metrics = menu_frontend_metrics(v);
    DSD_SNPRINTF(b, n, "IQ DC block [%s]", onoff(metrics.iq_dc_enabled));
    return b;
}

const char*
lbl_iq_dc_k(const void* v, char* b, size_t n) {
    dsd_frontend_metrics metrics = menu_frontend_metrics(v);
    DSD_SNPRINTF(b, n, "IQ DC shift k... [%d]", metrics.iq_dc_shift_k);
    return b;
}

const char*
lbl_ted_gain(const void* v, char* b, size_t n) {
    dsd_frontend_metrics metrics = menu_frontend_metrics(v);
    float g = metrics.ted_gain;
    int g_milli = (int)(g * 1000.0f + 0.5f);
    DSD_SNPRINTF(b, n, "CQPSK timing gain... [%d x0.001]", g_milli);
    return b;
}

const char*
lbl_cqpsk_timing_bias(const void* v, char* b, size_t n) {
    dsd_frontend_metrics metrics = menu_frontend_metrics(v);
    DSD_SNPRINTF(b, n, "CQPSK timing bias (EMA) %d", metrics.cqpsk_timing_bias);
    return b;
}

const char*
lbl_dsp_panel(const void* v, char* b, size_t n) {
    const UiCtx* c = (const UiCtx*)v;
    DSD_SNPRINTF(b, n, "DSP panel [%s]", onoff(c && c->opts && c->opts->frontend_display.show_dsp_panel));
    return b;
}

#endif /* USE_RADIO */
