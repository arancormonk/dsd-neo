// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Regression test (issue #800): the map view's scene-graph tree, the one a phone draws.
 *
 * Headless Qt has only the software scene graph, which the QML suite renders through (tst_map_view.qml). The RHI
 * tree is what every device draws, so it is built here through the test hook and inspected node by node:
 *
 *  - the layers: grid, tiles, accuracy circles and trails under one transform, marker sprites (one quad each, a
 *    selection halo under the selected track's), labels;
 *  - a pan within the margin moves the transform by exactly the offset the markers move and rebuilds nothing; a
 *    pan past it, a zoom, or new tracks rebuild;
 *  - tiles through the production worker on a canned transport: the view's tiles are asked for, drawn as image
 *    nodes where the tile list puts them, a tile from an older view that is no longer in view is dropped, the
 *    previous zoom's tiles stand in only until the new zoom's have all arrived, and nothing of them stays once
 *    tiles are off;
 *  - a view the device's position placed (Locate me, Follow my position, a fit around the dot) asks for its tiles
 *    memory only for the rest of the visit, and a change of origin alone asks again;
 *  - the sprite atlas is rebuilt when a marker colour changes.
 */

// LLVM 22/GCC 16 misclassifies these runtime test oracles as compile-time assertions.
// NOLINTBEGIN(cert-dcl03-c,misc-static-assert)

#include <QColor>
#include <QCoreApplication>
#include <QEventLoop>
#include <QGuiApplication>
#include <QImage>
#include <QList>
#include <QMatrix4x4>
#include <QObject>
#include <QPointF>
#include <QQuickWindow>
#include <QRectF>
#include <QSGGeometry>
#include <QSGGeometryNode>
#include <QSGImageNode>
#include <QSGMaterial>
#include <QSGTextureMaterial>
#include <QSizeF>
#include <QString>
#include <QThread>
#include <QVariant>
#include <Qt>
#include <QtGlobal>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <qflags.h>
#include <vector>

#include <dsd-neo/core/position.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/runtime/decode_clock.h>

#include "map_math.h"
#include "map_tile_cache.h"
#include "map_tile_worker.h"
#include "map_view_item.h"
#include "position_model.h"
#include "qml_map_stub.h"

class QSGTexture;

namespace stub = dsd_neo_qml_stub;
namespace mm = dsd_qt::map_math;
using dsd_qt::MapTileKey;
using dsd_qt::MapViewItem;
using dsd_qt::PositionModel;

namespace {

/* The tree's layers, in the order GpuRoot appends them. */
struct Layers {
    QSGGeometryNode* grid;
    QSGNode* tiles;
    QSGTransformNode* overlay;
    QSGGeometryNode* circles;
    QSGGeometryNode* trails;
    QSGGeometryNode* sprites;
    QSGNode* labels;
};

Layers
layers(QSGNode* root) {
    assert(root != nullptr && root->type() == QSGNode::BasicNodeType && root->childCount() == 5);
    Layers l{};
    l.grid = static_cast<QSGGeometryNode*>(root->childAtIndex(0));
    l.tiles = root->childAtIndex(1);
    assert(root->childAtIndex(2)->type() == QSGNode::TransformNodeType);
    l.overlay = static_cast<QSGTransformNode*>(root->childAtIndex(2));
    assert(l.overlay->childCount() == 2);
    l.circles = static_cast<QSGGeometryNode*>(l.overlay->childAtIndex(0));
    l.trails = static_cast<QSGGeometryNode*>(l.overlay->childAtIndex(1));
    l.sprites = static_cast<QSGGeometryNode*>(root->childAtIndex(3));
    l.labels = root->childAtIndex(4);
    return l;
}

void
push(uint32_t source, double lat, double lon, uint32_t err_m = 0U, uint16_t flags = 0U, int64_t age_s = 0) {
    dsd_position_fix f;
    dsd_position_fix_init(&f);
    f.protocol = DSD_SYNC_DMR_BS_DATA_POS;
    f.kind = DSD_POS_DMR_LRRP;
    f.source_id = source;
    f.flags = flags;
    f.lat_e7 = static_cast<int32_t>(std::llround(lat * 1.0e7));
    f.lon_e7 = static_cast<int32_t>(std::llround(lon * 1.0e7));
    f.err_m = err_m;
    f.rx_clock = static_cast<uint8_t>(DSD_DECODE_CLOCK_SYSTEM);
    f.rx_time_s = stub::map_wall_seconds() - age_s;
    f.reception_scope.kind = DSD_POS_SCOPE_DMR;
    f.reception_scope.a = 1U;
    f.reception_scope.b = 2U;
    f.sender_scope = f.reception_scope;
    stub::map_publish_fix(f);
}

/* Process events until @p done, at most ~5 s of loop turns (no clock read: the turns are counted). */
template <typename Done>
bool
spin_until(Done done) {
    for (int i = 0; i < 5000; i++) {
        if (done()) {
            return true;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 1);
        QCoreApplication::sendPostedEvents();
        QThread::msleep(1);
    }
    return done();
}

struct Fixture {
    QQuickWindow window;
    PositionModel model;
    MapViewItem* map;

    Fixture() : map(nullptr) {
        stub::map_reset_fixes();
        model.setSourceForTest(&stub::map_position_source);
        model.setWallClockForTest(&stub::map_wall_seconds);
        window.resize(400, 300);
        map = new MapViewItem(window.contentItem());
        map->setSize(QSizeF(400.0, 300.0));
        map->setCameraDurationMs(0);
        map->setModel(&model);
        window.show();
        map->centerOn(10.0, 20.0, 12.0);
    }

    ~Fixture() {
        delete map;
        model.clear();
    }

    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;
};

/* The layers carry what the model holds: a trail, a circle, one quad per marker, a halo under the selected one. */
void
test_layers() {
    Fixture f;
    QSGNode* root = f.map->testUpdateGpuTree(nullptr, 2.0);
    Layers l = layers(root);
    assert(l.grid->geometry()->vertexCount() > 0); /* the plain grid while tiles are off */
    assert(l.tiles->childCount() == 0);
    assert(l.trails->geometry()->vertexCount() == 0 && l.sprites->geometry()->vertexCount() == 0);

    push(1001U, 10.0, 19.99);
    push(1001U, 10.0, 20.01, 500U);
    push(1002U, 10.01, 20.0, 0U, DSD_POS_FLAG_VALIDITY_UNKNOWN);
    f.model.poll();
    root = f.map->testUpdateGpuTree(root, 2.0);
    l = layers(root);
    assert(f.map->markerCount() == 2);
    assert(l.trails->geometry()->vertexCount() > 0);
    assert(l.circles->geometry()->vertexCount() > 0);
    assert(l.sprites->geometry()->vertexCount() == 12);
    assert(l.sprites->geometry()->attributeCount() == 2); /* position and texture coordinate */
    auto* material = static_cast<QSGTextureMaterial*>(l.sprites->material());
    assert(material->texture() != nullptr && (material->flags() & QSGMaterial::Blending) != 0);
    assert(l.labels->childCount() == 2);

    /* Every sprite corner on the 2x device pixel grid. */
    const auto* v = l.sprites->geometry()->vertexDataAsTexturedPoint2D();
    for (int i = 0; i < l.sprites->geometry()->vertexCount(); i++) {
        assert(std::fabs((v[i].x * 2.0F) - std::round(v[i].x * 2.0F)) < 1.0e-3F);
    }

    f.map->setSelectedTrackId(f.map->trackAt(200.0 + 29.0, 150.0));
    assert(f.map->selectedTrackId() != 0U);
    root = f.map->testUpdateGpuTree(root, 2.0);
    l = layers(root);
    assert(l.sprites->geometry()->vertexCount() == 18);

    /* The atlas follows the marker colour. */
    const QSGTexture* before = material->texture();
    root = f.map->testUpdateGpuTree(root, 2.0);
    {
        const Layers now = layers(root);
        assert(static_cast<QSGTextureMaterial*>(now.sprites->material())->texture() == before);
    }
    f.map->setMarkerColor(QColor(10, 20, 30));
    root = f.map->testUpdateGpuTree(root, 2.0);
    {
        const Layers now = layers(root);
        assert(static_cast<QSGTextureMaterial*>(now.sprites->material())->texture() != before);
    }
    delete root;
}

/* A trail never crosses a break: a report on a new run starts the trail again, with no line back to the old run. */
void
test_break_is_not_bridged() {
    Fixture f;
    push(1004U, 10.0, 19.99);
    f.model.poll();
    stub::map_reset_fixes(); /* a new run */
    push(1004U, 10.0, 20.01);
    f.model.poll();
    assert(f.model.count() == 1 && f.map->markerCount() == 1);
    QSGNode* root = f.map->testUpdateGpuTree(nullptr, 1.0);
    {
        const Layers now = layers(root);
        assert(now.trails->geometry()->vertexCount() == 0);
    }
    push(1004U, 10.0, 20.02);
    f.model.poll();
    root = f.map->testUpdateGpuTree(root, 1.0);
    const QSGGeometry* g = layers(root).trails->geometry();
    assert(g->vertexCount() > 0);
    /* Only the new run's segment: nothing west of its first point (x = 229 at this zoom). */
    for (int i = 0; i < g->vertexCount(); i++) {
        assert(g->vertexDataAsColoredPoint2D()[i].x > 220.0F);
    }
    delete root;
}

/* A pan within the margin moves the overlay by the markers' offset and rebuilds nothing; past it, or once a zoom has
 * settled, it is built again. */
void
test_pan_moves_the_overlay() {
    Fixture f;
    push(1003U, 10.0, 19.99);
    push(1003U, 10.0, 20.01, 300U);
    f.model.poll();
    QSGNode* root = f.map->testUpdateGpuTree(nullptr, 1.0);
    Layers l = layers(root);
    const int trailVertices = l.trails->geometry()->vertexCount();
    const float firstX = l.trails->geometry()->vertexDataAsColoredPoint2D()[0].x;
    assert(trailVertices > 0 && l.overlay->matrix().isIdentity());

    const QVariant before = f.map->markerPoint(f.map->trackAt(229.0, 150.0));
    f.map->panBy(-41.5, 17.25);
    root = f.map->testUpdateGpuTree(root, 1.0);
    l = layers(root);
    const QPointF moved = f.map->markerPoint(f.map->trackAt(before.toPointF().x() - 41.5, 167.25)).toPointF();
    const QPointF shift = moved - before.toPointF();
    assert(std::fabs(shift.x() + 41.5) < 1.0e-6 && std::fabs(shift.y() - 17.25) < 1.0e-6);
    const QMatrix4x4 m = l.overlay->matrix();
    assert(std::fabs(m(0, 3) - static_cast<float>(shift.x())) < 1.0e-3F);
    assert(std::fabs(m(1, 3) - static_cast<float>(shift.y())) < 1.0e-3F);
    assert(l.trails->geometry()->vertexCount() == trailVertices);
    assert(std::fabs(l.trails->geometry()->vertexDataAsColoredPoint2D()[0].x - firstX) < 1.0e-6F);

    f.map->panBy(-300.0, 0.0); /* past half the view's larger side */
    root = f.map->testUpdateGpuTree(root, 1.0);
    l = layers(root);
    assert(l.overlay->matrix().isIdentity());

    /* A zoom in progress (a pinch frame) only scales the overlay: the trails are not built again. */
    f.map->testSettleZoom();
    root = f.map->testUpdateGpuTree(root, 1.0);
    const int settledVertices = layers(root).trails->geometry()->vertexCount();
    const float settledX = layers(root).trails->geometry()->vertexDataAsColoredPoint2D()[0].x;
    const QPointF built(settledX, layers(root).trails->geometry()->vertexDataAsColoredPoint2D()[0].y);
    f.map->zoomAbout(f.map->zoom() + 0.25, 10.0, 10.0);
    assert(f.map->testZooming());
    root = f.map->testUpdateGpuTree(root, 1.0);
    {
        const Layers now = layers(root);
        const QMatrix4x4 scaled = now.overlay->matrix();
        assert(std::fabs(scaled(0, 0) - std::exp2(0.25F)) < 1.0e-5F
               && std::fabs(scaled(1, 1) - std::exp2(0.25F)) < 1.0e-5F);
        assert(now.trails->geometry()->vertexCount() == settledVertices);
        assert(std::fabs(now.trails->geometry()->vertexDataAsColoredPoint2D()[0].x - settledX) < 1.0e-6F);
        /* The scaled overlay lands where the zoom about (10, 10) puts every point. */
        const QPointF mapped = scaled.map(built);
        const double k = std::exp2(0.25);
        assert(std::fabs(mapped.x() - (10.0 + ((built.x() - 10.0) * k))) < 1.0e-3);
        assert(std::fabs(mapped.y() - (10.0 + ((built.y() - 10.0) * k))) < 1.0e-3);
    }
    /* Once the zoom holds still it is built again at its own scale. */
    f.map->testSettleZoom();
    assert(!f.map->testZooming());
    root = f.map->testUpdateGpuTree(root, 1.0);
    {
        const Layers now = layers(root);
        assert(now.overlay->matrix().isIdentity());
    }
    /* More than half a zoom step from what was built: built again at once, zooming or not. */
    f.map->zoomAbout(f.map->zoom() + 0.75, 10.0, 10.0);
    root = f.map->testUpdateGpuTree(root, 1.0);
    {
        const Layers now = layers(root);
        assert(now.overlay->matrix().isIdentity());
    }
    f.map->testSettleZoom();

    /* A new report rebuilds even without a pan. */
    f.map->panBy(5.0, 0.0);
    root = f.map->testUpdateGpuTree(root, 1.0);
    {
        const Layers now = layers(root);
        assert(!now.overlay->matrix().isIdentity());
    }
    push(1003U, 10.0, 20.02);
    f.model.poll();
    root = f.map->testUpdateGpuTree(root, 1.0);
    {
        const Layers now = layers(root);
        assert(now.overlay->matrix().isIdentity());
    }
    delete root;
}

/* Tiles through the production worker on the canned transport. */
void
test_tiles() {
    Fixture f;
    stub::MapTileSourceStub tiles;
    f.map->setTileSource(&tiles);
    f.map->testPolish();
    assert(f.map->testRequestedTiles().isEmpty() && stub::map_tile_requests() == 0); /* off: nothing asked */

    tiles.setEnabled(true);
    f.map->testPolish();
    const QList<MapTileKey> asked = f.map->testRequestedTiles();
    const std::vector<dsd_qt::map_math::VisibleTile> visible =
        dsd_qt::map_math::visible_tiles(dsd_qt::map_math::clamp_viewport(
            dsd_qt::map_math::Viewport{dsd_qt::map_math::world_from_lat_lon(10.0, 20.0).x,
                                       dsd_qt::map_math::world_from_lat_lon(10.0, 20.0).y, 12.0, 400.0, 300.0}));
    assert(asked.size() == static_cast<int>(visible.size()) && !asked.isEmpty());
    assert(asked.front() == (MapTileKey{visible.front().z, visible.front().x, visible.front().y}));
    assert(spin_until([&] { return f.map->testHeldTiles() == asked.size(); }));

    QSGNode* root = f.map->testUpdateGpuTree(nullptr, 1.0);
    Layers l = layers(root);
    assert(l.tiles->childCount() == asked.size());
    auto* first = static_cast<QSGImageNode*>(l.tiles->firstChild());
    assert(first->texture() != nullptr);
    /* Drawn where the tile list puts it (snapped to whole pixels at ratio 1). */
    bool found = false;
    for (const auto& t : visible) {
        found = found
                || (std::fabs(first->rect().left() - std::round(t.left)) < 1.0e-6
                    && std::fabs(first->rect().top() - std::round(t.top)) < 1.0e-6);
    }
    assert(found);

    /* A late tile for a view the map has left is not drawn. */
    const MapTileKey gone{12, 0, 0};
    f.map->testDeliverTile(gone.z, gone.x, gone.y, QImage(256, 256, QImage::Format_ARGB32_Premultiplied));
    assert(f.map->testHeldTiles() == asked.size());

    /* Another tile source: nothing of the old one stays on screen, at once. */
    tiles.setUrlTemplate(QStringLiteral("https://tiles.example.invalid/{z}/{x}/{y}.png"));
    assert(f.map->testHeldTiles() == 0 && f.map->testRequestedTiles().isEmpty());
    root = f.map->testUpdateGpuTree(root, 1.0);
    {
        const Layers now = layers(root);
        assert(now.tiles->childCount() == 0);
    }
    f.map->testPolish();
    assert(spin_until([&] { return f.map->testHeldTiles() == asked.size(); }));

    /* A new zoom: the old tiles stand in until the new zoom's have all arrived, then go. */
    tiles.setEnabled(false);
    tiles.setEnabled(true); /* settle: nothing in flight */
    f.map->testPolish();
    assert(spin_until([&] { return f.map->testHeldTiles() == asked.size(); }));
    /* The canned tiles are no-store, which may never stand in (PR2 r1): put reusable copies in their place. */
    {
        QImage reusable(256, 256, QImage::Format_ARGB32_Premultiplied);
        reusable.fill(Qt::white);
        for (const MapTileKey& key : asked) {
            f.map->testDeliverTile(key.z, key.x, key.y, reusable, true);
        }
    }
    f.map->setZoom(13.0);
    f.map->testPolish();
    const int standing = f.map->testHeldTiles();
    assert(standing > 0);
    root = f.map->testUpdateGpuTree(root, 1.0);
    {
        const Layers now = layers(root);
        assert(now.tiles->childCount() >= standing);
    }
    const QList<MapTileKey> zoomed = f.map->testRequestedTiles();
    assert(!zoomed.isEmpty() && zoomed.front().z == 13);
    assert(spin_until([&] {
        f.map->testPolish();
        return f.map->testHeldTiles() == zoomed.size();
    }));

    /* Tiles off: nothing of them stays, and the textures go with the next sync. */
    tiles.setEnabled(false);
    f.map->testPolish();
    assert(f.map->testHeldTiles() == 0);
    root = f.map->testUpdateGpuTree(root, 1.0);
    {
        const Layers now = layers(root);
        assert(now.tiles->childCount() == 0);
    }

    /* Inactive (the List shown): no request, even when on. */
    const int before = stub::map_tile_requests();
    f.map->setActive(false);
    tiles.setEnabled(true);
    f.map->panBy(120.0, 0.0);
    f.map->testPolish();
    assert(f.map->testRequestedTiles().isEmpty());
    for (int i = 0; i < 50; i++) {
        QCoreApplication::processEvents();
    }
    assert(stub::map_tile_requests() == before);
    f.map->setTileSource(nullptr);
    delete root;
}

/* Backgrounding stops loading at once, from the state change itself: a hidden window is never polished, so a stop
 * left to the next polish would let the worker go on fetching the last view. Coming back asks again. */
void
test_background_stops_loading() {
    Fixture f;
    stub::MapTileSourceStub tiles;
    f.map->setTileSource(&tiles);
    tiles.setEnabled(true);
    f.map->testPolish();
    assert(!f.map->testRequestedTiles().isEmpty());
    assert(spin_until([&] { return f.map->testHeldTiles() > 0; }));
    /* Read through a function: the generation moves inside the call below, not in this scope. */
    const auto generation_now = [&tiles] { return tiles.generation(); };
    const quint64 generation = generation_now();

    f.map->testApplicationState(Qt::ApplicationSuspended); /* no polish follows */
    assert(f.map->testRequestedTiles().isEmpty());
    const quint64 cleared = generation_now();
    assert(cleared > generation); /* the worker's requests were cleared */
    assert(f.map->testHeldTiles() == 0);
    const int asked = stub::map_tile_requests();
    for (int i = 0; i < 50; i++) {
        QCoreApplication::processEvents();
    }
    assert(stub::map_tile_requests() == asked);

    /* A window that only lost focus keeps loading. */
    f.map->testApplicationState(Qt::ApplicationActive);
    f.map->testPolish();
    assert(!f.map->testRequestedTiles().isEmpty());
    f.map->testApplicationState(Qt::ApplicationInactive);
    assert(!f.map->testRequestedTiles().isEmpty());

    /* Leaving the window, or being hidden, stops it the same way. */
    f.map->setVisible(false);
    assert(f.map->testRequestedTiles().isEmpty() && f.map->testHeldTiles() == 0);
    f.map->setVisible(true);
    f.map->testPolish();
    assert(!f.map->testRequestedTiles().isEmpty());
    f.map->setParentItem(nullptr);
    assert(f.map->testRequestedTiles().isEmpty());
    f.map->setParentItem(f.window.contentItem());
    f.map->setTileSource(nullptr);
}

/* PR2 r1: the canvas's held tiles follow the worker's word. A withdrawn key goes at once; an image the cache rules
 * forbid reusing ends with its presentation (no stand-in, never brought back); the same image again keeps its toned
 * copy and texture. */
void
test_held_tiles_follow_the_cache_rules() {
    Fixture f;
    stub::MapTileSourceStub tiles;
    f.map->setTileSource(&tiles);
    tiles.setEnabled(true);
    f.map->testPolish();
    const QList<MapTileKey> asked = f.map->testRequestedTiles();
    assert(!asked.isEmpty());
    const MapTileKey k = asked.front();
    QImage image(256, 256, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::white);

    /* The same image again: no re-tone, no new texture. */
    f.map->testDeliverTile(k.z, k.x, k.y, image);
    const quint64 serial = f.map->testTileSerial(k.z, k.x, k.y);
    assert(serial != 0U);
    f.map->testDeliverTile(k.z, k.x, k.y, image);
    assert(f.map->testTileSerial(k.z, k.x, k.y) == serial);
    QImage other = image.copy();
    f.map->testDeliverTile(k.z, k.x, k.y, other);
    assert(f.map->testTileSerial(k.z, k.x, k.y) != serial); /* a new image is a new texture */

    /* Withdrawn: gone at once, even while in view. */
    f.map->testDeliverTile(k.z, k.x, k.y, QImage());
    assert(f.map->testTileSerial(k.z, k.x, k.y) == 0U);

    /* Not reusable: it does not stand in for the next zoom, so zooming back cannot bring it back. At zoom 13 only
     * the centre tile of zoom 12 is still in view, so it is the one stand-in there could be. */
    f.map->setTileSource(nullptr);
    for (const bool centreReusable : {true, false}) {
        stub::MapTileSourceStub quiet;
        f.map->setZoom(12.0);
        f.map->setTileSource(&quiet);
        quiet.setEnabled(true);
        f.map->testPolish();
        const QList<MapTileKey> view = f.map->testRequestedTiles();
        assert(!view.isEmpty() && view.front().z == 12);
        for (const MapTileKey& key : view) {
            f.map->testDeliverTile(key.z, key.x, key.y, image, key == view.front() ? centreReusable : true);
        }
        const MapTileKey centre = view.front();
        f.map->setZoom(13.0);
        f.map->testPolish(); /* the new zoom's tiles have not arrived */
        if (centreReusable) {
            assert(f.map->testHeldTiles() == 1 && f.map->testTileSerial(centre.z, centre.x, centre.y) != 0U);
            /* PR2 r2: once every child the server answered is a definitive miss, the parent stops standing in: the
             * missing tiles stay blank rather than showing enlarged parent imagery for good. */
            const QList<MapTileKey> children = f.map->testRequestedTiles();
            for (int i = 0; i + 1 < children.size(); ++i) {
                f.map->testDeliverTile(children[i].z, children[i].x, children[i].y, QImage());
                f.map->testPolish();
                assert(f.map->testTileSerial(centre.z, centre.x, centre.y) != 0U); /* one child still loading */
            }
            const MapTileKey last = children.back();
            f.map->testDeliverTile(last.z, last.x, last.y, QImage());
            f.map->testPolish();
            assert(f.map->testHeldTiles() == 0);
        } else {
            assert(f.map->testHeldTiles() == 0);
        }
        f.map->setTileSource(nullptr);
    }
}

/* Final review I1: every tile request of a view the device's position placed is memory only (Locate me, Follow my
 * position, a fit that took in the dot), for the rest of the item's life (the app's run), even after a pan or a move
 * elsewhere, since the screen keeps the view between visits; a new item asks by hand. */
void
test_device_views_ask_memory_only() {
    Fixture f;
    stub::MapTileSourceStub tiles;
    f.map->setTileSource(&tiles);
    tiles.setEnabled(true);
    f.map->testPolish();
    assert(!f.map->testRequestedTiles().isEmpty() && !tiles.memoryOnlyForTest()); /* placed by hand */

    f.map->setDeviceLatitude(10.01);
    f.map->setDeviceLongitude(20.01);
    f.map->setDeviceValid(true);
    f.map->testPolish();
    assert(!tiles.memoryOnlyForTest()); /* the dot alone does not move the camera */

    const bool centred = f.map->centerOnDevice();
    assert(centred);
    f.map->testPolish();
    assert(f.map->cameraUsedDevicePosition() && tiles.memoryOnlyForTest());
    f.map->panBy(40.0, 0.0);
    f.map->testPolish();
    assert(tiles.memoryOnlyForTest()); /* still near the device */

    f.map->centerOn(-40.0, 120.0, 6.0);
    f.map->testPolish();
    assert(f.map->cameraUsedDevicePosition() && tiles.memoryOnlyForTest()); /* for the item's life: the app's run */
    f.map->setTileSource(nullptr);

    /* A new item (the next run) asks by hand; following asks from the device before any fix moves the camera. */
    Fixture g;
    stub::MapTileSourceStub fresh;
    g.map->setTileSource(&fresh);
    fresh.setEnabled(true);
    g.map->testPolish();
    assert(!g.map->cameraUsedDevicePosition() && !fresh.memoryOnlyForTest());
    g.map->setFollowDevice(true); /* no fix yet: the camera does not move, the request is still memory only */
    g.map->testPolish();
    assert(fresh.memoryOnlyForTest());
    g.map->setFollowDevice(false);
    g.map->testPolish();
    assert(!fresh.memoryOnlyForTest());

    g.map->setDeviceLatitude(10.01);
    g.map->setDeviceLongitude(20.01);
    g.map->setDeviceValid(true);
    const bool fitted = g.map->fitAll(); /* the fit takes in the dot */
    assert(fitted);
    g.map->testPolish();
    assert(g.map->cameraUsedDevicePosition() && fresh.memoryOnlyForTest());
    g.map->setTileSource(nullptr);
}

/* A pan that moves a trail-bearing marker across the flip line (half a world from the centre) rebuilds the overlay:
 * the marker jumps a world width, and its trail and circle must jump with it rather than stay on the old copy. */
void
test_overlay_follows_the_wrap_copy() {
    Fixture f;
    f.map->centerOn(10.0, 0.0, 2.0); /* world 1024 px: half a world is 512 px, the overlay margin 200 px */
    push(1005U, 10.0, 176.0);
    push(1005U, 10.0, 176.4, 200000U); /* 0.49 world east of the centre: just inside the flip line */
    f.model.poll();
    QSGNode* root = f.map->testUpdateGpuTree(nullptr, 1.0);
    {
        const Layers now = layers(root);
        assert(now.overlay->matrix().isIdentity());
    }
    f.map->panBy(5.0, 0.0); /* stays on its copy: reused */
    root = f.map->testUpdateGpuTree(root, 1.0);
    {
        const Layers now = layers(root);
        assert(!now.overlay->matrix().isIdentity());
    }
    f.map->panBy(25.0, 0.0); /* crosses: the marker is now drawn a world to the west */
    root = f.map->testUpdateGpuTree(root, 1.0);
    {
        const Layers now = layers(root);
        assert(now.overlay->matrix().isIdentity());
    }
    delete root;
}

/* Label images are rendered when the scene is rebuilt, on the GUI thread, not inside the sync. */
void
test_labels_render_on_the_gui_thread() {
    Fixture f;
    assert(f.map->testLabelImages() == 0);
    push(1006U, 10.0, 20.0);
    f.model.poll();
    assert(f.map->testLabelImages() == 1);
}

} // namespace

namespace {

/* The label on the right of a marker at the view's centre, by its width (the names differ in length); 0 for none. */
double
right_label_width(QSGNode* root, double centre_x) {
    const Layers l = layers(root);
    for (QSGNode* n = l.labels->firstChild(); n != nullptr; n = n->nextSibling()) {
        const QRectF r = static_cast<QSGImageNode*>(n)->rect();
        if (r.left() > centre_x) {
            return r.width();
        }
    }
    return 0.0;
}

/* Two radios at one place want the same side for their labels: the selected one keeps it, else the one heard most
 * recently; the other takes another side. */
void
test_label_priority() {
    Fixture f;
    const double cx = f.map->width() / 2.0;
    push(7U, 10.0, 20.0, 0U, 0U, 30);           /* label "7": heard 30 s ago */
    push(1234567890U, 10.0, 20.0, 0U, 0U, 600); /* label "1234567890": heard 10 min ago */
    f.model.poll();
    QSGNode* root = f.map->testUpdateGpuTree(nullptr, 1.0);
    {
        const Layers now = layers(root);
        assert(now.labels->childCount() == 2);
    }
    const double newest = right_label_width(root, cx);
    assert(newest > 0.0);
    /* Drawn lowest priority first, so where two must overlap the higher one is on top: the newest is the last node. */
    {
        const Layers now = layers(root);
        const auto* last = static_cast<QSGImageNode*>(now.labels->lastChild());
        assert(std::fabs(last->rect().width() - newest) < 0.5);
    }
    push(7U, 10.0, 20.0, 0U, 0U, 900); /* now the other one is the more recent */
    f.model.poll();
    root = f.map->testUpdateGpuTree(root, 1.0);
    const double other = right_label_width(root, cx);
    assert(other > newest); /* the long name took the right */
    /* Selecting the short one gives it the right back. */
    for (const PositionModel::Row& row : f.model.rows()) {
        if (row.current.source_id == 7U) {
            f.map->setSelectedTrackId(row.id);
        }
    }
    root = f.map->testUpdateGpuTree(root, 1.0);
    assert(std::fabs(right_label_width(root, cx) - newest) < 0.5);
    delete root;
}

/* The settle timer itself: after a zoom step the item reports a zoom in progress, and once the zoom has held still
 * (the real single-shot timer, run by the event loop) it is over and the overlay is built at its own scale. */
void
test_zoom_settles_by_itself() {
    Fixture f;
    push(1007U, 10.0, 19.99);
    push(1007U, 10.0, 20.01, 300U);
    f.model.poll();
    assert(spin_until([&] { return !f.map->testZooming(); })); /* the fixture's own centring settles too */
    QSGNode* root = f.map->testUpdateGpuTree(nullptr, 1.0);
    f.map->zoomAbout(f.map->zoom() + 0.2, 200.0, 150.0);
    assert(f.map->testZooming());
    root = f.map->testUpdateGpuTree(root, 1.0);
    {
        const Layers now = layers(root);
        assert(!now.overlay->matrix().isIdentity());
    }
    assert(spin_until([&] { return !f.map->testZooming(); }));
    root = f.map->testUpdateGpuTree(root, 1.0);
    {
        const Layers now = layers(root);
        assert(now.overlay->matrix().isIdentity());
    }
    delete root;
}

/* An emergency's label that has to cover a marker is drawn on a solid plate: the covered marker does not show between
 * its letters. */
void
test_covering_label_has_a_plate() {
    Fixture f;
    const double px = 360.0 / (256.0 * 4096.0); /* degrees of longitude per pixel at zoom 12 */
    const double py = px * std::cos(10.0 * 3.14159265358979323846 / 180.0);
    push(2001U, 10.0, 20.0, 0U, DSD_POS_FLAG_EMERGENCY);
    const double around_x[4] = {30.0, -30.0, 0.0, 0.0};
    const double around_y[4] = {0.0, 0.0, -24.0, 24.0};
    for (int k = 0; k < 4; k++) {
        push(0U, 10.0 - (around_y[k] * py), 20.0 + (around_x[k] * px), 0U, DSD_POS_FLAG_SOURCE_UNVERIFIED);
    }
    f.model.poll();
    assert(f.map->markerCount() == 5);
    QSGNode* root = f.map->testUpdateGpuTree(nullptr, 1.0);
    QRectF label;
    {
        const Layers now = layers(root);
        assert(now.labels->childCount() == 1);
        label = static_cast<QSGImageNode*>(now.labels->firstChild())->rect();
    }
    /* It does cover a neighbour's marker (precondition)... */
    bool covers = false;
    for (int k = 0; k < 4; k++) {
        const QRectF marker(200.0 + around_x[k] - 8.0, 150.0 + around_y[k] - 8.0, 16.0, 16.0);
        covers = covers || label.intersects(marker);
    }
    assert(covers);
    /* ...and no pixel of it shows the marker colour (#E6EAF2): plate, alert-coloured letters and their halo only. */
    const QImage image = f.map->testSoftwareImage(1.0);
    int marker_coloured = 0;
    for (int y = static_cast<int>(label.top()) + 1; y < static_cast<int>(label.bottom()) - 1; y++) {
        for (int x = static_cast<int>(label.left()) + 1; x < static_cast<int>(label.right()) - 1; x++) {
            const QColor c = image.pixelColor(x, y);
            marker_coloured += (c.red() > 200 && c.green() > 200 && c.blue() > 200) ? 1 : 0;
        }
    }
    assert(marker_coloured == 0);
    delete root;
}

/* PR2 review: while the canvas is hidden (the List shown, the map closed with its screen kept) the model goes on, but
 * the scene is not rebuilt for each batch; it is built once when the canvas shows again. */
void
test_hidden_canvas_defers_the_scene() {
    Fixture f;
    f.map->setActive(false);
    const quint64 before = f.map->testSceneRebuilds();
    push(3001U, 10.0, 20.0);
    f.model.poll();
    push(3001U, 10.0, 20.001);
    f.model.poll();
    f.model.refreshAges();
    assert(f.map->testSceneRebuilds() == before && f.map->markerCount() == 0);
    f.map->setActive(true);
    assert(f.map->testSceneRebuilds() == before + 1U && f.map->markerCount() == 1);
    /* Hidden by visibility too, and a camera call while hidden builds what it needs. */
    f.map->setVisible(false);
    push(3002U, 10.01, 20.0);
    f.model.poll();
    assert(f.map->markerCount() == 1);
    const bool fitted = f.map->fitAll();
    assert(fitted && f.map->markerCount() == 2);
    f.map->setVisible(true);
}

/* PR2 review: a followed radio sits below the status chips (topInset); zooming (the buttons, a pinch) keeps it there
 * rather than about the geometric centre, which walked it down the screen. */
void
test_follow_zoom_keeps_the_point() {
    Fixture f;
    f.map->setTopInset(52.0);
    push(3003U, 10.0, 20.0);
    f.model.poll();
    const qulonglong id = f.map->trackAt(200.0, 150.0);
    assert(id != 0U);
    f.map->setFollowTrackId(id);
    const QPointF at = f.map->markerPoint(id).toPointF();
    assert(std::fabs(at.x() - 200.0) < 1.0e-6 && std::fabs(at.y() - (150.0 + 26.0)) < 1.0e-6);
    f.map->zoomIn();
    QPointF now = f.map->markerPoint(id).toPointF();
    assert(std::fabs(now.x() - at.x()) < 1.0e-6 && std::fabs(now.y() - at.y()) < 1.0e-6);
    f.map->zoomOut();
    f.map->zoomOut();
    now = f.map->markerPoint(id).toPointF();
    assert(std::fabs(now.y() - at.y()) < 1.0e-6);
    f.map->zoomAbout(f.map->zoom() + 0.7, 0.0, 0.0);
    now = f.map->markerPoint(id).toPointF();
    assert(std::fabs(now.x() - at.x()) < 1.0e-6 && std::fabs(now.y() - at.y()) < 1.0e-6);
    assert(f.map->followTrackId() == id);
}

/* PR2 review: an error that is only a lower bound ("more than 200 km") draws no accuracy disc: a disc would say the
 * radio is within it. */
void
test_lower_bound_error_has_no_disc() {
    Fixture f;
    push(3004U, 10.0, 20.0, 500U, DSD_POS_FLAG_ERR_LOWER_BOUND);
    f.model.poll();
    QSGNode* root = f.map->testUpdateGpuTree(nullptr, 1.0);
    {
        const Layers now = layers(root);
        assert(f.map->markerCount() == 1 && now.circles->geometry()->vertexCount() == 0);
    }
    push(3005U, 10.0, 20.0, 500U);
    f.model.poll();
    root = f.map->testUpdateGpuTree(root, 1.0);
    {
        const Layers now = layers(root);
        assert(now.circles->geometry()->vertexCount() > 0);
    }
    delete root;
}

/* PR2 review: a radio whose current fix is far off but whose newer emergency report (position validity unknown) is in
 * view: the label, with its word, stands by the ring that is seen. */
void
test_label_by_the_visible_ring() {
    Fixture f;
    push(3006U, 10.0, 25.0);                                                             /* far east of the view */
    push(3006U, 10.0, 20.0, 0U, DSD_POS_FLAG_VALIDITY_UNKNOWN | DSD_POS_FLAG_EMERGENCY); /* at the centre */
    f.model.poll();
    QSGNode* root = f.map->testUpdateGpuTree(nullptr, 1.0);
    {
        const Layers now = layers(root);
        assert(now.labels->childCount() == 1);
        const QRectF label = static_cast<QSGImageNode*>(now.labels->firstChild())->rect();
        assert(label.intersects(QRectF(150.0, 120.0, 100.0, 60.0))); /* beside the centre */
    }
    delete root;
}

/* PR2 review: a camera that wraps across its own date line (0.99 to 0.01, a short step east) moves every point on the
 * near side a world further; the overlay is built again rather than translated off its markers. */
void
test_overlay_follows_a_wrapping_camera() {
    Fixture f;
    f.map->setZoom(0.0); /* the floor: the world just fills the 400 px view */
    push(3007U, 0.0, 0.0, 2000000U);
    f.model.poll();
    f.map->setCenterLongitude(176.4); /* world x 0.99 */
    assert(spin_until([&] { return !f.map->testZooming(); }));
    QSGNode* root = f.map->testUpdateGpuTree(nullptr, 1.0);
    f.map->setCenterLongitude(-176.4); /* world x 0.01: 0.02 east */
    root = f.map->testUpdateGpuTree(root, 1.0);
    {
        const Layers now = layers(root);
        assert(now.overlay->matrix().isIdentity());
    }
    delete root;
}

/* PR2 review 2: the camera tracks the followed track's drawn mark, whatever changes it. The age filter hides the
 * current position and leaves the newer ring: the camera moves to the ring. */
void
test_follow_moves_when_the_filter_hides_the_marker() {
    Fixture f;
    push(4001U, 10.0, 21.0, 0U, 0U, 2400);                           /* 40 minutes old, far east */
    push(4001U, 10.0, 20.0, 0U, DSD_POS_FLAG_VALIDITY_UNKNOWN, 300); /* the ring, 5 minutes old */
    f.model.poll();
    const QVector<PositionModel::Row>& rows = f.model.rows();
    const auto track = std::find_if(rows.cbegin(), rows.cend(), [](const PositionModel::Row& r) { return !r.gap; });
    assert(track != rows.cend());
    const qulonglong id = track->id;
    f.map->setFollowTrackId(id);
    QPointF at = f.map->markerPoint(id).toPointF();
    assert(std::fabs(at.x() - 200.0) < 1.0e-6 && std::fabs(at.y() - 150.0) < 1.0e-6); /* the current position */
    f.model.setAgeFilterSeconds(900);                                                 /* hides it */
    at = f.map->markerPoint(id).toPointF();
    assert(std::fabs(at.x() - 200.0) < 1.0e-6 && std::fabs(at.y() - 150.0) < 1.0e-6); /* now the ring, centred */
    assert(f.map->followTrackId() == id);
    f.model.setAgeFilterSeconds(0);
}

/* PR2 review 2, independent review I2: the chips above the canvas (the Following chip laying out, tile trouble coming
 * and going) never move the camera. A camera that followed them would change the tiles in view, and the tile chip
 * with them: modelled here as a chip that toggles whenever the camera moves, which with a camera that re-anchors on
 * the inset bounces for as long as it is allowed to. A followed mark stays where it was centred, zooms keep it
 * there, and its next step is centred below the chips as they are then. */
void
test_chips_never_move_the_camera() {
    Fixture f;
    push(4002U, 10.0, 20.0);
    f.model.poll();
    const qulonglong id = f.map->trackAt(200.0, 150.0);
    f.map->setFollowTrackId(id);
    int moves = 0;
    QObject::connect(f.map, &MapViewItem::viewportChanged, f.map, [&f, &moves] {
        if (++moves < 20) {
            f.map->setTopInset(f.map->topInset() > 0.0 ? 0.0 : 52.0); /* the tile chip, after the view changed */
        }
    });
    for (int flip = 0; flip < 6; flip++) {
        f.map->setTopInset(flip % 2 == 0 ? 52.0 : 0.0);
    }
    assert(moves == 0);
    QObject::disconnect(f.map, &MapViewItem::viewportChanged, f.map, nullptr);
    f.map->setTopInset(52.0);
    QPointF at = f.map->markerPoint(id).toPointF();
    assert(std::fabs(at.x() - 200.0) < 1.0e-6 && std::fabs(at.y() - 150.0) < 1.0e-6); /* where it was centred */
    for (int i = 0; i < 5; i++) {
        f.map->zoomIn();
    }
    at = f.map->markerPoint(id).toPointF();
    assert(std::fabs(at.x() - 200.0) < 1.0e-6 && std::fabs(at.y() - 150.0) < 1.0e-6); /* zooms keep it put */
    push(4002U, 10.0, 20.00001);
    f.model.poll();
    at = f.map->markerPoint(id).toPointF();
    assert(std::fabs(at.x() - 200.0) < 1.0e-6 && std::fabs(at.y() - 176.0) < 1.0e-6); /* next step: below the chip */
}

/* Independent review I1: a pinch or wheel step during a follow's camera move (a new fix, or the follow's start)
 * zooms from where the follow is taking the camera and about the followed mark, so the mark ends centred and stays
 * so, the same with the camera animated as with reduced motion. */
void
follow_pinch_case(int duration_ms) {
    Fixture f;
    f.map->setCameraDurationMs(duration_ms);
    push(4004U, 10.0, 20.0);
    f.model.poll();
    const qulonglong id = f.map->trackAt(200.0, 150.0);
    f.map->setFollowTrackId(id);
    assert(spin_until([&f] { return !f.map->testCameraMoving(); }));
    const double px = 360.0 / (256.0 * 4096.0);
    push(4004U, 10.0, 20.0 + (145.0 * px)); /* 145 px east: the follow starts its step */
    f.model.poll();
    f.map->zoomAbout(f.map->zoom() + 0.5, 10.0, 10.0); /* a pinch at once, fingers anywhere */
    assert(spin_until([&f] { return !f.map->testCameraMoving(); }));
    QPointF at = f.map->markerPoint(id).toPointF();
    assert(std::fabs(at.x() - 200.0) < 1.0e-3 && std::fabs(at.y() - 150.0) < 1.0e-3);
    push(4005U, 10.1, 20.1); /* another radio heard */
    f.model.poll();
    f.map->zoomAbout(f.map->zoom() + 3.0, 390.0, 290.0);
    assert(spin_until([&f] { return !f.map->testCameraMoving(); }));
    at = f.map->markerPoint(id).toPointF();
    assert(std::fabs(at.x() - 200.0) < 1.0e-3 && std::fabs(at.y() - 150.0) < 1.0e-3);
    assert(f.map->followTrackId() == id);
}

void
test_pinch_during_a_follow_step() {
    follow_pinch_case(250);
    follow_pinch_case(0);
}

/* Independent review M1: a follow's later steps keep the zoom the user chose; only its first centring raises one too
 * low to centre the mark. */
void
test_follow_keeps_a_pinched_out_zoom() {
    Fixture f;
    push(4006U, 51.5, 20.0);
    f.model.poll();
    f.map->centerOn(51.5, 20.0, 12.0);
    const qulonglong id = f.map->trackAt(200.0, 150.0);
    assert(id != 0U);
    f.map->setFollowTrackId(id);
    f.map->zoomAbout(f.map->minimumZoom(), 200.0, 150.0); /* pinched right out */
    const double zoom = f.map->zoom();
    assert(zoom < 0.7);
    push(4006U, 51.5, 20.01);
    f.model.poll();
    assert(std::fabs(f.map->zoom() - zoom) < 1.0e-9);
}

/* The screen keeps the canvas between visits (the user's report: the view was lost on Back). While hidden: no follow
 * step moves it, a size the layout passes through clamps nothing, and on return a follow catches up at the zoom the user
 * left it at, not as a first centring. */
void
test_hidden_canvas_keeps_its_view() {
    Fixture f;
    push(4011U, 60.0, 20.0);
    f.model.poll();
    f.map->centerOn(60.0, 20.0, 12.0);
    const qulonglong id = f.map->trackAt(200.0, 150.0);
    assert(id != 0U);
    f.map->setFollowTrackId(id);
    f.map->zoomAbout(f.map->minimumZoom(), 200.0, 150.0); /* pinched right out */
    const double zoom = f.map->zoom();
    f.map->setVisible(false);
    push(4011U, 60.0, 21.0); /* the radio moves while hidden */
    f.model.poll();
    assert(std::fabs(f.map->centerLongitude() - 20.0) < 1.0e-6); /* no step while hidden */
    f.map->setVisible(true);
    assert(std::fabs(f.map->zoom() - zoom) < 1.0e-9);            /* the user's zoom, not a first centring's */
    assert(std::fabs(f.map->centerLongitude() - 21.0) < 1.0e-6); /* caught up with the move */
    f.map->setFollowTrackId(0U);

    /* North at zoom 2 on a 300 px canvas; a taller canvas while hidden would pin the centre further south. */
    f.map->centerOn(64.0, 0.0, 2.0);
    const double north = f.map->centerLatitude();
    f.map->setVisible(false);
    f.map->setSize(QSizeF(400.0, 900.0));
    f.map->setSize(QSizeF(400.0, 300.0));
    f.map->setVisible(true);
    assert(std::fabs(f.map->centerLatitude() - north) < 1.0e-9);
    /* The same while still shown: a band above the canvas hides as the screen closes, before the canvas itself. */
    f.map->setSize(QSizeF(400.0, 900.0));
    f.map->setSize(QSizeF(400.0, 300.0));
    assert(std::fabs(f.map->centerLatitude() - north) < 1.0e-9);

    /* A follow's camera move cut short by hiding is finished on return, though the radio has not moved. */
    push(4012U, -30.0, 100.0);
    f.model.poll();
    f.map->centerOn(-30.0, 100.0, 6.0);
    const qulonglong radio = f.map->trackAt(200.0, 150.0);
    assert(radio != 0U);
    f.map->centerOn(10.0, 60.0, 6.0); /* the radio well off the view */
    f.map->setCameraDurationMs(250);
    f.map->setFollowTrackId(radio); /* the move starts... */
    f.map->setVisible(false);       /* ...and the screen closes part way */
    const double stopped = f.map->centerLongitude();
    assert(std::fabs(stopped - 100.0) > 1.0);
    f.map->setCameraDurationMs(0);
    f.map->setVisible(true);
    assert(std::fabs(f.map->centerLongitude() - 100.0) < 1.0e-6 && std::fabs(f.map->zoom() - 6.0) < 1.0e-9);
    f.map->setFollowTrackId(0U);

    /* A first centring cut short (one that must raise a world-wide zoom to centre a mark near the pole) is done again
     * as a first on return, raise included. */
    push(4013U, 65.0, -40.0);
    f.model.poll();
    f.map->centerOn(65.0, -40.0, 6.0);
    const qulonglong polar = f.map->trackAt(200.0, 150.0);
    assert(polar != 0U);
    f.map->setZoom(0.0);
    f.map->setCameraDurationMs(250);
    f.map->setFollowTrackId(polar);
    f.map->setVisible(false);
    f.map->setCameraDurationMs(0);
    f.map->setVisible(true);
    const QPointF mark = f.map->markerPoint(polar).toPointF();
    assert(std::fabs(mark.x() - 200.0) < 1.0e-6 && std::fabs(mark.y() - 150.0) < 1.0e-6);
    f.map->setFollowTrackId(0U);

    /* A rotation while hidden (a size that lasts): on return the follow centres its mark for the new canvas, at the
     * user's zoom. */
    f.map->centerOn(65.0, -40.0, 6.0);
    f.map->setFollowTrackId(polar);
    f.map->setVisible(false);
    f.map->setSize(QSizeF(300.0, 400.0));
    f.map->setVisible(true);
    const QPointF turned = f.map->markerPoint(polar).toPointF();
    assert(std::fabs(turned.x() - 150.0) < 1.0e-6 && std::fabs(turned.y() - 200.0) < 1.0e-6
           && std::fabs(f.map->zoom() - 6.0) < 1.0e-9);
    f.map->setFollowTrackId(0U);
    f.map->setSize(QSizeF(400.0, 300.0));

    /* A zoom button's move cut short while following is left where it stopped, not replayed as a follow step. */
    f.map->centerOn(65.0, -40.0, 6.0);
    f.map->setFollowTrackId(polar);
    f.map->setCameraDurationMs(250);
    f.map->zoomIn();
    f.map->setVisible(false);
    const double stoppedZoom = f.map->zoom();
    f.map->setCameraDurationMs(0);
    f.map->setVisible(true);
    assert(std::fabs(f.map->zoom() - stoppedZoom) < 1.0e-9);
    f.map->setFollowTrackId(0U);

    /* The List shown (the canvas hidden) and the layout changing meanwhile does not start a follow over. */
    f.map->centerOn(65.0, -40.0, 6.0);
    f.map->setFollowTrackId(polar);
    f.map->zoomAbout(f.map->minimumZoom(), 200.0, 150.0); /* pinched right out */
    const double out = f.map->zoom();
    f.map->setVisible(false);
    f.map->setSize(QSizeF(400.0, 900.0));
    f.map->setSize(QSizeF(400.0, 300.0));
    f.map->setVisible(true);
    assert(std::fabs(f.map->zoom() - out) < 1.0e-9);
    /* Nor while shown: the tiles prompt can go (the List) before the canvas hides. */
    f.map->setSize(QSizeF(400.0, 900.0));
    f.map->setSize(QSizeF(400.0, 300.0));
    assert(std::fabs(f.map->zoom() - out) < 1.0e-9);
    f.map->setFollowTrackId(0U);
}

/* Independent review M2/M3: a new canvas size, or a view restored through the camera properties, re-centres a running
 * follow at once rather than leaving the mark where those put it until it moves. */
void
test_follow_recentres_after_resize_and_restore() {
    Fixture f;
    push(4007U, 10.0, 20.0);
    f.model.poll();
    const qulonglong id = f.map->trackAt(200.0, 150.0);
    f.map->setFollowTrackId(id);
    f.map->setCenterLatitude(40.0);
    f.map->setCenterLongitude(-70.0);
    QPointF at = f.map->markerPoint(id).toPointF();
    assert(std::fabs(at.x() - 200.0) < 1.0e-6 && std::fabs(at.y() - 150.0) < 1.0e-6);
    f.map->setFollowTrackId(0U);

    /* Following the device at 51.5 N from the world's zoom: the first centring raised the zoom for a 300 px canvas.
     * On an 800 px one (a rotation) the follow centres the dot again at that zoom, not a raised one: only a first
     * centring raises a zoom (a layout change must not undo a zoom the user pinched out to). Where the projection's
     * edge stops the centre, the dot is as near it as the canvas allows, and in view. */
    f.map->setZoom(0.0);
    f.map->setDeviceLatitude(51.5);
    f.map->setDeviceLongitude(20.0);
    f.map->setDeviceValid(true);
    f.map->setFollowDevice(true);
    const double raised = f.map->zoom();
    f.map->setSize(QSizeF(400.0, 800.0));
    assert(std::fabs(f.map->zoom() - std::max(raised, f.map->minimumZoom())) < 1.0e-9);
    const mm::Viewport vp = mm::clamp_viewport(mm::Viewport{
        mm::world_from_lat_lon(f.map->centerLatitude(), f.map->centerLongitude()).x,
        mm::world_from_lat_lon(f.map->centerLatitude(), f.map->centerLongitude()).y, f.map->zoom(), 400.0, 800.0});
    const mm::ScreenPoint dot = mm::screen_from_world(vp, mm::world_from_lat_lon(51.5, 20.0));
    assert(std::fabs(dot.x - 200.0) < 1.0e-6 && dot.y >= 0.0 && dot.y <= 800.0);
    f.map->setFollowDevice(false);
    f.map->setSize(QSizeF(400.0, 300.0));
}

/* PR2 review 2: centring a point the current zoom cannot put in the middle (at the world's zoom the whole world is
 * shorter than the canvas, so the centre is pinned to the equator) raises the zoom just enough; a zoom that can is
 * kept. */
void
test_centring_raises_a_zoom_that_cannot() {
    Fixture f;
    f.map->setZoom(0.0);
    f.map->setDeviceLatitude(51.5);
    f.map->setDeviceLongitude(20.0);
    f.map->setDeviceValid(true);
    const bool centred = f.map->centerOnDevice();
    assert(centred);
    const mm::Viewport vp = mm::clamp_viewport(mm::Viewport{
        mm::world_from_lat_lon(f.map->centerLatitude(), f.map->centerLongitude()).x,
        mm::world_from_lat_lon(f.map->centerLatitude(), f.map->centerLongitude()).y, f.map->zoom(), 400.0, 300.0});
    const mm::ScreenPoint dot = mm::screen_from_world(vp, mm::world_from_lat_lon(51.5, 20.0));
    assert(std::fabs(dot.x - 200.0) < 1.0e-6 && std::fabs(dot.y - 150.0) < 1.0e-6);
    assert(f.map->zoom() > 14.99 && f.map->zoom() < 15.01); /* Locate me goes in to the streets around the dot */
    f.map->setZoom(17.0);
    const bool again = f.map->centerOnDevice();
    assert(again && std::fabs(f.map->zoom() - 15.0) < 1.0e-9); /* the fix's own zoom, whatever the map had */
}

/* Locate me's zoom: the streets around a precise fix, and an approximate fix's whole circle (about half the shorter
 * side across), however wide. The view is 400 x 300. */
void
test_device_zoom_follows_the_accuracy() {
    Fixture f;
    f.map->setDeviceLatitude(51.5);
    f.map->setDeviceLongitude(20.0);
    f.map->setDeviceValid(true);
    f.map->setDeviceAccuracyM(0.0); /* unknown */
    assert(std::fabs(f.map->deviceZoom() - 15.0) < 1.0e-9);
    f.map->setDeviceAccuracyM(5.0);
    assert(std::fabs(f.map->deviceZoom() - 15.0) < 1.0e-9);
    f.map->setDeviceAccuracyM(2000.0);
    const double approximate = f.map->deviceZoom();
    const double radius_px = 2000.0 / mm::metres_per_px(51.5, approximate);
    assert(approximate > 11.0 && approximate < 15.0 && std::fabs(radius_px - 75.0) < 1.0e-6);
    f.map->setDeviceAccuracyM(10000.0); /* the widest fix a request keeps: its whole circle still shows */
    const double wide = f.map->deviceZoom();
    assert(wide < 11.0 && std::fabs((10000.0 / mm::metres_per_px(51.5, wide)) - 75.0) < 1.0e-6);
    f.map->setZoom(2.0);
    f.map->setDeviceAccuracyM(2000.0);
    assert(f.map->centerOnDevice() && std::fabs(f.map->zoom() - approximate) < 1.0e-9);
    /* Near a pole the projection's edge stops the centre: the accuracy's zoom stays, rather than one raised to centre
     * the dot that would cut its circle. */
    f.map->setDeviceLatitude(85.0);
    f.map->setDeviceAccuracyM(5000.0);
    const double polar = f.map->deviceZoom();
    assert(f.map->centerOnDevice() && std::fabs(f.map->zoom() - polar) < 1.0e-9);
}

/* PR2 review 2: a fit while following the live position takes in the device dot that will be drawn after the follow
 * ends (the screen gives it back its saved fix), not the live one. */
void
test_fit_takes_the_dot_left_after_follow() {
    Fixture f;
    f.map->setDeviceLatitude(10.0);
    f.map->setDeviceLongitude(20.0);
    f.map->setDeviceValid(true);
    f.map->setFollowDevice(true);
    /* What MapScreen does: live follow ending releases the live position; the dot goes back to the saved fix. */
    QObject::connect(f.map, &MapViewItem::followChanged, f.map, [&f] {
        if (!f.map->followDevice()) {
            f.map->setDeviceLatitude(12.0);
            f.map->setDeviceLongitude(23.0);
        }
    });
    push(4003U, 10.001, 20.001);
    f.model.poll();
    const bool fitted = f.map->fitAll();
    assert(fitted && !f.map->followDevice());
    const mm::Viewport vp = mm::clamp_viewport(mm::Viewport{
        mm::world_from_lat_lon(f.map->centerLatitude(), f.map->centerLongitude()).x,
        mm::world_from_lat_lon(f.map->centerLatitude(), f.map->centerLongitude()).y, f.map->zoom(), 400.0, 300.0});
    const mm::ScreenPoint dot = mm::screen_from_world(vp, mm::world_from_lat_lon(12.0, 23.0));
    assert(dot.x >= 0.0 && dot.x <= 400.0 && dot.y >= 0.0 && dot.y <= 300.0);
}

/* PR2 review 2: a label standing by a track's ring ranks by the ring's own age, not by the far-off current position's:
 * the radio heard five seconds ago (at its ring) keeps the side ahead of one heard five minutes ago. */
void
test_ring_label_ranks_by_its_own_age() {
    Fixture f;
    const double cx = f.map->width() / 2.0;
    push(7U, 10.0, 25.0, 0U, 0U, 600);                          /* its current position: far off, 10 minutes */
    push(7U, 10.0, 20.0, 0U, DSD_POS_FLAG_VALIDITY_UNKNOWN, 5); /* its ring: here, 5 seconds */
    push(1234567890U, 10.0, 20.0, 0U, 0U, 300);                 /* another radio here, 5 minutes */
    f.model.poll();
    QSGNode* root = f.map->testUpdateGpuTree(nullptr, 1.0);
    const double right = right_label_width(root, cx);
    double widest = 0.0;
    {
        const Layers now = layers(root);
        assert(now.labels->childCount() == 2);
        for (QSGNode* n = now.labels->firstChild(); n != nullptr; n = n->nextSibling()) {
            widest = std::max(widest, static_cast<QSGImageNode*>(n)->rect().width());
        }
    }
    /* The short label ("7") holds the right; the long one went to another side. */
    assert(right > 0.0 && right < widest);
    delete root;
}

} // namespace

int
main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    test_layers();
    test_break_is_not_bridged();
    test_pan_moves_the_overlay();
    test_tiles();
    test_background_stops_loading();
    test_device_views_ask_memory_only();
    test_held_tiles_follow_the_cache_rules();
    test_overlay_follows_the_wrap_copy();
    test_labels_render_on_the_gui_thread();
    test_label_priority();
    test_zoom_settles_by_itself();
    test_covering_label_has_a_plate();
    test_hidden_canvas_defers_the_scene();
    test_follow_zoom_keeps_the_point();
    test_lower_bound_error_has_no_disc();
    test_label_by_the_visible_ring();
    test_overlay_follows_a_wrapping_camera();
    test_follow_moves_when_the_filter_hides_the_marker();
    test_chips_never_move_the_camera();
    test_pinch_during_a_follow_step();
    test_follow_keeps_a_pinched_out_zoom();
    test_hidden_canvas_keeps_its_view();
    test_follow_recentres_after_resize_and_restore();
    test_centring_raises_a_zoom_that_cannot();
    test_device_zoom_follows_the_accuracy();
    test_fit_takes_the_dot_left_after_follow();
    test_ring_label_ranks_by_its_own_age();
    (void)dsd_qt::MapTileWorker::shutdownThreads(dsd_qt::MapTileWorker::kShutdownJoinMs);
    (void)DSD_FPRINTF(stdout, "UI_QT_MAP_VIEW: ok\n");
    return 0;
}

// NOLINTEND(cert-dcl03-c,misc-static-assert)
