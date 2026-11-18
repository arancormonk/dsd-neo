// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "map_view_item.h"

#include <QEasingCurve>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QMatrix4x4>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPointF>
#include <QQuickWindow>
#include <QRectF>
#include <QSGGeometry>
#include <QSGGeometryNode>
#include <QSGImageNode>
#include <QSGNode>
#include <QSGRendererInterface>
#include <QSGTexture>
#include <QSGTextureMaterial>
#include <QSGTransformNode>
#include <QSGVertexColorMaterial>
#include <QSet>
#include <QVariantAnimation>
#include <Qt>
#include <QtMath>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <dsd-neo/core/position.h>
#include <iterator>
#include <vector>

#include "map_geometry.h"

namespace dsd_qt {

namespace mm = map_math;
namespace mg = map_geometry;
namespace ms = map_scene;

namespace {

/* A sprite cell's edge, in marker sizes: room round the marker for its halo, a double outline and the selection
 * halo, which is the whole cell. */
constexpr double kSpriteScale = 1.75;
/* Tiles kept from another zoom to stand in, scaled, while the view's own load. */
constexpr int kMaxStandIns = 64;
/* Trails and circles are built for the view grown by this fraction of its larger side each way; a pan within it
 * only moves them. */
constexpr double kPanMarginFraction = 0.5;
/* Space between a marker's edge and its label, logical pixels, and the label's halo around its text. */
constexpr double kLabelGap = 3.0;
constexpr double kLabelHalo = 2.0;

bool
differs(double a, double b) {
    return std::fabs(a - b) > 1.0e-12;
}

bool
same_viewport(const mm::Viewport& a, const mm::Viewport& b) {
    return !differs(a.center_x, b.center_x) && !differs(a.center_y, b.center_y) && !differs(a.zoom, b.zoom)
           && !differs(a.width, b.width) && !differs(a.height, b.height);
}

quint8
unit_to_byte(float v) {
    if (!(v > 0.0F)) {
        return 0;
    }
    return (v >= 1.0F) ? 255 : static_cast<quint8>(std::lround(v * 255.0F));
}

/** @brief A run of vertices drawn in one colour. */
struct ColorSpan {
    const std::vector<mg::StrokeVertex>* vertices = nullptr;
    QColor color;
};

QSGGeometryNode*
make_colored_node() {
    auto* node = new QSGGeometryNode;
    auto* geometry =
        new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(), 0, 0, QSGGeometry::UnsignedIntType);
    geometry->setDrawingMode(QSGGeometry::DrawTriangles);
    node->setGeometry(geometry);
    node->setMaterial(new QSGVertexColorMaterial);
    node->setFlags(QSGNode::OwnsGeometry | QSGNode::OwnsMaterial);
    return node;
}

QSGGeometryNode*
make_sprite_node() {
    auto* node = new QSGGeometryNode;
    auto* geometry =
        new QSGGeometry(QSGGeometry::defaultAttributes_TexturedPoint2D(), 0, 0, QSGGeometry::UnsignedIntType);
    geometry->setDrawingMode(QSGGeometry::DrawTriangles);
    node->setGeometry(geometry);
    auto* material = new QSGTextureMaterial;
    material->setFiltering(QSGTexture::Linear);
    material->setFlag(QSGMaterial::Blending, true); /* the atlas is premultiplied and mostly transparent */
    node->setMaterial(material);
    node->setFlags(QSGNode::OwnsGeometry | QSGNode::OwnsMaterial);
    return node;
}

/** @brief Load stroke vertices into @p node, each span in its colour, premultiplied by coverage. */
void
set_colored_vertices(QSGGeometryNode* node, const ColorSpan* spans, int span_count) {
    size_t total = 0;
    for (int s = 0; s < span_count; s++) {
        total += spans[s].vertices->size();
    }
    QSGGeometry* geometry = node->geometry();
    if (geometry->vertexCount() != static_cast<int>(total)) {
        geometry->allocate(static_cast<int>(total));
    }
    QSGGeometry::ColoredPoint2D* out = geometry->vertexDataAsColoredPoint2D();
    for (int s = 0; s < span_count; s++) {
        const QColor& c = spans[s].color;
        const float alpha = c.alphaF();
        const float red = c.redF();
        const float green = c.greenF();
        const float blue = c.blueF();
        for (const mg::StrokeVertex& v : *spans[s].vertices) {
            const float a = alpha * std::fmin(1.0F, std::fmax(0.0F, v.coverage));
            out->set(v.x, v.y, unit_to_byte(red * a), unit_to_byte(green * a), unit_to_byte(blue * a), unit_to_byte(a));
            out++;
        }
    }
    node->markDirty(QSGNode::DirtyGeometry);
}

void
set_sprite_vertices(QSGGeometryNode* node, const std::vector<mg::SpriteVertex>& sprites) {
    QSGGeometry* geometry = node->geometry();
    const int count = static_cast<int>(sprites.size());
    if (geometry->vertexCount() != count) {
        geometry->allocate(count);
    }
    QSGGeometry::TexturedPoint2D* out = geometry->vertexDataAsTexturedPoint2D();
    for (int i = 0; i < count; i++) {
        const mg::SpriteVertex& v = sprites[static_cast<size_t>(i)];
        out[i].set(v.x, v.y, v.u, v.v);
    }
    node->markDirty(QSGNode::DirtyGeometry);
}

/** @brief The name a label shows for a track: its label, else its callsign, else its radio ID; "" for none. */
QString
track_name(const PositionModel::Row& row) {
    if (!row.label.isEmpty()) {
        return row.label;
    }
    const dsd_position_fix& fix = row.latest();
    QString callsign =
        QString::fromUtf8(fix.source_text, static_cast<int>(qstrnlen(fix.source_text, sizeof fix.source_text)))
            .trimmed();
    if (!callsign.isEmpty()) {
        return callsign;
    }
    return (fix.source_id != 0U) ? QString::number(fix.source_id) : QString();
}

/** @brief Draw order rank: plain tracks first, then emergencies, the selected track on top. */
int
draw_rank(const MapViewItem::TrackDraw& t) {
    if (t.selected) {
        return 2;
    }
    return t.emergency ? 1 : 0;
}

/** @brief The trail of @p row in world coordinates, a non-finite point at every break. */
void
world_trail(const PositionModel::Row& row, MapViewItem::TrackDraw& t) {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const int n = static_cast<int>(row.trail.size());
    t.trail.reserve(static_cast<size_t>(n) * 2U);
    t.dashedTo.reserve(static_cast<size_t>(n) * 2U);
    for (int k = 0; k < n; k++) {
        const PositionModel::Point& p = row.trail.at(k);
        if (k > 0 && p.breakBefore) {
            t.trail.push_back(mm::WorldPoint{nan, nan});
            t.dashedTo.push_back(0U);
        }
        mm::WorldPoint w{nan, nan};
        (void)mm::world_from_e7(p.latE7, p.lonE7, &w); /* out of range stays a break */
        t.trail.push_back(w);
        t.dashedTo.push_back((k > 0 && (p.flags & DSD_POS_FLAG_FORMAT_PROVISIONAL) != 0U) ? 1U : 0U);
    }
}

/** @brief Premultiplied ink-to-paper ramp, one entry per luminance step. */
void
tone_ramp(const QColor& ink, const QColor& paper, QRgb* ramp) {
    for (int l = 0; l < 256; l++) {
        const double t = static_cast<double>(l) / 255.0;
        const int r = qRound((ink.red() * (1.0 - t)) + (paper.red() * t));
        const int g = qRound((ink.green() * (1.0 - t)) + (paper.green() * t));
        const int b = qRound((ink.blue() * (1.0 - t)) + (paper.blue() * t));
        ramp[l] = qRgb(r, g, b);
    }
}

/** @brief The grid's lines in view: tile edges at the view's tile zoom, inside the world's north and south edges. */
struct GridLines {
    std::vector<double> xs; /* vertical lines, from top to bottom */
    std::vector<double> ys; /* horizontal lines, across the view */
    double top = 0.0;
    double bottom = 0.0;
};

GridLines
grid_lines(const mm::Viewport& vp) {
    GridLines g;
    const double world = mm::world_size_px(vp.zoom);
    const double cell = world / std::ldexp(1.0, mm::tile_zoom(vp.zoom));
    if (!(cell > 1.0) || !(vp.width > 0.0) || !(vp.height > 0.0)) {
        return g;
    }
    const double origin_x = (vp.width / 2.0) - (vp.center_x * world);
    const double origin_y = (vp.height / 2.0) - (vp.center_y * world);
    g.top = std::max(0.0, origin_y);
    g.bottom = std::min(vp.height, origin_y + world);
    /* Whole-number steps from the first edge in view, so no error builds up along the row. */
    const double first_x = std::ceil(-origin_x / cell);
    for (int i = 0; origin_x + ((first_x + i) * cell) <= vp.width; i++) {
        g.xs.push_back(origin_x + ((first_x + i) * cell));
    }
    const double first_y = std::ceil((g.top - origin_y) / cell);
    for (int i = 0; origin_y + ((first_y + i) * cell) <= g.bottom; i++) {
        g.ys.push_back(origin_y + ((first_y + i) * cell));
    }
    return g;
}

QPainterPath
diamond_path(double c, double half) {
    QPainterPath path;
    path.moveTo(c, c - half);
    path.lineTo(c + half, c);
    path.lineTo(c, c + half);
    path.lineTo(c - half, c);
    path.closeSubpath();
    return path;
}

QPainterPath
ring_path(double c, double radius) {
    QPainterPath path;
    path.addEllipse(QPointF(c, c), radius, radius);
    return path;
}

} // namespace

