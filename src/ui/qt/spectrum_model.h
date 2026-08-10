// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Live wideband spectrum frames and viewport state exposed to QML.
 *
 * Polls the app-control wideband getter on its own timer rather than riding
 * the 250 ms UI tick: a waterfall needs frames several times faster than the
 * status card does, and the getter is an atomic copy that is safe to call from
 * any thread. It must never touch dsd_app_get_latest_snapshot() /
 * ..._opts_snapshot(), which are strictly single-consumer and belong to
 * UiController — everything a spectrum frame needs (bins, center, span) is
 * published inside the frame itself.
 *
 * Production is enabled only while the view is on screen and the app is in the
 * foreground, so the FFT costs nothing the rest of the time.
 */

#ifndef DSD_NEO_SRC_UI_QT_SPECTRUM_MODEL_H_
#define DSD_NEO_SRC_UI_QT_SPECTRUM_MODEL_H_

#include <QList>
#include <QObject>
#include <QTimer>
#include <QVariantList>
#include <QtGlobal>

#include "spectrum_math.h"

namespace dsd_qt {

class SpectrumModel : public QObject {
    Q_OBJECT

    /* Two groups, for the same reason MetricsModel splits its signals: a new
     * frame arrives ~15x a second and must repaint the trace, but it must not
     * re-lay-out the axis labels or re-evaluate the gesture bindings, which
     * only move when the viewport does. */
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged)
    Q_PROPERTY(bool hasData READ hasData NOTIFY frameChanged)
    Q_PROPERTY(double centerFreqHz READ centerFreqHz NOTIFY frameChanged)
    Q_PROPERTY(double spanHz READ spanHz NOTIFY frameChanged)
    Q_PROPERTY(int binCount READ binCount NOTIFY frameChanged)
    Q_PROPERTY(double zoom READ zoom WRITE setZoom NOTIFY viewChanged)
    Q_PROPERTY(double viewOffsetHz READ viewOffsetHz WRITE setViewOffsetHz NOTIFY viewChanged)
    Q_PROPERTY(double edgeOvershootHz READ edgeOvershootHz NOTIFY viewChanged)
    Q_PROPERTY(double viewLowHz READ viewLowHz NOTIFY viewChanged)
    Q_PROPERTY(double viewHighHz READ viewHighHz NOTIFY viewChanged)
    Q_PROPERTY(double viewSpanHz READ viewSpanHz NOTIFY viewChanged)

  public:
    explicit SpectrumModel(QObject* parent = nullptr);
    ~SpectrumModel() override;

    /** @brief Whether frames are being produced and polled. */
    bool
    active() const {
        return m_active;
    }

    void setActive(bool on);

    /** @brief Whether a frame has been received and not since invalidated. */
    bool
    hasData() const {
        return m_has_data;
    }

    /** @brief Center of the published frame, in Hz. This is the axis authority. */
    double
    centerFreqHz() const {
        return m_center_hz;
    }

    /** @brief Width of the published frame, in Hz (the SDR capture bandwidth). */
    double
    spanHz() const {
        return m_span_hz;
    }

    /** @brief Number of bins in the published frame. */
    int
    binCount() const {
        return m_bin_count;
    }

    /** @brief Current magnification, 1x (whole span) to 8x. */
    double
    zoom() const {
        return m_zoom;
    }

    void setZoom(double zoom_level);

    /** @brief Viewport center relative to the frame center, in Hz. Always clamped. */
    double
    viewOffsetHz() const {
        return m_offset_hz;
    }

    void setViewOffsetHz(double hz);

    /**
     * @brief How far past the capture edge the last pan asked to go, in Hz.
     *
     * Signed, and 0 whenever the request fitted. A gesture that ends with this
     * non-zero is the user asking for a frequency the hardware is not covering;
     * the view turns exactly one of those into a retune, on release.
     */
    double
    edgeOvershootHz() const {
        return m_overshoot_hz;
    }

    /** @brief Low edge of the visible window, in Hz. */
    double
    viewLowHz() const {
        return window().low_hz;
    }

    /** @brief High edge of the visible window, in Hz. */
    double
    viewHighHz() const {
        return window().high_hz;
    }

