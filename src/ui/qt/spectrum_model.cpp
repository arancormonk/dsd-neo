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
constexpr int kPollIntervalMs = DSD_WIDEBAND_SPECTRUM_PERIOD_MS;
constexpr int kMaxBins = DSD_WIDEBAND_SPECTRUM_BINS;

/* How far above the frame's mean level a bin has to sit before "next signal" will
 * move the radio to it. Low enough that a weak but real carrier still counts, high
 * enough that the control does not walk across noise one bin at a time. */
constexpr double kNextPeakExcessDb = 8.0;

bool
state_is_showing(Qt::ApplicationState state) {
    return state != Qt::ApplicationSuspended && state != Qt::ApplicationHidden;
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
     * recognised as one and still clears the history, on the frame that proves it. */
    (void)applyOffset(0.0);
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

    const double bin_width_hz = m_span_hz / static_cast<double>(m_bin_count);
    int half_width_bins = static_cast<int>(spectrum_math::snap_window_hz(win.span_hz) / bin_width_hz);
    if (half_width_bins < 1) {
        half_width_bins = 1;
    }
    /* The snap may only answer with something the user can see. Its width comes
     * from the view span, so near an edge it otherwise reaches off screen and a
     * tap lands on a carrier that was never displayed. */
    const int view_lo_bin = spectrum_math::freq_to_bin(m_center_hz, m_span_hz, m_bin_count, win.low_hz);
    const int view_hi_bin = spectrum_math::freq_to_bin(m_center_hz, m_span_hz, m_bin_count, win.high_hz);
    const int peak = spectrum_math::peak_search_bin(m_bins.constData(), m_bin_count, tapped_bin, half_width_bins,
                                                    view_lo_bin, view_hi_bin);
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
     * never shown and could not have been asking for. */
    const int view_lo_bin = spectrum_math::freq_to_bin(m_center_hz, m_span_hz, m_bin_count, win.low_hz);
    const int view_hi_bin = spectrum_math::freq_to_bin(m_center_hz, m_span_hz, m_bin_count, win.high_hz);
    const int center_bin = spectrum_math::freq_to_bin(m_center_hz, m_span_hz, m_bin_count, m_center_hz);

    /* The gap is the snap window: inside it a tap would have reached the same
     * signal, so anything nearer than that is not a different channel. */
    const double bin_width_hz = m_span_hz / static_cast<double>(m_bin_count);
    int gap_bins = static_cast<int>(spectrum_math::snap_window_hz(win.span_hz) / bin_width_hz);
    if (gap_bins < 1) {
        gap_bins = 1;
    }
    const int peak =
        spectrum_math::directional_peak_bin(m_bins.constData(), m_bin_count, center_bin, (direction >= 0) ? 1 : -1,
                                            gap_bins, kNextPeakExcessDb, view_lo_bin, view_hi_bin);
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
