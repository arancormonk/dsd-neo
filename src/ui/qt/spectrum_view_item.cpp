// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "spectrum_view_item.h"

#include <QPainter>
#include <QRectF>
#include <Qt>
#include <cmath>
#include <dsd-neo/core/wideband_spectrum.h>
#include <qpen.h>

#include "spectrum_model.h"

namespace dsd_qt {

namespace {

/* Waterfall history buffer. The width is one published frame, so a row keeps
 * every bin the producer measured and zoom can reach back into history without
 * resampling; the height is a couple of minutes at the publish rate. */
constexpr int kWaterfallWidth = DSD_WIDEBAND_SPECTRUM_BINS;
constexpr int kWaterfallRows = 240;

/**
 * @brief Peak-pick @p count bins down into a narrower @p out_width, for count > out_width.
 *
 * Peak-picking, not averaging: a narrow carrier that lands between two columns
 * must stay visible rather than being averaged into the noise.
 */
void
resample_peak_decimate(const float* bins, int first, int count, float* out, int out_width) {
    for (int x = 0; x < out_width; x++) {
        int i0 = static_cast<int>((static_cast<long long>(x) * count) / out_width);
        int i1 = static_cast<int>((static_cast<long long>(x + 1) * count) / out_width);
        if (i1 <= i0) {
            i1 = i0 + 1;
        }
        if (i1 > count) {
            i1 = count;
        }
        float peak = bins[first + i0];
        for (int i = i0 + 1; i < i1; i++) {
            if (bins[first + i] > peak) {
                peak = bins[first + i];
            }
        }
        out[x] = peak;
    }
}

/** @brief Spread @p count bins across a wider @p out_width by repetition, for count < out_width. */
void
resample_repeat(const float* bins, int first, int count, float* out, int out_width) {
    for (int x = 0; x < out_width; x++) {
        int src = static_cast<int>((static_cast<long long>(x) * count) / out_width);
        if (src < 0) {
            src = 0;
        }
        if (src > count - 1) {
            src = count - 1;
        }
        out[x] = bins[first + src];
    }
}

/**
 * @brief Reduce @p count bins starting at @p first into @p out_width columns.
 *
 * Mirrors spectrum_resample_columns() in the terminal visualizer.
 */
void
resample_columns(const float* bins, int first, int count, float* out, int out_width) {
    if (!bins || !out || out_width <= 0) {
        return;
    }
    if (count <= 0) {
        for (int x = 0; x < out_width; x++) {
            out[x] = 0.0F;
        }
        return;
    }
    if (count == out_width) {
        /* The waterfall's own case: every published frame is exactly
         * DSD_WIDEBAND_SPECTRUM_BINS wide and so is the history image, so the general
         * paths would spend two 64-bit divisions per column deriving the identity map. */
        for (int x = 0; x < out_width; x++) {
            out[x] = bins[first + x];
        }
        return;
    }
    if (count > out_width) {
        resample_peak_decimate(bins, first, count, out, out_width);
        return;
    }
    resample_repeat(bins, first, count, out, out_width);
}

int
lerp_channel(int from, int to, double t) {
    const double v = static_cast<double>(from) + (t * static_cast<double>(to - from));
    if (!(v > 0.0)) {
        return 0;
    }
    return (v > 255.0) ? 255 : static_cast<int>(std::lround(v));
}

} // namespace

// ---------------------------------------------------------------- trace ----

SpectrumTraceItem::SpectrumTraceItem(QQuickItem* parent) : QQuickPaintedItem(parent) {}

SpectrumTraceItem::~SpectrumTraceItem() = default;

void
SpectrumTraceItem::setModel(SpectrumModel* next) {
    if (m_model == next) {
        return;
    }
    if (m_model) {
        disconnect(m_model, nullptr, this, nullptr);
    }
    m_model = next;
    if (m_model) {
        connect(m_model, &SpectrumModel::frameChanged, this, &SpectrumTraceItem::onFrame);
        connect(m_model, &SpectrumModel::viewChanged, this, &SpectrumTraceItem::onFrame);
        connect(m_model, &SpectrumModel::retuned, this, &SpectrumTraceItem::onRetuned);
    }
    m_range.reset();
    m_have_frame = false;
    m_last_frame_index = 0;
    Q_EMIT modelChanged();
    update();
}

void
SpectrumTraceItem::setLineColor(const QColor& color) {
    if (m_line_color == color) {
        return;
    }
    m_line_color = color;
    Q_EMIT colorsChanged();
    update();
}

void
SpectrumTraceItem::setAreaColor(const QColor& color) {
    if (m_area_color == color) {
        return;
    }
    m_area_color = color;
    Q_EMIT colorsChanged();
    update();
}

void
SpectrumTraceItem::setGridColor(const QColor& color) {
    if (m_grid_color == color) {
        return;
    }
    m_grid_color = color;
    Q_EMIT colorsChanged();
    update();
}

void
SpectrumTraceItem::onFrame() {
    /* Folded here rather than in paint(): the range is an EMA that relaxes a few
     * percent per call, and this slot also runs on viewChanged, so folding at
     * paint time would advance it at touch rate during a pan and settle the trace
     * on a different scale than the waterfall shows for the same bins. */
    if (m_model != nullptr && m_model->hasData()) {
        const quint64 index = m_model->frameIndex();
        if (!m_have_frame || index != m_last_frame_index) {
            m_last_frame_index = index;
            const int n = m_model->binCount();
            const QVector<float>& bins = m_model->bins();
            if (n > 0 && bins.size() >= n) {
                m_have_frame = true;
                m_range.update(bins.constData(), n);
            }
        }
    }
    update();
}

void
SpectrumTraceItem::onRetuned() {
    /* The new frequency's noise floor has nothing to do with the old one's.
     * The frame stamp goes with it: a retune bumps the model's index before the
     * frame captured at the new center is announced, so keeping the stamp would
     * skip the fold for exactly the frame the new range has to be built from. */
    m_range.reset();
    m_have_frame = false;
    m_last_frame_index = 0;
    update();
}

void
SpectrumTraceItem::paint(QPainter* painter) {
    if (!painter) {
        return;
    }
    const int w = static_cast<int>(width());
    const int h = static_cast<int>(height());
    if (w <= 1 || h <= 1) {
        return;
    }

    painter->setRenderHint(QPainter::Antialiasing, true);

    /* The grid is drawn even with no data, so the panel reads as a chart that is
     * waiting rather than as a hole in the screen. */
    if (m_grid_color.alpha() > 0) {
        QPen grid(m_grid_color);
        grid.setWidthF(1.0);
        painter->setPen(grid);
        for (int i = 1; i < 4; i++) {
            const double y = (static_cast<double>(h) * i) / 4.0;
            painter->drawLine(QPointF(0.0, y), QPointF(static_cast<double>(w), y));
        }
    }

    if (!m_model || !m_model->hasData()) {
        return;
    }
    const int n = m_model->binCount();
    const QVector<float>& bins = m_model->bins();
    if (n <= 0 || bins.size() < n) {
        return;
    }

    /* A model handed over after its first frame has landed announces nothing until
     * the next one, so seed here rather than draw a flat trace for a frame. Stamped
     * with the frame it seeded from, exactly as onFrame() does: left unstamped, the
     * next viewChanged — the first touch-move of a pan — folds this same frame in a
     * second time, and the trace's range then runs one relaxation step ahead of the
     * waterfall's copy of it for the rest of the session. */
    if (!m_range.seeded) {
        m_range.update(bins.constData(), n);
        m_last_frame_index = m_model->frameIndex();
        m_have_frame = true;
    }

    const double center = m_model->centerFreqHz();
    const double span = m_model->spanHz();
    const int lo = spectrum_math::freq_to_bin(center, span, n, m_model->viewLowHz());
    const int hi = spectrum_math::freq_to_bin(center, span, n, m_model->viewHighHz());
    const int count = (hi >= lo) ? (hi - lo + 1) : 1;

    /* Index 0 and w+1 are the polygon's baseline corners; the trace occupies
     * 1..w, which is where the polyline is drawn from. */
    m_points.resize(w + 2);
    m_columns.resize(w);
    resample_columns(bins.constData(), lo, count, m_columns.data(), w);
    for (int x = 0; x < w; x++) {
        const double t = m_range.normalize(static_cast<double>(m_columns[x]));
        m_points[x + 1] = QPointF(static_cast<double>(x), static_cast<double>(h) * (1.0 - t));
    }
    m_points[0] = QPointF(0.0, static_cast<double>(h));
    m_points[w + 1] = QPointF(static_cast<double>(w - 1), static_cast<double>(h));

    if (m_area_color.alpha() > 0) {
        painter->setPen(Qt::NoPen);
        painter->setBrush(m_area_color);
        painter->drawPolygon(m_points.constData(), w + 2);
    }

    QPen line(m_line_color);
    line.setWidthF(1.5);
    painter->setBrush(Qt::NoBrush);
    painter->setPen(line);
    painter->drawPolyline(m_points.constData() + 1, w);
}

// ------------------------------------------------------------ waterfall ----

WaterfallItem::WaterfallItem(QQuickItem* parent)
    : QQuickPaintedItem(parent), m_image(kWaterfallWidth, kWaterfallRows, QImage::Format_RGB32) {
    rebuildRamp();
    clearHistory();
}

WaterfallItem::~WaterfallItem() = default;

void
WaterfallItem::setModel(SpectrumModel* next) {
    if (m_model == next) {
        return;
    }
    if (m_model) {
        disconnect(m_model, nullptr, this, nullptr);
    }
    m_model = next;
    if (m_model) {
        connect(m_model, &SpectrumModel::frameChanged, this, &WaterfallItem::onFrame);
        /* Pan and zoom are a source rectangle, so they need a repaint but not a
         * new row. */
        connect(m_model, &SpectrumModel::viewChanged, this, [this]() { update(); });
        connect(m_model, &SpectrumModel::retuned, this, &WaterfallItem::clearHistory);
    }
    m_range.reset();
    clearHistory();
    Q_EMIT modelChanged();
}

void
WaterfallItem::setColdColor(const QColor& color) {
    if (m_cold_color == color) {
        return;
    }
    m_cold_color = color;
    Q_EMIT colorsChanged();
    rebuildRamp();
    /* Rows already written hold their old palette, and a half-recolored
     * waterfall reads as corruption. */
    clearHistory();
}

void
WaterfallItem::setMidColor(const QColor& color) {
    if (m_mid_color == color) {
        return;
    }
    m_mid_color = color;
    Q_EMIT colorsChanged();
    rebuildRamp();
    clearHistory();
}

void
WaterfallItem::setHotColor(const QColor& color) {
    if (m_hot_color == color) {
        return;
    }
    m_hot_color = color;
    Q_EMIT colorsChanged();
    rebuildRamp();
    clearHistory();
}

void
WaterfallItem::clearHistory() {
    m_image.fill(m_cold_color);
    /* Rows are written at the cursor and the cursor walks backwards, so the
     * slice above it is always newest-first. */
    m_cursor = kWaterfallRows - 1;
    m_range.reset();
    /* Deliberately not stamped with the model's current frame index. A retune
     * bumps that index and only then announces the frame captured at the new
     * center, so claiming it here would drop the first row after every tune. */
    m_have_row = false;
    m_last_frame_index = 0;
    m_rows_written = 0;
    update();
}

void
WaterfallItem::rebuildRamp() {
    for (int i = 0; i < kRampSteps; i++) {
        const double t = static_cast<double>(i) / static_cast<double>(kRampSteps - 1);
        if (t <= 0.5) {
            const double u = t * 2.0;
            m_ramp[i] = qRgb(lerp_channel(m_cold_color.red(), m_mid_color.red(), u),
                             lerp_channel(m_cold_color.green(), m_mid_color.green(), u),
                             lerp_channel(m_cold_color.blue(), m_mid_color.blue(), u));
            continue;
        }
        const double u = (t - 0.5) * 2.0;
        m_ramp[i] = qRgb(lerp_channel(m_mid_color.red(), m_hot_color.red(), u),
                         lerp_channel(m_mid_color.green(), m_hot_color.green(), u),
                         lerp_channel(m_mid_color.blue(), m_hot_color.blue(), u));
    }
}

QRgb
WaterfallItem::rampColor(double t) const {
    /* Clamped rather than trusted: AutoRange::normalize() is the only caller
     * today and already returns [0, 1], but an out-of-range index here reads
     * off the end of the table instead of producing a wrong color. */
    int i = static_cast<int>(std::lround(t * static_cast<double>(kRampSteps - 1)));
    if (i < 0) {
        i = 0;
    } else if (i >= kRampSteps) {
        i = kRampSteps - 1;
    }
    return m_ramp[i];
}

void
WaterfallItem::onFrame() {
    if (!m_model || !m_model->hasData()) {
        return;
    }
    const quint64 index = m_model->frameIndex();
    if (m_have_row && index == m_last_frame_index) {
        return; /* a repaint request, not a new row */
    }
    const int n = m_model->binCount();
    const QVector<float>& bins = m_model->bins();
    if (n <= 0 || bins.size() < n) {
        return;
    }
    /* Stamped only once the frame is known to be usable, as SpectrumTraceItem does:
     * claiming it before the check would make a frame this declined to draw look
     * like one already drawn, and the row would be lost from history for good. */
    m_last_frame_index = index;
    m_have_row = true;
    m_range.update(bins.constData(), n);

    /* Always full span: that is what lets zoom and pan reach back through
     * history without invalidating it.
     *
     * Read straight out of the frame when it is already that wide, which is every
     * frame the producer publishes: staging it costs a 4 KB copy and a second pass
     * over the same values, fifteen times a second, to hand the row loop a pointer
     * it could have had. The resample stays for the odd frame that is not. */
    const float* columns = bins.constData();
    if (n != kWaterfallWidth) {
        m_columns.resize(kWaterfallWidth);
        resample_columns(bins.constData(), 0, n, m_columns.data(), kWaterfallWidth);
        columns = m_columns.constData();
    }

    QRgb* row = reinterpret_cast<QRgb*>(m_image.scanLine(m_cursor));
    for (int x = 0; x < kWaterfallWidth; x++) {
        row[x] = rampColor(m_range.normalize(static_cast<double>(columns[x])));
    }
    m_cursor = (m_cursor - 1 + kWaterfallRows) % kWaterfallRows;
    m_rows_written++;
    update();
}

void
WaterfallItem::paint(QPainter* painter) {
    if (!painter) {
        return;
    }
    const double w = width();
    const double h = height();
    if (!(w > 1.0) || !(h > 1.0)) {
        return;
    }

    /* Which slice of the full-span rows the viewport is looking at.
     *
     * Deliberately not gated on hasData(): invalidateFrame() drops the frame but
     * keeps the center, the span and the viewport, precisely so the history stays
     * meaningful across a pause. Requiring a live frame here would snap the picture
     * from the zoomed slice back to 1x every time the app backgrounds or production
     * stops, and snap it back a frame later. */
    double src_x = 0.0;
    double src_w = kWaterfallWidth;
    if (m_model && m_model->spanHz() > 0.0) {
        const double span = m_model->spanHz();
        const double low = m_model->centerFreqHz() - (span / 2.0);
        const double frac_lo = (m_model->viewLowHz() - low) / span;
        const double frac_hi = (m_model->viewHighHz() - low) / span;
        src_x = frac_lo * kWaterfallWidth;
        src_w = (frac_hi - frac_lo) * kWaterfallWidth;
        if (!(src_w > 0.0)) {
            src_w = kWaterfallWidth;
            src_x = 0.0;
        }
    }

    /* Two blits because the ring wraps: rows above the cursor are the recent
     * history, rows below it are the older tail. */
    const int newest_count = kWaterfallRows - 1 - m_cursor;
    const int older_count = kWaterfallRows - newest_count;
    const double newest_h = (h * newest_count) / kWaterfallRows;

    painter->setRenderHint(QPainter::SmoothPixmapTransform, true);
    if (newest_count > 0) {
        painter->drawImage(QRectF(0.0, 0.0, w, newest_h), m_image,
                           QRectF(src_x, static_cast<double>(m_cursor + 1), src_w, static_cast<double>(newest_count)));
    }
    if (older_count > 0) {
        painter->drawImage(QRectF(0.0, newest_h, w, h - newest_h), m_image,
                           QRectF(src_x, 0.0, src_w, static_cast<double>(older_count)));
    }
}

} // namespace dsd_qt