    /** @brief Width of the visible window, in Hz. */
    double
    viewSpanHz() const {
        return window().span_hz;
    }

    /**
     * @brief Frequency a tap at @p x_fraction across the view is asking for.
     *
     * Snaps to the strongest bin near the touch, because a fingertip is far
     * wider than a channel. Returns 0 when there is no frame to read, which the
     * caller must check before submitting a tune.
     */
    Q_INVOKABLE double tapFrequencyHz(double x_fraction) const;

    /**
     * @brief Frequency of the next signal above (@p direction >= 0) or below center.
     *
     * Only signals that are visible and stand clear of the noise count, and the
     * one already tuned is skipped. Returns 0 when there is no such signal, which
     * the caller must check — this control has to do nothing on an empty band
     * rather than retune to where the radio already is.
     *
     * "Center" is the tuned carrier while it is on screen, and the middle of the
     * view once the pan has taken it off: only what is visible can be answered
     * with, so an off-screen reference would leave one direction with nothing to
     * search. A retune resets the pan, so the two coincide again immediately after.
     */
    Q_INVOKABLE double nextPeakHz(int direction) const;

    /** @brief Set the zoom to @p zoom_level, holding @p x_fraction's frequency still. */
    Q_INVOKABLE void zoomToAnchored(double zoom_level, double x_fraction);

    /** @brief Axis labels as {freqHz, xFraction, label} maps, at most @p max_ticks. */
    Q_INVOKABLE QVariantList axisTicks(int max_ticks) const;

    /** @brief Forget a pending edge overshoot, after acting on it. */
    Q_INVOKABLE void clearOvershoot();

    /** @brief Return the viewport to the whole span at 1x. */
    Q_INVOKABLE void resetView();

#ifdef DSD_NEO_TEST_HOOKS
    /**
     * @brief Poll the producer once, exactly as the timer does.
     *
     * Lets a test drive frames in without waiting out the publish period, and
     * without the arrival order depending on how a run was scheduled.
     */
    void
    testPoll() {
        tick();
    }
#endif

    /** @brief The live bin buffer, for the painted items. Only binCount() entries are valid. */
    const QVector<float>&
    bins() const {
        return m_bins;
    }

    /**
     * @brief Increments once per frame actually taken from the producer.
     *
     * Not once per poll: the timer here and the producer's publish period
     * free-run, so a painter needs to know when the picture changed rather than
     * when it was looked at.
     */
    quint64
    frameIndex() const {
        return m_frame_index;
    }

  Q_SIGNALS:
    /** @brief A new frame landed, or the frame was invalidated. */
    void frameChanged();
    /** @brief The visible window moved (zoom, pan, or a frame geometry change). */
    void viewChanged();
    /** @brief Production started or stopped. */
    void activeChanged();
    /**
     * @brief The front end moved to a different frequency.
     *
     * Waterfall history is only meaningful at one center, so a consumer holding
     * history must drop it here.
     */
    void retuned();

  private:
    void tick();
    void applyRunState();
    /** @return true when the stored offset or overshoot actually moved. */
    bool applyOffset(double hz);
    void invalidateFrame();

    spectrum_math::ViewWindow
    window() const {
        return spectrum_math::view_window(m_center_hz, m_span_hz, m_zoom, m_offset_hz, nullptr);
    }

    QVector<float> m_bins;
    QTimer m_timer;
    double m_center_hz = 0.0;
    double m_span_hz = 0.0;
    int m_bin_count = 0;
    bool m_has_data = false;
    /* Whether m_center_hz/m_span_hz describe a frame that was actually received.
     * Outlives m_has_data on purpose: invalidateFrame() drops the frame but keeps
     * the geometry, so the next frame can still be recognised as a retune. */
    bool m_have_geometry = false;
    double m_zoom = spectrum_math::kMinZoom;
    double m_offset_hz = 0.0;
    double m_overshoot_hz = 0.0;
    quint64 m_frame_index = 0;
    /* The producer's identity for the frame in m_bins; 0 while there is none. */
    quint32 m_frame_serial = 0;
    bool m_active = false;
    bool m_app_foreground = true;
    bool m_running = false;
};

} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_SPECTRUM_MODEL_H_ */
