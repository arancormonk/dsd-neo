// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "spectrum_model.h"

#include <QCoreApplication>
#include <QDebug>
#include <QGuiApplication>
#include <QMap>
#include <QString>
#include <QVariant>
#include <QVariantMap>
#include <Qt>
#include <cmath>
#include <stdint.h>

#include <dsd-neo/app_control/frontend.h>
#include <dsd-neo/core/wideband_spectrum.h>

namespace dsd_qt {

namespace {

/* Both come from the producer's own contract rather than being chosen here: a
 * poll slower than the publish period drops frames, and a buffer shorter than a
 * frame is refused outright. See <dsd-neo/core/wideband_spectrum.h>. */
/* Half the publish period, not the whole of it. Polling at exactly the producer's
 * rate is the boundary case that drops rows: the two clocks free-run, and a
 * CoarseTimer is allowed to fire late, so a poll that slips past one period
 * repeatedly finds a frame the producer has already replaced. Polling faster is
 * cheap — tick() rejects a re-read by serial before doing any work — and it is
 * what the header's own note ("a consumer polling faster only re-copies a frame
 * the producer has not replaced yet") describes as the safe direction. */
constexpr int kPollIntervalMs = DSD_WIDEBAND_SPECTRUM_PERIOD_MS / 2;
constexpr int kMaxBins = DSD_WIDEBAND_SPECTRUM_BINS;

/* How far above the frame's mean level a bin has to sit before "next signal" will
 * move the radio to it. Low enough that a weak but real carrier still counts, high
 * enough that the control does not walk across noise one bin at a time. */
constexpr double kNextPeakExcessDb = 8.0;

bool
state_is_showing(Qt::ApplicationState state) {
    return state != Qt::ApplicationSuspended && state != Qt::ApplicationHidden;
}

/**
 * @brief The visible bin range and the snap width, in bins.
 *
 * Shared by the tap and the hop because it is the same rule for both: the answer
 * has to be on screen, and anything nearer than the snap window is the same
 * channel a tap would already have reached. Two copies of it would let a tap and
 * a hop resolve to different carriers on one screen.
 */
struct ViewBins {
    int lo;
    int hi;
    int snap;
};

ViewBins
view_bins(double center_hz, double span_hz, int bin_count, const spectrum_math::ViewWindow& win) {
    ViewBins out;
    out.lo = spectrum_math::freq_to_bin(center_hz, span_hz, bin_count, win.low_hz);
    out.hi = spectrum_math::freq_to_bin(center_hz, span_hz, bin_count, win.high_hz);
    out.snap = 1;
    /* Guarded here rather than only at the call sites, as every helper in
     * spectrum_math.h is: a zero bin count or span makes the division infinite,
     * and narrowing an infinity to int is undefined behaviour rather than a large
     * number the clamp below would catch. */
    if (bin_count > 0 && span_hz > 0.0) {
        const double bin_width_hz = span_hz / static_cast<double>(bin_count);
        const double snap = spectrum_math::snap_window_hz(win.span_hz) / bin_width_hz;
        if (snap > 1.0) {
            out.snap = (snap > static_cast<double>(bin_count)) ? bin_count : static_cast<int>(snap);
        }
    }
    return out;
}

} // namespace

SpectrumModel::SpectrumModel(QObject* parent) : QObject(parent), m_bins(kMaxBins, 0.0F) {
    m_timer.setInterval(kPollIntervalMs);
    /* Coarse: a waterfall row landing a few ms late is invisible, and the
     * precise timer would keep a phone's CPU out of its idle states. */
    m_timer.setTimerType(Qt::CoarseTimer);
    connect(&m_timer, &QTimer::timeout, this, &SpectrumModel::tick);

    /* A backgrounded app draws nothing, so producing frames for it is pure
     * battery cost. Qt reports this for the whole application, which is right:
     * the Android service may still be decoding, but nobody is looking.
     * Inactive is deliberately still "showing" — it only means unfocused (a
     * dialog or the notification shade on top), and the view is visible under
     * it. Only Suspended and Hidden mean nothing is on screen. */
    if (QGuiApplication* app = qobject_cast<QGuiApplication*>(QCoreApplication::instance())) {
        m_app_foreground = state_is_showing(app->applicationState());
        connect(app, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
            m_app_foreground = state_is_showing(state);
            applyRunState();
        });
    }
}

SpectrumModel::~SpectrumModel() {
    if (m_running) {
        dsd_app_frontend_wideband_spectrum_set_enabled(0);
    }
}

void
SpectrumModel::setActive(bool on) {
    if (m_active == on) {
        return;
    }
    m_active = on;
    applyRunState();
    Q_EMIT activeChanged();
}