// ------------------------------------------------------------ GPU tree ----

/** @brief The RHI tree's root: its layers, and the textures they draw (deleted with it, on the render thread). */
class MapViewItem::GpuRoot : public QSGNode {
  public:
    GpuRoot()
        : grid(make_colored_node()), tiles(new QSGNode), overlay(new QSGTransformNode), circles(make_colored_node()),
          trails(make_colored_node()), sprites(make_sprite_node()), labels(new QSGNode) {
        appendChildNode(grid);
        appendChildNode(tiles);
        appendChildNode(overlay);
        overlay->appendChildNode(circles);
        overlay->appendChildNode(trails);
        appendChildNode(sprites);
        appendChildNode(labels);
    }

    ~GpuRoot() override {
        dropTiles();
        dropLabels();
        delete atlas;
    }

    GpuRoot(const GpuRoot&) = delete;
    GpuRoot& operator=(const GpuRoot&) = delete;

    void
    dropTiles() {
        for (auto it = tileTextures.begin(); it != tileTextures.end(); ++it) {
            delete it.value().texture;
        }
        tileTextures.clear();
    }

    void
    dropLabels() {
        qDeleteAll(labelTextures);
        labelTextures.clear();
    }

    struct TileTexture {
        QSGTexture* texture = nullptr;
        quint64 serial = 0;
    };

    QSGGeometryNode* grid;
    QSGNode* tiles;
    QSGTransformNode* overlay;
    QSGGeometryNode* circles;
    QSGGeometryNode* trails;
    QSGGeometryNode* sprites;
    QSGNode* labels;
    QHash<MapTileKey, TileTexture> tileTextures;
    QSGTexture* atlas = nullptr;
    quint64 atlasBuilt = 0;
    QHash<QString, QSGTexture*> labelTextures;
    quint64 labelsBuilt = 0;
    /* What the overlay (circles, trails) was built for. */
    quint64 overlaySerial = 0;
    mm::Viewport overlayVp;
    double overlayRatio = 0.0;
};

// ----------------------------------------------------------- the item ----

MapViewItem::MapViewItem(QQuickItem* parent) : QQuickItem(parent), m_camera(new QVariantAnimation(this)) {
    setFlag(ItemHasContents, true);
    /* Tiles, trails and labels run past the edge; the canvas is a window onto the map. */
    setClip(true);
    m_camera->setStartValue(0.0);
    m_camera->setEndValue(1.0);
    m_camera->setEasingCurve(QEasingCurve::OutCubic);
    connect(m_camera, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
        setViewport(ms::interpolate_viewport(m_animFrom, m_animTo, value.toDouble()));
    });
    if (const auto* app = qobject_cast<QGuiApplication*>(QCoreApplication::instance())) {
        connect(app, &QGuiApplication::applicationStateChanged, this, &MapViewItem::onApplicationStateChanged);
        onApplicationStateChanged(QGuiApplication::applicationState());
    }
}

MapViewItem::~MapViewItem() {
    if (m_tiles && !m_requested.isEmpty()) {
        m_tiles->clearRequests();
    }
}

// ------------------------------------------------------------ sources ----

void
MapViewItem::setModel(PositionModel* next) {
    if (m_model.data() == next) {
        return;
    }
    if (m_model) {
        disconnect(m_model.data(), nullptr, this, nullptr);
    }
    m_model = next;
    if (m_model) {
        connect(m_model.data(), &PositionModel::tracksChanged, this, &MapViewItem::onTracksChanged);
        connect(m_model.data(), &PositionModel::agesChanged, this, &MapViewItem::onAgesChanged);
        connect(m_model.data(), &QObject::destroyed, this, &MapViewItem::onModelDestroyed);
    }
    Q_EMIT modelChanged();
    onTracksChanged();
}

void
MapViewItem::onModelDestroyed() {
    m_model.clear();
    Q_EMIT modelChanged();
    onTracksChanged();
}

void
MapViewItem::setTileSource(MapTileWorker* next) {
    if (m_tiles.data() == next) {
        return;
    }
    dropTileRequests();
    if (m_tiles) {
        disconnect(m_tiles.data(), nullptr, this, nullptr);
    }
    m_tiles = next;
    if (m_tiles) {
        connect(m_tiles.data(), &MapTileWorker::tileReady, this, &MapViewItem::onTileReady);
        connect(m_tiles.data(), &MapTileWorker::enabledChanged, this, &MapViewItem::onTileSettingsChanged);
        connect(m_tiles.data(), &MapTileWorker::urlTemplateChanged, this, &MapViewItem::onTileSettingsChanged);
        connect(m_tiles.data(), &QObject::destroyed, this, &MapViewItem::onTileSourceDestroyed);
    }
    Q_EMIT tileSourceChanged();
    onTileSettingsChanged();
}

void
MapViewItem::onTileSourceDestroyed() {
    m_tiles.clear();
    m_requested.clear();
    Q_EMIT tileSourceChanged();
    onTileSettingsChanged();
}

void
MapViewItem::onTileSettingsChanged() {
    /* Tiles off or another source: nothing of the old one may stay on screen, and the view is asked for again. */
    m_tileImages.clear();
    m_requested.clear();
    polish();
    update();
}

void
MapViewItem::setActive(bool on) {
    if (m_active == on) {
        return;
    }
    m_active = on;
    Q_EMIT activeChanged();
    polish();
    update();
}

void
MapViewItem::onApplicationStateChanged(Qt::ApplicationState state) {
    /* Only a hidden or suspended app stops loading: a desktop window that merely lost focus is Inactive. */
    const bool shown = state != Qt::ApplicationHidden && state != Qt::ApplicationSuspended;
    if (shown == m_appActive) {
        return;
    }
    m_appActive = shown;
    polish();
    update();
}

// --------------------------------------------------------------- scene ----

void
MapViewItem::onTracksChanged() {
    rebuildScene();
    m_sceneSerial++;
    followNow();
    update();
}

void
MapViewItem::onAgesChanged() {
    /* Ages move fill strengths (sprites only) and what the filter shows; trails and circles are rebuilt only when
     * the set drawn with them changed. */
    const auto drawn = [](const TrackDraw& t) { return (t.id << 1U) | (t.hasMarker ? 1U : 0U); };
    std::vector<quint64> before(m_scene.size());
    std::transform(m_scene.begin(), m_scene.end(), before.begin(), drawn);
    rebuildScene();
    std::vector<quint64> after(m_scene.size());
    std::transform(m_scene.begin(), m_scene.end(), after.begin(), drawn);
    if (before != after) {
        m_sceneSerial++;
    }
    update();
}

void
MapViewItem::sceneChanged() {
    rebuildScene();
    m_sceneSerial++;
    update();
}

