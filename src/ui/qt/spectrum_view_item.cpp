// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "spectrum_view_item.h"

#include <QPainter>
#include <QPointF>
#include <QQuickWindow>
#include <QRectF>
#include <QSGFlatColorMaterial>
#include <QSGGeometry>
#include <QSGGeometryNode>
#include <QSGImageNode>
#include <QSGRendererInterface>
#include <QSGTexture>
#include <QSGVertexColorMaterial>
#include <QSizeF>
#include <Qt>
#include <QtMath>
#include <cmath>
#include <cstddef>
#include <dsd-neo/core/wideband_spectrum.h>
#include <qpen.h>

#include "spectrum_model.h"
#include "spectrum_trace_geometry.h"

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

namespace {

/* QPainter's pens, kept: the trace was a 1.5 px line, the grid 1 px. */
constexpr float kTraceHalfWidth = 0.75F;
constexpr float kGridHalfWidth = 0.5F;
constexpr int kGridLines = 3;

/**
 * @brief A layer of stroke triangles colored per vertex.
 *
 * 32-bit index type with no indices: Qt merges only 16-bit-indexed geometry
 * into shared batches, whose index runs are cut every 65534 vertices, and a
 * wide desktop trace passes that. Kept out of merging, the list is drawn on its
 * own without indices.
 */
QSGGeometryNode*
make_stroke_node() {
    auto* node = new QSGGeometryNode;
    auto* geometry =
        new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(), 0, 0, QSGGeometry::UnsignedIntType);
    geometry->setDrawingMode(QSGGeometry::DrawTriangles);
    node->setGeometry(geometry);
    node->setMaterial(new QSGVertexColorMaterial);
    node->setFlags(QSGNode::OwnsGeometry | QSGNode::OwnsMaterial);
    return node;
}

/** @brief The filled area: one flat color, a strip of two vertices per column. */
QSGGeometryNode*
make_area_node() {
    auto* node = new QSGGeometryNode;
    auto* geometry = new QSGGeometry(QSGGeometry::defaultAttributes_Point2D(), 0, 0, QSGGeometry::UnsignedIntType);
    geometry->setDrawingMode(QSGGeometry::DrawTriangleStrip);
    node->setGeometry(geometry);
    node->setMaterial(new QSGFlatColorMaterial);
    node->setFlags(QSGNode::OwnsGeometry | QSGNode::OwnsMaterial);
    return node;
}

/** @brief The GPU tree's root, so the next sync finds its layers where it left them. */
class TraceGpuRoot : public QSGNode {
  public:
    TraceGpuRoot() : grid(make_stroke_node()), area(make_area_node()), line(make_stroke_node()) {
        appendChildNode(grid);
        appendChildNode(area);
        appendChildNode(line);
    }

    QSGGeometryNode* grid;
    QSGGeometryNode* area;
    QSGGeometryNode* line;
};

/**
 * @brief Where the software path's image goes: at its own pixel scale, from the item's origin.
 *
 * The image is the item's size times the pixel ratio, rounded up to whole
 * pixels, so at a fractional ratio it is a fraction of a pixel larger than the
 * item. Mapped onto exactly the item it would be squeezed by that fraction,
 * shifting the chart and filtering its antialiased edges a second time; at its
 * own scale the extra sliver lies past the item's edge, where the clip drops it.
 */
QRectF
software_image_rect(const QImage& image) {
    return QRectF(QPointF(0.0, 0.0), image.deviceIndependentSize());
}

quint8
unit_to_byte(float v) {
    if (!(v > 0.0F)) {
        return 0;
    }
    return (v >= 1.0F) ? 255 : static_cast<quint8>(std::lround(v * 255.0F));
}

/**
 * @brief Load stroke vertices into @p node in @p color, premultiplied by color alpha times coverage.
 *
 * QSGVertexColorMaterial expects premultiplied colors, so a vertex whose
 * coverage is zero is transparent black, not the line color at alpha 0.
 */
