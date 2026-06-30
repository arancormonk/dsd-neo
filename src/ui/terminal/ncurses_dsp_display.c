// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */
/*
 * ncurses_dsp_display.c
 * DSP status panel (RTL-SDR pipeline state)
 */

#include <curses.h>
#include <dsd-neo/app_control/frontend.h>
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
    dsd_frontend_metrics metrics;
    int cq;
    int cq_timing;
    int iqb;
    int dc_k;
    int dc_on;
    int mod;
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
    (void)dsd_app_frontend_get_metrics(NULL, state, &snap->metrics);
    snap->cq = snap->metrics.cqpsk_enable;
    snap->cq_timing = snap->metrics.cqpsk_timing_active;
    snap->iqb = snap->metrics.iq_balance;
    snap->dc_on = snap->metrics.iq_dc_enabled;
    snap->dc_k = snap->metrics.iq_dc_shift_k;
    snap->mod = state ? state->rf_mod : (snap->cq ? 1 : 0);
    snap->modlab = dsp_status_mod_label(snap->mod);
}

static void
dsp_status_print_ted(const dsp_status_snapshot* snap) {
    ui_print_kv_line("CQPSK Timing", "[%s] sps:%d g:%.3f bias:%d", snap->cq_timing ? "On" : "Off",
                     snap->metrics.ted_sps, snap->metrics.ted_gain, snap->metrics.cqpsk_timing_bias);
}

static void
dsp_status_print_front_and_path(const dsp_status_snapshot* snap) {
    ui_print_kv_line("Front", "IQBal:%s  IQ-DC:%s k:%d", snap->iqb ? "On" : "Off", snap->dc_on ? "On" : "Off",
                     snap->dc_k);
    ui_print_kv_line("Path", "Mod:%s  CQ:%s", snap->modlab, snap->cq ? "On" : "Off");
    dsp_status_print_ted(snap);
    if (snap->mod == 1 || snap->cq) {
        ui_print_kv_line("CQPSK Path", "[%s]", snap->cq ? "On" : "Off");
    }
}

static void
dsp_status_print_cqpsk_metrics(const dsp_status_snapshot* snap) {
    const dsd_frontend_metrics* metrics = &snap->metrics;
    ui_print_kv_line("FLL BE", "Freq=%+0.1f Hz", metrics->fll_band_edge_freq_hz);
    ui_print_kv_line("Carrier", "NCO=%+0.1f Hz  %s", metrics->cfo_hz, metrics->carrier_lock ? "Locked" : "Acq");
    ui_print_kv_line("Costas/NCO", "ErrS=%d ErrR=%d Conf=%0.2f Fade=%d%% NCO(q15)=%d Fs=%d Hz", metrics->costas_err_q14,
                     metrics->costas.err_raw_avg_q14, (double)metrics->costas.confidence_avg_q14 / 16384.0,
                     metrics->costas.zero_conf_pct, metrics->nco_q15, metrics->demod_rate_hz);
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
        dsp_status_print_cqpsk_metrics(&snap);
    }
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
