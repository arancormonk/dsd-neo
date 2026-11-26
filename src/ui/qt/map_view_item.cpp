// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "map_view_item.h"

#include <QAbstractAnimation>
#include <QByteArray>
#include <QChar>
#include <QCoreApplication>
#include <QDebug>
#include <QEasingCurve>
#include <QGuiApplication>
#include <QPointF>
#include <QRectF>
#include <QSet>
#include <QVariantAnimation>
#include <Qt>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <dsd-neo/core/position.h>
#include <iterator>
#include <limits>
#include <qrgb.h>
#include <stdlib.h>
#include <utility>
#include <vector>

#include "map_geometry.h"
#include "map_math.h"
#include "map_view_detail.h"
#include "spectrum_trace_geometry.h"

namespace dsd_qt {

namespace mm = map_math;
namespace mg = map_geometry;
namespace ms = map_scene;
using map_view_detail::differs;
using map_view_detail::draw_rank;

namespace {

/* Tiles kept from another zoom to stand in, scaled, while the view's own load. */
constexpr int kMaxStandIns = 64;

bool
same_viewport(const mm::Viewport& a, const mm::Viewport& b) {
    return !differs(a.center_x, b.center_x) && !differs(a.center_y, b.center_y) && !differs(a.zoom, b.zoom)
           && !differs(a.width, b.width) && !differs(a.height, b.height);
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

} // namespace

// ----------------------------------------------------------- the item ----

MapViewItem::MapViewItem(QQuickItem* parent) : QQuickItem(parent), m_camera(new QVariantAnimation(this)) {
    setFlag(ItemHasContents, true);
    /* Tiles, trails and labels run past the edge; the canvas is a window onto the map. */
    setClip(true);
    m_camera->setStartValue(0.0);
    m_camera->setEndValue(1.0);
    m_camera->setEasingCurve(QEasingCurve::OutCubic);
    m_zoomSettle.setSingleShot(true);
    m_zoomSettle.setInterval(kZoomSettleMs);
    connect(&m_zoomSettle, &QTimer::timeout, this, &MapViewItem::zoomSettled);
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
    if (!on) {
        releaseTiles();
    }
    catchUpScene();
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
    if (!shown) {
        /* At once, not at the next polish: a hidden window is not polished (Android destroys its surface), and the
         * worker would go on loading the last view in the background. */
        releaseTiles();
    }
    polish();
    update();
}

void
MapViewItem::releaseTiles() {
    dropTileRequests();
    if (!m_tileImages.isEmpty()) {
        m_tileImages.clear();
        update();
    }
}

// --------------------------------------------------------------- scene ----

bool
MapViewItem::canvasShown() const {
    return m_active && isVisible() && window() != nullptr;
}

void
MapViewItem::ensureScene() {
    if (!m_sceneStale) {
        return;
    }
    m_sceneStale = false;
    rebuildScene();
    m_sceneSerial++;
    followNow();
    update();
}

void
MapViewItem::catchUpScene() {
    if (canvasShown()) {
        ensureScene();
    }
}

void
MapViewItem::onTracksChanged() {
    /* Hidden (the List shown, or the map closed with its screen kept): the model goes on ingesting, but the scene is
     * built once, when the canvas is shown again (catchUpScene()), not for every batch on another screen. */
    m_sceneStale = true;
    catchUpScene();
}

void
MapViewItem::onAgesChanged() {
    if (m_sceneStale || !canvasShown()) {
        m_sceneStale = true;
        return;
    }
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
    followNow(); /* the age filter may have hidden the followed mark, or shown its other one */
    update();
}

void
MapViewItem::sceneChanged() {
    m_sceneStale = true;
    catchUpScene();
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
        /* A lower-bound error ("more than 200 km") has no bound to draw: a disc of that radius would say the radio is
         * within it. No disc; the sheet and the List say "more than". */
        const bool lowerBound = (row.current.flags & DSD_POS_FLAG_ERR_LOWER_BOUND) != 0U;
        t.errM = lowerBound ? 0.0 : static_cast<double>(row.current.err_m);
        t.latDeg = static_cast<double>(row.current.lat_e7) / 1.0e7;
        t.ageS = row.ageS;
        /* An anonymous row holds one point (the model never extends it), so it has no trail. */
        world_trail(row, t);
    }
    if (row.hasUnknown && row.unknownShown && mm::world_from_e7(row.unknown.lat_e7, row.unknown.lon_e7, &t.ring)) {
        t.hasRing = true;
        t.ringStyle.ring = true;
        t.ringStyle.dashed_outline = row.anonymous;
        t.ringStyle.double_outline = (row.unknown.flags & DSD_POS_FLAG_FORMAT_PROVISIONAL) != 0U;
        t.ringStyle.alert = t.emergency;
        t.ringAgeS = row.unknownAgeS;
        if (!t.hasMarker) {
            t.ageS = row.unknownAgeS;
        }
    }
    return t.hasMarker || t.hasRing;
}

} // namespace

