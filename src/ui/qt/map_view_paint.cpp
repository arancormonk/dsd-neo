// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief MapViewItem's drawing: the RHI node tree, the software scene graph's image, the sprite atlas and labels.
 *
 * Runs in the scene graph's sync (updatePaintNode()) while the GUI thread is blocked, from the scene the item
 * built on the GUI thread (map_view_item.cpp).
 */

#include "map_view_item.h"

#include <QBrush>
#include <QColor>
#include <QFontMetricsF>
#include <QHash>
#include <QImage>
#include <QList>
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
#include <QSGMaterial>
#include <QSGRendererInterface>
#include <QSGTexture>
#include <QSGTextureMaterial>
#include <QSGVertexColorMaterial>
#include <QSet>
#include <QSizeF>
#include <QString>
#include <Qt>
#include <QtAlgorithms>
#include <QtGlobal>
#include <QtMath>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <stdlib.h>
#include <vector>

#include "map_geometry.h"
#include "map_math.h"
#include "map_scene.h"
#include "map_tile_cache.h"
#include "map_view_detail.h"
#include "spectrum_trace_geometry.h"

class QFont;

namespace dsd_qt {

namespace mm = map_math;
namespace mg = map_geometry;
namespace ms = map_scene;
using map_view_detail::differs;

namespace {

/* A sprite cell's edge, in marker sizes: room round the marker for its halo, a double outline and the selection
 * halo, which is the whole cell. */
constexpr double kSpriteScale = 1.75;
/* Trails and circles are built for the view grown by this fraction of its larger side each way; a pan within it
 * only moves them. */
constexpr double kPanMarginFraction = 0.5;
/* Space between a marker's edge and its label, logical pixels, and the label's halo around its text. */
constexpr double kLabelGap = 3.0;
constexpr double kLabelHalo = 2.0;
/* Cell of the grid labels are checked against their neighbours in, logical pixels. */
constexpr float kLabelGridCell = 48.0F;

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

/** @brief @p c with full alpha: trails overlap themselves at sharp turns, so a translucent one would show doubled
 * joins; they are always drawn opaque, whatever colour the screen binds. */
QColor
opaque(QColor c) {
    c.setAlpha(255);
    return c;
}

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
    std::vector<long long> overlayCopies; /* the wrap copy of each point the overlay was placed by */
};

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
        /* In pen widths: dashes of 2, gaps of 1.6 (3 and 2.4 px at the 1.5 px outline): two to a side of the diamond,
         * gaps wide enough to read at marker size. */
        pen.setDashPattern({2.0, 1.6});
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
        /* Fill strength reads the same on any tile: over the halo colour, not over the map. A dashed outline stands
         * apart from the fill, a band of halo between them, so its gaps show as gaps: a fill touching it closed them
         * and a fresh unconfirmed marker read as a notched solid one. */
        const QPainterPath body =
            style.dashed_outline ? diamond_path(c, (size_px / 2.0) - outline - (1.25 * ratio)) : shape;
        p.setPen(Qt::NoPen);
        p.fillPath(shape, colors.halo);
        QColor fill = color;
        fill.setAlphaF(fill.alphaF() * static_cast<double>(ms::fill_opacity(style.fill_level)));
        p.fillPath(body, fill);
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
render_label(const QString& text, const QFont& font, const QColor& color, const QColor& halo, double ratio,
             bool plate) {
    const QFontMetricsF fm(font);
    const double w = fm.horizontalAdvance(text) + (2.0 * kLabelHalo);
    const double h = fm.height() + (2.0 * kLabelHalo);
    QImage image(std::max(1, qCeil(w * ratio)), std::max(1, qCeil(h * ratio)), QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(ratio);
    image.fill(Qt::transparent);
    QPainter p(&image);
    p.setRenderHint(QPainter::Antialiasing, true);
    if (plate) {
        /* An emergency's label that has to cover a marker: on a solid plate, so the marker cannot show between the
         * letters. */
        p.setPen(Qt::NoPen);
        p.setBrush(halo);
        p.drawRoundedRect(QRectF(0.0, 0.0, w, h), kLabelHalo, kLabelHalo);
    }
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
    if (images.size() <= static_cast<qsizetype>(keys.size()) + 64) {
        return;
    }
    const QSet<QString> wanted(keys.begin(), keys.end());
    for (auto it = images.begin(); it != images.end();) {
        it = wanted.contains(it.key()) ? std::next(it) : images.erase(it);
    }
}

} // namespace

const QColor&
MapViewItem::labelColorFor(int role) const {
    if (role == 2) {
        return m_alertColor;
    }
    return (role == 1) ? m_selectedColor : m_labelColor;
}

void
MapViewItem::ensureLabelCache(double ratio) {
    if (m_labelBuilt != m_labelSerial || differs(m_labelRatio, ratio)) {
        m_labelImages.clear();
        m_labelBuilt = m_labelSerial;
        m_labelRatio = ratio;
        m_labelVersion++;
    }
}

const QImage&
MapViewItem::labelImage(const TrackDraw& t, double ratio) {
    auto found = m_labelImages.find(t.labelKey);
    if (found == m_labelImages.end()) {
        found = m_labelImages.insert(
            t.labelKey, render_label(t.label, m_labelFont, labelColorFor(t.labelRole), m_haloColor, ratio, false));
    }
    return found.value();
}

QString
MapViewItem::plateLabelKey(const TrackDraw& t, double ratio) {
    const QString key = t.labelKey + QStringLiteral("\nplate");
    if (!m_labelImages.contains(key)) {
        m_labelImages.insert(key,
                             render_label(t.label, m_labelFont, labelColorFor(t.labelRole), m_haloColor, ratio, true));
    }
    return key;
}

void
MapViewItem::prepareLabels() {
    /* On the GUI thread, so the sync only places labels: rasterising two hundred of them there would lengthen the
     * time the GUI thread is blocked. */
    if (window() == nullptr) {
        return;
    }
    const double ratio = window()->effectiveDevicePixelRatio();
    ensureLabelCache(ratio);
    m_labelKeys.clear();
    for (const int i : m_labelOrder) {
        const TrackDraw& t = m_scene[static_cast<size_t>(i)];
        (void)labelImage(t, ratio);
        m_labelKeys.push_back(t.labelKey);
    }
    prune_label_images(m_labelImages, m_labelKeys);
}

MapViewItem::LabelSlot
MapViewItem::labelAnchor(const TrackDraw& t) const {
    /* The label stands by a mark that is on screen: the current position when it shows, else the open ring when
     * that shows (a track can have one in view and the other far off; an emergency's word must stay with what is
     * seen). Neither on screen: the current position, whose label place_labels() then leaves out. The label ranks
     * among equals by that mark's own age. */
    const double half = m_markerSize / 2.0;
    const auto shows = [&](const mm::ScreenPoint& p) {
        return p.x >= -half && p.x <= m_vp.width + half && p.y >= -half && p.y <= m_vp.height + half;
    };
    const mm::ScreenPoint marker = mm::screen_from_world(m_vp, t.marker);
    const mm::ScreenPoint ring = mm::screen_from_world(m_vp, t.ring);
    LabelSlot slot;
    slot.rank = map_view_detail::label_rank(t);
    if (t.hasMarker && (shows(marker) || !t.hasRing || !shows(ring))) {
        slot.at = marker;
        slot.ageS = t.ageS;
    } else {
        slot.at = ring;
        slot.ageS = t.ringAgeS;
    }
    return slot;
}

void
MapViewItem::buildLabels(double ratio) {
    ensureLabelCache(ratio);
    /* What a label may not cover: every marker drawn (its own included: no side reaches it), the device's dot, and
     * the screen's controls and chips. */
    const auto view_w = static_cast<float>(m_vp.width);
    const auto view_h = static_cast<float>(m_vp.height);
    m_labelSpace.reset(view_w, view_h, kLabelGridCell);
    const auto half = static_cast<float>((m_markerSize / 2.0) + 1.0);
    m_labelSpace.marker_half = static_cast<float>(m_markerSize / 2.0);
    for (const MarkerDraw& m : m_markers) {
        const mm::ScreenPoint at = mm::screen_from_world(m_vp, m.at);
        m_labelSpace.markers.add(
            ms::LabelBox{static_cast<float>(at.x) - half, static_cast<float>(at.y) - half, 2.0F * half, 2.0F * half});
    }
    mm::WorldPoint device{};
    if (deviceWorld(&device)) {
        const mm::ScreenPoint at = mm::screen_from_world(m_vp, device);
        m_labelSpace.markers.add(
            ms::LabelBox{static_cast<float>(at.x) - half, static_cast<float>(at.y) - half, 2.0F * half, 2.0F * half});
    }
    for (const ms::LabelBox& b : m_labelObstacles) {
        m_labelSpace.keep_out.add(b);
    }
    /* Where each label stands this frame, and so the age it ranks by: the mark it stands by may not be the one the
     * scene's order was taken from. */
    m_labelSlots.clear();
    for (const int i : m_labelOrder) {
        LabelSlot slot = labelAnchor(m_scene[static_cast<size_t>(i)]);
        slot.scene = i;
        m_labelSlots.push_back(slot);
    }
    std::stable_sort(m_labelSlots.begin(), m_labelSlots.end(), [](const LabelSlot& a, const LabelSlot& b) {
        return a.rank != b.rank ? a.rank > b.rank : map_view_detail::heard_sooner(a.ageS, b.ageS);
    });
    m_labelRequests.clear();
    m_labelKeys.clear();
    for (const LabelSlot& slot : m_labelSlots) {
        const TrackDraw& t = m_scene[static_cast<size_t>(slot.scene)];
        const QSizeF size = labelImage(t, ratio).deviceIndependentSize();
        ms::LabelRequest request{static_cast<float>(slot.at.x), static_cast<float>(slot.at.y),
                                 static_cast<float>(size.width()), static_cast<float>(size.height())};
        request.urgent = t.emergency;
        m_labelRequests.push_back(request);
        m_labelKeys.push_back(t.labelKey);
    }
    m_labelBoxes.resize(m_labelRequests.size());
    m_labelKeep.assign(m_labelRequests.size(), 0U);
    const auto reach = static_cast<float>((m_markerSize / 2.0) + kLabelGap);
    (void)ms::place_labels(m_labelRequests.data(), static_cast<int>(m_labelRequests.size()), reach,
                           ms::LabelBox{0.0F, 0.0F, view_w, view_h}, m_labelSpace, m_labelBoxes.data(),
                           m_labelKeep.data());
    /* Drawn lowest priority first, so where two must overlap (two emergencies) the higher one is on top. */
    m_labels.clear();
    for (size_t i = m_labelRequests.size(); i-- > 0;) {
        if (m_labelKeep[i] != ms::kLabelDropped) {
            const ms::LabelBox& b = m_labelBoxes[i];
            const QString key = (m_labelKeep[i] == ms::kLabelCovering)
                                    ? plateLabelKey(m_scene[static_cast<size_t>(m_labelSlots[i].scene)], ratio)
                                    : m_labelKeys[i];
            m_labels.push_back(LabelDraw{key, b.left, b.top, b.width, b.height});
        }
    }
}

// ------------------------------------------------------- paint: shared ----

std::vector<MapViewItem::TileDraw>
MapViewItem::tileDraws(double ratio) const {
    std::vector<TileDraw> draws;
    /* Stand-ins from another zoom first, under the view's own tiles; the farthest zoom lowest. */
    const int tz = mm::tile_zoom(m_vp.zoom);
    for (auto it = m_tileImages.cbegin(); it != m_tileImages.cend(); ++it) {
        if (!it.value().current) {
            const ms::ScreenRect r = ms::tile_rect(m_vp, it.key().z, it.key().x, it.key().y);
            if (ms::rect_in_view(r, m_vp.width, m_vp.height)) {
                draws.push_back(TileDraw{it.key(), it.value().toned, it.value().serial, ms::snap_rect(r, ratio)});
            }
        }
    }
    std::stable_sort(draws.begin(), draws.end(), [tz](const TileDraw& a, const TileDraw& b) {
        return std::abs(a.key.z - tz) > std::abs(b.key.z - tz);
    });
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

const std::vector<MapViewItem::SpriteQuad>&
MapViewItem::spriteQuads() {
    std::vector<SpriteQuad>& quads = m_quads;
    quads.clear();
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

const std::vector<MapViewItem::Circle>&
MapViewItem::circles() {
    std::vector<Circle>& out = m_circles;
    out.clear();
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
    /* The copies are counted against the camera carried on from the built one the short way round (the way
     * view_transform() moves the content): a camera that wrapped from 0.99 to 0.01 moved 0.02 east, so a point that
     * was west of it is now drawn a world further east, and must be built again. */
    wrapCopies(ms::continued_viewport(root->overlayVp, m_vp), m_copies);
    /* A pan within the margin only moves the trails and circles. While a zoom is in progress (a pinch, the camera)
     * they are scaled as well, up to half a zoom step each way, and built again once it settles: rebuilding 200
     * trails of 100 points for every pinch frame cost a phone about 30 ms a frame. */
    const bool same =
        root->overlaySerial == m_sceneSerial && !differs(root->overlayRatio, ratio) && root->overlayCopies == m_copies;
    const bool fits = m_zooming ? ms::reusable_scaled(root->overlayVp, m_vp, margin, ms::kZoomScaleReach)
                                : ms::reusable(root->overlayVp, m_vp, margin);
    if (same && fits) {
        const ms::ViewTransform t = ms::view_transform(root->overlayVp, m_vp);
        QMatrix4x4 matrix;
        matrix.translate(static_cast<float>(t.dx), static_cast<float>(t.dy));
        matrix.scale(static_cast<float>(t.scale));
        if (root->overlay->matrix() != matrix) {
            root->overlay->setMatrix(matrix);
        }
        return;
    }
    root->overlaySerial = m_sceneSerial;
    root->overlayVp = m_vp;
    root->overlayRatio = ratio;
    wrapCopies(m_vp, m_copies); /* the copies as built, against this very camera */
    root->overlayCopies = m_copies;
    root->overlay->setMatrix(QMatrix4x4());
    const auto m = static_cast<float>(margin);
    const mg::ClipRect clip{-m, -m, static_cast<float>(m_vp.width) + m, static_cast<float>(m_vp.height) + m};
    const auto feather = static_cast<float>(1.0 / ratio);
    m_discs.clear();
    for (const Circle& c : circles()) {
        (void)mg::append_accuracy_circle(static_cast<float>(c.x), static_cast<float>(c.y), static_cast<float>(c.r),
                                         feather, clip, m_discs);
    }
    mg::TrailStyle solid;
    solid.half_width = static_cast<float>(m_trailWidth / 2.0);
    solid.feather = feather;
    mg::TrailStyle dashed = solid;
    dashed.dash = static_cast<float>(m_dashLength);
    dashed.gap = static_cast<float>(m_dashGap);
    m_plainTrails.clear();
    m_chosenTrails.clear();
    forEachTrail([&](const TrackDraw& t, const ms::TrailRuns& runs) {
        std::vector<mg::StrokeVertex>& out = t.selected ? m_chosenTrails : m_plainTrails;
        (void)mg::append_trail(runs.solid_x.data(), runs.solid_y.data(), static_cast<int>(runs.solid_x.size()), solid,
                               &clip, out);
        (void)mg::append_trail(runs.dashed_x.data(), runs.dashed_y.data(), static_cast<int>(runs.dashed_x.size()),
                               dashed, &clip, out);
    });
    const ColorSpan circleSpan{&m_discs, m_accuracyColor};
    set_colored_vertices(root->circles, &circleSpan, 1);
    const ColorSpan trailSpans[2] = {{&m_plainTrails, opaque(m_trailColor)},
                                     {&m_chosenTrails, opaque(m_selectedColor)}};
    set_colored_vertices(root->trails, trailSpans, 2);
}

void
MapViewItem::wrapCopies(const mm::Viewport& vp, std::vector<long long>& out) const {
    out.clear();
    for (const TrackDraw& t : m_scene) {
        if (t.hasMarker) { /* its trail ends there and its circle sits there */
            out.push_back(ms::wrap_copy(vp, t.marker));
        }
    }
    mm::WorldPoint device{};
    if (deviceWorld(&device)) {
        out.push_back(ms::wrap_copy(vp, device));
    }
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
    m_sprites.clear();
    const auto size = static_cast<float>(m_markerSize * kSpriteScale);
    for (const SpriteQuad& q : spriteQuads()) {
        const mg::AtlasCell cell = mg::atlas_cell(q.cell % ms::kAtlasColumns, q.cell / ms::kAtlasColumns, m_atlasCellPx,
                                                  m_atlas.width(), m_atlas.height());
        (void)mg::append_marker_quad(static_cast<float>(q.x), static_cast<float>(q.y), size, static_cast<float>(ratio),
                                     cell, m_sprites);
    }
    set_sprite_vertices(root->sprites, m_sprites);
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
    QSet<QString>& used = m_usedLabels;
    used.clear();
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
        const QColor color = opaque(t.selected ? m_selectedColor : m_trailColor);
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
