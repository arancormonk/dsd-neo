// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Frequency/bin/viewport arithmetic for the spectrum view.
 *
 * Deliberately Qt-free and header-only so the parts that are easy to get
 * subtly wrong — which bin a tap lands on, where a clamped pan window sits,
 * whether a pinch keeps the frequency under the fingers still — are testable
 * without a window, a scene graph, or a running decoder.
 *
 * Bin convention matches what the io layer actually publishes: bins are
 * fftshifted, so bin i of n sits at `center + (i - n/2) * span/n`. Bin n/2 is
 * DC, i.e. exactly the tuned center. (A tempting alternative is to treat the n
 * bins as tiles and put bin centers at `(i + 0.5) * span/n`; that is off by
 * half a bin — 750 Hz at n=1024 over 1.536 MHz — and would bias every
 * tap-to-tune by that much.)
 */

#ifndef DSD_NEO_SRC_UI_QT_SPECTRUM_MATH_H_
#define DSD_NEO_SRC_UI_QT_SPECTRUM_MATH_H_

#include <cmath>

namespace dsd_qt {
namespace spectrum_math {

/** @brief Lowest and highest zoom the view allows. */
constexpr double kMinZoom = 1.0;
constexpr double kMaxZoom = 8.0;

/** @brief Tap snap half-width bounds, in Hz. */
constexpr double kMinSnapHz = 6000.0;
constexpr double kMaxSnapHz = 25000.0;

/** @brief Clamp a requested zoom into the supported range. */
inline double
clamp_zoom(double zoom) {
    if (!(zoom > kMinZoom)) { // also catches NaN
        return kMinZoom;
    }
    return (zoom > kMaxZoom) ? kMaxZoom : zoom;
}

/** @brief Center frequency of bin @p i, in Hz. */
inline double
bin_to_freq_hz(double center, double span, int n, int i) {
    if (n <= 0) {
        return center;
    }
    return center + ((static_cast<double>(i) - (static_cast<double>(n) / 2.0)) * span / static_cast<double>(n));
}

/** @brief Bin nearest frequency @p f, clamped to [0, n-1]. */
inline int
freq_to_bin(double center, double span, int n, double f) {
    if (n <= 0) {
        return 0;
    }
    if (!(span > 0.0)) {
        return n / 2;
    }
    const double low = center - (span / 2.0);
    const double raw = std::floor((((f - low) / span) * static_cast<double>(n)) + 0.5);
    if (!(raw > 0.0)) {
        return 0;
    }
    if (raw > static_cast<double>(n - 1)) {
        return n - 1;
    }
    return static_cast<int>(raw);
}

/** @brief The visible slice of the capture span. */
struct ViewWindow {
    double low_hz = 0.0;
    double high_hz = 0.0;
    double span_hz = 0.0;
};

/**
 * @brief The window a zoom/pan pair actually shows, kept inside the capture span.
 *
 * The requested window is centered on `center + offset_hz` and `span/zoom`
 * wide. Panning past an edge is not an error — it is how the user asks for a
 * frequency the hardware is not covering — so the window clamps and the excess
 * is reported through @p overshoot_hz for the caller to turn into a retune.
 *
 * @param overshoot_hz [out] Optional; requested offset minus granted offset,
 *                     signed, 0 while the window fits.
 */
inline ViewWindow
view_window(double center, double span, double zoom, double offset_hz, double* overshoot_hz) {
    if (overshoot_hz) {
        *overshoot_hz = 0.0;
    }
    ViewWindow win;
    if (!(span > 0.0)) {
        win.low_hz = center;
        win.high_hz = center;
        win.span_hz = 0.0;
        return win;
    }

    const double view_span = span / clamp_zoom(zoom);
    const double half = view_span / 2.0;
    const double min_center = (center - (span / 2.0)) + half;
    const double max_center = (center + (span / 2.0)) - half;

    double view_center = center + offset_hz;
    if (min_center >= max_center) {
        view_center = center; /* fully zoomed out: there is nowhere to pan */
    } else if (view_center < min_center) {
        view_center = min_center;
    } else if (view_center > max_center) {
        view_center = max_center;
    }

    if (overshoot_hz) {
        *overshoot_hz = (center + offset_hz) - view_center;
    }
    win.low_hz = view_center - half;
    win.high_hz = view_center + half;
    win.span_hz = view_span;
    return win;
}

/**
 * @brief How far either side of a tap to look for the real peak, in Hz.
 *
 * Scales with the zoom so a tap means the same thing on screen at every zoom
 * level, and is capped so a fully zoomed-out tap cannot drag the receiver two
 * channels away from where the finger landed.
 */
inline double
snap_window_hz(double view_span_hz) {
    const double raw = view_span_hz / 20.0;
    if (!(raw > kMinSnapHz)) {
        return kMinSnapHz;
    }
    return (raw > kMaxSnapHz) ? kMaxSnapHz : raw;
}

/**
 * @brief Strongest bin within +/-@p half_width_bins of @p center_bin, bounds-clamped.
 *
 * The search starts at @p center_bin and only leaves it for a strictly stronger
 * bin, which is what makes it a snap rather than a magnet: over flat noise
 * every candidate ties, and a search seeded at the window edge would answer
 * with that edge — pulling a deliberate tap on a quiet channel tens of kHz away
 * every time.
 */
inline int
peak_search_bin(const float* db, int n, int center_bin, int half_width_bins) {
    if (!db || n <= 0) {
        return 0;
    }
    if (center_bin < 0) {
        center_bin = 0;
    }
    if (center_bin > n - 1) {
        center_bin = n - 1;
    }
    if (half_width_bins < 0) {
        half_width_bins = 0;
    }
    int lo = center_bin - half_width_bins;
    int hi = center_bin + half_width_bins;
    if (lo < 0) {
        lo = 0;
    }
    if (hi > n - 1) {
        hi = n - 1;
    }
    int best = center_bin;
    for (int i = lo; i <= hi; i++) {
        if (db[i] > db[best]) {
            best = i;
        }
    }
    return best;
}

/**
 * @brief Offset that holds the frequency under @p x_fraction still across a zoom.
 *
 * A pinch that moves the signal out from under the fingers reads as broken, so
 * the anchor frequency is measured in the old window and re-placed at the same
 * screen fraction in the new one. The result is a raw offset — feed it back
 * through view_window(), which clamps it.
 */
inline double
zoom_anchor_offset_hz(double center, double span, double old_zoom, double old_offset_hz, double new_zoom,
                      double x_fraction) {
    const ViewWindow old_win = view_window(center, span, old_zoom, old_offset_hz, nullptr);
    if (!(old_win.span_hz > 0.0)) {
        return 0.0;
    }
    if (x_fraction < 0.0) {
        x_fraction = 0.0;
    }
    if (x_fraction > 1.0) {
        x_fraction = 1.0;
    }
    const double anchor_hz = old_win.low_hz + (x_fraction * old_win.span_hz);
    const double new_span = span / clamp_zoom(new_zoom);
    const double new_center = anchor_hz + ((0.5 - x_fraction) * new_span);
    return new_center - center;
}

/**
 * @brief A 1/2/5x10^k axis step no smaller than `view_span_hz / max_ticks`.
 *
 * Round numbers only: an axis labelled 851.0000 / 851.2000 / 851.4000 is
 * readable at a glance, one labelled 851.0374 / 851.2921 is not. Because the
 * step rounds up, a window can still contain max_ticks + 1 multiples of it;
 * the caller caps the list it emits.
 *
 * @return The step in Hz, or 0 when there is nothing to label.
 */
inline double
nice_tick_step_hz(double view_span_hz, int max_ticks) {
    if (!(view_span_hz > 0.0)) {
        return 0.0;
    }
    if (max_ticks < 1) {
        max_ticks = 1;
    }
    const double raw = view_span_hz / static_cast<double>(max_ticks);
    const double exponent = std::floor(std::log10(raw));
    const double magnitude = std::pow(10.0, exponent);
    const double base = raw / magnitude;
    double nice = 10.0;
    if (base <= 1.0) {
        nice = 1.0;
    } else if (base <= 2.0) {
        nice = 2.0;
    } else if (base <= 5.0) {
        nice = 5.0;
    }
    return nice * magnitude;
}

/**
 * @brief A display range that follows the signal without flickering.
 *
 * Ranging to each frame's own min and max makes the picture breathe on every
 * update and turns an empty band into amplified noise. This tracks a floor and
 * a ceiling and relaxes toward the current frame a few percent at a time, so a
 * transmission appearing is a signal rising into a stable frame rather than the
 * whole display rescaling around it.
 *
 * The floor comes from the mean rather than the minimum: most bins are noise,
 * so the mean is the noise floor, while a single deep null would drag a
 * minimum-based floor tens of dB down.
 */
struct AutoRange {
    double min_db = 0.0;
    double max_db = 0.0;
    bool seeded = false;