void
MapViewItem::orderLabels() {
    /* Highest priority first: the selected track, emergencies, then the most recently heard. Labels are placed in this
     * order, except that emergencies go first and every other label yields to them (place_labels()). A label that
     * cannot find a clear side is left out, unless it is an emergency's: that one is never dropped, and may cover a
     * marker or another emergency's label. */
    m_labelOrder.clear();
    for (size_t i = m_scene.size(); i-- > 0;) {
        if (!m_scene[i].label.isEmpty()) {
            m_labelOrder.push_back(static_cast<int>(i));
        }
    }
    std::stable_sort(m_labelOrder.begin(), m_labelOrder.end(), [&](int a, int b) {
        const TrackDraw& ta = m_scene[static_cast<size_t>(a)];
        const TrackDraw& tb = m_scene[static_cast<size_t>(b)];
        const int ra = map_view_detail::label_rank(ta);
        const int rb = map_view_detail::label_rank(tb);
        return ra != rb ? ra > rb : map_view_detail::heard_sooner(ta, tb);
    });
}

void
MapViewItem::rebuildScene() {
    m_sceneRebuilds++;
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
        t.labelRole = t.selected ? 1 : 0;
        if (t.emergency) {
            t.labelRole = 2; /* an emergency outranks the selection */
        }
        t.labelKey = QString::number(t.labelRole) + QLatin1Char('\n') + t.label;
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
    orderLabels();
    prepareLabels();
    const int count = static_cast<int>(m_markers.size());
    if (count != m_markerCount) {
        m_markerCount = count;
        Q_EMIT markerCountChanged();
    }
}

// ----------------------------------------------------------- viewport ----

mm::Viewport
MapViewItem::viewportForSize() const {
    /* From the view last asked for, not the one a passing size clamped: a canvas briefly taller (a band above it
     * hiding as the screen closes) must not move the view kept for the next visit. */
    mm::Viewport vp = m_asked;
    vp.width = width();
    vp.height = height();
    return mm::clamp_viewport(vp);
}

void
MapViewItem::setViewport(const mm::Viewport& next) {
    m_asked = next;
    mm::Viewport vp = next;
    vp.width = width();
    vp.height = height();
    vp = mm::clamp_viewport(vp);
    if (same_viewport(vp, m_vp)) {
        return;
    }
    if (differs(vp.zoom, m_vp.zoom)) {
        /* Zooming: the overlay is scaled until the zoom has held still for kZoomSettleMs (zoomSettled()). */
        m_zooming = true;
        m_zoomSettle.start();
    }
    m_vp = vp;
    Q_EMIT viewportChanged();
    polish();
    update();
}

void
MapViewItem::zoomSettled() {
    m_zooming = false;
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
    rearmFollow();
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
    rearmFollow();
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
    rearmFollow();
}

double
MapViewItem::minimumZoom() const {
    return mm::min_zoom_for_width(width());
}

void
MapViewItem::geometryChange(const QRectF& new_geometry, const QRectF& old_geometry) {
    QQuickItem::geometryChange(new_geometry, old_geometry);
    if (new_geometry.size() != old_geometry.size()) {
        /* The centre stays where it was asked to be (a size that clamps it only for a while, as a band hides while the
         * screen closes, moves nothing for good); the zoom floor may rise with the width. A followed mark is centred
         * again: where centring puts it depends on the canvas's size. */
        setViewport(m_asked);
        rearmFollow();
        polish();
        update();
    }
}