namespace {

/** @brief What the canvas draws for one row: its current marker with trail and circle, and its open ring. */
bool
track_draw(const PositionModel::Row& row, int fresh_s, int faded_s, MapViewItem::TrackDraw& t) {
    t.id = row.id;
    t.emergency = (row.latest().flags & DSD_POS_FLAG_EMERGENCY) != 0U;
    if (row.hasCurrent && row.currentShown && mm::world_from_e7(row.current.lat_e7, row.current.lon_e7, &t.marker)) {
        t.hasMarker = true;
        t.markerStyle.dashed_outline = row.anonymous;
        t.markerStyle.double_outline = (row.current.flags & DSD_POS_FLAG_FORMAT_PROVISIONAL) != 0U;
        t.markerStyle.alert = t.emergency;
        t.markerStyle.fill_level = ms::fill_level(row.ageS, fresh_s, faded_s);
        t.errM = static_cast<double>(row.current.err_m);
        t.latDeg = static_cast<double>(row.current.lat_e7) / 1.0e7;
        /* An anonymous row holds one point (the model never extends it), so it has no trail. */
        world_trail(row, t);
    }
    if (row.hasUnknown && row.unknownShown && mm::world_from_e7(row.unknown.lat_e7, row.unknown.lon_e7, &t.ring)) {
        t.hasRing = true;
        t.ringStyle.ring = true;
        t.ringStyle.dashed_outline = row.anonymous;
        t.ringStyle.double_outline = (row.unknown.flags & DSD_POS_FLAG_FORMAT_PROVISIONAL) != 0U;
        t.ringStyle.alert = t.emergency;
    }
    return t.hasMarker || t.hasRing;
}

} // namespace

void
MapViewItem::rebuildScene() {
    m_scene.clear();
    m_markers.clear();
    const QVector<PositionModel::Row> empty;
    const QVector<PositionModel::Row>& rows = m_model ? m_model->rows() : empty;
    /* Rows are newest first; the oldest are drawn first, under the newer. */
    for (int i = static_cast<int>(rows.size()) - 1; i >= 0; i--) {
        const PositionModel::Row& row = rows.at(i);
        TrackDraw t;
        if (row.gap || !track_draw(row, m_freshS, m_fadedS, t)) {
            continue;
        }
        t.selected = (row.id == m_selected);
        const QString name = track_name(row);
        if (!t.emergency) {
            t.label = name;
        } else {
            t.label = name.isEmpty() ? m_emergencyText : m_emergencyText + QStringLiteral(" \u00B7 ") + name;
        }
        m_scene.push_back(std::move(t));
    }
    std::stable_sort(m_scene.begin(), m_scene.end(),
                     [](const TrackDraw& a, const TrackDraw& b) { return draw_rank(a) < draw_rank(b); });
    for (size_t i = 0; i < m_scene.size(); i++) {
        const TrackDraw& t = m_scene[i];
        if (t.hasRing) {
            m_markers.push_back(MarkerDraw{t.ring, ms::marker_cell(t.ringStyle), static_cast<int>(i)});
        }
        if (t.hasMarker) {
            m_markers.push_back(MarkerDraw{t.marker, ms::marker_cell(t.markerStyle), static_cast<int>(i)});
        }
    }
    const int count = static_cast<int>(m_markers.size());
    if (count != m_markerCount) {
        m_markerCount = count;
        Q_EMIT markerCountChanged();
    }
}

// ----------------------------------------------------------- viewport ----

mm::Viewport
MapViewItem::viewportForSize() const {
    mm::Viewport vp = m_vp;
    vp.width = width();
    vp.height = height();
    return mm::clamp_viewport(vp);
}

void
MapViewItem::setViewport(const mm::Viewport& next) {
    mm::Viewport vp = next;
    vp.width = width();
    vp.height = height();
    vp = mm::clamp_viewport(vp);
    if (same_viewport(vp, m_vp)) {
        return;
    }
    m_vp = vp;
    Q_EMIT viewportChanged();
    polish();
    update();
}

double
MapViewItem::centerLatitude() const {
    return mm::lat_lon_from_world(mm::WorldPoint{m_vp.center_x, m_vp.center_y}).lat;
}

double
MapViewItem::centerLongitude() const {
    return mm::lat_lon_from_world(mm::WorldPoint{m_vp.center_x, m_vp.center_y}).lon;
}

void
MapViewItem::setCenterLatitude(double lat) {
    if (!std::isfinite(lat)) {
        return;
    }
    stopCamera();
    mm::Viewport vp = m_vp;
    vp.center_y = mm::world_from_lat_lon(lat, centerLongitude()).y;
    setViewport(vp);
}

void
MapViewItem::setCenterLongitude(double lon) {
    if (!std::isfinite(lon)) {
        return;
    }
    stopCamera();
    mm::Viewport vp = m_vp;
    vp.center_x = mm::world_from_lat_lon(centerLatitude(), lon).x;
    setViewport(vp);
}

void
MapViewItem::setZoom(double level) {
    if (!std::isfinite(level)) {
        return;
    }
    stopCamera();
    mm::Viewport vp = m_vp;
    vp.zoom = level;
    setViewport(vp);
}

double
MapViewItem::minimumZoom() const {
    return mm::min_zoom_for_width(width());
}

void
MapViewItem::geometryChange(const QRectF& new_geometry, const QRectF& old_geometry) {
    QQuickItem::geometryChange(new_geometry, old_geometry);
    if (new_geometry.size() != old_geometry.size()) {
        /* The centre stays where it was; the zoom floor may rise with the width. */
        setViewport(m_vp);
        polish();
        update();
    }
}

void
MapViewItem::itemChange(ItemChange change, const ItemChangeData& value) {
    QQuickItem::itemChange(change, value);
    if (change == ItemDevicePixelRatioHasChanged || change == ItemVisibleHasChanged || change == ItemSceneChange) {
        polish();
        update();
    }
}

// ------------------------------------------------------------- camera ----

void
MapViewItem::stopCamera() {
    m_camera->stop();
}

void
MapViewItem::moveCamera(const mm::Viewport& target) {
    m_camera->stop();
    mm::Viewport to = target;
    to.width = width();
    to.height = height();
    to = mm::clamp_viewport(to);
    if (m_cameraMs <= 0) {
        setViewport(to);
        return;
    }
    m_animFrom = m_vp;
    m_animTo = to;
    m_camera->setDuration(m_cameraMs);
    m_camera->start();
}

void
MapViewItem::panBy(double dx, double dy) {
    stopCamera();
    stopFollowing();
    setViewport(mm::pan(m_vp, dx, dy));
}

void
MapViewItem::zoomAbout(double level, double x, double y) {
    if (!std::isfinite(level)) {
        return;
    }
    stopCamera();
    if (m_followTrack != 0U || m_followDevice) {
        x = width() / 2.0;
        y = height() / 2.0;
    }
    setViewport(mm::zoom_about(m_vp, level, x, y));
}

void
MapViewItem::zoomIn() {
    const mm::Viewport base = (m_camera->state() == QAbstractAnimation::Running) ? m_animTo : m_vp;
    moveCamera(mm::zoom_about(base, base.zoom + 1.0, width() / 2.0, height() / 2.0));
}

void
MapViewItem::zoomOut() {
    const mm::Viewport base = (m_camera->state() == QAbstractAnimation::Running) ? m_animTo : m_vp;
    moveCamera(mm::zoom_about(base, base.zoom - 1.0, width() / 2.0, height() / 2.0));
}

std::vector<mm::WorldPoint>
MapViewItem::markerPoints() const {
    std::vector<mm::WorldPoint> points(m_markers.size());
    std::transform(m_markers.begin(), m_markers.end(), points.begin(), [](const MarkerDraw& m) { return m.at; });
    return points;
}

const MapViewItem::TrackDraw*
MapViewItem::findTrack(qulonglong trackId) const {
    const auto found =
        std::find_if(m_scene.begin(), m_scene.end(), [trackId](const TrackDraw& t) { return t.id == trackId; });
    return (found != m_scene.end()) ? &*found : nullptr;
}

bool
MapViewItem::fitAll() {
    std::vector<mm::WorldPoint> points = markerPoints();
    mm::WorldPoint device{};
    const bool withDevice = deviceWorld(&device);
    if (withDevice) {
        points.push_back(device);
    }
    mm::Viewport out;
    if (!mm::fit_points(viewportForSize(), points.data(), static_cast<int>(points.size()), m_fitPadding, m_fitMaxZoom,
                        &out)) {
        return false;
    }
    stopFollowing();
    if (withDevice) {
        markUsedDevice();
    }
    moveCamera(out);
    return true;
}

