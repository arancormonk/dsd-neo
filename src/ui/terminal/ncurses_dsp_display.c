// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */
/*
 * ncurses_dsp_display.c
 * DSP status panel (RTL-SDR pipeline state)
 */

#include <curses.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/ui/ncurses_dsp_display.h>
#include <dsd-neo/ui/ui_prims.h>
#include <stdarg.h>
#include <string.h>
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "dsd-neo/platform/platform.h"
#include "ncurses_dsp_status_format.h"

#ifdef USE_RTLSDR
#include <dsd-neo/io/rtl_stream_c.h>
#endif

#ifdef USE_RADIO

/* Small helpers to align key/value fields to a consistent value column. */
static inline void
ui_print_label_pad(const char* label) {
    const int value_col = 14; /* column (from label start) where values begin */
    int lab_len = (int)strlen(label);
    if (lab_len < 0) {
        lab_len = 0;
    }
    /* Draw the left border in the primary UI color (teal/cyan) for consistency */
    ui_print_lborder();
    addch(' ');
    addstr(label);
    addch(':');
    int need = value_col - (lab_len + 1); /* +1 for ':' */
    if (need < 1) {
        need = 1; /* at least one space after ':' */
    }
    for (int i = 0; i < need; i++) {
        addch(' ');
    }
}

static void ui_print_kv_line(const char* label, const char* fmt, ...) DSD_ATTR_FORMAT(printf, 2, 3);

static void
ui_print_kv_line(const char* label, const char* fmt, ...) {
    ui_print_label_pad(label);
    va_list ap;
    va_start(ap, fmt);
    vw_printw(stdscr, fmt, ap);
    va_end(ap);
    addch('\n');
}

static void
dsp_status_print_squelch(const dsd_opts* opts) {
    if (!opts) {
        return;
    }
    char status[64];
    if (ui_dsp_format_squelch_status(opts->rtl_pwr, opts->rtl_squelch_level, status, sizeof(status)) == 0) {
        ui_print_kv_line("Squelch", "%s", status);
    }
}
#endif

#ifdef USE_RTLSDR

typedef struct {
    int cq;
    int fll;
    int ted;
    int iqb;
    int dc_k;
    int dc_on;
    int ted_force;
    int clk_mode;
    int clk_sync;
    int agc_on;
    int lim_on;
    int mod;
    float agc_tgt;
    float agc_min;
    float agc_up;
    float agc_down;
    const char* modlab;
} dsp_status_snapshot;

static const char*
dsp_status_mod_label(int mod) {
    if (mod == 1) {
        return "CQPSK";
    }
    if (mod == 2) {
        return "GFSK";
    }
    return "C4FM";
}

static void
dsp_status_capture(dsp_status_snapshot* snap, const dsd_state* state) {
    DSD_MEMSET(snap, 0, sizeof(*snap));
    rtl_stream_dsp_get(&snap->cq, &snap->fll, &snap->ted);
    snap->iqb = rtl_stream_get_iq_balance();
    snap->dc_on = rtl_stream_get_iq_dc(&snap->dc_k);
    snap->ted_force = rtl_stream_get_ted_force();
    snap->clk_mode = rtl_stream_get_c4fm_clk();
    snap->clk_sync = rtl_stream_get_c4fm_clk_sync();
    snap->agc_on = rtl_stream_get_fm_agc();
    rtl_stream_get_fm_agc_params(&snap->agc_tgt, &snap->agc_min, &snap->agc_up, &snap->agc_down);
    snap->lim_on = rtl_stream_get_fm_limiter();
    snap->mod = state ? state->rf_mod : (snap->cq ? 1 : 0);
    snap->modlab = dsp_status_mod_label(snap->mod);
}

static void
dsp_status_print_ted(const dsp_status_snapshot* snap) {
    int ted_sps = rtl_stream_get_ted_sps();
    float ted_gain = rtl_stream_get_ted_gain();
    int ted_bias = rtl_stream_ted_bias(NULL);
    ui_print_kv_line("TED", "[%s] sps:%d g:%.3f bias:%d%s", snap->ted ? "On" : "Off", ted_sps, ted_gain, ted_bias,
                     snap->ted_force ? " force" : "");
}

static void
dsp_status_print_front_and_path(const dsp_status_snapshot* snap) {
    ui_print_kv_line("Front", "IQBal:%s  IQ-DC:%s k:%d", snap->iqb ? "On" : "Off", snap->dc_on ? "On" : "Off",
                     snap->dc_k);
    ui_print_kv_line("Path", "Mod:%s  CQ:%s", snap->modlab, snap->cq ? "On" : "Off");
    ui_print_kv_line("FLL", "[%s]", snap->fll ? "On" : "Off");
    dsp_status_print_ted(snap);
    if (snap->mod == 1 || snap->cq) {
        ui_print_kv_line("CQPSK Path", "[%s]", snap->cq ? "On" : "Off");
    }
}