void
MapViewItem::itemChange(ItemChange change, const ItemChangeData& value) {
    QQuickItem::itemChange(change, value);
    /* Leaving the window or the screen stops loading at once: an item with no window, or a hidden one, is not
     * polished, so updatePolish() would not get to it. */
    const bool gone =
        (change == ItemSceneChange && value.window == nullptr) || (change == ItemVisibleHasChanged && !value.boolValue);
    if (gone) {
        hidden();
    }
    if (change == ItemVisibleHasChanged && value.boolValue) {
        shownAgain();
    }
    if (change == ItemVisibleHasChanged || change == ItemSceneChange) {
        catchUpScene();
    }
    if (change == ItemDevicePixelRatioHasChanged || change == ItemVisibleHasChanged || change == ItemSceneChange) {
        polish();
        update();
    }
}

void
MapViewItem::hidden() {
    releaseTiles();
    /* The view the screen keeps is the one shown when it hid: a camera move still running stops there, and no follow
     * step moves it while hidden (followNow()), so the dot going back from a live fix to the saved one as the map
     * closes cannot carry the camera with it. A follow's move cut short (not a zoom button's) is finished on return:
     * a first centring is done again as a first, a later step at the user's zoom. */
    if (m_camera->state() == QAbstractAnimation::Running && m_animFollow && following()) {
        if (m_followStepFirst) {
            m_followedSet = false;
        } else {
            m_followCatchUp = true;
        }
    }
    stopCamera();
    m_hiddenSize = size();
}

void
MapViewItem::shownAgain() {
    /* Back on screen: the kept view fitted to the size the canvas has now, and a follow still on catches up with a mark
     * that moved meanwhile, a move cut short, or a canvas resized while hidden (a rotation), at the zoom the user left
     * it at. The screen calls recentreFollow() again once the chips above have their height. */
    if (size() != m_hiddenSize) {
        m_followCatchUp = true;
    }
    setViewport(m_asked);
    followNow();
}

// ------------------------------------------------------------- camera ----

void
MapViewItem::stopCamera() {
    m_camera->stop();
}

void
MapViewItem::moveCamera(const mm::Viewport& target) {
    m_camera->stop();
    m_animFollow = false; /* followNow() marks its own moves */
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
    if (following()) {
        /* A pinch or wheel step while following keeps the follow (a pan ends it): the step starts from where the
         * follow is taking the camera, not from a frame half way there, and zooms about the followed mark, so the
         * mark stays put with or without the camera's animation. */
        const mm::Viewport base = cameraBase();
        const QPointF anchor = followAnchor(base);
        stopCamera();
        setViewport(mm::zoom_about(base, level, anchor.x(), anchor.y()));
        return;
    }
    stopCamera();
    setViewport(mm::zoom_about(m_vp, level, x, y));
}

#ifdef DSD_NEO_TEST_HOOKS
bool
MapViewItem::testCameraMoving() const {
    return m_camera->state() == QAbstractAnimation::Running;
}
#endif

bool
MapViewItem::following() const {
    return m_followTrack != 0U || m_followDevice;
}

QPointF
MapViewItem::followAnchor(const mm::Viewport& base) const {
    /* The followed mark where @p base puts it: where centring put it, below the chips as they were then. A change of
     * the chips since (they come and go with tile trouble) moves neither the camera nor this point. */
    if (m_followedSet) {
        const mm::ScreenPoint s = mm::screen_from_world(base, m_followedAt);
        return QPointF(s.x, s.y);
    }
    return focusPoint();
}

mm::Viewport
MapViewItem::cameraBase() const {
    /* Where the camera is going: a move started while one runs (a follow step during a zoom-button animation)
     * keeps the other's target rather than the frame it happens to be at. */
    return (m_camera->state() == QAbstractAnimation::Running) ? m_animTo : m_vp;
}

QPointF
MapViewItem::focusPoint() const {
    /* The middle of the part of the canvas below topInset: where every centring puts its target (belowTopInset()). */
    const double inset = std::min(m_topInset, std::max(0.0, height() / 2.0));
    return QPointF(width() / 2.0, (height() / 2.0) + (inset / 2.0));
}