bool
MapViewItem::centerOnTrack(qulonglong trackId) {
    const TrackDraw* t = findTrack(trackId);
    if (t == nullptr) {
        return false;
    }
    mm::Viewport vp = m_vp;
    const mm::WorldPoint at = t->hasMarker ? t->marker : t->ring;
    vp.center_x = at.x;
    vp.center_y = at.y;
    moveCamera(vp);
    return true;
}

bool
MapViewItem::centerOnDevice() {
    mm::WorldPoint at{};
    if (!deviceWorld(&at)) {
        return false;
    }
    markUsedDevice();
    mm::Viewport vp = m_vp;
    vp.center_x = at.x;
    vp.center_y = at.y;
    moveCamera(vp);
    return true;
}

void
MapViewItem::centerOn(double lat, double lon, double level) {
    if (!std::isfinite(lat) || !std::isfinite(lon)) {
        return;
    }
    const mm::WorldPoint at = mm::world_from_lat_lon(lat, lon);
    mm::Viewport vp = m_vp;
    vp.center_x = at.x;
    vp.center_y = at.y;
    if (level >= 0.0 && std::isfinite(level)) {
        vp.zoom = level;
    }
    moveCamera(vp);
}

qulonglong
MapViewItem::trackAt(double x, double y) const {
    const std::vector<mm::WorldPoint> points = markerPoints();
    const int hit = mm::hit_test(m_vp, points.data(), static_cast<int>(points.size()), x, y, m_hitRadius);
    if (hit < 0) {
        return 0U;
    }
    return m_scene[static_cast<size_t>(m_markers[static_cast<size_t>(hit)].track)].id;
}

QVariant
MapViewItem::markerPoint(qulonglong trackId) const {
    const TrackDraw* t = findTrack(trackId);
    if (t == nullptr) {
        return QVariant();
    }
    const mm::ScreenPoint s = mm::screen_from_world(m_vp, t->hasMarker ? t->marker : t->ring);
    return QVariant(QPointF(s.x, s.y));
}

// ---------------------------------------------------------- following ----

void
MapViewItem::setSelectedTrackId(qulonglong id) {
    if (m_selected == id) {
        return;
    }
    m_selected = id;
    Q_EMIT selectedTrackIdChanged();
    sceneChanged();
}

void
MapViewItem::setFollowTrackId(qulonglong id) {
    if (m_followTrack == id) {
        return;
    }
    m_followTrack = id;
    if (id != 0U) {
        m_followDevice = false;
    }
    Q_EMIT followChanged();
    followNow();
}

void
MapViewItem::setFollowDevice(bool on) {
    if (m_followDevice == on) {
        return;
    }
    m_followDevice = on;
    if (on) {
        m_followTrack = 0U;
    }
    Q_EMIT followChanged();
    followNow();
}

void
MapViewItem::stopFollowing() {
    if (m_followTrack == 0U && !m_followDevice) {
        return;
    }
    m_followTrack = 0U;
    m_followDevice = false;
    Q_EMIT followChanged();
}

void
MapViewItem::followNow() {
    if (m_followDevice) {
        (void)centerOnDevice();
        return;
    }
    if (m_followTrack == 0U) {
        return;
    }
    /* A track the model no longer holds (cleared, evicted) ends the follow; one the filter hides waits. */
    if (!m_model || m_model->rowForTrack(m_followTrack) < 0) {
        stopFollowing();
        return;
    }
    (void)centerOnTrack(m_followTrack);
}

bool
MapViewItem::deviceWorld(mm::WorldPoint* out) const {
    if (!m_deviceValid || !std::isfinite(m_deviceLat) || !std::isfinite(m_deviceLon) || std::fabs(m_deviceLat) > 90.0
        || std::fabs(m_deviceLon) > 180.0) {
        return false;
    }
    *out = mm::world_from_lat_lon(m_deviceLat, m_deviceLon);
    return true;
}

void
MapViewItem::markUsedDevice() {
    if (m_usedDevice) {
        return;
    }
    m_usedDevice = true;
    Q_EMIT cameraUsedDevicePositionChanged();
}

void
MapViewItem::resetCameraUsedDevicePosition() {
    if (!m_usedDevice) {
        return;
    }
    m_usedDevice = false;
    Q_EMIT cameraUsedDevicePositionChanged();
}

void
MapViewItem::setDeviceValid(bool valid) {
    if (m_deviceValid == valid) {
        return;
    }
    m_deviceValid = valid;
    m_sceneSerial++;
    Q_EMIT deviceChanged();
    followNow();
    update();
}

void
MapViewItem::setDeviceLatitude(double lat) {
    if (!differs(m_deviceLat, lat)) {
        return;
    }
    m_deviceLat = lat;
    m_sceneSerial++;
    Q_EMIT deviceChanged();
    followNow();
    update();
}

void
MapViewItem::setDeviceLongitude(double lon) {
    if (!differs(m_deviceLon, lon)) {
        return;
    }
    m_deviceLon = lon;
    m_sceneSerial++;
    Q_EMIT deviceChanged();
    followNow();
    update();
}

void
MapViewItem::setDeviceAccuracyM(double metres) {
    if (!differs(m_deviceAccuracyM, metres)) {
        return;
    }
    m_deviceAccuracyM = metres;
    m_sceneSerial++;
    Q_EMIT deviceChanged();
    update();
}

// ---------------------------------------------------- style properties ----

bool
MapViewItem::assignColor(QColor& member, const QColor& next) {
    if (member == next) {
        return false;
    }
    member = next;
    Q_EMIT colorsChanged();
    update();
    return true;
}

bool
MapViewItem::assignSize(double& member, double next) {
    if (!std::isfinite(next) || next < 0.0 || !differs(member, next)) {
        return false;
    }
    member = next;
    update();
    return true;
}

void
MapViewItem::setMarkerColor(const QColor& c) {
    if (assignColor(m_markerColor, c)) {
        m_atlasSerial++;
    }
}

void
MapViewItem::setHaloColor(const QColor& c) {
    if (assignColor(m_haloColor, c)) {
        m_atlasSerial++;
        m_labelSerial++;
    }
}

void
MapViewItem::setAlertColor(const QColor& c) {
    if (assignColor(m_alertColor, c)) {
        m_atlasSerial++;
        m_labelSerial++;
    }
}

void
MapViewItem::setSelectedColor(const QColor& c) {
    if (assignColor(m_selectedColor, c)) {
        m_atlasSerial++;
        m_labelSerial++;
        m_sceneSerial++;
    }
}

void
MapViewItem::setTrailColor(const QColor& c) {
    if (assignColor(m_trailColor, c)) {
        m_sceneSerial++;
    }
}

void
MapViewItem::setAccuracyColor(const QColor& c) {
    if (assignColor(m_accuracyColor, c)) {
        m_sceneSerial++;
    }
}

void
MapViewItem::setDeviceColor(const QColor& c) {
    if (assignColor(m_deviceColor, c)) {
        m_atlasSerial++;
    }
}

void
MapViewItem::setLabelColor(const QColor& c) {
    if (assignColor(m_labelColor, c)) {
        m_labelSerial++;
    }
}

void
MapViewItem::setGridColor(const QColor& c) {
    (void)assignColor(m_gridColor, c);
}

void
MapViewItem::setTileInkColor(const QColor& c) {
    if (assignColor(m_tileInk, c)) {
        retoneTiles();
    }
}

void
MapViewItem::setTilePaperColor(const QColor& c) {
    if (assignColor(m_tilePaper, c)) {
        retoneTiles();
    }
}

void
MapViewItem::setMarkerSize(double size) {
    if (assignSize(m_markerSize, size)) {
        m_atlasSerial++;
        Q_EMIT styleChanged();
    }
}

void
MapViewItem::setTrailWidth(double width) {
    if (assignSize(m_trailWidth, width)) {
        m_sceneSerial++;
        Q_EMIT styleChanged();
    }
}

void
MapViewItem::setDashLength(double length) {
    if (assignSize(m_dashLength, length)) {
        m_sceneSerial++;
        Q_EMIT styleChanged();
    }
}