    /** @brief Fold one frame in. */
    void
    update(const float* db, int n) {
        if (!db || n <= 0) {
            return;
        }
        double sum = 0.0;
        double peak = static_cast<double>(db[0]);
        for (int i = 0; i < n; i++) {
            const double v = static_cast<double>(db[i]);
            sum += v;
            if (v > peak) {
                peak = v;
            }
        }
        const double want_min = (sum / static_cast<double>(n)) - 6.0;
        const double want_max = peak + 3.0;
        if (!seeded) {
            min_db = want_min;
            max_db = want_max;
            seeded = true;
            return;
        }
        const double relax = 0.05;
        min_db += relax * (want_min - min_db);
        max_db += relax * (want_max - max_db);
    }

    /** @brief Height of the range in dB, floored so it can always be divided by. */
    double
    span_db() const {
        const double span = max_db - min_db;
        return (span < 20.0) ? 20.0 : span;
    }

    /** @brief Where @p db sits in the range, clamped to [0, 1]. */
    double
    normalize(double db) const {
        const double t = (db - min_db) / span_db();
        if (!(t > 0.0)) {
            return 0.0;
        }
        return (t > 1.0) ? 1.0 : t;
    }

    /** @brief Forget the tracked range (new session, new frequency). */
    void
    reset() {
        min_db = 0.0;
        max_db = 0.0;
        seeded = false;
    }
};

} // namespace spectrum_math
} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_SPECTRUM_MATH_H_ */
