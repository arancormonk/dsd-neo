// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "ui_snr_readout.h"

#ifdef USE_RADIO
#include <dsd-neo/app_control/frontend.h>
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

ui_snr_readout
ui_snr_readout_for_mod(int rf_mod) {
    ui_snr_readout out;
    out.mod_label = ui_snr_mod_label(rf_mod);

#ifdef USE_RADIO
    /* One fetch with every fallback filled, then the shared selection rule. Fetching
     * per modulation with a single-fallback mask meant three consumes for one readout,
     * and left each frontend free to disagree about which estimator a modulation uses.
     * See dsd_app_frontend_snr_for_mod(). */
    dsd_frontend_metrics metrics;
    (void)dsd_app_frontend_get_metrics_with_snr_fallbacks(&metrics, DSD_FRONTEND_SNR_FALLBACK_ALL);

    const dsd_frontend_snr_readout snr = dsd_app_frontend_snr_for_mod(&metrics, rf_mod);
    /* Carried through even when invalid: it is the reading this modulation's estimator
     * actually reported, which the meter logs alongside the invalid flag. */
    out.snr_db = snr.snr_db;
    out.valid = snr.valid;
#else
    out.valid = 0;
    out.snr_db = (double)UI_SNR_INVALID_DB;
#endif

    return out;
}