void
MapViewItem::setDashGap(double gap) {
    if (assignSize(m_dashGap, gap)) {
        m_sceneSerial++;
        Q_EMIT styleChanged();
    }
}

void
MapViewItem::setHitRadius(double radius) {
    if (assignSize(m_hitRadius, radius)) {
        Q_EMIT behaviourChanged();
    }
}

void
MapViewItem::setFitPadding(double padding) {
    if (assignSize(m_fitPadding, padding)) {
        Q_EMIT behaviourChanged();
    }
}

void
MapViewItem::setFitMaxZoom(double level) {
    if (assignSize(m_fitMaxZoom, level)) {
        Q_EMIT behaviourChanged();
    }
}

void
MapViewItem::setLabelFont(const QFont& font) {
    if (m_labelFont == font) {
        return;
    }
    m_labelFont = font;
    m_labelSerial++;
    Q_EMIT styleChanged();
    update();
}

void
MapViewItem::setEmergencyText(const QString& text) {
    if (m_emergencyText == text) {
        return;
    }
    m_emergencyText = text;
    Q_EMIT styleChanged();
    sceneChanged();
}

void
MapViewItem::setFreshSeconds(int seconds) {
    if (m_freshS == seconds || seconds < 0) {
        return;
    }
    m_freshS = seconds;
    Q_EMIT styleChanged();
    sceneChanged();
}

void
MapViewItem::setFadedSeconds(int seconds) {
    if (m_fadedS == seconds || seconds < 0) {
        return;
    }
    m_fadedS = seconds;
    Q_EMIT styleChanged();
    sceneChanged();
}

void
MapViewItem::setCameraDurationMs(int ms) {
    if (m_cameraMs == ms || ms < 0) {
        return;
    }
    m_cameraMs = ms;
    Q_EMIT behaviourChanged();
}

// --------------------------------------------------------------- tiles ----

bool
MapViewItem::tilesWanted() const {
    return m_tiles && m_tiles->enabled() && m_active && m_appActive && isVisible() && window() != nullptr
           && width() > 0.0 && height() > 0.0;
}

void
MapViewItem::dropTileRequests() {
    if (m_tiles && !m_requested.isEmpty()) {
        m_tiles->clearRequests();
    }
    m_requested.clear();
}

void
MapViewItem::updatePolish() {
    m_vp = viewportForSize();
    const QList<map_math::VisibleTile> none;
    if (!tilesWanted()) {
        dropTileRequests();
        if (!m_tileImages.isEmpty()) {
            m_tileImages.clear();
            update();
        }
        m_visible = none;
        return;
    }
    const std::vector<mm::VisibleTile> visible = mm::visible_tiles(m_vp);
    m_visible = QList<mm::VisibleTile>(visible.begin(), visible.end());
    QList<MapTileKey> keys;
    QSet<MapTileKey> inView;
    keys.reserve(m_visible.size());
    for (const mm::VisibleTile& t : m_visible) {
        const MapTileKey key{t.z, t.x, t.y};
        if (!inView.contains(key)) {
            inView.insert(key);
            keys.append(key);
        }
    }
    if (keys != m_requested) {
        m_requested = keys;
        (void)m_tiles->requestTiles(keys);
    }
    /* Held images: the view's own, and stand-ins from another zoom while some of the view's are still loading. */
    bool complete = true;
    for (const MapTileKey& key : keys) {
        complete = complete && m_tileImages.contains(key);
    }
    int standIns = 0;
    for (auto it = m_tileImages.begin(); it != m_tileImages.end();) {
        it.value().current = inView.contains(it.key());
        const ms::ScreenRect r = ms::tile_rect(m_vp, it.key().z, it.key().x, it.key().y);
        const bool keep = it.value().current
                          || (!complete && standIns < kMaxStandIns && ms::rect_in_view(r, m_vp.width, m_vp.height));
        if (!keep) {
            it = m_tileImages.erase(it);
            continue;
        }
        standIns += it.value().current ? 0 : 1;
        ++it;
    }
}

void
MapViewItem::onTileReady(quint64 generation, int z, int x, int y, const QImage& image, bool stale) {
    /* Any generation (a stale copy too) is drawn while its tile is in view; a fresher one replaces it. */
    (void)generation;
    (void)stale;
    /* A result for an older request is drawn only while its tile is still in view. */
    const MapTileKey key{z, x, y};
    if (!tilesWanted() || !m_requested.contains(key) || image.isNull()) {
        return;
    }
    HeldTile held;
    held.source = image;
    held.toned = toneTile(image);
    held.serial = ++m_tileSerial;
    held.current = true;
    m_tileImages.insert(key, held);
    polish();
    update();
}

QImage
MapViewItem::toneTile(const QImage& image) const {
    QImage src = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    QRgb ramp[256];
    tone_ramp(m_tileInk, m_tilePaper, ramp);
    for (int row = 0; row < src.height(); row++) {
        auto* line = reinterpret_cast<QRgb*>(src.scanLine(row));
        for (int col = 0; col < src.width(); col++) {
            const QRgb p = line[col];
            const int a = qAlpha(p);
            if (a == 0) {
                continue;
            }
            /* Premultiplied luminance over alpha is the colour's own luminance. */
            const int l = ((qRed(p) * 54) + (qGreen(p) * 183) + (qBlue(p) * 19)) >> 8;
            const int lum = std::min(255, (l * 255) / a);
            const QRgb t = ramp[lum];
            line[col] = qRgba((qRed(t) * a) / 255, (qGreen(t) * a) / 255, (qBlue(t) * a) / 255, a);
        }
    }
    return src;
}

void
MapViewItem::retoneTiles() {
    for (auto it = m_tileImages.begin(); it != m_tileImages.end(); ++it) {
        it.value().toned = toneTile(it.value().source);
        it.value().serial = ++m_tileSerial;
    }
}

// ------------------------------------------------------- sprites, labels ----

namespace {

struct SpriteColors {
    QColor marker;
    QColor halo;
    QColor alert;
    QColor selected;
    QColor device;
};

void
stroke(QPainter& p, const QPainterPath& path, const QColor& color, double width, bool dashed) {
    QPen pen(color);
    pen.setWidthF(width);
    pen.setJoinStyle(Qt::RoundJoin);
    pen.setCapStyle(dashed ? Qt::FlatCap : Qt::RoundCap);
    if (dashed) {
        pen.setDashPattern({1.6, 1.2});
    }
    p.setBrush(Qt::NoBrush);
    p.setPen(pen);
    p.drawPath(path);
}

/** @brief One marker cell (map_scene.h's styles), @p cell_px device pixels square. */
void
paint_marker_cell(QPainter& p, const ms::MarkerStyle& style, double cell_px, double size_px, double ratio,
                  const SpriteColors& colors) {
    const double c = cell_px / 2.0;
    const double outline = 1.5 * ratio;
    const double halo = outline + (2.0 * ratio);
    const QColor color = style.alert ? colors.alert : colors.marker;
    const QPainterPath shape = style.ring ? ring_path(c, size_px * 0.42) : diamond_path(c, size_px / 2.0);
    if (style.double_outline) {
        const QPainterPath outer = style.ring ? ring_path(c, (size_px * 0.42) + (3.0 * ratio))
                                              : diamond_path(c, (size_px / 2.0) + (3.5 * ratio));
        stroke(p, outer, colors.halo, ratio * 3.0, false);
        stroke(p, outer, color, ratio, false);
    }
    stroke(p, shape, colors.halo, halo, false);
    if (!style.ring) {
        /* Fill strength reads the same on any tile: over the halo colour, not over the map. */
        p.setPen(Qt::NoPen);
        p.fillPath(shape, colors.halo);
        QColor fill = color;
        fill.setAlphaF(fill.alphaF() * static_cast<double>(ms::fill_opacity(style.fill_level)));
        p.fillPath(shape, fill);
    }
    stroke(p, shape, color, outline, style.dashed_outline);
}

void
paint_special_cell(QPainter& p, int cell, double cell_px, double size_px, double ratio, const SpriteColors& colors) {
    const double c = cell_px / 2.0;
    p.setPen(Qt::NoPen);
    if (cell == ms::kSelectionHaloCell) {
        QColor fill = colors.selected;
        fill.setAlphaF(fill.alphaF() * 0.3);
        const QPainterPath halo = ring_path(c, c - (1.0 * ratio));
        p.fillPath(halo, fill);
        stroke(p, halo, colors.selected, 1.5 * ratio, false);
        return;
    }
    QColor glow = colors.device;
    glow.setAlphaF(glow.alphaF() * 0.25);
    p.fillPath(ring_path(c, size_px * 0.75), glow);
    const QPainterPath dot = ring_path(c, size_px * 0.36);
    p.fillPath(dot, colors.device);
    stroke(p, dot, colors.halo, 2.0 * ratio, false);
}

} // namespace