static void
dsp_status_print_cqpsk_metrics(void) {
    double cfo = rtl_stream_get_cfo_hz();
    int clk = rtl_stream_get_carrier_lock();
    rtl_stream_costas_metrics cm;
    DSD_MEMSET(&cm, 0, sizeof(cm));
    int e14 = rtl_stream_get_costas_err_q14();
    if (rtl_stream_get_costas_metrics(&cm) == 0) {
        e14 = cm.err_smooth_avg_q14;
    }
    int nco_q15 = rtl_stream_get_nco_q15();
    int Fs = rtl_stream_get_demod_rate_hz();
    double fll_be_hz = rtl_stream_get_fll_band_edge_freq_hz();
    ui_print_kv_line("FLL BE", "Freq=%+0.1f Hz", fll_be_hz);
    ui_print_kv_line("Carrier", "NCO=%+0.1f Hz  %s", cfo, clk ? "Locked" : "Acq");
    ui_print_kv_line("Costas/NCO", "ErrS=%d ErrR=%d Conf=%0.2f Fade=%d%% NCO(q15)=%d Fs=%d Hz", e14, cm.err_raw_avg_q14,
                     (double)cm.confidence_avg_q14 / 16384.0, cm.zero_conf_pct, nco_q15, Fs);
}

static void
dsp_status_print_fsk_metrics(void) {
    rtl_stream_fsk_metrics fm;
    DSD_MEMSET(&fm, 0, sizeof(fm));
    if (rtl_stream_get_fsk_metrics(&fm) != 0 || !fm.valid) {
        return;
    }
    ui_print_kv_line("FSK Soft", "rel:%u min:%u low:%4.1f%% clip:%4.1f%% err:%.3f snr:%4.1f dB", fm.mean_reliability,
                     fm.min_reliability, fm.low_reliability_pct, fm.clip_pct, fm.rms_error, fm.evm_snr_db);
    ui_print_kv_line("FSK Track", "acq:%s e:%.2f score:%.4f upd:%llu skip:%llu", fm.timing_acquired ? "Y" : "N",
                     fm.track_last_error, fm.track_last_score, (unsigned long long)fm.track_updates,
                     (unsigned long long)fm.track_skips);
}

static void
dsp_status_print_mode_tail(const dsp_status_snapshot* snap) {
    if (snap->mod == 0 || snap->clk_mode != 0) {
        const char* clk = (snap->clk_mode == 1) ? "EL" : (snap->clk_mode == 2) ? "MM" : "Off";
        ui_print_kv_line("C4FM", "CLK:%s%s", clk, (snap->clk_mode && snap->clk_sync) ? " (sync)" : "");
    }
    if (snap->mod != 1 || snap->agc_on || snap->lim_on) {
        ui_print_kv_line("FM AGC", "[%s] tgt:%.3f min:%.3f up:%.2f dn:%.2f | LIM:%s", snap->agc_on ? "On" : "Off",
                         snap->agc_tgt, snap->agc_min, snap->agc_up, snap->agc_down, snap->lim_on ? "On" : "Off");
    }
}
#endif

/* Print a compact DSP status summary (which blocks are active). */
void
print_dsp_status(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
#ifdef USE_RADIO
    /* Preserve current color pair so our colored header/HR won't force default */
#ifdef PRETTY_COLORS
    attr_t saved_attrs = 0;
    short saved_pair = 0;
    attr_get(&saved_attrs, &saved_pair, NULL);
#endif

#ifdef USE_RTLSDR
    dsp_status_snapshot snap;
    dsp_status_capture(&snap, state);
#endif

    ui_print_header("DSP");
    attron(COLOR_PAIR(14)); /* explicit yellow for DSP items */
#ifdef USE_RTLSDR
    dsp_status_print_front_and_path(&snap);
#endif
    dsp_status_print_squelch(opts);
#ifdef USE_RTLSDR
    if (snap.cq) {
        dsp_status_print_cqpsk_metrics();
    }
    dsp_status_print_fsk_metrics();
    dsp_status_print_mode_tail(&snap);
#endif
    attroff(COLOR_PAIR(14));
    attron(COLOR_PAIR(4));
    ui_print_hr();
    attroff(COLOR_PAIR(4));
    /* Restore previously active color pair (e.g., banner color) */
#ifdef PRETTY_COLORS
    attr_set(saved_attrs, saved_pair, NULL);
#endif
#endif
}
