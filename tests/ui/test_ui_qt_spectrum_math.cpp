// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Regression test: the spectrum viewport arithmetic (spectrum_math.h).
 *
 * These are the parts of a spectrum view that are wrong in ways nobody notices
 * until the receiver lands on the wrong channel: which bin a tap maps to, where
 * a clamped pan actually sits and how much of the pan was refused, and whether
 * a pinch keeps the signal under the fingers.
 */

// LLVM 22/GCC 16 misclassifies these runtime test oracles as compile-time assertions.
// NOLINTBEGIN(cert-dcl03-c,misc-static-assert)

#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>

#include <dsd-neo/core/safe_api.h>

#include "spectrum_math.h"

namespace sm = dsd_qt::spectrum_math;

namespace {

constexpr double kCenter = 851000000.0;
constexpr double kSpan = 1536000.0;
constexpr int kBins = 1024;
/* A millihertz. Absurdly strict for a radio, but loose enough that double
 * rounding on absolute frequencies near 851 MHz is not mistaken for a bug. */
constexpr double kTolHz = 1.0e-3;

bool
near(double got, double want, double tol) {
    return std::fabs(got - want) <= tol;
}

void
test_bin_freq_round_trip(void) {
    /* Bin n/2 is DC: exactly the tuned center, not half a bin off it. */
    assert(near(sm::bin_to_freq_hz(kCenter, kSpan, kBins, kBins / 2), kCenter, kTolHz));
    assert(near(sm::bin_to_freq_hz(kCenter, kSpan, kBins, 0), kCenter - (kSpan / 2.0), kTolHz));
    assert(
        near(sm::bin_to_freq_hz(kCenter, kSpan, kBins, kBins - 1), kCenter + (kSpan / 2.0) - (kSpan / kBins), kTolHz));

    for (int i = 0; i < kBins; i++) {
        const double f = sm::bin_to_freq_hz(kCenter, kSpan, kBins, i);
        assert(sm::freq_to_bin(kCenter, kSpan, kBins, f) == i);
    }

    /* Off the ends clamps rather than wrapping or indexing out of range. */
    assert(sm::freq_to_bin(kCenter, kSpan, kBins, kCenter - kSpan) == 0);
    assert(sm::freq_to_bin(kCenter, kSpan, kBins, kCenter + kSpan) == kBins - 1);

    /* A +200 kHz tone lands where the io-layer orientation test says it does. */
    const int tone = sm::freq_to_bin(kCenter, kSpan, kBins, kCenter + 200000.0);
    const int expected = (kBins / 2) + static_cast<int>(std::lround(200000.0 / kSpan * kBins));
    assert(tone == expected);

    /* Degenerate inputs do not divide by zero. */
    assert(near(sm::bin_to_freq_hz(kCenter, kSpan, 0, 3), kCenter, kTolHz));
    assert(sm::freq_to_bin(kCenter, 0.0, kBins, kCenter) == kBins / 2);
}

void
test_view_window_clamping(void) {
    double overshoot = -1.0;

    /* Fully zoomed out there is nowhere to pan: any offset is refused whole. */
    sm::ViewWindow win = sm::view_window(kCenter, kSpan, 1.0, 0.0, &overshoot);
    assert(near(win.span_hz, kSpan, kTolHz));
    assert(near(win.low_hz, kCenter - (kSpan / 2.0), kTolHz));
    assert(near(overshoot, 0.0, kTolHz));

    sm::view_window(kCenter, kSpan, 1.0, 100000.0, &overshoot);
    assert(near(overshoot, 100000.0, kTolHz));

    /* At 4x the window is a quarter wide and can travel 3/8 of the span each way. */
    const double quarter = kSpan / 4.0;
    const double reach = (kSpan / 2.0) - (quarter / 2.0);
    win = sm::view_window(kCenter, kSpan, 4.0, 0.0, &overshoot);
    assert(near(win.span_hz, quarter, kTolHz));
    assert(near(overshoot, 0.0, kTolHz));

    win = sm::view_window(kCenter, kSpan, 4.0, reach, &overshoot);
    assert(near(win.high_hz, kCenter + (kSpan / 2.0), kTolHz));
    assert(near(overshoot, 0.0, kTolHz));

    /* Past the top edge: clamped, and the excess is reported positive. */
    win = sm::view_window(kCenter, kSpan, 4.0, reach + 50000.0, &overshoot);
    assert(near(win.high_hz, kCenter + (kSpan / 2.0), kTolHz));
    assert(near(overshoot, 50000.0, kTolHz));

    /* Past the bottom edge: negative. */
    win = sm::view_window(kCenter, kSpan, 4.0, -reach - 50000.0, &overshoot);
    assert(near(win.low_hz, kCenter - (kSpan / 2.0), kTolHz));
    assert(near(overshoot, -50000.0, kTolHz));

    /* Out-of-range zoom is clamped, not honoured. */
    win = sm::view_window(kCenter, kSpan, 100.0, 0.0, &overshoot);
    assert(near(win.span_hz, kSpan / sm::kMaxZoom, kTolHz));
    win = sm::view_window(kCenter, kSpan, 0.1, 0.0, &overshoot);
    assert(near(win.span_hz, kSpan, kTolHz));

    /* No span, no window — and no NaN. */
    win = sm::view_window(kCenter, 0.0, 4.0, 1000.0, &overshoot);
    assert(near(win.span_hz, 0.0, kTolHz));
    assert(near(overshoot, 0.0, kTolHz));
}

void
test_snap_window(void) {
    /* Zoomed all the way out the snap is capped, or a tap near one channel
     * could drag the receiver to another. */
    assert(near(sm::snap_window_hz(kSpan), 25000.0, kTolHz));
    /* 8x of that span: 192 kHz / 20. */
    assert(near(sm::snap_window_hz(kSpan / 8.0), 9600.0, kTolHz));
    /* And a floor, so a deep zoom still tolerates a fat fingertip. */
    assert(near(sm::snap_window_hz(1000.0), 6000.0, kTolHz));
}

void
test_peak_search(void) {
    std::vector<float> db(64, -90.0F);
    db[10] = -20.0F; /* near peak */
    db[40] = -5.0F;  /* stronger, but far away */

    assert(sm::peak_search_bin(db.data(), 64, 12, 5) == 10);
    /* A stronger peak outside the window must not win. */
    assert(sm::peak_search_bin(db.data(), 64, 12, 5) != 40);
    /* Widen far enough and it does. */
    assert(sm::peak_search_bin(db.data(), 64, 12, 40) == 40);

    /* Over flat noise nothing is stronger than anything else, and the answer
     * must be where the tap landed. Seeding the search at the window edge would
     * silently drag every tap on a quiet channel to that edge. */
    const std::vector<float> flat(64, -90.0F);
    assert(sm::peak_search_bin(flat.data(), 64, 30, 16) == 30);
    assert(sm::peak_search_bin(flat.data(), 64, 0, 16) == 0);
    assert(sm::peak_search_bin(flat.data(), 64, 63, 16) == 63);

    /* Windows clip at the array edges rather than reading past them. */
    db[0] = -1.0F;
    assert(sm::peak_search_bin(db.data(), 64, 1, 10) == 0);
    db[63] = -1.0F;
    assert(sm::peak_search_bin(db.data(), 64, 62, 10) == 63);

    /* A center outside the array is clamped in. */
    assert(sm::peak_search_bin(db.data(), 64, -5, 0) == 0);
    assert(sm::peak_search_bin(db.data(), 64, 900, 0) == 63);

    /* Defensive: no data, no crash. */
    assert(sm::peak_search_bin(nullptr, 64, 3, 2) == 0);
    assert(sm::peak_search_bin(db.data(), 0, 3, 2) == 0);
}

/* The frequency shown at x_fraction, after clamping. */
double
freq_at(double zoom, double offset, double x_fraction) {
    const sm::ViewWindow win = sm::view_window(kCenter, kSpan, zoom, offset, nullptr);
    return win.low_hz + (x_fraction * win.span_hz);
}

void
test_zoom_anchor(void) {
    const double fractions[] = {0.0, 0.25, 0.5, 0.75, 1.0};
    const double zooms[] = {1.0, 2.0, 4.0, 8.0};
    for (double x : fractions) {
        for (double from : zooms) {
            for (double to : zooms) {
                /* Start from a pan that is legal at `from`. */
                const double offset = (from > 1.0) ? ((kSpan / 2.0) - (kSpan / from / 2.0)) * 0.5 : 0.0;
                const double before = freq_at(from, offset, x);
                const double next = sm::zoom_anchor_offset_hz(kCenter, kSpan, from, offset, to, x);
                const double after = freq_at(to, next, x);

                if (to >= from) {
                    /* Zooming in only ever shrinks the window, so the anchor is
                     * always reachable and must be held exactly. */
                    assert(near(after, before, kTolHz));
                    continue;
                }
                /* Zooming out can ask for a window wider than the space left on
                 * one side. Then the anchor cannot be held — but the only
                 * acceptable reason is that the window is flush against a
                 * capture edge, never an arbitrary drift. */
                const sm::ViewWindow win = sm::view_window(kCenter, kSpan, to, next, nullptr);
                const bool flush = near(win.low_hz, kCenter - (kSpan / 2.0), kTolHz)
                                   || near(win.high_hz, kCenter + (kSpan / 2.0), kTolHz);
                assert(near(after, before, kTolHz) || flush);
            }
        }
    }

    /* Zooming in at an anchor away from the middle really does hold it exact. */
    const double before = freq_at(1.0, 0.0, 0.25);
    const double next = sm::zoom_anchor_offset_hz(kCenter, kSpan, 1.0, 0.0, 4.0, 0.25);
    assert(near(freq_at(4.0, next, 0.25), before, kTolHz));

    /* Zooming out from the middle has room on both sides, so it is exact too. */
    const double mid_before = freq_at(8.0, 0.0, 0.75);
    const double mid_next = sm::zoom_anchor_offset_hz(kCenter, kSpan, 8.0, 0.0, 2.0, 0.75);
    assert(near(freq_at(2.0, mid_next, 0.75), mid_before, kTolHz));
}

void
test_nice_tick_step(void) {
    /* Every step is 1, 2 or 5 times a power of ten... */
    const double spans[] = {kSpan, kSpan / 2.0, kSpan / 8.0, 12500.0, 137.0, 4.2e6};
    for (double span : spans) {
        for (int max_ticks = 1; max_ticks <= 8; max_ticks++) {
            const double step = sm::nice_tick_step_hz(span, max_ticks);
            assert(step > 0.0);
            /* ...and never finer than the requested density. */
            assert(step >= (span / max_ticks) * (1.0 - 1e-9));
            const double mantissa = step / std::pow(10.0, std::floor(std::log10(step)));
            assert(near(mantissa, 1.0, 1e-9) || near(mantissa, 2.0, 1e-9) || near(mantissa, 5.0, 1e-9));
            /* Rounding up bounds the label count at max_ticks + 1. */
            assert(std::floor(span / step) + 1.0 <= max_ticks + 1);
        }
    }

    assert(near(sm::nice_tick_step_hz(0.0, 5), 0.0, kTolHz));
    assert(sm::nice_tick_step_hz(kSpan, 0) > 0.0);
}

} // namespace

int
main(void) {
    test_bin_freq_round_trip();
    test_view_window_clamping();
    test_snap_window();
    test_peak_search();
    test_zoom_anchor();
    test_nice_tick_step();
    (void)DSD_FPRINTF(stdout, "UI_QT_SPECTRUM_MATH: ok\n");
    return 0;
}

// NOLINTEND(cert-dcl03-c,misc-static-assert)