void
MapViewItem::buildAtlas(double ratio) {
    if (m_atlasBuilt == m_atlasSerial && !differs(m_atlasRatio, ratio) && !m_atlas.isNull()) {
        return;
    }
    m_atlasBuilt = m_atlasSerial;
    m_atlasRatio = ratio;
    m_atlasVersion++;
    m_atlasCellPx = std::max(1, static_cast<int>(std::lround(m_markerSize * kSpriteScale * ratio)));
    m_atlas =
        QImage(m_atlasCellPx * ms::kAtlasColumns, m_atlasCellPx * ms::kAtlasRows, QImage::Format_ARGB32_Premultiplied);
    m_atlas.fill(Qt::transparent);
    const SpriteColors colors{m_markerColor, m_haloColor, m_alertColor, m_selectedColor, m_deviceColor};
    const double cell_px = static_cast<double>(m_atlasCellPx);
    QPainter p(&m_atlas);
    p.setRenderHint(QPainter::Antialiasing, true);
    for (int cell = 0; cell < ms::kMarkerCellCount; cell++) {
        const int column = cell % ms::kAtlasColumns;
        const int row = cell / ms::kAtlasColumns;
        p.save();
        p.translate(static_cast<double>(column) * cell_px, static_cast<double>(row) * cell_px);
        p.setClipRect(QRectF(0.0, 0.0, cell_px, cell_px));
        ms::MarkerStyle style;
        if (ms::marker_style_of_cell(cell, &style)) {
            paint_marker_cell(p, style, cell_px, m_markerSize * ratio, ratio, colors);
        } else {
            paint_special_cell(p, cell, cell_px, m_markerSize * ratio, ratio, colors);
        }
        p.restore();
    }
}