void
MapViewItem::zoomIn() {
    zoomStep(1.0);
}

void
MapViewItem::zoomOut() {
    zoomStep(-1.0);
}

void
MapViewItem::zoomStep(double steps) {
    /* The buttons: animated, from where the camera is going, about the followed mark while following, else about the
     * middle below the chips. */
    const mm::Viewport base = cameraBase();
    const QPointF anchor = following() ? followAnchor(base) : focusPoint();
    moveCamera(mm::zoom_about(base, base.zoom + steps, anchor.x(), anchor.y()));
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
MapViewItem::fitAll(bool withDevice) {
    ensureScene();
    return fitPoints(markerPoints(), withDevice);
}

bool
MapViewItem::fitAllWith(double lat, double lon) {
    ensureScene();
    std::vector<mm::WorldPoint> points = markerPoints();
    if (std::isfinite(lat) && std::isfinite(lon) && std::fabs(lat) <= 90.0 && std::fabs(lon) <= 180.0) {
        points.push_back(mm::world_from_lat_lon(lat, lon));
    }
    return fitPoints(std::move(points));
}

bool
MapViewItem::fitPoints(std::vector<mm::WorldPoint> points, bool takeDevice) {
    /* A fit ends any follow first: ending live follow gives the device dot back its saved fix (the screen releases the
     * live position), and the fit must take in the dot that will be drawn, not the one that was. */
    stopFollowing();
    mm::WorldPoint device{};
    const bool withDevice = takeDevice && deviceWorld(&device);
    if (withDevice) {
        points.push_back(device);
    }
    /* Fit into the part below the screen's chips, then place that part's middle where the fit put the view's. */
    mm::Viewport clear = viewportForSize();
    const double inset = std::min(m_topInset, std::max(0.0, clear.height - (2.0 * m_fitPadding) - 1.0));
    clear.height -= inset;
    mm::Viewport out;
    if (!mm::fit_points(clear, points.data(), static_cast<int>(points.size()), m_fitPadding, m_fitMaxZoom, &out)) {
        return false;
    }
    out.height = clear.height + inset;
    out.center_y -= (inset / 2.0) / mm::world_size_px(out.zoom);
    if (withDevice) {
        markUsedDevice();
    }
    moveCamera(out);
    return true;
}

bool
MapViewItem::centerOnTrack(qulonglong trackId) {
    ensureScene();
    const TrackDraw* t = findTrack(trackId);
    if (t == nullptr) {
        return false;
    }
    moveCamera(centredOn(cameraBase(), t->hasMarker ? t->marker : t->ring));
    return true;
}

bool
MapViewItem::centerOnDevice() {
    mm::WorldPoint at{};
    if (!deviceWorld(&at)) {
        return false;
    }
    markUsedDevice();
    mm::Viewport vp = cameraBase();
    vp.zoom = deviceZoom();
    /* The accuracy's zoom is kept even where it cannot put the dot in the middle (near a pole the projection's edge
     * stops the centre): raising it would cut the circle the zoom was chosen to show. */
    moveCamera(centredOn(vp, at, false));
    return true;
}

double
MapViewItem::deviceZoom() const {
    constexpr double kNear = 15.0; /* a few streets either way on a phone */
    const double side = std::min(width(), height());
    if (!(side > 0.0) || !(m_deviceAccuracyM > 0.0) || !std::isfinite(m_deviceAccuracyM)
        || !std::isfinite(m_deviceLat)) {
        return kNear;
    }
    /* The circle's radius spans a quarter of the shorter side, and each zoom step halves the metres per pixel:
     * metres_per_px(lat, 0) / 2^zoom = radius / (side / 4). */
    const double level = std::log2(mm::metres_per_px(m_deviceLat, 0.0) * side / (4.0 * m_deviceAccuracyM));
    if (!std::isfinite(level)) {
        return kNear;
    }
    /* No floor but the projection's: an approximate fix's circle can be kilometres across, and all of it shows. */
    return std::max(mm::kMinZoom, std::min(kNear, level));
}

void
MapViewItem::centerOn(double lat, double lon, double level) {
    if (!std::isfinite(lat) || !std::isfinite(lon)) {
        return;
    }
    const mm::WorldPoint at = mm::world_from_lat_lon(lat, lon);
    mm::Viewport vp = cameraBase();
    if (level >= 0.0 && std::isfinite(level)) {
        vp.zoom = level;
    }
    moveCamera(centredOn(vp, at));
}

mm::Viewport
MapViewItem::centredOn(mm::Viewport vp, mm::WorldPoint at, bool raiseZoom) const {
    /* The one way a point is centred (one-shot or followed): at focusPoint(), below the chips. At a zoom too low to
     * put it there (the whole world shorter than the canvas pins the centre to the equator; near a pole the edge stops
     * it) the zoom is raised just enough; a zoom that can is kept. */
    const double h = std::max(0.0, height());
    const double inset = std::min(m_topInset, h / 2.0);
    const double north = (inset + h) / 2.0; /* pixels the view reaches above the point */
    const double south = (h - inset) / 2.0; /* and below it */
    double world = mm::world_size_px(vp.zoom);
    if (at.y > 0.0 && north / at.y > world) {
        world = north / at.y;
    }
    if (at.y < 1.0 && south / (1.0 - at.y) > world) {
        world = south / (1.0 - at.y);
    }
    if (raiseZoom) {
        vp.zoom = std::min(mm::kMaxZoom, std::max(vp.zoom, std::log2(world / mm::kTileSize)));
    }
    vp.center_x = at.x;
    vp.center_y = at.y;
    return belowTopInset(vp);
}

mm::Viewport
MapViewItem::belowTopInset(mm::Viewport vp) const {
    const double inset = std::min(m_topInset, std::max(0.0, height() / 2.0));
    vp.center_y -= (inset / 2.0) / mm::world_size_px(vp.zoom);
    return vp;
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

QVariant
MapViewItem::pointFor(double lat, double lon) const {
    if (!std::isfinite(lat) || !std::isfinite(lon) || std::fabs(lat) > 90.0 || std::fabs(lon) > 180.0) {
        return QVariant();
    }
    const mm::ScreenPoint s = mm::screen_from_world(m_vp, mm::world_from_lat_lon(lat, lon));
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
    m_followedSet = false;
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
    m_followedSet = false;
    Q_EMIT followChanged();
    followNow();
    polish(); /* the tiles' origin may have changed */
}

void
MapViewItem::stopFollowing() {
    if (m_followTrack == 0U && !m_followDevice) {
        return;
    }
    m_followTrack = 0U;
    m_followDevice = false;
    m_followedSet = false;
    Q_EMIT followChanged();
}

bool
MapViewItem::followTarget(mm::WorldPoint* at) {
    if (m_followDevice) {
        return deviceWorld(at);
    }
    if (m_followTrack == 0U) {
        return false;
    }
    /* A track the model no longer holds (cleared, evicted) ends the follow; one the filter hides waits. */
    if (!m_model || m_model->rowForTrack(m_followTrack) < 0) {
        stopFollowing();
        return false;
    }
    ensureScene();
    const TrackDraw* t = findTrack(m_followTrack);
    if (t == nullptr) {
        return false;
    }
    /* The mark drawn for it: the current position, else (the filter hid it) the open ring. */
    *at = t->hasMarker ? t->marker : t->ring;
    return true;
}

void
MapViewItem::followNow() {
    /* The one place the camera tracks a followed mark, whatever moved it: a new fix, the age filter hiding one of the
     * track's marks or showing the other, the device's position, or the chips above (topInset) changing where centred
     * lies. The camera moves only when the target or that place changed, so an age tick or a zoom leaves it be. */
    mm::WorldPoint at{};
    if (!isVisible() || !followTarget(&at)) {
        return; /* hidden: the kept view stays put (itemChange() re-arms it on return) */
    }
    if (m_followedSet && !m_followCatchUp && !differs(at.x, m_followedAt.x) && !differs(at.y, m_followedAt.y)) {
        return;
    }
    /* A follow's first centring may raise a zoom too low to centre the mark; its later steps keep the zoom the user
     * chose (a pinch out to see where the radio is stays out). */
    m_followCatchUp = false;
    m_followStepFirst = !m_followedSet;
    const bool first = !m_followedSet;
    m_followedSet = true;
    m_followedAt = at;
    if (m_followDevice) {
        markUsedDevice();
    }
    moveCamera(centredOn(cameraBase(), at, first));
    m_animFollow = true;
}

void
MapViewItem::recentreFollow() {
    /* Centred below the chips as they are now (the screen has just laid them out), at the user's zoom. */
    if (!following()) {
        return;
    }
    m_followCatchUp = true;
    followNow();
}

void
MapViewItem::rearmFollow() {
    /* The camera was put somewhere by other means (a restored view, a new canvas size): the follow centres its mark
     * again now, as at its start. Not while hidden: a size the layout passes through then (the List shown) is no
     * reason to start the follow over, and the kept view comes back from the view asked for (m_asked). */
    if (!following() || !isVisible()) {
        return;
    }
    /* At the zoom the follow has: a layout change (the tiles prompt going as the List shows) is no reason to raise a
     * zoom the user pinched out to, as a first centring would. */
    m_followCatchUp = true;
    followNow();
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
    polish(); /* the view's tiles are asked for again, from the device */
}

void
MapViewItem::resetCameraUsedDevicePosition() {
    if (!m_usedDevice) {
        return;
    }
    m_usedDevice = false;
    Q_EMIT cameraUsedDevicePositionChanged();
    polish();
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
    polish(); /* labels are drawn again, on the GUI thread */
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
MapViewItem::setTopInset(double inset) {
    /* Never a camera move: the chips come and go with tile trouble, and a camera that followed them would change the
     * tiles in view, and so the chips again. A followed mark stays where it was centred (zooms keep it there,
     * followAnchor()); its next step is centred below the chips as they are then. */
    if (assignSize(m_topInset, inset)) {
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
    polish();
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
MapViewItem::setLabelObstacles(const QVariantList& rects) {
    if (m_labelObstacleList == rects) {
        return;
    }
    m_labelObstacleList = rects;
    m_labelObstacles.clear();
    for (const QVariant& v : rects) {
        const QRectF r = v.toRectF();
        if (r.isValid() && std::isfinite(r.x()) && std::isfinite(r.y()) && std::isfinite(r.width())
            && std::isfinite(r.height())) {
            m_labelObstacles.push_back(ms::LabelBox{static_cast<float>(r.x()), static_cast<float>(r.y()),
                                                    static_cast<float>(r.width()), static_cast<float>(r.height())});
        }
    }
    Q_EMIT behaviourChanged();
    update();
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

bool
MapViewItem::tilesFromDevice() const {
    /* Sticky for the visit: after Locate me or a fit around the dot, a pan by hand still starts near the device. */
    return m_usedDevice || m_followDevice;
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
    prepareLabels();
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
    const bool fromDevice = tilesFromDevice();
    if (keys != m_requested || fromDevice != m_requestedFromDevice) {
        m_requested = keys;
        m_requestedFromDevice = fromDevice;
        (void)m_tiles->requestTiles(keys, fromDevice ? MapTileWorker::ViewOrigin::FromDevice
                                                     : MapTileWorker::ViewOrigin::ByHand);
    }
    /* Held images: the view's own, and stand-ins from another zoom while some of the view's are still loading. */
    bool complete = true;
    for (const MapTileKey& key : keys) {
        complete = complete && m_tileImages.contains(key);
    }
    keepStandIns(inView, complete);
}

void
MapViewItem::keepStandIns(const QSet<MapTileKey>& inView, bool complete) {
    /* Stand-ins only while some of the view's own tiles are missing; the nearest zoom first, then the tiles nearest
     * the centre, at most kMaxStandIns. */
    struct Candidate {
        MapTileKey key;
        int zoomGap = 0;
        double distance = 0.0;
    };

    complete = complete || tilesSettled(inView);
    dropUnreusableTiles(inView);
    std::vector<Candidate> candidates;
    const int tz = mm::tile_zoom(m_vp.zoom);
    for (auto it = m_tileImages.begin(); it != m_tileImages.end(); ++it) {
        it.value().current = inView.contains(it.key());
        const ms::ScreenRect r = ms::tile_rect(m_vp, it.key().z, it.key().x, it.key().y);
        if (!it.value().current && complete) {
            continue;
        }
        if (!it.value().current && ms::rect_in_view(r, m_vp.width, m_vp.height)) {
            const double dx = ((r.left + r.right) / 2.0) - (m_vp.width / 2.0);
            const double dy = ((r.top + r.bottom) / 2.0) - (m_vp.height / 2.0);
            candidates.push_back(Candidate{it.key(), std::abs(it.key().z - tz), (dx * dx) + (dy * dy)});
        }
    }
    std::stable_sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
        return a.zoomGap != b.zoomGap ? a.zoomGap < b.zoomGap : a.distance < b.distance;
    });
    QSet<MapTileKey> kept;
    for (size_t i = 0; i < candidates.size() && i < static_cast<size_t>(kMaxStandIns); i++) {
        kept.insert(candidates[i].key);
    }
    for (auto it = m_tileImages.begin(); it != m_tileImages.end();) {
        if (it.value().current || kept.contains(it.key())) {
            ++it;
        } else {
            it = m_tileImages.erase(it);
        }
    }
}

bool
MapViewItem::tilesSettled(const QSet<MapTileKey>& inView) {
    /* A key the server answered "no tile" for counts as settled: it stays blank (a parent stand-in must not cover it
     * for good). Misses are kept only for keys still in view, and only for the source that gave them. */
    const QString source = m_tiles ? m_tiles->urlTemplate() : QString();
    if (source != m_tileMissingSource) {
        m_tileMissing.clear();
        m_tileMissingSource = source;
    }
    for (auto it = m_tileMissing.begin(); it != m_tileMissing.end();) {
        it = inView.contains(*it) ? std::next(it) : m_tileMissing.erase(it);
    }
    return std::all_of(inView.cbegin(), inView.cend(), [this](const MapTileKey& key) {
        return m_tileImages.contains(key) || m_tileMissing.contains(key);
    });
}

void
MapViewItem::dropUnreusableTiles(const QSet<MapTileKey>& inView) {
    /* An image the cache rules do not let us show again (no-store, no-cache, must-revalidate) ends with its
     * presentation: once out of the view's tiles it is dropped, so it never stands in or comes back. */
    for (auto it = m_tileImages.begin(); it != m_tileImages.end();) {
        if (!it.value().reusable && !inView.contains(it.key())) {
            it = m_tileImages.erase(it);
        } else {
            ++it;
        }
    }
}

void
MapViewItem::onTileReady(quint64 generation, int z, int x, int y, const QImage& image, bool stale, bool reusable) {
    /* Any generation (a stale copy too) is drawn while its tile is in view; a fresher one replaces it. */
    (void)generation;
    (void)stale;
    const MapTileKey key{z, x, y};
    if (image.isNull()) {
        /* Withdrawn: the server has no usable tile there now, so a stale copy shown before goes at once, and the
         * key counts as settled (blank), not as still loading. */
        const bool held = m_tileImages.remove(key) > 0;
        const bool wanted = tilesWanted() && m_requested.contains(key);
        if (wanted) {
            m_tileMissing.insert(key);
        }
        if (held || wanted) {
            polish();
            update();
        }
        return;
    }
    /* A result for an older request is drawn only while its tile is still in view. */
    if (!tilesWanted() || !m_requested.contains(key)) {
        return;
    }
    m_tileMissing.remove(key);
    const auto held = m_tileImages.find(key);
    if (held != m_tileImages.end() && held.value().source.cacheKey() == image.cacheKey()) {
        /* The same image again (a memory hit for a re-requested view): keep its toned copy and texture. */
        held.value().current = true;
        held.value().reusable = reusable;
        return;
    }
    HeldTile fresh;
    fresh.source = image;
    fresh.toned = toneTile(image);
    fresh.serial = ++m_tileSerial;
    fresh.current = true;
    fresh.reusable = reusable;
    m_tileImages.insert(key, fresh);
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

} // namespace dsd_qt
