// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Canned wideband spectrum for the QML suite.
 *
 * UI_QT_QML_CALL_LISTS links Qt and the two spectrum classes but no engine, so
 * the app-control boundary those classes call is supplied here: a flat noise
 * floor with one unmistakable peak, gated on the enable flag exactly as the
 * real getter is, so the view's start/stop behaviour is under test too.
 */

#include <stdint.h>

#include <dsd-neo/app_control/frontend.h>

#include "qml_spectrum_stub.h"

using namespace dsd_neo_qml_stub;

namespace {

int g_enabled = 0;

} // namespace

extern "C" int
dsd_app_frontend_wideband_spectrum_get(float* out_db, int max_bins, uint32_t* out_center_freq_hz,
                                       uint32_t* out_span_hz) {
    if (!out_db || max_bins <= 0 || g_enabled == 0) {
        return 0;
    }
    const int n = (max_bins < kSpectrumBins) ? max_bins : kSpectrumBins;
    for (int i = 0; i < n; i++) {
        out_db[i] = -80.0F;
    }
    /* A three-bin peak, so a snap search that is off by one still finds it and a
     * search that ignores the window entirely still fails. */
    for (int i = kSpectrumPeakBin - 1; i <= kSpectrumPeakBin + 1; i++) {
        if (i >= 0 && i < n) {
            out_db[i] = (i == kSpectrumPeakBin) ? -18.0F : -30.0F;
        }
    }
    if (out_center_freq_hz) {
        *out_center_freq_hz = kSpectrumCenterHz;
    }
    if (out_span_hz) {
        *out_span_hz = kSpectrumSpanHz;
    }
    return n;
}

extern "C" void
dsd_app_frontend_wideband_spectrum_set_enabled(int on) {
    g_enabled = on ? 1 : 0;
}
