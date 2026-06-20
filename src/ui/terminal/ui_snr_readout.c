// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "ui_snr_readout.h"

#ifdef USE_RADIO
#include <dsd-neo/io/rtl_stream_c.h>
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

static double
ui_snr_get_c4fm_value(void) {
    double snr = rtl_stream_get_snr_c4fm();
    if (!ui_snr_value_is_valid(snr)) {
        double fb = rtl_stream_estimate_snr_c4fm_eye();
        if (ui_snr_value_is_valid(fb)) {
            snr = fb;
        }
    }
    return snr;
}

static double
ui_snr_get_qpsk_value(void) {
    double snr = rtl_stream_get_snr_cqpsk();
    if (!ui_snr_value_is_valid(snr)) {
        double fb = rtl_stream_estimate_snr_qpsk_const();
        if (ui_snr_value_is_valid(fb)) {
            snr = fb;
        } else {
            double snr_c = rtl_stream_get_snr_c4fm();
            double snr_g = rtl_stream_get_snr_gfsk();
            double snr_fb = (snr_c > snr_g) ? snr_c : snr_g;
            if (ui_snr_value_is_valid(snr_fb)) {
                snr = snr_fb;
            }
        }
    }
    return snr;
}

static double
ui_snr_get_gfsk_value(void) {
    double snr = rtl_stream_get_snr_gfsk();
    if (!ui_snr_value_is_valid(snr)) {
        double fb = rtl_stream_estimate_snr_gfsk_eye();
        if (ui_snr_value_is_valid(fb)) {
            snr = fb;
        }
    }
    return snr;
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
    } else {
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
ui_snr_readout_reset_for_test(void) {}
#endif