namespace {

QImage
render_label(const QString& text, const QFont& font, const QColor& color, const QColor& halo, double ratio) {
    const QFontMetricsF fm(font);
    const double w = fm.horizontalAdvance(text) + (2.0 * kLabelHalo);
    const double h = fm.height() + (2.0 * kLabelHalo);
    QImage image(std::max(1, qCeil(w * ratio)), std::max(1, qCeil(h * ratio)), QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(ratio);
    image.fill(Qt::transparent);
    QPainter p(&image);
    p.setRenderHint(QPainter::Antialiasing, true);
    QPainterPath path;
    path.addText(kLabelHalo, kLabelHalo + fm.ascent(), font, text);
    QPen pen(halo);
    pen.setWidthF(kLabelHalo * 2.0);
    pen.setJoinStyle(Qt::RoundJoin);
    p.strokePath(path, pen);
    p.fillPath(path, color);
    return image;
}

} // namespace

namespace {

/** @brief Names come and go: keep the images a frame can ask for, not every label ever drawn. */
void
prune_label_images(QHash<QString, QImage>& images, const std::vector<QString>& keys) {
    if (images.size() <= (4 * static_cast<qsizetype>(keys.size())) + 64) {
        return;
    }
    const QSet<QString> wanted(keys.begin(), keys.end());
    for (auto it = images.begin(); it != images.end();) {
        it = wanted.contains(it.key()) ? std::next(it) : images.erase(it);
    }
}

} // namespace

void
MapViewItem::buildLabels(double ratio) {
    if (m_labelBuilt != m_labelSerial || differs(m_labelRatio, ratio)) {
        m_labelImages.clear();
        m_labelBuilt = m_labelSerial;
        m_labelRatio = ratio;
        m_labelVersion++;
    }

    struct Candidate {
        int track;
        int rank;
        int fill;
    };

    std::vector<Candidate> order;
    for (size_t i = m_scene.size(); i-- > 0;) {
        const TrackDraw& t = m_scene[i];
        if (!t.label.isEmpty()) {
            order.push_back(Candidate{static_cast<int>(i), draw_rank(t), t.hasMarker ? t.markerStyle.fill_level : 99});
        }
    }
    std::stable_sort(order.begin(), order.end(), [](const Candidate& a, const Candidate& b) {
        return a.rank != b.rank ? a.rank > b.rank : a.fill < b.fill;
    });
    std::vector<ms::LabelBox> boxes;
    std::vector<QString> keys;
    const double offset = (m_markerSize / 2.0) + kLabelGap;
    for (const Candidate& c : order) {
        const TrackDraw& t = m_scene[static_cast<size_t>(c.track)];
        const QColor& plain = t.selected ? m_selectedColor : m_labelColor;
        const QColor& color = t.emergency ? m_alertColor : plain;
        const QString key = color.name(QColor::HexArgb) + QLatin1Char('\n') + t.label;
        auto found = m_labelImages.constFind(key);
        if (found == m_labelImages.constEnd()) {
            found = m_labelImages.insert(key, render_label(t.label, m_labelFont, color, m_haloColor, ratio));
        }
        const QSizeF size = found.value().deviceIndependentSize();
        const mm::ScreenPoint at = mm::screen_from_world(m_vp, t.hasMarker ? t.marker : t.ring);
        boxes.push_back(ms::LabelBox{
            static_cast<float>(at.x + offset),
            static_cast<float>(at.y - (size.height() / 2.0)),
            static_cast<float>(size.width()),
            static_cast<float>(size.height()),
        });
        keys.push_back(key);
    }
    std::vector<uint8_t> keep(boxes.size(), 0U);
    (void)ms::place_labels(boxes.data(), static_cast<int>(boxes.size()), static_cast<float>(m_vp.width),
                           static_cast<float>(m_vp.height), keep.data());
    m_labels.clear();
    for (size_t i = 0; i < boxes.size(); i++) {
        if (keep[i] != 0U) {
            const ms::LabelBox& b = boxes[i];
            m_labels.push_back(LabelDraw{keys[i], b.left, b.top, b.width, b.height});
        }
    }
    prune_label_images(m_labelImages, keys);
}

// ------------------------------------------------------- paint: shared ----

std::vector<MapViewItem::TileDraw>
MapViewItem::tileDraws(double ratio) const {
    std::vector<TileDraw> draws;
    /* Stand-ins from another zoom first, under the view's own tiles. */
    for (auto it = m_tileImages.cbegin(); it != m_tileImages.cend(); ++it) {
        if (!it.value().current) {
            const ms::ScreenRect r = ms::tile_rect(m_vp, it.key().z, it.key().x, it.key().y);
            if (ms::rect_in_view(r, m_vp.width, m_vp.height)) {
                draws.push_back(TileDraw{it.key(), it.value().toned, it.value().serial, ms::snap_rect(r, ratio)});
            }
        }
    }
    for (const mm::VisibleTile& t : m_visible) {
        const MapTileKey key{t.z, t.x, t.y};
        const auto held = m_tileImages.constFind(key);
        if (held == m_tileImages.cend() || !held.value().current) {
            continue;
        }
        ms::ScreenRect r;
        r.left = t.left;
        r.top = t.top;
        r.right = t.left + t.size;
        r.bottom = t.top + t.size;
        draws.push_back(TileDraw{key, held.value().toned, held.value().serial, ms::snap_rect(r, ratio)});
    }
    return draws;
}

std::vector<MapViewItem::SpriteQuad>
MapViewItem::spriteQuads() const {
    std::vector<SpriteQuad> quads;
    quads.reserve((m_markers.size() * 2U) + 1U);
    for (const MarkerDraw& m : m_markers) {
        const mm::ScreenPoint s = mm::screen_from_world(m_vp, m.at);
        if (m_scene[static_cast<size_t>(m.track)].selected) {
            quads.push_back(SpriteQuad{ms::kSelectionHaloCell, s.x, s.y});
        }
        quads.push_back(SpriteQuad{m.cell, s.x, s.y});
    }
    mm::WorldPoint device{};
    if (deviceWorld(&device)) {
        const mm::ScreenPoint s = mm::screen_from_world(m_vp, device);
        quads.push_back(SpriteQuad{ms::kDeviceDotCell, s.x, s.y});
    }
    return quads;
}

std::vector<MapViewItem::Circle>
MapViewItem::circles() const {
    std::vector<Circle> out;
    for (const TrackDraw& t : m_scene) {
        if (t.hasMarker && t.errM > 0.0) {
            const mm::ScreenPoint s = mm::screen_from_world(m_vp, t.marker);
            out.push_back(Circle{s.x, s.y, mm::metres_to_px(t.errM, t.latDeg, m_vp.zoom)});
        }
    }
    mm::WorldPoint device{};
    if (m_deviceAccuracyM > 0.0 && deviceWorld(&device)) {
        const mm::ScreenPoint s = mm::screen_from_world(m_vp, device);
        out.push_back(Circle{s.x, s.y, mm::metres_to_px(m_deviceAccuracyM, m_deviceLat, m_vp.zoom)});
    }
    return out;
}

template <typename Visit>
void
MapViewItem::forEachTrail(Visit&& visit) {
    for (const TrackDraw& t : m_scene) {
        const int n = static_cast<int>(t.trail.size());
        if (n < 2) {
            continue;
        }
        m_xs.resize(static_cast<size_t>(n));
        m_ys.resize(static_cast<size_t>(n));
        mm::project_track(m_vp, t.trail.data(), n, m_xs.data(), m_ys.data());
        ms::split_trail(m_xs.data(), m_ys.data(), t.dashedTo.data(), n, m_runs);
        visit(t, m_runs);
    }
}

// ---------------------------------------------------------- paint: GPU ----

QSGNode*
MapViewItem::updateGpuTree(QSGNode* old, QQuickWindow* win, double ratio) {
    auto* root = (old != nullptr && old->type() == QSGNode::BasicNodeType) ? static_cast<GpuRoot*>(old) : nullptr;
    if (root == nullptr) {
        delete old;
        root = new GpuRoot;
    }
    gpuGrid(root, ratio);
    gpuTiles(root, win, ratio);
    gpuOverlay(root, ratio);
    gpuSprites(root, win, ratio);
    gpuLabels(root, win, ratio);
    return root;
}

void
MapViewItem::gpuGrid(GpuRoot* root, double ratio) {
    std::vector<mg::StrokeVertex> lines;
    if (m_gridColor.alpha() > 0) {
        const GridLines g = grid_lines(m_vp);
        mg::TrailStyle style;
        style.half_width = 0.5F;
        style.feather = static_cast<float>(1.0 / ratio);
        for (const double x : g.xs) {
            const float xs[2] = {static_cast<float>(x), static_cast<float>(x)};
            const float ys[2] = {static_cast<float>(g.top), static_cast<float>(g.bottom)};
            (void)mg::append_trail(xs, ys, 2, style, nullptr, lines);
        }
        for (const double y : g.ys) {
            const float xs[2] = {0.0F, static_cast<float>(m_vp.width)};
            const float ys[2] = {static_cast<float>(y), static_cast<float>(y)};
            (void)mg::append_trail(xs, ys, 2, style, nullptr, lines);
        }
    }
    const ColorSpan span{&lines, m_gridColor};
    set_colored_vertices(root->grid, &span, 1);
}

void
MapViewItem::gpuTiles(GpuRoot* root, QQuickWindow* win, double ratio) {
    const std::vector<TileDraw> draws = tileDraws(ratio);
    /* Replaced textures are deleted only once no node points at them. */
    std::vector<QSGTexture*> garbage;
    QSet<MapTileKey> used;
    for (const TileDraw& d : draws) {
        GpuRoot::TileTexture& tex = root->tileTextures[d.key];
        if (tex.texture == nullptr || tex.serial != d.serial) {
            garbage.push_back(tex.texture);
            tex.texture = win->createTextureFromImage(d.image);
            tex.serial = d.serial;
        }
        used.insert(d.key);
    }
    QSGNode* child = root->tiles->firstChild();
    for (const TileDraw& d : draws) {
        auto* node = static_cast<QSGImageNode*>(child);
        if (node == nullptr) {
            node = win->createImageNode();
            node->setOwnsTexture(false);
            node->setFiltering(QSGTexture::Linear);
            root->tiles->appendChildNode(node);
        }
        node->setTexture(root->tileTextures.value(d.key).texture);
        node->setRect(QRectF(d.rect.left, d.rect.top, d.rect.right - d.rect.left, d.rect.bottom - d.rect.top));
        child = node->nextSibling();
    }
    while (child != nullptr) {
        QSGNode* next = child->nextSibling();
        root->tiles->removeChildNode(child);
        delete child;
        child = next;
    }
    for (auto it = root->tileTextures.begin(); it != root->tileTextures.end();) {
        if (!used.contains(it.key())) {
            garbage.push_back(it.value().texture);
            it = root->tileTextures.erase(it);
        } else {
            ++it;
        }
    }
    for (const QSGTexture* t : garbage) {
        delete t;
    }
}

void
MapViewItem::gpuOverlay(GpuRoot* root, double ratio) {
    const double margin = kPanMarginFraction * std::max(m_vp.width, m_vp.height);
    if (root->overlaySerial == m_sceneSerial && !differs(root->overlayRatio, ratio)
        && ms::reusable(root->overlayVp, m_vp, margin)) {
        const mm::ScreenPoint d = ms::pan_offset(root->overlayVp, m_vp);
        QMatrix4x4 matrix;
        matrix.translate(static_cast<float>(d.x), static_cast<float>(d.y));
        if (root->overlay->matrix() != matrix) {
            root->overlay->setMatrix(matrix);
        }
        return;
    }
    root->overlaySerial = m_sceneSerial;
    root->overlayVp = m_vp;
    root->overlayRatio = ratio;
    root->overlay->setMatrix(QMatrix4x4());
    const auto m = static_cast<float>(margin);
    const mg::ClipRect clip{-m, -m, static_cast<float>(m_vp.width) + m, static_cast<float>(m_vp.height) + m};
    const auto feather = static_cast<float>(1.0 / ratio);
    std::vector<mg::StrokeVertex> discs;
    for (const Circle& c : circles()) {
        (void)mg::append_accuracy_circle(static_cast<float>(c.x), static_cast<float>(c.y), static_cast<float>(c.r),
                                         feather, clip, discs);
    }
    mg::TrailStyle solid;
    solid.half_width = static_cast<float>(m_trailWidth / 2.0);
    solid.feather = feather;
    mg::TrailStyle dashed = solid;
    dashed.dash = static_cast<float>(m_dashLength);
    dashed.gap = static_cast<float>(m_dashGap);
    std::vector<mg::StrokeVertex> plain;
    std::vector<mg::StrokeVertex> chosen;
    forEachTrail([&](const TrackDraw& t, const ms::TrailRuns& runs) {
        std::vector<mg::StrokeVertex>& out = t.selected ? chosen : plain;
        (void)mg::append_trail(runs.solid_x.data(), runs.solid_y.data(), static_cast<int>(runs.solid_x.size()), solid,
                               &clip, out);
        (void)mg::append_trail(runs.dashed_x.data(), runs.dashed_y.data(), static_cast<int>(runs.dashed_x.size()),
                               dashed, &clip, out);
    });
    const ColorSpan circleSpan{&discs, m_accuracyColor};
    set_colored_vertices(root->circles, &circleSpan, 1);
    const ColorSpan trailSpans[2] = {{&plain, m_trailColor}, {&chosen, m_selectedColor}};
    set_colored_vertices(root->trails, trailSpans, 2);
}

void
MapViewItem::gpuSprites(GpuRoot* root, QQuickWindow* win, double ratio) {
    buildAtlas(ratio);
    auto* material = static_cast<QSGTextureMaterial*>(root->sprites->material());
    if (root->atlas == nullptr || root->atlasBuilt != m_atlasVersion) {
        QSGTexture* next = win->createTextureFromImage(m_atlas);
        material->setTexture(next);
        delete root->atlas;
        root->atlas = next;
        root->atlasBuilt = m_atlasVersion;
        root->sprites->markDirty(QSGNode::DirtyMaterial);
    }
    std::vector<mg::SpriteVertex> sprites;
    const auto size = static_cast<float>(m_markerSize * kSpriteScale);
    for (const SpriteQuad& q : spriteQuads()) {
        const mg::AtlasCell cell = mg::atlas_cell(q.cell % ms::kAtlasColumns, q.cell / ms::kAtlasColumns, m_atlasCellPx,
                                                  m_atlas.width(), m_atlas.height());
        (void)mg::append_marker_quad(static_cast<float>(q.x), static_cast<float>(q.y), size, static_cast<float>(ratio),
                                     cell, sprites);
    }
    set_sprite_vertices(root->sprites, sprites);
}

void
MapViewItem::gpuLabels(GpuRoot* root, QQuickWindow* win, double ratio) {
    buildLabels(ratio);
    if (root->labelsBuilt != m_labelVersion) {
        /* Every node is re-pointed below before anything renders. */
        root->dropLabels();
        root->labelsBuilt = m_labelVersion;
    }
    QSGNode* child = root->labels->firstChild();
    QSet<QString> used;
    for (const LabelDraw& l : m_labels) {
        QSGTexture*& tex = root->labelTextures[l.key];
        if (tex == nullptr) {
            tex = win->createTextureFromImage(m_labelImages.value(l.key));
        }
        used.insert(l.key);
        auto* node = static_cast<QSGImageNode*>(child);
        if (node == nullptr) {
            node = win->createImageNode();
            node->setOwnsTexture(false);
            node->setFiltering(QSGTexture::Linear);
            root->labels->appendChildNode(node);
        }
        node->setTexture(tex);
        const double left = std::round(static_cast<double>(l.left) * ratio) / ratio;
        const double top = std::round(static_cast<double>(l.top) * ratio) / ratio;
        node->setRect(QRectF(left, top, static_cast<double>(l.width), static_cast<double>(l.height)));
        child = node->nextSibling();
    }
    while (child != nullptr) {
        QSGNode* next = child->nextSibling();
        root->labels->removeChildNode(child);
        delete child;
        child = next;
    }
    for (auto it = root->labelTextures.begin(); it != root->labelTextures.end();) {
        if (!used.contains(it.key())) {
            delete it.value();
            it = root->labelTextures.erase(it);
        } else {
            ++it;
        }
    }
}

// ----------------------------------------------------- paint: software ----

namespace {

void
paint_runs(QPainter& p, const std::vector<float>& xs, const std::vector<float>& ys) {
    QPainterPath path;
    bool open = false;
    for (size_t i = 0; i < xs.size(); i++) {
        if (!std::isfinite(xs[i]) || !std::isfinite(ys[i])) {
            open = false;
            continue;
        }
        const QPointF at(static_cast<double>(xs[i]), static_cast<double>(ys[i]));
        if (open) {
            path.lineTo(at);
        } else {
            path.moveTo(at);
            open = true;
        }
    }
    p.drawPath(path);
}

QPen
trail_pen(const QColor& color, double width, double dash, double gap) {
    QPen pen(color);
    pen.setWidthF(width);
    pen.setJoinStyle(Qt::RoundJoin);
    if (dash > 0.0 && width > 0.0) {
        pen.setCapStyle(Qt::FlatCap);
        pen.setDashPattern({dash / width, std::max(gap, 0.01) / width});
    } else {
        pen.setCapStyle(Qt::RoundCap);
    }
    return pen;
}

} // namespace

QImage
MapViewItem::paintSoftwareImage(double ratio) {
    QImage image(std::max(1, qCeil(width() * ratio)), std::max(1, qCeil(height() * ratio)),
                 QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(ratio);
    image.fill(Qt::transparent);
    QPainter p(&image);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    const GridLines g = grid_lines(m_vp);
    p.setPen(QPen(m_gridColor, 1.0));
    for (const double x : g.xs) {
        p.drawLine(QPointF(x, g.top), QPointF(x, g.bottom));
    }
    for (const double y : g.ys) {
        p.drawLine(QPointF(0.0, y), QPointF(m_vp.width, y));
    }
    for (const TileDraw& d : tileDraws(ratio)) {
        p.drawImage(QRectF(d.rect.left, d.rect.top, d.rect.right - d.rect.left, d.rect.bottom - d.rect.top), d.image);
    }
    p.setPen(Qt::NoPen);
    p.setBrush(m_accuracyColor);
    const QRectF view(0.0, 0.0, m_vp.width, m_vp.height);
    for (const Circle& c : circles()) {
        if (std::hypot(std::max(c.x, m_vp.width - c.x), std::max(c.y, m_vp.height - c.y)) <= c.r) {
            p.fillRect(view, m_accuracyColor); /* holds the whole view: a huge ellipse would only cost time */
        } else if (std::hypot(std::max(0.0, std::fabs(c.x - (m_vp.width / 2.0)) - (m_vp.width / 2.0)),
                              std::max(0.0, std::fabs(c.y - (m_vp.height / 2.0)) - (m_vp.height / 2.0)))
                   < c.r) {
            p.drawEllipse(QPointF(c.x, c.y), c.r, c.r);
        }
    }
    p.setBrush(Qt::NoBrush);
    forEachTrail([&](const TrackDraw& t, const ms::TrailRuns& runs) {
        const QColor& color = t.selected ? m_selectedColor : m_trailColor;
        p.setPen(trail_pen(color, m_trailWidth, 0.0, 0.0));
        paint_runs(p, runs.solid_x, runs.solid_y);
        p.setPen(trail_pen(color, m_trailWidth, m_dashLength, m_dashGap));
        paint_runs(p, runs.dashed_x, runs.dashed_y);
    });
    buildAtlas(ratio);
    const auto size = static_cast<float>(m_markerSize * kSpriteScale);
    std::vector<mg::SpriteVertex> quad;
    for (const SpriteQuad& q : spriteQuads()) {
        quad.clear();
        if (mg::append_marker_quad(static_cast<float>(q.x), static_cast<float>(q.y), size, static_cast<float>(ratio),
                                   mg::AtlasCell{0.0F, 0.0F, 0.0F, 0.0F}, quad)
            != 6) {
            continue;
        }
        const QRectF target(quad[0].x, quad[0].y, quad[5].x - quad[0].x, quad[5].y - quad[0].y);
        const double cell = static_cast<double>(m_atlasCellPx);
        const int column = q.cell % ms::kAtlasColumns;
        const int row = q.cell / ms::kAtlasColumns;
        p.drawImage(target, m_atlas,
                    QRectF(static_cast<double>(column) * cell, static_cast<double>(row) * cell, cell, cell));
    }
    buildLabels(ratio);
    for (const LabelDraw& l : m_labels) {
        const double left = std::round(static_cast<double>(l.left) * ratio) / ratio;
        const double top = std::round(static_cast<double>(l.top) * ratio) / ratio;
        p.drawImage(QPointF(left, top), m_labelImages.value(l.key));
    }
    return image;
}

QSGNode*
MapViewItem::updatePaintNode(QSGNode* old, UpdatePaintNodeData* data) {
    (void)data;
    QQuickWindow* win = window();
    if (win == nullptr || width() < 1.0 || height() < 1.0) {
        delete old;
        return nullptr;
    }
    m_vp = viewportForSize();
    const double ratio = win->effectiveDevicePixelRatio();
    if (win->rendererInterface()->graphicsApi() != QSGRendererInterface::Software) {
        return updateGpuTree(old, win, ratio);
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
    const QImage image = paintSoftwareImage(ratio);
    node->setTexture(win->createTextureFromImage(image));
    node->setRect(QRectF(QPointF(0.0, 0.0), image.deviceIndependentSize()));
    return node;
}

#ifdef DSD_NEO_TEST_HOOKS
QSGNode*
MapViewItem::testUpdateGpuTree(QSGNode* old, double pixel_ratio) {
    QQuickWindow* win = window();
    if (win == nullptr) {
        delete old;
        return nullptr;
    }
    m_vp = viewportForSize();
    return updateGpuTree(old, win, pixel_ratio);
}

QImage
MapViewItem::testSoftwareImage(double pixel_ratio) {
    m_vp = viewportForSize();
    return paintSoftwareImage(pixel_ratio);
}
#endif

} // namespace dsd_qt