void
set_stroke_vertices(QSGGeometryNode* node, const spectrum_geometry::StrokeVertex* v, int count, const QColor& color) {
    QSGGeometry* geometry = node->geometry();
    if (geometry->vertexCount() != count) {
        geometry->allocate(count);
    }
    QSGGeometry::ColoredPoint2D* out = geometry->vertexDataAsColoredPoint2D();
    const float alpha = static_cast<float>(color.alphaF());
    const float red = static_cast<float>(color.redF());
    const float green = static_cast<float>(color.greenF());
    const float blue = static_cast<float>(color.blueF());
    for (int i = 0; i < count; i++) {
        const float a = alpha * std::fmin(1.0F, std::fmax(0.0F, v[i].coverage));
        out[i].set(v[i].x, v[i].y, unit_to_byte(red * a), unit_to_byte(green * a), unit_to_byte(blue * a),
                   unit_to_byte(a));
    }
    node->markDirty(QSGNode::DirtyGeometry);
}

void
set_area_vertices(QSGGeometryNode* node, const spectrum_geometry::AreaVertex* v, int count, const QColor& color) {
    QSGGeometry* geometry = node->geometry();
    if (geometry->vertexCount() != count) {
        geometry->allocate(count);
    }
    QSGGeometry::Point2D* out = geometry->vertexDataAsPoint2D();
    for (int i = 0; i < count; i++) {
        out[i].set(v[i].x, v[i].y);
    }
    node->markDirty(QSGNode::DirtyGeometry);
    auto* material = static_cast<QSGFlatColorMaterial*>(node->material());
    if (material->color() != color) {
        material->setColor(color); /* also turns blending on for a translucent color */
        node->markDirty(QSGNode::DirtyMaterial);
    }
}

} // namespace

SpectrumTraceItem::SpectrumTraceItem(QQuickItem* parent) : QQuickItem(parent) {
    setFlag(ItemHasContents, true);
    /* A painted item was confined to its rectangle; geometry is not. The caps,
     * bevels and fringes reach past their points, and a saturated peak would
     * draw over whatever sits above the panel. Axis-aligned, so a scissor. */
    setClip(true);
}

SpectrumTraceItem::~SpectrumTraceItem() = default;

void
SpectrumTraceItem::setModel(SpectrumModel* next) {
    if (m_model.data() == next) {
        return;
    }
    if (m_model) {
        disconnect(m_model.data(), nullptr, this, nullptr);
    }
    m_model = next;
    if (m_model) {
        connect(m_model.data(), &SpectrumModel::frameChanged, this, &SpectrumTraceItem::onFrame);
        connect(m_model.data(), &SpectrumModel::viewChanged, this, &SpectrumTraceItem::onFrame);
        connect(m_model.data(), &SpectrumModel::retuned, this, &SpectrumTraceItem::onRetuned);
        connect(m_model.data(), &QObject::destroyed, this, &SpectrumTraceItem::onModelDestroyed);
    }
    forgetFrames();
    Q_EMIT modelChanged();
    update();
}

void
SpectrumTraceItem::onModelDestroyed() {
    /* The pointer is already null; what is left is the state that came from it. */
    m_model.clear();
    forgetFrames();
    Q_EMIT modelChanged();
    update();
}

void
SpectrumTraceItem::forgetFrames() {
    m_range.reset();
    m_have_frame = false;
    m_last_frame_index = 0;
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
    /* Folded here rather than at draw time: the range is an EMA that relaxes a few
     * percent per call, and this slot also runs on viewChanged, so folding at
     * draw time would advance it at touch rate during a pan and settle the trace
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
    forgetFrames();
    update();
}

void
SpectrumTraceItem::geometryChange(const QRectF& new_geometry, const QRectF& old_geometry) {
    QQuickItem::geometryChange(new_geometry, old_geometry);
    if (new_geometry.size() != old_geometry.size()) {
        update();
    }
}

void
SpectrumTraceItem::itemChange(ItemChange change, const ItemChangeData& value) {
    QQuickItem::itemChange(change, value);
    /* The feather is one device pixel and the software image is sized in them. */
    if (change == ItemDevicePixelRatioHasChanged) {
        update();
    }
}

/**
 * @brief Map the current frame onto one point per pixel column, in m_xs/m_ys.
 *
 * Runs in the scene graph's sync, with the GUI thread blocked, as paint() did.
 * @return Whether there is a frame to draw.
 */
