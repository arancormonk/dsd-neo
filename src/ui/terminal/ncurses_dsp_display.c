// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */
/*
 * ncurses_dsp_display.c
 * DSP status panel (radio pipeline state)
 */

#include <curses.h>
#include <dsd-neo/app_control/frontend.h>
#include <dsd-neo/app_control/squelch_view.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/power.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/runtime/number_text.h>
#include <dsd-neo/ui/ncurses_dsp_display.h>
#include <dsd-neo/ui/ui_prims.h>
#include <string.h>
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "ncurses_dsp_status_format.h"
#include "ui_row.h"

#ifdef USE_RADIO

/* The panel width a DSP line fits. */
static int
dsp_panel_cols(void) {
    int rows = 0;
    int cols = 80;
    getmaxyx(stdscr, rows, cols);
    (void)rows;
    return (cols < 1) ? 80 : cols;
}

/* A reading at a fixed width: no negative zero, and never wider (">", "<" past it), so the fields after it hold their
   columns while it changes (issue #669). */
static const char*
dsp_num(char* buf, size_t size, double value, int width, int precision, unsigned flags) {
    (void)dsd_number_text(buf, size, value, width, precision, flags | DSD_NUMBER_TEXT_SATURATE);
    return buf;
}

/* One DSP line: the border in the primary UI color (teal/cyan), then the label padded to the value column (14 from the
   label's start), then the caller's fields, cut to the panel instead of wrapped (issue #669). */
static void
dsp_row_begin(ui_row* row, const char* label) {
    enum { value_col = 14 };

    ui_print_lborder();
    ui_row_begin(row, dsp_panel_cols(), 1);
    const int lab_len = (int)strlen(label);
    const int need = value_col - (lab_len + 1) < 1 ? 1 : value_col - (lab_len + 1); /* +1 for ':' */
    ui_row_field(row, " %s:%*s", label, need, "");
}

static void
dsp_status_print_squelch(const dsd_opts* opts, const dsd_state* state) {
    if (!opts) {
        return;
    }
    /* The threshold in force, marked when a scan row or target set it (issue #521). */
    dsd_app_squelch_view view;
    const int view_ok = dsd_app_squelch_view_get(opts, state, &view) == 0;
    const int row_override = view_ok && view.row_override;
    ui_row row;
    if (view_ok && (view.effective_auto || view.effective_noise)) {
        /* The dynamic squelch's gate, the channel power and the setting with what it shows (issue #518 follow-up), each
           at a fixed width. */
        char text[96];
        char ch[16];
        (void)dsd_app_squelch_view_format_ex(&view, DSD_APP_SQUELCH_VIEW_FIXED, text, sizeof text);
        const char* gate =
            view.effective_off ? "Off" : ((view.auto_gate_open || view.noise_gate_open) ? "Open" : "Closed");
        dsp_row_begin(&row, "Squelch");
        ui_row_field(&row, "%-6s", gate);
        ui_row_field(&row, " ch:%s dB", dsp_num(ch, sizeof ch, pwr_to_dB(opts->rtl_pwr), 6, 1, 0));
        ui_row_field(&row, " sql:%s", text);
        ui_row_end(&row);
        return;
    }
    char status[72];
    if (ui_dsp_format_squelch_status(opts->rtl_pwr, opts->rtl_squelch_level, row_override, status, sizeof(status))
        == 0) {
        dsp_row_begin(&row, "Squelch");
        ui_row_field(&row, "%s", status);
        ui_row_end(&row);
    }
}

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
    (void)dsd_app_frontend_get_metrics(&snap->metrics);
    snap->cq = snap->metrics.cqpsk_enable;
    snap->cq_timing = snap->metrics.cqpsk_timing_active;
    snap->iqb = snap->metrics.iq_balance;
    snap->dc_on = snap->metrics.iq_dc_enabled;
    snap->dc_k = snap->metrics.iq_dc_shift_k;
    snap->mod = state ? state->rf_mod : (snap->cq ? 1 : 0);
    snap->modlab = dsp_status_mod_label(snap->mod);
}

static const char*
dsp_on_off(int on) {
    return on ? "On" : "Off";
}