void
SpectrumModel::applyRunState() {
    const bool run = m_active && m_app_foreground;
    if (run == m_running) {
        return;
    }
    m_running = run;
    dsd_app_frontend_wideband_spectrum_set_enabled(run ? 1 : 0);
    if (run) {
        m_timer.start();
        return;
    }
    m_timer.stop();
    /* Reopening the view must not flash the last session's panorama. A gap
     * *while* running is different — see tick(), which holds the last frame. */
    invalidateFrame();
}

void
SpectrumModel::invalidateFrame() {
    if (!m_has_data) {
        return;
    }
    m_has_data = false;
    m_bin_count = 0;
    m_frame_serial = 0;
    m_frame_index++;
    /* Deliberately no retuned(): pausing production is not the front end moving,
     * and a consumer holding history (the waterfall) would throw away minutes of
     * it every time the app backgrounds or a sheet opens over the view — which is
     * the very thing keeping the view's Loader alive is meant to prevent. The
     * center and span are kept for the same reason: tick() compares the next
     * frame against them, so a retune that happened while paused is still
     * recognised as one and still clears the history, on the frame that proves it.
     *
     * The pan offset is kept with them, and for the same reason: WaterfallItem is
     * deliberately not gated on hasData() so its retained history stays on screen
     * across a pause, but it derives its source rect from viewLowHz/viewHighHz --
     * so zeroing the offset here would slide those minutes of history sideways to
     * the middle of the span the moment a sheet opened, and leave it there. Only
     * the overshoot goes: that is an unfinished pan gesture, and there is no
     * gesture left to finish. */
    m_overshoot_hz = 0.0;
    Q_EMIT frameChanged();
    Q_EMIT viewChanged();
}

void
SpectrumModel::tick() {
    uint32_t center_hz = 0;
    uint32_t span_hz = 0;
    uint32_t serial = 0;
    const int n = dsd_app_frontend_wideband_spectrum_get(m_bins.data(), kMaxBins, &center_hz, &span_hz, &serial);
    if (n <= 0) {
        /* No frame yet, or one was just invalidated by a retune in flight.
         * Holding the last picture beats flickering to empty and back. */
        return;
    }
    /* This timer and the producer's publish period free-run against each other,
     * so a poll landing on the frame already drawn is routine rather than
     * exceptional. Announcing it as new would repaint identical pixels and, on
     * the waterfall, add a duplicate row — history that says a signal lasted
     * longer than it did. */
    if (m_has_data && serial == m_frame_serial) {
        return;
    }
    m_frame_serial = serial;

    /* Exact comparison is right: both sides came from the same uint32 Hz
     * values, so "different" means the front end actually moved. */
    const double center = static_cast<double>(center_hz);
    const double span = static_cast<double>(span_hz);
    /* Against the last geometry seen, not against "is there a frame right now":
     * production stops and starts with the view, and the front end can move while
     * it is stopped, so the comparison has to survive an invalidation. */
    const bool moved = m_have_geometry && (center != m_center_hz || span != m_span_hz);
    const bool geometry_changed = (center != m_center_hz) || (span != m_span_hz) || (n != m_bin_count);

    m_center_hz = center;
    m_span_hz = span;
    m_bin_count = n;
    m_has_data = true;
    m_have_geometry = true;
    m_frame_index++;

    bool view_moved = false;
    if (moved) {
        /* Recenter but keep the magnification: the user zoomed in for a reason,
         * and the thing they tapped is now the middle of the span. */
        view_moved = applyOffset(0.0);
        Q_EMIT retuned();
    } else if (geometry_changed) {
        /* A span change can put a previously legal offset out of bounds. */
        view_moved = applyOffset(m_offset_hz);
    }

    Q_EMIT frameChanged();
    if (view_moved || geometry_changed) {
        Q_EMIT viewChanged();
    }
}

bool
SpectrumModel::applyOffset(double hz) {
    if (!std::isfinite(hz)) {
        hz = 0.0;
    }
    double overshoot = 0.0;
    (void)spectrum_math::view_window(m_center_hz, m_span_hz, m_zoom, hz, &overshoot);
    const double granted = hz - overshoot;
    if (granted == m_offset_hz && overshoot == m_overshoot_hz) {
        return false;
    }
    m_offset_hz = granted;
    m_overshoot_hz = overshoot;
    return true;
}

void
SpectrumModel::setViewOffsetHz(double hz) {
    if (applyOffset(hz)) {
        Q_EMIT viewChanged();
    }
}

void
SpectrumModel::setZoom(double zoom_level) {
    zoomToAnchored(zoom_level, 0.5);
}

void
SpectrumModel::zoomToAnchored(double zoom_level, double x_fraction) {
    if (!std::isfinite(zoom_level)) {
        return;
    }
    const double next = spectrum_math::clamp_zoom(zoom_level);
    if (next == m_zoom) {
        return;
    }
    const double anchored =
        spectrum_math::zoom_anchor_offset_hz(m_center_hz, m_span_hz, m_zoom, m_offset_hz, next, x_fraction);
    m_zoom = next;
    (void)applyOffset(anchored);
    Q_EMIT viewChanged();
}

