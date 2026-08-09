// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Regression test: what the spectrum model does with the frames it is handed,
 * and what the waterfall does with the model.
 *
 * The viewport arithmetic itself is UI_QT_SPECTRUM_MATH's job. What lives here
 * is the part that only shows up once a producer and a poll timer are running
 * against each other on separate clocks:
 *
 *  - a poll that re-reads the frame already on screen must not be announced as
 *    a new one, or the waterfall scrolls duplicate rows and reports a signal as
 *    lasting longer than it did;
 *  - the first frame captured at a new center must survive the history clear
 *    that the retune triggers, because it is the one row that is certain to be
 *    worth seeing;
 *  - a tap must never resolve to a frequency outside the visible window.
 *
 * The producer is stubbed here rather than mocked through the engine: the model
 * only ever sees the app-control getter, so supplying that is the whole of it.
 */

// LLVM 22/GCC 16 misclassifies these runtime test oracles as compile-time assertions.
// NOLINTBEGIN(cert-dcl03-c,misc-static-assert)

#include <QGuiApplication>
#include <QObject>
#include <QtGlobal>
#include <cassert>
#include <cstdint>
#include <stdio.h>

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/wideband_spectrum.h>

#include "spectrum_model.h"
#include "spectrum_view_item.h"

namespace {

/* ------------------------------------------------------------- producer ---- */

constexpr int kBins = DSD_WIDEBAND_SPECTRUM_BINS;
constexpr uint32_t kCenterHz = 851000000U;
constexpr uint32_t kSpanHz = 1536000U;

float g_bins[kBins];
uint32_t g_center_hz = kCenterHz;
uint32_t g_span_hz = kSpanHz;
uint32_t g_serial = 0;
int g_enabled = 0;
int g_get_calls = 0;

/** @brief Flat noise with one strong bin, as a real capture with one carrier. */
void
seed_bins(int peak_bin) {
    for (int i = 0; i < kBins; i++) {
        g_bins[i] = -80.0F;
    }
    if (peak_bin >= 0 && peak_bin < kBins) {
        g_bins[peak_bin] = -20.0F;
    }
}

/** @brief Publish a frame, as the demod thread would. */
void
publish(uint32_t center_hz) {
    g_center_hz = center_hz;
    g_serial++;
}

} // namespace

extern "C" int
dsd_app_frontend_wideband_spectrum_get(float* out_db, int max_bins, uint32_t* out_center_freq_hz, uint32_t* out_span_hz,
                                       uint32_t* out_frame_serial) {
    g_get_calls++;
    if (!out_db || max_bins < kBins || g_enabled == 0 || g_serial == 0) {
        return 0;
    }
    for (int i = 0; i < kBins; i++) {
        out_db[i] = g_bins[i];
    }
    if (out_center_freq_hz) {
        *out_center_freq_hz = g_center_hz;
    }
    if (out_span_hz) {
        *out_span_hz = g_span_hz;
    }
    if (out_frame_serial) {
        *out_frame_serial = g_serial;
    }
    return kBins;
}

extern "C" void
dsd_app_frontend_wideband_spectrum_set_enabled(int on) {
    g_enabled = on ? 1 : 0;
}