static void
dsp_status_print_ted(const dsp_status_snapshot* snap) {
    char num[16];
    ui_row row;
    dsp_row_begin(&row, "CQPSK Timing");
    ui_row_field(&row, "[%-3s]", dsp_on_off(snap->cq_timing));
    ui_row_field(&row, " sps:%s", dsp_num(num, sizeof num, (double)snap->metrics.ted_sps, 2, 0, 0));
    ui_row_field(&row, " g:%s", dsp_num(num, sizeof num, (double)snap->metrics.ted_gain, 5, 3, 0));
    ui_row_field(&row, " bias:%s",
                 dsp_num(num, sizeof num, (double)snap->metrics.cqpsk_timing_bias, 6, 0, DSD_NUMBER_TEXT_PLUS));
    ui_row_end(&row);
}

static void
dsp_status_print_front_and_path(const dsp_status_snapshot* snap) {
    char num[16];
    ui_row row;
    dsp_row_begin(&row, "Front");
    ui_row_field(&row, "IQBal:%-3s", dsp_on_off(snap->iqb));
    ui_row_field(&row, "  IQ-DC:%-3s", dsp_on_off(snap->dc_on));
    ui_row_field(&row, " k:%s", dsp_num(num, sizeof num, (double)snap->dc_k, 2, 0, 0));
    ui_row_end(&row);
    dsp_row_begin(&row, "Path");
    ui_row_field(&row, "Mod:%-5s", snap->modlab);
    ui_row_field(&row, "  CQ:%-3s", dsp_on_off(snap->cq));
    ui_row_end(&row);
    dsp_status_print_ted(snap);
    if (snap->mod == 1 || snap->cq) {
        dsp_row_begin(&row, "CQPSK Path");
        ui_row_field(&row, "[%s]", dsp_on_off(snap->cq));
        ui_row_end(&row);
    }
}

static void
dsp_status_print_cqpsk_metrics(const dsp_status_snapshot* snap) {
    const dsd_frontend_metrics* metrics = &snap->metrics;
    char num[16];
    ui_row row;
    dsp_row_begin(&row, "FLL BE");
    ui_row_field(&row, "Freq=%s Hz",
                 dsp_num(num, sizeof num, metrics->fll_band_edge_freq_hz, 8, 1, DSD_NUMBER_TEXT_PLUS));
    ui_row_end(&row);
    dsp_row_begin(&row, "Carrier");
    ui_row_field(&row, "NCO=%s Hz", dsp_num(num, sizeof num, metrics->cfo_hz, 8, 1, DSD_NUMBER_TEXT_PLUS));
    ui_row_field(&row, "  %s", metrics->carrier_lock ? "Locked" : "Acq");
    ui_row_end(&row);
    dsp_row_begin(&row, "Costas/NCO");
    ui_row_field(&row, "ErrS=%s", dsp_num(num, sizeof num, (double)metrics->costas_err_q14, 6, 0, 0));
    ui_row_field(&row, " ErrR=%s", dsp_num(num, sizeof num, (double)metrics->costas.err_raw_avg_q14, 6, 0, 0));
    ui_row_field(&row, " Conf=%s",
                 dsp_num(num, sizeof num, (double)metrics->costas.confidence_avg_q14 / 16384.0, 4, 2, 0));
    ui_row_field(&row, " Fade=%s%%", dsp_num(num, sizeof num, (double)metrics->costas.zero_conf_pct, 3, 0, 0));
    ui_row_field(&row, " NCO(q15)=%s", dsp_num(num, sizeof num, (double)metrics->nco_q15, 6, 0, 0));
    ui_row_field(&row, " Fs=%s Hz", dsp_num(num, sizeof num, (double)metrics->demod_rate_hz, 6, 0, 0));
    ui_row_end(&row);
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

    dsp_status_snapshot snap;
    dsp_status_capture(&snap, state);

    ui_print_header("DSP");
    attron(COLOR_PAIR(14)); /* explicit yellow for DSP items */
    dsp_status_print_front_and_path(&snap);
    dsp_status_print_squelch(opts, state);
    if (snap.cq) {
        dsp_status_print_cqpsk_metrics(&snap);
    }
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