bool
SpectrumTraceItem::buildTrace(int w, int h) {
    if (m_model == nullptr || !m_model->hasData() || w <= 1 || h <= 1) {
        return false;
    }
    const int n = m_model->binCount();
    const QVector<float>& bins = m_model->bins();
    if (n <= 0 || bins.size() < n) {
        return false;
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

    const auto columns = static_cast<size_t>(w);
    m_columns.resize(w);
    m_xs.resize(columns);
    m_ys.resize(columns);
    resample_columns(bins.constData(), lo, count, m_columns.data(), w);
    for (int x = 0; x < w; x++) {
        const double t = m_range.normalize(static_cast<double>(m_columns[x]));
        m_xs[static_cast<size_t>(x)] = static_cast<float>(x);
        m_ys[static_cast<size_t>(x)] = static_cast<float>(static_cast<double>(h) * (1.0 - t));
    }
    return true;
}

QSGNode*
SpectrumTraceItem::updateGpuTree(QSGNode* old, int w, int h, double pixel_ratio, bool have_trace) {
    auto* root = (old != nullptr && old->type() == QSGNode::BasicNodeType) ? static_cast<TraceGpuRoot*>(old) : nullptr;
    if (root == nullptr) {
        delete old;
        root = new TraceGpuRoot;
    }
    const float feather = (pixel_ratio > 0.0) ? static_cast<float>(1.0 / pixel_ratio) : 1.0F;
    const auto fw = static_cast<float>(w);
    const auto fh = static_cast<float>(h);

    /* The grid is drawn even with no data, so the panel reads as a chart that is
     * waiting rather than as a hole in the screen. */
    int count = 0;
    if (m_grid_color.alpha() > 0) {
        const int per_line = spectrum_geometry::stroke_vertex_capacity(2);
        m_stroke.resize(static_cast<size_t>(per_line) * static_cast<size_t>(kGridLines));
        for (int i = 1; i <= kGridLines; i++) {
            const float y = (fh * static_cast<float>(i)) / static_cast<float>(kGridLines + 1);
            const float xs[2] = {0.0F, fw};
            const float ys[2] = {y, y};
            count += spectrum_geometry::build_stroke(xs, ys, 2, kGridHalfWidth, feather, y, m_stroke.data() + count);
        }
    }
    set_stroke_vertices(root->grid, m_stroke.data(), count, m_grid_color);

    count = 0;
    if (have_trace && m_area_color.alpha() > 0) {
        m_area.resize(static_cast<size_t>(w) * 2U);
        count = spectrum_geometry::build_area_strip(m_ys.data(), w, fh, m_area.data());
    }
    set_area_vertices(root->area, m_area.data(), count, m_area_color);

    count = 0;
    if (have_trace && m_line_color.alpha() > 0) {
        m_stroke.resize(static_cast<size_t>(spectrum_geometry::stroke_vertex_capacity(w)));
        count =
            spectrum_geometry::build_stroke(m_xs.data(), m_ys.data(), w, kTraceHalfWidth, feather, fh, m_stroke.data());
    }
    set_stroke_vertices(root->line, m_stroke.data(), count, m_line_color);
    return root;
}

/**
 * @brief The whole chart, drawn the way the painted item drew it, for the software scene graph.
 */
QImage
SpectrumTraceItem::paintSoftwareImage(int w, int h, double pixel_ratio, bool have_trace) const {
    const double ratio = (pixel_ratio > 0.0) ? pixel_ratio : 1.0;
    QImage image(qCeil(w * ratio), qCeil(h * ratio), QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(ratio);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    if (m_grid_color.alpha() > 0) {
        QPen grid(m_grid_color);
        grid.setWidthF(1.0);
        painter.setPen(grid);
        for (int i = 1; i <= kGridLines; i++) {
            const double y = (static_cast<double>(h) * i) / static_cast<double>(kGridLines + 1);
            painter.drawLine(QPointF(0.0, y), QPointF(static_cast<double>(w), y));
        }
    }
    if (!have_trace) {
        return image;
    }
    /* Index 0 and w+1 are the polygon's baseline corners; the trace is 1..w. */
    QVector<QPointF> points(w + 2);
    for (int x = 0; x < w; x++) {
        points[x + 1] = QPointF(static_cast<double>(m_xs[static_cast<size_t>(x)]),
                                static_cast<double>(m_ys[static_cast<size_t>(x)]));
    }
    points[0] = QPointF(0.0, static_cast<double>(h));
    points[w + 1] = QPointF(static_cast<double>(w - 1), static_cast<double>(h));
    if (m_area_color.alpha() > 0) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(m_area_color);
        painter.drawPolygon(points.constData(), w + 2);
    }
    QPen line(m_line_color);
    line.setWidthF(static_cast<double>(kTraceHalfWidth) * 2.0);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(line);
    painter.drawPolyline(points.constData() + 1, w);
    return image;
}

QSGNode*
SpectrumTraceItem::updatePaintNode(QSGNode* old, UpdatePaintNodeData* /*data*/) {
    QQuickWindow* win = window();
    const int w = static_cast<int>(width());
    const int h = static_cast<int>(height());
    if (win == nullptr || w <= 1 || h <= 1) {
        delete old;
        return nullptr;
    }
    const double ratio = win->effectiveDevicePixelRatio();
    const bool have_trace = buildTrace(w, h);
    if (win->rendererInterface()->graphicsApi() != QSGRendererInterface::Software) {
        return updateGpuTree(old, w, h, ratio, have_trace);
    }
    /* The software scene graph skips plain geometry nodes; it does draw images. */
    auto* node =
        (old != nullptr && old->type() == QSGNode::GeometryNodeType) ? static_cast<QSGImageNode*>(old) : nullptr;
    if (node == nullptr) {
        delete old;
        node = win->createImageNode();
        node->setOwnsTexture(true);
        node->setFiltering(QSGTexture::Linear);
    }
    const QImage image = paintSoftwareImage(w, h, ratio, have_trace);
    node->setTexture(win->createTextureFromImage(image));
    node->setRect(software_image_rect(image));
    return node;
}

#ifdef DSD_NEO_TEST_HOOKS
QSGNode*
SpectrumTraceItem::testUpdateGpuTree(QSGNode* old, double width, double height, double pixel_ratio) {
    const int w = static_cast<int>(width);
    const int h = static_cast<int>(height);
    return updateGpuTree(old, w, h, pixel_ratio, buildTrace(w, h));
}

void
SpectrumTraceItem::testSoftwareRectSize(double width, double height, double pixel_ratio, double* shown_width,
                                        double* shown_height) {
    const QRectF rect = software_image_rect(testSoftwareImage(width, height, pixel_ratio));
    *shown_width = (rect.x() < 0.5 && rect.y() < 0.5 && rect.x() > -0.5 && rect.y() > -0.5) ? rect.width() : -1.0;
    *shown_height = rect.height();
}

QImage
SpectrumTraceItem::testSoftwareImage(double width, double height, double pixel_ratio) {
    const int w = static_cast<int>(width);
    const int h = static_cast<int>(height);
    return paintSoftwareImage(w, h, pixel_ratio, buildTrace(w, h));
}
#endif

// ------------------------------------------------------------ waterfall ----

WaterfallItem::WaterfallItem(QQuickItem* parent)
    : QQuickPaintedItem(parent), m_image(kWaterfallWidth, kWaterfallRows, QImage::Format_RGB32) {
    rebuildRamp();
    clearHistory();
}

WaterfallItem::~WaterfallItem() = default;

void
WaterfallItem::setModel(SpectrumModel* next) {
    if (m_model.data() == next) {
        return;
    }
    if (m_model) {
        disconnect(m_model.data(), nullptr, this, nullptr);
    }
    m_model = next;
    if (m_model) {
        connect(m_model.data(), &SpectrumModel::frameChanged, this, &WaterfallItem::onFrame);
        /* Pan and zoom are a source rectangle, so they need a repaint but not a
         * new row. */
        connect(m_model.data(), &SpectrumModel::viewChanged, this, [this]() { update(); });
        connect(m_model.data(), &SpectrumModel::retuned, this, &WaterfallItem::clearHistory);
        connect(m_model.data(), &QObject::destroyed, this, &WaterfallItem::onModelDestroyed);
    }
    m_range.reset();
    clearHistory();
    Q_EMIT modelChanged();
}

void
WaterfallItem::onModelDestroyed() {
    /* As setModel(nullptr) would, without touching the model that is going away:
     * its pointer is already null, and history drawn from it means nothing now. */
    m_model.clear();
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
