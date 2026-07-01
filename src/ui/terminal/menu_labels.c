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
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/io/tcp_input.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/runtime/config.h>
#include <stdint.h>
#include <string.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "dsd-neo/platform/sockets.h"
#include "dsd-neo/runtime/call_alert.h"
#include "menu_env.h"
#include "menu_internal.h"
#include "menu_items.h"
#include "ui_key_status.h"

// ---- Visibility/predicate functions ----

bool
io_always_on(const void* ctx) {
    (void)ctx;
    return true;
}

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
    UiCtx* c = (UiCtx*)ctx;
    if (!c || !c->opts) {
        return false;
    }
    return (c->opts->audio_in_type == AUDIO_IN_RTL);
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

bool
dsp_cq_on(const void* v) {
    dsd_frontend_metrics metrics = menu_frontend_metrics(v);
    return metrics.cqpsk_enable != 0;
}

int
ui_current_mod(const void* v) {
    UiCtx* c = (UiCtx*)v;
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

// DSP submenu arrays declared in menu_items.h

bool
dsp_ted_any(const void* v) {
    return ui_submenu_has_visible(DSP_TED_ITEMS, DSP_TED_ITEMS_LEN, v) ? true : false;
}
#endif

// ---- State labels ----

const char*
lbl_invert_all(const void* v, char* b, size_t n) {
    UiCtx* c = (UiCtx*)v;
    DSD_SNPRINTF(b, n, "Toggle Signal Inversion [%s]", c->opts->inverted_dmr ? "Active" : "Inactive");
    return b;
}

const char*
lbl_toggle_payload(const void* v, char* b, size_t n) {
    UiCtx* c = (UiCtx*)v;
    DSD_SNPRINTF(b, n, "Toggle Payload Logging [%s]", c->opts->payload ? "Active" : "Inactive");
    return b;
}

const char*
lbl_trunk(const void* v, char* b, size_t n) {
    UiCtx* c = (UiCtx*)v;
    DSD_SNPRINTF(b, n, "Toggle Trunking [%s]", c->opts->p25_trunk ? "Active" : "Inactive");
    return b;
}

const char*
lbl_scan(const void* v, char* b, size_t n) {
    UiCtx* c = (UiCtx*)v;
    DSD_SNPRINTF(b, n, "Toggle Scanning Mode [%s]", c->opts->scanner_mode ? "Active" : "Inactive");
    return b;
}

const char*
lbl_lcw(const void* v, char* b, size_t n) {
    UiCtx* c = (UiCtx*)v;
    DSD_SNPRINTF(b, n, "Toggle P25 LCW Retune [%s]", c->opts->p25_lcw_retune ? "Active" : "Inactive");
    return b;
}

const char*
lbl_p25_enc_lockout(const void* v, char* b, size_t n) {
    UiCtx* c = (UiCtx*)v;
    int on = (c && c->opts) ? ((c->opts->trunk_tune_enc_calls == 0) ? 1 : 0) : 0;
    DSD_SNPRINTF(b, n, "P25 Encrypted Call Lockout [%s]", on ? "On" : "Off");
    return b;
}

const char*
lbl_crc_relax(const void* v, char* b, size_t n) {
    UiCtx* c = (UiCtx*)v;
    int relaxed = (c->opts->aggressive_framesync == 0);
    DSD_SNPRINTF(b, n, "Toggle Relaxed CRC checks [%s]", relaxed ? "Active" : "Inactive");
    return b;
}

const char*
lbl_allow(const void* v, char* b, size_t n) {
    UiCtx* c = (UiCtx*)v;
    DSD_SNPRINTF(b, n, "Toggle Allow/White List [%s]", c->opts->trunk_use_allow_list ? "Active" : "Inactive");
    return b;
}

const char*
lbl_tune_group(const void* v, char* b, size_t n) {
    UiCtx* c = (UiCtx*)v;
    DSD_SNPRINTF(b, n, "Toggle Tune Group Calls [%s]", c->opts->trunk_tune_group_calls ? "Active" : "Inactive");
    return b;
}

const char*
lbl_tune_priv(const void* v, char* b, size_t n) {
    UiCtx* c = (UiCtx*)v;
    DSD_SNPRINTF(b, n, "Toggle Tune Private Calls [%s]", c->opts->trunk_tune_private_calls ? "Active" : "Inactive");
    return b;
}

const char*
lbl_tune_data(const void* v, char* b, size_t n) {
    UiCtx* c = (UiCtx*)v;
    DSD_SNPRINTF(b, n, "Toggle Tune Data Calls [%s]", c->opts->trunk_tune_data_calls ? "Active" : "Inactive");
    return b;
}

const char*
lbl_rev_mute(const void* v, char* b, size_t n) {
    UiCtx* c = (UiCtx*)v;
    DSD_SNPRINTF(b, n, "Toggle Reverse Mute [%s]", c->opts->reverse_mute ? "Active" : "Inactive");
    return b;
}

const char*
lbl_dmr_le(const void* v, char* b, size_t n) {
    UiCtx* c = (UiCtx*)v;
    DSD_SNPRINTF(b, n, "Toggle DMR Late Entry [%s]", c->opts->dmr_le ? "Active" : "Inactive");
    return b;
}

const char*
lbl_slotpref(const void* v, char* b, size_t n) {
    UiCtx* c = (UiCtx*)v;
    const char* now = (c->opts->slot_preference == 0) ? "1" : (c->opts->slot_preference == 1) ? "2" : "Auto";
    DSD_SNPRINTF(b, n, "Set TDMA Slot Preference... [now %s]", now);
    return b;
}

const char*
lbl_slots_on(const void* v, char* b, size_t n) {
    UiCtx* c = (UiCtx*)v;
    const char* now = (c->opts->slot1_on && c->opts->slot2_on)
                          ? "both"
                          : (c->opts->slot1_on ? "1" : (c->opts->slot2_on ? "2" : "off"));
    DSD_SNPRINTF(b, n, "Set TDMA Synth Slots... [now %s]", now);
    return b;
}

const char*
lbl_muting(const void* v, char* b, size_t n) {
    UiCtx* c = (UiCtx*)v;
    int dmr = (c->opts->dmr_mute_encL == 1 && c->opts->dmr_mute_encR == 1);
    int p25 = (c->opts->unmute_encrypted_p25 == 0);
    int active = (dmr && p25);
    DSD_SNPRINTF(b, n, "Toggle Encrypted Audio Muting [%s]", active ? "Active" : "Inactive");
    return b;
}

const char*
lbl_call_alert(const void* v, char* b, size_t n) {
    UiCtx* c = (UiCtx*)v;
    DSD_SNPRINTF(b, n, "Toggle Call Alert Beep [%s]", c->opts->call_alert ? "Active" : "Inactive");
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
    DSD_SNPRINTF(b, n, "Call Alert Events [%s]", call_alert_events_name(events));
    return b;
}

const char*
lbl_pref_cc(const void* v, char* b, size_t n) {
    UiCtx* c = (UiCtx*)v;
    DSD_SNPRINTF(b, n, "Prefer P25 CC Candidates [%s]", c->opts->p25_prefer_candidates ? "Active" : "Inactive");
    return b;
}

// ---- IO labels ----

const char*
lbl_current_output(const void* vctx, char* b, size_t n) {
    UiCtx* c = (UiCtx*)vctx;
    const char* name;
    switch (c->opts->audio_out_type) {
        case 0: name = "Pulse Digital"; break;
        case 8: name = "UDP"; break;
        default: name = "?"; break;
    }
    if (c->opts->audio_out_type == 0) {
        if (c->opts->pa_output_idx[0]) {
            size_t prefix = strlen("Current Output: Pulse []") - 2; /* exclude %s */
            int m = (n > prefix) ? (int)(n - prefix) : 0;
            DSD_SNPRINTF(b, n, "Current Output: Pulse [%.*s]", m, c->opts->pa_output_idx);
        } else {
            DSD_SNPRINTF(b, n, "Current Output: Pulse [default]");
        }
    } else if (c->opts->audio_out_type == 8) {
        int m = (n > 32) ? (int)(n - 32) : 0; /* leave room for prefix and port */
        DSD_SNPRINTF(b, n, "Current Output: UDP %.*s:%d", m, c->opts->udp_hostname, c->opts->udp_portno);
    } else {
        DSD_SNPRINTF(b, n, "Current Output: %s", name);
    }
    return b;
}

static const char*
lbl_current_input_tcp(const dsd_opts* opts, char* b, size_t n) {
    int m = (n > 32) ? (int)(n - 32) : 0;
    DSD_SNPRINTF(b, n, "Current Input: TCP %.*s:%d", m, opts->tcp_hostname, opts->tcp_portno);
    return b;
}

static const char*
lbl_current_input_udp(const dsd_opts* opts, char* b, size_t n) {
    const char* addr = opts->udp_in_bindaddr[0] ? opts->udp_in_bindaddr : "127.0.0.1";
    int m = (n > 32) ? (int)(n - 32) : 0;
    DSD_SNPRINTF(b, n, "Current Input: UDP %.*s:%d", m, addr, opts->udp_in_portno);
    return b;
}

static const char*
lbl_current_input_file_like(const dsd_opts* opts, char* b, size_t n) {
    int m = (n > 18) ? (int)(n - 18) : 0;
    DSD_SNPRINTF(b, n, "Current Input: %.*s", m, opts->audio_in_dev);
    return b;
}

static const char*
lbl_current_input_rtl(const dsd_opts* opts, char* b, size_t n) {
    if (menu_audio_in_is_soapy(opts)) {
        if (strncmp(opts->audio_in_dev, "soapy:", 6) == 0 && opts->audio_in_dev[6] != '\0') {
            size_t prefix = strlen("Current Input: SoapySDR []") - 2;
            int m = (n > prefix) ? (int)(n - prefix) : 0;
            DSD_SNPRINTF(b, n, "Current Input: SoapySDR [%.*s]", m, opts->audio_in_dev + 6);
        } else {
            DSD_SNPRINTF(b, n, "Current Input: SoapySDR");
        }
        return b;
    }

    DSD_SNPRINTF(b, n, "Current Input: RTL-SDR dev %d", opts->rtl_dev_index);
    return b;
}

const char*
lbl_current_input(const void* vctx, char* b, size_t n) {
    const UiCtx* c = (const UiCtx*)vctx;
    if (c == NULL || c->opts == NULL) {
        DSD_SNPRINTF(b, n, "Current Input: ?");
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
        case AUDIO_IN_PULSE: DSD_SNPRINTF(b, n, "Current Input: Pulse"); return b;
        case AUDIO_IN_STDIN: DSD_SNPRINTF(b, n, "Current Input: STDIN"); return b;
        default: DSD_SNPRINTF(b, n, "Current Input: ?"); return b;
    }
}

const char*
lbl_out_mute(const void* vctx, char* b, size_t n) {
    UiCtx* c = (UiCtx*)vctx;
    DSD_SNPRINTF(b, n, "Mute Output [%s]", (c->opts->audio_out == 0) ? "On" : "Off");
    return b;
}

const char*
lbl_monitor(const void* v, char* b, size_t n) {
    UiCtx* c = (UiCtx*)v;
    DSD_SNPRINTF(b, n, "Toggle Source Audio Monitor [%s]", c->opts->monitor_input_audio ? "Active" : "Inactive");
    return b;
}

const char*
lbl_cosine(const void* v, char* b, size_t n) {
    UiCtx* c = (UiCtx*)v;
    DSD_SNPRINTF(b, n, "Toggle Cosine Filter [%s]", c->opts->use_cosine_filter ? "Active" : "Inactive");
    return b;
}

const char*
lbl_input_volume(const void* vctx, char* b, size_t n) {
    UiCtx* c = (UiCtx*)vctx;
    int m = c->opts->input_volume_multiplier;
    if (m < 1) {
        m = 1;
    }
    DSD_SNPRINTF(b, n, "Input Volume: %dX", m);
    return b;
}

const char*
lbl_tcp(const void* vctx, char* b, size_t n) {
    UiCtx* c = (UiCtx*)vctx;
    int active = (c->opts->audio_in_type == AUDIO_IN_TCP && tcp_input_is_valid(c->opts->tcp_in_ctx));
    if (c->opts->tcp_hostname[0] != '\0' && c->opts->tcp_portno > 0) {
        int m = (n > 32) ? (int)(n - 32) : 0;
        if (active) {
            DSD_SNPRINTF(b, n, "TCP Direct Audio: %.*s:%d [Active]", m, c->opts->tcp_hostname, c->opts->tcp_portno);
        } else {
            DSD_SNPRINTF(b, n, "TCP Direct Audio: %.*s:%d [Inactive]", m, c->opts->tcp_hostname, c->opts->tcp_portno);
        }
    } else {
        DSD_SNPRINTF(b, n, active ? "TCP Direct Audio [Active]" : "Start TCP Direct Audio [Inactive]");
    }
    return b;
}

const char*
lbl_rigctl(const void* vctx, char* b, size_t n) {
    UiCtx* c = (UiCtx*)vctx;
    int connected = (c->opts->use_rigctl && c->opts->rigctl_sockfd != DSD_INVALID_SOCKET);
    if (c->opts->rigctlhostname[0] != '\0' && c->opts->rigctlportno > 0) {
        int m = (n > 24) ? (int)(n - 24) : 0;
        if (connected) {
            DSD_SNPRINTF(b, n, "Rigctl: %.*s:%d [Active]", m, c->opts->rigctlhostname, c->opts->rigctlportno);
        } else {
            DSD_SNPRINTF(b, n, "Rigctl: %.*s:%d [Inactive]", m, c->opts->rigctlhostname, c->opts->rigctlportno);
        }
    } else {
        DSD_SNPRINTF(b, n, connected ? "Rigctl [Active]" : "Configure Rigctl [Inactive]");
    }
    return b;
}

const char*
lbl_sym_save(const void* vctx, char* b, size_t n) {
    UiCtx* c = (UiCtx*)vctx;
    if (c->opts->symbol_out_f) {
        size_t prefix = strlen("Save Symbols to File [Active: ]") - 2; /* exclude %s */
        int m = (n > prefix) ? (int)(n - prefix) : 0;
        DSD_SNPRINTF(b, n, "Save Symbols to File [Active: %.*s]", m, c->opts->symbol_out_file);
    } else {
        DSD_SNPRINTF(b, n, "Save Symbols to File [Inactive]");
    }
    return b;
}

const char*
lbl_per_call_wav(const void* vctx, char* b, size_t n) {
    const UiCtx* c = (const UiCtx*)vctx;
    if (c->opts->dmr_stereo_wav == 1 && c->opts->wav_out_f != NULL) {
        DSD_SNPRINTF(b, n, "Save Per-Call WAV [Active]");
    } else {
        DSD_SNPRINTF(b, n, "Save Per-Call WAV [Inactive]");
    }
    return b;
}

const char*
lbl_stop_symbol_playback(const void* vctx, char* b, size_t n) {
    UiCtx* c = (UiCtx*)vctx;
    if (c->opts->symbolfile != NULL && c->opts->audio_in_type == AUDIO_IN_SYMBOL_BIN) {
        if (c->opts->audio_in_dev[0] != '\0') {
            DSD_SNPRINTF(b, n, "Stop Symbol Playback [Active: %s]", c->opts->audio_in_dev);
        } else {
            DSD_SNPRINTF(b, n, "Stop Symbol Playback [Active]");
        }
    } else {
        DSD_SNPRINTF(b, n, "Stop Symbol Playback [Inactive]");
    }
    return b;
}

const char*
lbl_stop_symbol_capture(const void* vctx, char* b, size_t n) {
    UiCtx* c = (UiCtx*)vctx;
    if (c->opts->symbol_out_f) {
        if (c->opts->symbol_out_file[0] != '\0') {
            DSD_SNPRINTF(b, n, "Stop Symbol Capture [Active: %s]", c->opts->symbol_out_file);
        } else {
            DSD_SNPRINTF(b, n, "Stop Symbol Capture [Active]");
        }
    } else {
        DSD_SNPRINTF(b, n, "Stop Symbol Capture [Inactive]");
    }
    return b;
}

const char*
lbl_replay_last(const void* vctx, char* b, size_t n) {
    UiCtx* c = (UiCtx*)vctx;
    if (c->opts->audio_in_dev[0] != '\0') {
        dsd_stat_t sb;
        if (dsd_stat_path(c->opts->audio_in_dev, &sb) == 0) {
            DSD_SNPRINTF(b, n, "Replay Last Symbol Capture [%s]", c->opts->audio_in_dev);
            return b;
        }
    }
    DSD_SNPRINTF(b, n, "Replay Last Symbol Capture [Inactive]");
    return b;
}

// ---- Inversion labels ----

const char*
lbl_inv_x2(const void* v, char* b, size_t n) {
    UiCtx* c = (UiCtx*)v;
    DSD_SNPRINTF(b, n, "Invert X2-TDMA [%s]", c->opts->inverted_x2tdma ? "Active" : "Inactive");
    return b;
}

const char*
lbl_inv_dmr(const void* v, char* b, size_t n) {
    UiCtx* c = (UiCtx*)v;
    DSD_SNPRINTF(b, n, "Invert DMR [%s]", c->opts->inverted_dmr ? "Active" : "Inactive");
    return b;
}

const char*
lbl_inv_dpmr(const void* v, char* b, size_t n) {
    UiCtx* c = (UiCtx*)v;
    DSD_SNPRINTF(b, n, "Invert dPMR [%s]", c->opts->inverted_dpmr ? "Active" : "Inactive");
    return b;
}

const char*
lbl_inv_m17(const void* v, char* b, size_t n) {
    UiCtx* c = (UiCtx*)v;
    DSD_SNPRINTF(b, n, "Invert M17 [%s]", c->opts->inverted_m17 ? "Active" : "Inactive");
    return b;
}

// ---- Env/Advanced labels ----

const char*
lbl_ftz_daz(const void* v, char* b, size_t n) {
    (void)v;
#if defined(__SSE__) || defined(__SSE2__)
    const dsdneoRuntimeConfig* cfg = dsd_neo_get_config();
    int on = (cfg && cfg->ftz_daz_enable) ? 1 : 0;
    DSD_SNPRINTF(b, n, "SSE FTZ/DAZ: %s", on ? "On" : "Off");
    return b;
#else
    DSD_SNPRINTF(b, n, "SSE FTZ/DAZ: Unavailable");
    return b;
#endif
}

const char*
lbl_input_warn(const void* v, char* b, size_t n) {
    UiCtx* c = (UiCtx*)v;
    double thr = c ? c->opts->input_warn_db : env_get_double("DSD_NEO_INPUT_WARN_DB", -40.0);
    DSD_SNPRINTF(b, n, "Low Input Warning: %.1f dBFS", thr);
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
    DSD_SNPRINTF(b, n, "Deemphasis: %s", s);
    return b;
}

const char*
lbl_audio_lpf(const void* v, char* b, size_t n) {
    (void)v;
    const dsdneoRuntimeConfig* cfg = dsd_neo_get_config();
    if (cfg && cfg->audio_lpf_is_set && !cfg->audio_lpf_disable && cfg->audio_lpf_cutoff_hz > 0) {
        DSD_SNPRINTF(b, n, "Audio LPF: %d Hz", cfg->audio_lpf_cutoff_hz);
    } else {
        DSD_SNPRINTF(b, n, "Audio LPF: Off");
    }
    return b;
}

const char*
lbl_window_freeze(const void* v, char* b, size_t n) {
    (void)v;
    const dsdneoRuntimeConfig* cfg = dsd_neo_get_config();
    int on = (cfg && cfg->window_freeze_is_set) ? cfg->window_freeze : 0;
    DSD_SNPRINTF(b, n, "Freeze Symbol Window: %s", on ? "On" : "Off");
    return b;
}

const char*
lbl_auto_ppm_snr(const void* v, char* b, size_t n) {
    (void)v;
    double d = env_get_double("DSD_NEO_AUTO_PPM_SNR_DB", 6.0);
    DSD_SNPRINTF(b, n, "Auto-PPM SNR threshold: %.1f dB", d);
    return b;
}

const char*
lbl_auto_ppm_pwr(const void* v, char* b, size_t n) {
    (void)v;
    double d = env_get_double("DSD_NEO_AUTO_PPM_PWR_DB", -80.0);
    DSD_SNPRINTF(b, n, "Auto-PPM Min power: %.1f dB", d);
    return b;
}

const char*
lbl_auto_ppm_zeroppm(const void* v, char* b, size_t n) {
    (void)v;
    double p = env_get_double("DSD_NEO_AUTO_PPM_ZEROLOCK_PPM", 0.6);
    DSD_SNPRINTF(b, n, "Auto-PPM Zero-lock PPM: %.2f", p);
    return b;
}

const char*
lbl_auto_ppm_zerohz(const void* v, char* b, size_t n) {
    (void)v;
    int h = env_get_int("DSD_NEO_AUTO_PPM_ZEROLOCK_HZ", 60);
    DSD_SNPRINTF(b, n, "Auto-PPM Zero-lock Hz: %d", h);
    return b;
}

const char*
lbl_auto_ppm_freeze(const void* v, char* b, size_t n) {
    (void)v;
    const dsdneoRuntimeConfig* cfg = dsd_neo_get_config();
    int on = (cfg && cfg->auto_ppm_freeze_enable) ? 1 : 0;
    DSD_SNPRINTF(b, n, "Auto-PPM Freeze: %s", on ? "On" : "Off");
    return b;
}

const char*
lbl_tcp_prebuf(const void* v, char* b, size_t n) {
    (void)v;
    int ms = env_get_int("DSD_NEO_TCP_PREBUF_MS", 30);
    DSD_SNPRINTF(b, n, "RTL-TCP Prebuffer: %d ms", ms);
    return b;
}

const char*
lbl_tcp_rcvbuf(const void* v, char* b, size_t n) {
    (void)v;
    int sz = env_get_int("DSD_NEO_TCP_RCVBUF", 0);
    if (sz > 0) {
        DSD_SNPRINTF(b, n, "RTL-TCP SO_RCVBUF: %d bytes", sz);
    } else {
        DSD_SNPRINTF(b, n, "RTL-TCP SO_RCVBUF: system default");
    }
    return b;
}

const char*
lbl_tcp_rcvtimeo(const void* v, char* b, size_t n) {
    (void)v;
    int ms = env_get_int("DSD_NEO_TCP_RCVTIMEO", 0);
    if (ms > 0) {
        DSD_SNPRINTF(b, n, "RTL-TCP SO_RCVTIMEO: %d ms", ms);
    } else {
        DSD_SNPRINTF(b, n, "RTL-TCP SO_RCVTIMEO: off");
    }
    return b;
}

const char*
lbl_tcp_waitall(const void* v, char* b, size_t n) {
    (void)v;
    const dsdneoRuntimeConfig* cfg = dsd_neo_get_config();
    int on = (cfg && cfg->tcp_waitall_enable) ? 1 : 0;
    DSD_SNPRINTF(b, n, "RTL-TCP MSG_WAITALL: %s", on ? "On" : "Off");
    return b;
}

const char*
lbl_rt_sched(const void* v, char* b, size_t n) {
    (void)v;
    const dsdneoRuntimeConfig* cfg = dsd_neo_get_config();
    int on = (cfg && cfg->rt_sched_enable) ? 1 : 0;
    DSD_SNPRINTF(b, n, "Realtime Scheduling: %s", on ? "On" : "Off");
    return b;
}

const char*
lbl_mt(const void* v, char* b, size_t n) {
    (void)v;
    const dsdneoRuntimeConfig* cfg = dsd_neo_get_config();
    int on = (cfg && cfg->mt_is_set) ? cfg->mt_enable : 0;
    DSD_SNPRINTF(b, n, "Intra-block MT: %s", on ? "On" : "Off");
    return b;
}

// ---- P25 follower labels ----

static double
lbl_p25_num(const void* v, const char* env_name, double defv) {
    (void)v;
    return env_get_double(env_name, defv);
}

const char*
lbl_p25_vc_grace(const void* v, char* b, size_t n) {
    DSD_SNPRINTF(b, n, "P25: VC grace (s): %.3f", lbl_p25_num(v, "DSD_NEO_P25_VC_GRACE", 0.0));
    return b;
}

const char*
lbl_p25_min_follow(const void* v, char* b, size_t n) {
    DSD_SNPRINTF(b, n, "P25: Min follow dwell (s): %.3f", lbl_p25_num(v, "DSD_NEO_P25_MIN_FOLLOW_DWELL", 0.0));
    return b;
}

const char*
lbl_p25_grant_voice(const void* v, char* b, size_t n) {
    DSD_SNPRINTF(b, n, "P25: Grant->Voice timeout (s): %.3f", lbl_p25_num(v, "DSD_NEO_P25_GRANT_VOICE_TO", 0.0));
    return b;
}

const char*
lbl_p25_retune_backoff(const void* v, char* b, size_t n) {
    DSD_SNPRINTF(b, n, "P25: Retune backoff (s): %.3f", lbl_p25_num(v, "DSD_NEO_P25_RETUNE_BACKOFF", 0.0));
    return b;
}

const char*
lbl_p25_cc_grace(const void* v, char* b, size_t n) {
    DSD_SNPRINTF(b, n, "P25: CC hunt grace (s): %.3f", lbl_p25_num(v, "DSD_NEO_P25_CC_GRACE", 0.0));
    return b;
}

const char*
lbl_p25_force_extra(const void* v, char* b, size_t n) {
    DSD_SNPRINTF(b, n, "P25: Force release extra (s): %.3f", lbl_p25_num(v, "DSD_NEO_P25_FORCE_RELEASE_EXTRA", 0.0));
    return b;
}

const char*
lbl_p25_force_margin(const void* v, char* b, size_t n) {
    DSD_SNPRINTF(b, n, "P25: Force release margin (s): %.3f", lbl_p25_num(v, "DSD_NEO_P25_FORCE_RELEASE_MARGIN", 0.0));
    return b;
}

const char*
lbl_p25_p1_err_pct(const void* v, char* b, size_t n) {
    DSD_SNPRINTF(b, n, "P25p1: Err-hold pct: %.1f%%", lbl_p25_num(v, "DSD_NEO_P25P1_ERR_HOLD_PCT", 0.0));
    return b;
}

const char*
lbl_p25_p1_err_sec(const void* v, char* b, size_t n) {
    DSD_SNPRINTF(b, n, "P25p1: Err-hold sec: %.3f", lbl_p25_num(v, "DSD_NEO_P25P1_ERR_HOLD_S", 0.0));
    return b;
}

// ---- UI display labels ----

const char*
lbl_ui_p25_metrics(const void* v, char* b, size_t n) {
    UiCtx* c = (UiCtx*)v;
    DSD_SNPRINTF(b, n, "Show P25 Metrics [%s]",
                 (c && c->opts && c->opts->frontend_display.show_p25_metrics) ? "On" : "Off");
    return b;
}

const char*
lbl_ui_p25_affil(const void* v, char* b, size_t n) {
    UiCtx* c = (UiCtx*)v;
    DSD_SNPRINTF(b, n, "Show P25 Affiliations [%s]",
                 (c && c->opts && c->opts->frontend_display.show_p25_affiliations) ? "On" : "Off");
    return b;
}

const char*
lbl_ui_p25_ga(const void* v, char* b, size_t n) {
    UiCtx* c = (UiCtx*)v;
    DSD_SNPRINTF(b, n, "Show P25 Group Affiliation [%s]",
                 (c && c->opts && c->opts->frontend_display.show_p25_group_affiliations) ? "On" : "Off");
    return b;
}

const char*
lbl_ui_p25_neighbors(const void* v, char* b, size_t n) {
    UiCtx* c = (UiCtx*)v;
    DSD_SNPRINTF(b, n, "Show P25 Neighbors [%s]",
                 (c && c->opts && c->opts->frontend_display.show_p25_neighbors) ? "On" : "Off");
    return b;
}

const char*
lbl_ui_p25_iden(const void* v, char* b, size_t n) {
    UiCtx* c = (UiCtx*)v;
    DSD_SNPRINTF(b, n, "Show P25 IDEN Plan [%s]",
                 (c && c->opts && c->opts->frontend_display.show_p25_iden_plan) ? "On" : "Off");
    return b;
}

const char*
lbl_ui_p25_ccc(const void* v, char* b, size_t n) {
    UiCtx* c = (UiCtx*)v;
    DSD_SNPRINTF(b, n, "Show P25 CC Candidates [%s]",
                 (c && c->opts && c->opts->frontend_display.show_p25_cc_candidates) ? "On" : "Off");
    return b;
}

const char*
lbl_ui_channels(const void* v, char* b, size_t n) {
    UiCtx* c = (UiCtx*)v;
    DSD_SNPRINTF(b, n, "Show Channels [%s]", (c && c->opts && c->opts->frontend_display.show_channels) ? "On" : "Off");
    return b;
}

const char*
lbl_ui_p25_callsign(const void* v, char* b, size_t n) {
    UiCtx* c = (UiCtx*)v;
    DSD_SNPRINTF(b, n, "Show P25 Callsign Decode [%s]",
                 (c && c->opts && c->opts->frontend_display.show_p25_callsign_decode) ? "On" : "Off");
    return b;
}

// ---- LRRP labels ----

const char*
lbl_lrrp_current(const void* vctx, char* b, size_t n) {
    UiCtx* c = (UiCtx*)vctx;
    if (c->opts->lrrp_file_output && c->opts->lrrp_out_file[0] != '\0') {
        DSD_SNPRINTF(b, n, "LRRP Output [Active: %s]", c->opts->lrrp_out_file);
    } else {
        DSD_SNPRINTF(b, n, "LRRP Output [Inactive]");
    }
    return b;
}

// ---- Keys labels ----

const char*
lbl_key_force_bp(const void* v, char* b, size_t n) {
    UiCtx* c = (UiCtx*)v;
    const int on = (c && c->state && c->state->M == 1);
    DSD_SNPRINTF(b, n, "Force BP/Scr Priority [%s]", on ? "Active" : "Inactive");
    return b;
}

const char*
lbl_key_hytera(const void* v, char* b, size_t n) {
    const UiCtx* c = (const UiCtx*)v;
    if (!c || !c->state) {
        DSD_SNPRINTF(b, n, "Hytera Privacy (HEX)");
        return b;
    }
    const dsd_state* s = c->state;
    const int loaded = (s->H != 0ULL && s->tyt_bp == 0);
    if (!loaded) {
        DSD_SNPRINTF(b, n, "Hytera Privacy (HEX)");
        return b;
    }
    const unsigned int segment_count = ui_hytera_key_segment_count(s);
    const char* kind = (segment_count == 1U) ? "40-bit" : ((segment_count == 2U) ? "128-bit" : "256-bit");
    DSD_SNPRINTF(b, n, "Hytera Privacy (HEX) [%s]", kind);
    return b;
}

const char*
lbl_m17_user_data(const void* v, char* b, size_t n) {
    UiCtx* c = (UiCtx*)v;
    const char* s = (c && c->state && c->state->m17dat[0]) ? c->state->m17dat : "<unset>";
    int m = (int)n - 18;
    if (m < 0) {
        m = 0;
    }
    DSD_SNPRINTF(b, n, "M17 Encoder User Data: %.*s", m, s);
    return b;
}

// ---- DSP labels (USE_RADIO only) ----

#ifdef USE_RADIO

const char*
lbl_onoff_cq(const void* v, char* b, size_t n) {
    dsd_frontend_metrics metrics = menu_frontend_metrics(v);
    DSD_SNPRINTF(b, n, "Toggle CQPSK [%s]", metrics.cqpsk_enable ? "Active" : "Inactive");
    return b;
}

const char*
lbl_onoff_iqbal(const void* v, char* b, size_t n) {
    dsd_frontend_metrics metrics = menu_frontend_metrics(v);
    DSD_SNPRINTF(b, n, "Toggle IQ Balance [%s]", metrics.iq_balance ? "Active" : "Inactive");
    return b;
}

const char*
lbl_iq_dc(const void* v, char* b, size_t n) {
    dsd_frontend_metrics metrics = menu_frontend_metrics(v);
    DSD_SNPRINTF(b, n, "IQ DC Block [%s]", metrics.iq_dc_enabled ? "On" : "Off");
    return b;
}

const char*
lbl_iq_dc_k(const void* v, char* b, size_t n) {
    dsd_frontend_metrics metrics = menu_frontend_metrics(v);
    DSD_SNPRINTF(b, n, "IQ DC Shift k: %d (+/-)", metrics.iq_dc_shift_k);
    return b;
}

const char*
lbl_ted_gain(const void* v, char* b, size_t n) {
    dsd_frontend_metrics metrics = menu_frontend_metrics(v);
    float g = metrics.ted_gain;
    int g_milli = (int)(g * 1000.0f + 0.5f);
    DSD_SNPRINTF(b, n, "CQPSK Timing Gain: %d (x0.001, +/-)", g_milli);
    return b;
}

const char*
lbl_cqpsk_timing_bias(const void* v, char* b, size_t n) {
    dsd_frontend_metrics metrics = menu_frontend_metrics(v);
    DSD_SNPRINTF(b, n, "CQPSK Timing Bias (EMA): %d", metrics.cqpsk_timing_bias);
    return b;
}

const char*
lbl_dsp_panel(const void* v, char* b, size_t n) {
    UiCtx* c = (UiCtx*)v;
    DSD_SNPRINTF(b, n, "Show DSP Panel [%s]",
                 (c && c->opts && c->opts->frontend_display.show_dsp_panel) ? "On" : "Off");
    return b;
}

const char*
lbl_rtl_bias(const void* v, char* b, size_t n) {
    UiCtx* c = (UiCtx*)v;
    DSD_SNPRINTF(b, n, "Bias Tee: %s", (c->opts->rtl_bias_tee ? "On" : "Off"));
    return b;
}

const char*
lbl_rtl_rtltcp_autotune(const void* v, char* b, size_t n) {
    UiCtx* c = (UiCtx*)v;
    DSD_SNPRINTF(b, n, "RTL-TCP Adaptive Networking: %s", (c->opts->rtltcp_autotune ? "On" : "Off"));
    return b;
}

const char*
lbl_rtl_auto_ppm(const void* v, char* b, size_t n) {
    const UiCtx* c = (const UiCtx*)v;
    int on = dsd_app_frontend_auto_ppm_enabled((c && c->opts) ? c->opts->rtl_auto_ppm : 0);
    DSD_SNPRINTF(b, n, "Auto-PPM (Spectrum): %s", on ? "On" : "Off");
    return b;
}

const char*
lbl_rtl_tuner_autogain(const void* v, char* b, size_t n) {
    (void)v;
    const dsdneoRuntimeConfig* cfg = dsd_neo_get_config();
    int on = dsd_app_frontend_tuner_autogain_enabled((cfg && cfg->tuner_autogain_enable) ? 1 : 0);
    DSD_SNPRINTF(b, n, "Tuner Autogain: %s", on ? "On" : "Off");
    return b;
}

#endif /* USE_RADIO */