namespace {

/* ---------------------------------------------------------------- cases ---- */

/*
 * The poll timer and the publish period free-run, so most polls land on a frame
 * that is already drawn. Announcing one as new is what fills the waterfall with
 * rows nothing happened in.
 */
void
test_a_repeated_frame_is_not_a_new_frame(void) {
    seed_bins(kBins / 2);
    g_serial = 0;
    dsd_qt::SpectrumModel model;
    /* A plain counter rather than QSignalSpy: that lives in Qt's testlib, which
     * distributions package separately, and nothing else here needs it. */
    int frames = 0;
    QObject::connect(&model, &dsd_qt::SpectrumModel::frameChanged, &model, [&frames]() { frames++; });

    model.setActive(true);
    publish(kCenterHz);

    model.testPoll();
    assert(model.hasData());
    const quint64 first = model.frameIndex();
    assert(frames == 1);

    /* Three more polls, no new publish: the producer has not replaced the frame. */
    model.testPoll();
    model.testPoll();
    model.testPoll();
    assert(model.frameIndex() == first);
    assert(frames == 1);

    /* ...and a fresh publish is announced again. */
    publish(kCenterHz);
    model.testPoll();
    assert(model.frameIndex() == first + 1);
    assert(frames == 2);

    model.setActive(false);
}

/*
 * A retune clears the waterfall's history, which is right — rows measured at
 * the old center mean nothing at the new one. The frame that arrives *with* the
 * retune was captured at the new center, so it must still become a row.
 */
void
test_the_first_frame_after_a_retune_becomes_a_row(void) {
    seed_bins(kBins / 2);
    g_serial = 0;
    dsd_qt::SpectrumModel model;
    dsd_qt::WaterfallItem waterfall;
    waterfall.setModel(&model);
    int retunes = 0;
    QObject::connect(&model, &dsd_qt::SpectrumModel::retuned, &model, [&retunes]() { retunes++; });

    model.setActive(true);
    publish(kCenterHz);
    model.testPoll();
    assert(waterfall.testRowsWritten() == 1);

    publish(kCenterHz);
    model.testPoll();
    assert(waterfall.testRowsWritten() == 2);

    /* The tap lands, the front end moves, and the next frame is at the new
     * center. History resets to nothing — and then immediately takes that
     * frame, rather than skipping it as already seen. */
    publish(kCenterHz + 1000000U);
    model.testPoll();
    assert(retunes == 1);
    assert(waterfall.testRowsWritten() == 1);

    publish(kCenterHz + 1000000U);
    model.testPoll();
    assert(waterfall.testRowsWritten() == 2);

    model.setActive(false);
}

/*
 * The snap width is derived from the view span, so at high zoom it reaches
 * further than the screen does. A tap must never answer with a carrier the user
 * cannot see — that reads as a tap that landed somewhere random.
 */
void
test_a_tap_never_snaps_outside_the_visible_window(void) {
    /* One loud bin near the top of the capture, quiet everywhere else. */
    const int loud_bin = kBins - 8;
    seed_bins(loud_bin);
    g_serial = 0;
    dsd_qt::SpectrumModel model;

    model.setActive(true);
    publish(kCenterHz);
    model.testPoll();
    assert(model.hasData());

    /* Zoom in and pan so the loud bin sits just past the right-hand edge. */
    model.setZoom(8.0);
    const double bin_width = static_cast<double>(kSpanHz) / static_cast<double>(kBins);
    const double loud_hz =
        static_cast<double>(kCenterHz) + ((static_cast<double>(loud_bin) - (kBins / 2.0)) * bin_width);
    const double view_span = static_cast<double>(kSpanHz) / 8.0;
    /* Put the window's high edge a few bins below the carrier. */
    model.setViewOffsetHz((loud_hz - (4.0 * bin_width)) - (view_span / 2.0) - static_cast<double>(kCenterHz));

    const double high = model.viewHighHz();
    const double low = model.viewLowHz();
    assert(loud_hz > high); /* the carrier really is off screen */

    /* A tap on the right-hand edge, where the unbounded search would reach it. */
    const double tapped = model.tapFrequencyHz(1.0);
    assert(tapped >= low && tapped <= high);

    model.setActive(false);
}

/*
 * Production follows the view, so a closed view costs no FFT at all, and a
 * reopened one never shows the frame the last session left behind.
 */
void
test_production_follows_the_view(void) {
    seed_bins(kBins / 2);
    g_serial = 0;
    dsd_qt::SpectrumModel model;

    assert(g_enabled == 0);
    model.setActive(true);
    assert(g_enabled == 1);
    publish(kCenterHz);
    model.testPoll();
    assert(model.hasData());

    model.setActive(false);
    assert(g_enabled == 0);
    assert(!model.hasData());
    assert(model.binCount() == 0);
}

} // namespace

int
main(int argc, char** argv) {
    /* The waterfall is a QQuickItem, so it needs an application object; the
     * offscreen platform comes from the CTest registration. */
    QGuiApplication app(argc, argv);

    test_a_repeated_frame_is_not_a_new_frame();
    test_the_first_frame_after_a_retune_becomes_a_row();
    test_a_tap_never_snaps_outside_the_visible_window();
    test_production_follows_the_view();

    assert(g_get_calls > 0);
    (void)DSD_FPRINTF(stdout, "UI_QT_SPECTRUM_MODEL: ok\n");
    return 0;
}

// NOLINTEND(cert-dcl03-c,misc-static-assert)
