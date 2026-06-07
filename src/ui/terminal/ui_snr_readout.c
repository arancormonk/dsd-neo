// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "ui_snr_readout.h"

#ifdef USE_RADIO
#include <dsd-neo/io/rtl_stream_c.h>
#include <math.h>
#endif

enum { UI_SNR_INVALID_DB = -50 };

static const char*
ui_snr_mod_label(int rf_mod) {
    if (rf_mod == 1) {
        return "QPSK";
    }
    if (rf_mod == 2) {
        return "GFSK";
    }
    return "C4FM";
}

#ifdef USE_RADIO
static int
ui_snr_value_is_valid(double snr) {
    return snr > (double)UI_SNR_INVALID_DB;
}

enum {
    UI_SNR_STALE_THRESHOLD_CDB = 5, /* 0.05 dB in centi-dB */
    UI_SNR_STALE_LIMIT = 40
};

typedef struct ui_snr_stale_state {
    double last_snr;
    int stable_count;
} ui_snr_stale_state;

static ui_snr_stale_state g_c4fm_stale = {-999.0, 0};
static ui_snr_stale_state g_qpsk_stale = {-999.0, 0};
static ui_snr_stale_state g_gfsk_stale = {-999.0, 0};

static double
ui_snr_maybe_replace_stale(double snr, ui_snr_stale_state* state, double (*fallback_fn)(void)) {
    if (!state || !fallback_fn) {
        return snr;
    }

    int delta_cdb = (int)(fabs(snr - state->last_snr) * 100.0);
    if (delta_cdb < UI_SNR_STALE_THRESHOLD_CDB) {
        if (++state->stable_count >= UI_SNR_STALE_LIMIT) {
            double fb = fallback_fn();
            if (ui_snr_value_is_valid(fb)) {
                snr = fb;
            }
            state->stable_count = 0;
        }
    } else {
        state->stable_count = 0;
    }
    state->last_snr = snr;
    return snr;
}

#ifdef DSD_NEO_TEST_HOOKS
static void
ui_snr_stale_reset(ui_snr_stale_state* state) {
    if (!state) {
        return;
    }
    state->last_snr = -999.0;
    state->stable_count = 0;
}
#endif

static double
ui_snr_get_c4fm_value(void) {
    double snr = rtl_stream_get_snr_c4fm();
    if (!ui_snr_value_is_valid(snr)) {
        double fb = rtl_stream_estimate_snr_c4fm_eye();
        if (ui_snr_value_is_valid(fb)) {
            snr = fb;
            g_c4fm_stale.stable_count = 0;
        }
        return snr;
    }

    return ui_snr_maybe_replace_stale(snr, &g_c4fm_stale, rtl_stream_estimate_snr_c4fm_eye);
}

static double
ui_snr_get_qpsk_value(void) {
    double snr = rtl_stream_get_snr_cqpsk();
    if (!ui_snr_value_is_valid(snr)) {
        double fb = rtl_stream_estimate_snr_qpsk_const();
        if (ui_snr_value_is_valid(fb)) {
            snr = fb;
            g_qpsk_stale.stable_count = 0;
        } else {
            double snr_c = rtl_stream_get_snr_c4fm();
            double snr_g = rtl_stream_get_snr_gfsk();
            double snr_fb = (snr_c > snr_g) ? snr_c : snr_g;
            if (ui_snr_value_is_valid(snr_fb)) {
                snr = snr_fb;
            }
        }
        return snr;
    }

    return ui_snr_maybe_replace_stale(snr, &g_qpsk_stale, rtl_stream_estimate_snr_qpsk_const);
}

static double
ui_snr_get_gfsk_value(void) {
    double snr = rtl_stream_get_snr_gfsk();
    if (!ui_snr_value_is_valid(snr)) {
        double fb = rtl_stream_estimate_snr_gfsk_eye();
        if (ui_snr_value_is_valid(fb)) {
            snr = fb;
            g_gfsk_stale.stable_count = 0;
        }
        return snr;
    }

    return ui_snr_maybe_replace_stale(snr, &g_gfsk_stale, rtl_stream_estimate_snr_gfsk_eye);
}

static int
ui_snr_try_fsk_soft_value(double* out_snr) {
    if (!out_snr) {
        return 0;
    }

    rtl_stream_fsk_metrics metrics;
    if (rtl_stream_get_fsk_metrics(&metrics) != 0 || !metrics.valid || !ui_snr_value_is_valid(metrics.evm_snr_db)) {
        return 0;
    }

    *out_snr = (double)metrics.evm_snr_db;
    return 1;
}
#endif

ui_snr_readout
ui_snr_readout_for_mod(int rf_mod) {
    ui_snr_readout out;
    out.mod_label = ui_snr_mod_label(rf_mod);

#ifdef USE_RADIO
    double snr = (double)UI_SNR_INVALID_DB;
    if (rf_mod == 1) {
        snr = ui_snr_get_qpsk_value();
    } else if (!ui_snr_try_fsk_soft_value(&snr)) {
        snr = (rf_mod == 2) ? ui_snr_get_gfsk_value() : ui_snr_get_c4fm_value();
    }
    out.snr_db = snr;
    out.valid = ui_snr_value_is_valid(snr);
#else
    out.valid = 0;
    out.snr_db = (double)UI_SNR_INVALID_DB;
#endif

    return out;
}

#ifdef DSD_NEO_TEST_HOOKS
void
ui_snr_readout_reset_for_test(void) {
#ifdef USE_RADIO
    ui_snr_stale_reset(&g_c4fm_stale);
    ui_snr_stale_reset(&g_qpsk_stale);
    ui_snr_stale_reset(&g_gfsk_stale);
#endif
}
#endif