void
SpectrumModel::clearOvershoot() {
    if (m_overshoot_hz == 0.0) {
        return;
    }
    m_overshoot_hz = 0.0;
    Q_EMIT viewChanged();
}

void
SpectrumModel::resetView() {
    const bool zoom_moved = (m_zoom != spectrum_math::kMinZoom);
    m_zoom = spectrum_math::kMinZoom;
    const bool offset_moved = applyOffset(0.0);
    if (zoom_moved || offset_moved) {
        Q_EMIT viewChanged();
    }
}

double
SpectrumModel::tapFrequencyHz(double x_fraction) const {
    if (!m_has_data || m_bin_count <= 0 || !(m_span_hz > 0.0)) {
        return 0.0;
    }
    if (!std::isfinite(x_fraction)) {
        return 0.0;
    }
    if (x_fraction < 0.0) {
        x_fraction = 0.0;
    }
    if (x_fraction > 1.0) {
        x_fraction = 1.0;
    }

    const spectrum_math::ViewWindow win = window();
    const double tapped_hz = win.low_hz + (x_fraction * win.span_hz);
    const int tapped_bin = spectrum_math::freq_to_bin(m_center_hz, m_span_hz, m_bin_count, tapped_hz);

    /* The snap may only answer with something the user can see. Its width comes
     * from the view span, so near an edge it otherwise reaches off screen and a
     * tap lands on a carrier that was never displayed. */
    const ViewBins view = view_bins(m_center_hz, m_span_hz, m_bin_count, win);
    const int peak =
        spectrum_math::peak_search_bin(m_bins.constData(), m_bin_count, tapped_bin, view.snap, view.lo, view.hi);
    return spectrum_math::bin_to_freq_hz(m_center_hz, m_span_hz, m_bin_count, peak);
}

double
SpectrumModel::nextPeakHz(int direction) const {
    if (!m_has_data || m_bin_count <= 0 || !(m_span_hz > 0.0)) {
        return 0.0;
    }
    const spectrum_math::ViewWindow win = window();
    /* Same rule as the tap: only answer with something on screen. Hopping to a
     * carrier outside the window would move the radio to a signal the user was
     * never shown and could not have been asking for. The gap is the same snap
     * window: inside it a tap would have reached the same signal, so anything
     * nearer than that is not a different channel.
     *
     * "Above" and "below" are measured from the tuned carrier while it is on
     * screen, and from the middle of the view once the pan has taken it off —
     * see directional_seed_bin(). Both bounds and seed have to come from the same
     * window or one of the two directions searches nothing. */
    const ViewBins view = view_bins(m_center_hz, m_span_hz, m_bin_count, win);
    const int center_bin = spectrum_math::freq_to_bin(m_center_hz, m_span_hz, m_bin_count, m_center_hz);
    const int seed = spectrum_math::directional_seed_bin(center_bin, view.lo, view.hi);
    const int peak =
        spectrum_math::directional_peak_bin(m_bins.constData(), m_bin_count, seed, (direction >= 0) ? 1 : -1, view.snap,
                                            kNextPeakExcessDb, view.lo, view.hi);
    if (peak < 0) {
        return 0.0;
    }
    return spectrum_math::bin_to_freq_hz(m_center_hz, m_span_hz, m_bin_count, peak);
}

QVariantList
SpectrumModel::axisTicks(int max_ticks) const {
    QVariantList ticks;
    if (!m_has_data || max_ticks < 1) {
        return ticks;
    }
    const spectrum_math::ViewWindow win = window();
    if (!(win.span_hz > 0.0)) {
        return ticks;
    }
    const double step = spectrum_math::nice_tick_step_hz(win.span_hz, max_ticks);
    if (!(step > 0.0)) {
        return ticks;
    }

    ticks.reserve(max_ticks);
    /* Each label is computed from the first one and an integer count rather
     * than accumulated: at 850 MHz, repeated addition drifts the later labels
     * off their own grid lines. */
    const double first = std::ceil(win.low_hz / step) * step;
    for (int k = 0; k < max_ticks; k++) {
        const double freq = first + (static_cast<double>(k) * step);
        if (freq > win.high_hz) {
            break;
        }
        QVariantMap entry;
        entry[QStringLiteral("freqHz")] = freq;
        entry[QStringLiteral("xFraction")] = (freq - win.low_hz) / win.span_hz;
        /* MHz to 4 decimals: 100 Hz resolution reads every channel plan we
         * decode without turning the axis into a wall of digits. */
        entry[QStringLiteral("label")] = QString::number(freq / 1.0e6, 'f', 4);
        ticks.append(entry);
    }
    return ticks;
}

} // namespace dsd_qt
