// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Shape of the canned wideband spectrum the QML tests run against.
 *
 * The QML suite deliberately links no engine, so the app-control wideband
 * getter is stubbed (qml_spectrum_stub.cpp). These constants describe the frame
 * it hands back, so a test can compute where the peak should be rather than
 * hard-coding a frequency that would drift if the fixture changed.
 */

#ifndef DSD_NEO_TESTS_UI_QML_SPECTRUM_STUB_H_
#define DSD_NEO_TESTS_UI_QML_SPECTRUM_STUB_H_

#include <dsd-neo/core/wideband_spectrum.h>

namespace dsd_neo_qml_stub {

/* The published width, not a number of its own: the consumer refuses a frame
 * that is not exactly this wide, so a fixture that drifted from it would fail
 * as "no frames arrived" rather than as a mismatch. */
constexpr int kSpectrumBins = DSD_WIDEBAND_SPECTRUM_BINS;
constexpr unsigned int kSpectrumCenterHz = 851000000U;
constexpr unsigned int kSpectrumSpanHz = 1536000U;
/* Well away from DC, so a tap landing on it cannot be confused with a tap that
 * simply did nothing and left the view centered. */
constexpr int kSpectrumPeakBin = 700;

/** @brief Frequency of the stub's peak bin, using the published bin convention. */
constexpr double
spectrum_peak_hz() {
    return static_cast<double>(kSpectrumCenterHz)
           + ((static_cast<double>(kSpectrumPeakBin) - (kSpectrumBins / 2.0)) * static_cast<double>(kSpectrumSpanHz)
              / static_cast<double>(kSpectrumBins));
}

} // namespace dsd_neo_qml_stub

#endif /* DSD_NEO_TESTS_UI_QML_SPECTRUM_STUB_H_ */
