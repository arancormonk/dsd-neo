// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */
/*
 * ncurses_dsp_display.c
 * DSP status panel (RTL-SDR pipeline state)
 */

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/ui/ncurses_dsp_display.h>

#include "dsd-neo/core/state_fwd.h"

#ifdef USE_RTLSDR
#include <curses.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/io/rtl_stream_c.h>
#include <dsd-neo/ui/ui_prims.h>
#include <stdarg.h>
#include <string.h>

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

static void
ui_print_kv_line(const char* label, const char* fmt, ...) {
    ui_print_label_pad(label);
    va_list ap;
    va_start(ap, fmt);
    vw_printw(stdscr, fmt, ap);
    va_end(ap);
    addch('\n');
}
#endif

/* Print a compact DSP status summary (which blocks are active). */
void
print_dsp_status(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
#ifdef USE_RTLSDR
    /* Preserve current color pair so our colored header/HR won't force default */
#ifdef PRETTY_COLORS
    attr_t saved_attrs = 0;
    short saved_pair = 0;
    attr_get(&saved_attrs, &saved_pair, NULL);
#endif
    int cq = 0, fll = 0, ted = 0;
    rtl_stream_dsp_get(&cq, &fll, &ted);
    int iqb = rtl_stream_get_iq_balance();
    int dc_k = 0;
    int dc_on = rtl_stream_get_iq_dc(&dc_k);
    int ted_force = rtl_stream_get_ted_force();
    int clk_mode = rtl_stream_get_c4fm_clk();
    int clk_sync = rtl_stream_get_c4fm_clk_sync();
    float agc_tgt = 0.0f, agc_min = 0.0f, agc_up = 0.0f, agc_down = 0.0f;
    int agc_on = rtl_stream_get_fm_agc();
    rtl_stream_get_fm_agc_params(&agc_tgt, &agc_min, &agc_up, &agc_down);
    int lim_on = rtl_stream_get_fm_limiter();

    ui_print_header("DSP");
    attron(COLOR_PAIR(14)); /* explicit yellow for DSP items */
    /* Determine current modulation for capability-aware display: 0=C4FM, 1=CQPSK, 2=GFSK */
    int mod = (state ? state->rf_mod : (cq ? 1 : 0));
    const char* modlab = "C4FM";
    if (mod == 1) {
        modlab = "CQPSK";
    } else if (mod == 2) {
        modlab = "GFSK";
    }

    /* Front-end helpers and path selection */
    ui_print_kv_line("Front", "IQBal:%s  IQ-DC:%s k:%d", iqb ? "On" : "Off", dc_on ? "On" : "Off", dc_k);
    ui_print_kv_line("Path", "Mod:%s  CQ:%s", modlab, cq ? "On" : "Off");
    ui_print_kv_line("FLL", "[%s]", fll ? "On" : "Off");
    /* Show TED status and basic timing metrics regardless of modulation so forced TED is visible. */
    {
        int ted_sps = rtl_stream_get_ted_sps();
        float ted_gain = rtl_stream_get_ted_gain();
        int ted_bias = rtl_stream_ted_bias(NULL);
        ui_print_kv_line("TED", "[%s] sps:%d g:%.3f bias:%d%s", ted ? "On" : "Off", ted_sps, ted_gain, ted_bias,
                         ted_force ? " force" : "");
    }
    if (mod == 1 || cq) {
        ui_print_kv_line("CQPSK Path", "[%s]", cq ? "On" : "Off");
    }

    if (cq) {
#ifdef USE_RTLSDR
        extern double rtl_stream_get_cfo_hz(void);
        extern double rtl_stream_get_residual_cfo_hz(void);
        extern int rtl_stream_get_carrier_lock(void);
        extern int rtl_stream_get_costas_err_q14(void);
        extern int rtl_stream_get_costas_metrics(rtl_stream_costas_metrics * out);
        extern int rtl_stream_get_nco_q15(void);
        extern int rtl_stream_get_demod_rate_hz(void);
        extern double rtl_stream_get_fll_band_edge_freq_hz(void);
        double cfo = rtl_stream_get_cfo_hz();
        int clk = rtl_stream_get_carrier_lock();
        rtl_stream_costas_metrics cm;
        memset(&cm, 0, sizeof(cm));
        int e14 = rtl_stream_get_costas_err_q14();
        if (rtl_stream_get_costas_metrics(&cm) == 0) {
            e14 = cm.err_smooth_avg_q14;
        }
        int nco_q15 = rtl_stream_get_nco_q15();
        int Fs = rtl_stream_get_demod_rate_hz();
        double fll_be_hz = rtl_stream_get_fll_band_edge_freq_hz();
        /* FLL band-edge shows coarse frequency offset being tracked */
        ui_print_kv_line("FLL BE", "Freq=%+0.1f Hz", fll_be_hz);
        /* Residual CFO is from FM discriminator, not meaningful for CQPSK - hide it */
        ui_print_kv_line("Carrier", "NCO=%+0.1f Hz  %s", cfo, clk ? "Locked" : "Acq");
        ui_print_kv_line("Costas/NCO", "ErrS=%d ErrR=%d Conf=%0.2f Fade=%d%% NCO(q15)=%d Fs=%d Hz", e14,
                         cm.err_raw_avg_q14, (double)cm.confidence_avg_q14 / 16384.0, cm.zero_conf_pct, nco_q15, Fs);
        {
            rtl_stream_cqpsk_eq_status eq;
            memset(&eq, 0, sizeof(eq));
            if (rtl_stream_get_cqpsk_eq_status(&eq) == 0) {
                const char* eq_state = "Off";
                if (eq.enabled) {
                    eq_state = eq.initialized ? ((eq.symbols >= 500U) ? "Run" : "Warm") : "Init";
                }
                ui_print_kv_line("CMA EQ", "[%s] taps:%d mu:%.4g syms:%u", eq_state, eq.taps, eq.mu, eq.symbols);
                if (eq.enabled) {
                    ui_print_kv_line("CMA Metric", "mag2:%.3f tgt:%.3f err:%.4f side:%.3f E:%.2f", eq.mag2_ema,
                                     eq.modulus, eq.err_ema, eq.max_side_tap_mag, eq.tap_energy);
                }
            }
        }
#else
        ui_print_kv_line("Carrier", "(RTL disabled)");
#endif
    }

    if (mod == 0 || clk_mode != 0) {
        const char* clk = (clk_mode == 1) ? "EL" : (clk_mode == 2) ? "MM" : "Off";
        ui_print_kv_line("C4FM", "CLK:%s%s", clk, (clk_mode && clk_sync) ? " (sync)" : "");
    }
    if (mod != 1 || agc_on || lim_on) {
        ui_print_kv_line("FM AGC", "[%s] tgt:%.3f min:%.3f up:%.2f dn:%.2f | LIM:%s", agc_on ? "On" : "Off", agc_tgt,
                         agc_min, agc_up, agc_down, lim_on ? "On" : "Off");
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
