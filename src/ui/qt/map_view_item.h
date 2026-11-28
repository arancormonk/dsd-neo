// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The map canvas (issue #800): tiles, trails, accuracy circles, markers and labels, as scene-graph nodes.
 *
 * A QQuickItem, like SpectrumTraceItem (#844): a painted map of 200 tracks of
 * 100 points would stroke twenty times the polyline that made the painted
 * spectrum stutter. On an RHI window each tile is a QSGImageNode, trails and
 * accuracy circles are triangles from map_geometry.h under one transform node
 * (a pan within the margin they were built with only moves that node), markers
 * are quads into one sprite atlas, and labels are image nodes. Qt's software
 * scene graph, which every QML test uses, skips geometry nodes, so there the
 * whole map is painted with QPainter into one image, as
 * SpectrumTraceItem::paintSoftwareImage() does.
 *
 * What is drawn follows spec 4.6, one visual variable per axis (map_scene.h):
 * fill strength is freshness, a dashed outline an unconfirmed source, an open
 * ring a fix that may be old or invalid (never joined to the trail), a double
 * outline and a dashed trail a format no published specification confirms,
 * the alert colour an emergency (with the word in the label), the selected
 * colour the selected track's trail, halo and label. Fixes the age filter
 * hides are not drawn. Every colour, width and size is a property the screen
 * binds to Theme.
 *
 * Input is the screen's: its handlers call panBy(), zoomAbout(), trackAt() and
 * the camera invokables. The item holds no gesture state.
 *
 * Tiles come from a MapTileWorker (tileSource). The item asks for the tiles in
 * view on every pan or zoom, keeps an image only while its tile is in view (or
 * stands in, scaled, for the new zoom's tiles while they load), and drops its
 * requests while it is inactive, hidden, or the app is in the background.
 * Once the device's position has placed the camera (cameraUsedDevicePosition, for the rest of the item's life, which
 * is the app's run) or while the camera follows the device, every request is ViewOrigin::FromDevice, so the tile cache
 * keeps nothing of it on disk.
 */

#ifndef DSD_NEO_SRC_UI_QT_MAP_VIEW_ITEM_H_
#define DSD_NEO_SRC_UI_QT_MAP_VIEW_ITEM_H_

#include <QColor>
#include <QFont>
#include <QHash>
#include <QImage>
#include <QList>
#include <QObject>
#include <QPointF>
#include <QPointer>
#include <QQuickItem>
#include <QSet>
#include <QSizeF>
#include <QString>
#include <QTimer>
#include <QVariant>
#include <QVariantList>
#include <Qt>
#include <QtGlobal>
#include <cstdint>
#include <vector>

#include "map_geometry.h"
#include "map_math.h"
#include "map_scene.h"
#include "map_tile_cache.h"
#include "map_tile_worker.h"
#include "position_model.h"

class QQuickWindow;
class QSGNode;
class QVariantAnimation;

namespace dsd_qt {

namespace spectrum_geometry {
struct StrokeVertex;
} // namespace spectrum_geometry

class MapViewItem : public QQuickItem {
    Q_OBJECT
    /** The tracks (the `positions` context property). */
    Q_PROPERTY(dsd_qt::PositionModel* model READ model WRITE setModel NOTIFY modelChanged)
    /** Where tiles come from; null or off: the plain grid. */
    Q_PROPERTY(dsd_qt::MapTileWorker* tileSource READ tileSource WRITE setTileSource NOTIFY tileSourceChanged)
    /** False while the map is not what the user sees (the List is shown): no tile is asked for. */
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged)

    /** The view: centre in degrees and fractional zoom. Setting them moves at once and stops any camera animation. */
    Q_PROPERTY(double centerLatitude READ centerLatitude WRITE setCenterLatitude NOTIFY viewportChanged)
    Q_PROPERTY(double centerLongitude READ centerLongitude WRITE setCenterLongitude NOTIFY viewportChanged)
    Q_PROPERTY(double zoom READ zoom WRITE setZoom NOTIFY viewportChanged)
    /** The zoom range at this size: the floor is where the world fills the width. */
    Q_PROPERTY(double minimumZoom READ minimumZoom NOTIFY viewportChanged)
    Q_PROPERTY(double maximumZoom READ maximumZoom CONSTANT)

    /** The selected track (0: none): its trail, halo and label take selectedColor. */
    Q_PROPERTY(qulonglong selectedTrackId READ selectedTrackId WRITE setSelectedTrackId NOTIFY selectedTrackIdChanged)
    /** The followed track (0: none): the camera centres on its current position as it moves. A pan stops it. */
    Q_PROPERTY(qulonglong followTrackId READ followTrackId WRITE setFollowTrackId NOTIFY followChanged)
    /** The camera follows the device's position. A pan stops it; following a track stops it. */
    Q_PROPERTY(bool followDevice READ followDevice WRITE setFollowDevice NOTIFY followChanged)

    /** The device's own position (spec 4.7): a dot with a faint accuracy circle, while valid. */
    Q_PROPERTY(bool deviceValid READ deviceValid WRITE setDeviceValid NOTIFY deviceChanged)
    Q_PROPERTY(double deviceLatitude READ deviceLatitude WRITE setDeviceLatitude NOTIFY deviceChanged)
    Q_PROPERTY(double deviceLongitude READ deviceLongitude WRITE setDeviceLongitude NOTIFY deviceChanged)
    Q_PROPERTY(double deviceAccuracyM READ deviceAccuracyM WRITE setDeviceAccuracyM NOTIFY deviceChanged)
    /** The camera has centred on or fitted the device's position, at any time in this item's life (spec 4.8: the
     *  saved viewport must not be derived from it, and every later view starts from one that was). */
    Q_PROPERTY(bool cameraUsedDevicePosition READ cameraUsedDevicePosition NOTIFY cameraUsedDevicePositionChanged)

    /** Markers drawn now (current positions and open rings the age filter shows). */
    Q_PROPERTY(int markerCount READ markerCount NOTIFY markerCountChanged)

    /** Colours (bind to Theme). */
    Q_PROPERTY(QColor markerColor READ markerColor WRITE setMarkerColor NOTIFY colorsChanged)
    Q_PROPERTY(QColor haloColor READ haloColor WRITE setHaloColor NOTIFY colorsChanged)
    Q_PROPERTY(QColor alertColor READ alertColor WRITE setAlertColor NOTIFY colorsChanged)
    Q_PROPERTY(QColor selectedColor READ selectedColor WRITE setSelectedColor NOTIFY colorsChanged)
    Q_PROPERTY(QColor trailColor READ trailColor WRITE setTrailColor NOTIFY colorsChanged)
    Q_PROPERTY(QColor accuracyColor READ accuracyColor WRITE setAccuracyColor NOTIFY colorsChanged)
    Q_PROPERTY(QColor deviceColor READ deviceColor WRITE setDeviceColor NOTIFY colorsChanged)
    Q_PROPERTY(QColor labelColor READ labelColor WRITE setLabelColor NOTIFY colorsChanged)
    Q_PROPERTY(QColor gridColor READ gridColor WRITE setGridColor NOTIFY colorsChanged)
    /** Tiles are drawn greyscale, their black at tileInkColor and their white at tilePaperColor. */
    Q_PROPERTY(QColor tileInkColor READ tileInkColor WRITE setTileInkColor NOTIFY colorsChanged)
    Q_PROPERTY(QColor tilePaperColor READ tilePaperColor WRITE setTilePaperColor NOTIFY colorsChanged)

    /** Sizes, logical pixels, and the label's font and emergency word. */
    Q_PROPERTY(double markerSize READ markerSize WRITE setMarkerSize NOTIFY styleChanged)
    Q_PROPERTY(double trailWidth READ trailWidth WRITE setTrailWidth NOTIFY styleChanged)
    Q_PROPERTY(double dashLength READ dashLength WRITE setDashLength NOTIFY styleChanged)
    Q_PROPERTY(double dashGap READ dashGap WRITE setDashGap NOTIFY styleChanged)
    Q_PROPERTY(QFont labelFont READ labelFont WRITE setLabelFont NOTIFY styleChanged)
    Q_PROPERTY(QString emergencyText READ emergencyText WRITE setEmergencyText NOTIFY styleChanged)
    /** Freshness: solid up to freshSeconds, faintest from fadedSeconds (and with no age). */
    Q_PROPERTY(int freshSeconds READ freshSeconds WRITE setFreshSeconds NOTIFY styleChanged)
    Q_PROPERTY(int fadedSeconds READ fadedSeconds WRITE setFadedSeconds NOTIFY styleChanged)

    /** Behaviour: tap radius, fit padding and zoom cap, and the camera animation (0: none; reduced motion). */
    Q_PROPERTY(double hitRadius READ hitRadius WRITE setHitRadius NOTIFY behaviourChanged)
    Q_PROPERTY(double fitPadding READ fitPadding WRITE setFitPadding NOTIFY behaviourChanged)
    Q_PROPERTY(double fitMaxZoom READ fitMaxZoom WRITE setFitMaxZoom NOTIFY behaviourChanged)
    /** The height at the top the screen covers with chips (logical px): fits and centring aim below it. */
    Q_PROPERTY(double topInset READ topInset WRITE setTopInset NOTIFY behaviourChanged)
    /** Rectangles (item coordinates) a label must keep clear of: the screen's controls and chips over the canvas. */
    Q_PROPERTY(QVariantList labelObstacles READ labelObstacles WRITE setLabelObstacles NOTIFY behaviourChanged)
    Q_PROPERTY(int cameraDurationMs READ cameraDurationMs WRITE setCameraDurationMs NOTIFY behaviourChanged)

  public:
    explicit MapViewItem(QQuickItem* parent = nullptr);
    ~MapViewItem() override;

    PositionModel*
    model() const {
        return m_model.data();
    }

    void setModel(PositionModel* next);

    MapTileWorker*
    tileSource() const {
        return m_tiles.data();
    }

    void setTileSource(MapTileWorker* next);

    bool
    active() const {
        return m_active;
    }

    void setActive(bool on);

    double centerLatitude() const;
    void setCenterLatitude(double lat);
    double centerLongitude() const;
    void setCenterLongitude(double lon);

    double
    zoom() const {
        return m_vp.zoom;
    }

    void setZoom(double level);
    double minimumZoom() const;

    static double
    maximumZoom() {
        return map_math::kMaxZoom;
    }

    qulonglong
    selectedTrackId() const {
        return m_selected;
    }

    void setSelectedTrackId(qulonglong id);

    qulonglong
    followTrackId() const {
        return m_followTrack;
    }

    void setFollowTrackId(qulonglong id);

    bool
    followDevice() const {
        return m_followDevice;
    }

    void setFollowDevice(bool on);

    bool
    deviceValid() const {
        return m_deviceValid;
    }

    void setDeviceValid(bool valid);

    double
    deviceLatitude() const {
        return m_deviceLat;
    }

    void setDeviceLatitude(double lat);

    double
    deviceLongitude() const {
        return m_deviceLon;
    }

    void setDeviceLongitude(double lon);

    double
    deviceAccuracyM() const {
        return m_deviceAccuracyM;
    }

    void setDeviceAccuracyM(double metres);

    bool
    cameraUsedDevicePosition() const {
        return m_usedDevice;
    }

    int
    markerCount() const {
        return m_markerCount;
    }

    // clang-format off
    QColor markerColor() const { return m_markerColor; }
    void setMarkerColor(const QColor& c);
    QColor haloColor() const { return m_haloColor; }
    void setHaloColor(const QColor& c);
    QColor alertColor() const { return m_alertColor; }
    void setAlertColor(const QColor& c);
    QColor selectedColor() const { return m_selectedColor; }
    void setSelectedColor(const QColor& c);
    QColor trailColor() const { return m_trailColor; }
    void setTrailColor(const QColor& c);
    QColor accuracyColor() const { return m_accuracyColor; }
    void setAccuracyColor(const QColor& c);
    QColor deviceColor() const { return m_deviceColor; }
    void setDeviceColor(const QColor& c);
    QColor labelColor() const { return m_labelColor; }
    void setLabelColor(const QColor& c);
    QColor gridColor() const { return m_gridColor; }
    void setGridColor(const QColor& c);
    QColor tileInkColor() const { return m_tileInk; }
    void setTileInkColor(const QColor& c);
    QColor tilePaperColor() const { return m_tilePaper; }
    void setTilePaperColor(const QColor& c);

    double markerSize() const { return m_markerSize; }
    void setMarkerSize(double size);
    double trailWidth() const { return m_trailWidth; }
    void setTrailWidth(double width);
    double dashLength() const { return m_dashLength; }
    void setDashLength(double length);
    double dashGap() const { return m_dashGap; }
    void setDashGap(double gap);
    QFont labelFont() const { return m_labelFont; }
    void setLabelFont(const QFont& font);
    QString emergencyText() const { return m_emergencyText; }
    void setEmergencyText(const QString& text);
    int freshSeconds() const { return m_freshS; }
    void setFreshSeconds(int seconds);
    int fadedSeconds() const { return m_fadedS; }
    void setFadedSeconds(int seconds);

    double hitRadius() const { return m_hitRadius; }
    void setHitRadius(double radius);
    double fitPadding() const { return m_fitPadding; }
    void setFitPadding(double padding);
    double fitMaxZoom() const { return m_fitMaxZoom; }
    void setFitMaxZoom(double level);
    double topInset() const { return m_topInset; }
    void setTopInset(double inset);
    QVariantList labelObstacles() const { return m_labelObstacleList; }
    void setLabelObstacles(const QVariantList& rects);
    int cameraDurationMs() const { return m_cameraMs; }
    void setCameraDurationMs(int ms);
    // clang-format on

    /** @brief Drag by (@p dx, @p dy) logical pixels: the map follows the finger. Stops following and the camera. */
    Q_INVOKABLE void panBy(double dx, double dy);
    /** @brief Zoom to @p level keeping the point under (@p x, @p y) there (pinch, wheel). While following, about the
     *  centre instead, so the followed point stays put. Immediate. */
    Q_INVOKABLE void zoomAbout(double level, double x, double y);
    /** @brief One whole zoom step in or out about the centre, animated (the accessible zoom buttons). */
    Q_INVOKABLE void zoomIn();
    Q_INVOKABLE void zoomOut();
    /** @brief Show every marker drawn, and the device's dot when valid and @p withDevice (the map's opening leaves out
     *  a saved fix it cannot trust), animated. False when there is nothing. */
    Q_INVOKABLE bool fitAll(bool withDevice = true);
    /** @brief fitAll() with (@p lat, @p lon) shown too: a pin the screen draws over the map (a History row's fix). */
    Q_INVOKABLE bool fitAllWith(double lat, double lon);
    /** @brief Centre on track @p trackId's marker (its current position, else its open ring), animated. */
    Q_INVOKABLE bool centerOnTrack(qulonglong trackId);
    /** @brief Centre on the device's position at deviceZoom() (Locate me, the map opening on the user, the List's own
     *  row), animated. False while it is not valid. */
    Q_INVOKABLE bool centerOnDevice();
    /** @brief The zoom centerOnDevice() goes in to: the streets around the dot (15 at most), with its accuracy circle
     *  about half the view's shorter side across, however wide an approximate fix's circle is. */
    Q_INVOKABLE double deviceZoom() const;
    /** @brief Centre on (@p lat, @p lon) at @p level (negative: keep the zoom), animated. */
    Q_INVOKABLE void centerOn(double lat, double lon, double level = -1.0);
    /** @brief The track whose marker a tap at (@p x, @p y) means, within hitRadius; 0 for none. */
    Q_INVOKABLE qulonglong trackAt(double x, double y) const;
    /** @brief Where track @p trackId's marker is drawn now (item coordinates); undefined when it is not drawn. */
    Q_INVOKABLE QVariant markerPoint(qulonglong trackId) const;
    /** @brief Where (@p lat, @p lon) is drawn now (item coordinates, the copy nearest the centre), for a pin the screen
     *  draws over the map (a History row's saved fix); undefined for a position out of range. */
    Q_INVOKABLE QVariant pointFor(double lat, double lon) const;
    /** @brief Forget that the device's position placed the camera, as a new item starts (tests begin each case from
     *  one). The screen never calls it: the view it keeps between visits still derives from the device. */
    Q_INVOKABLE void resetCameraUsedDevicePosition();
    /** @brief Centre a running follow's mark again at the user's zoom: below the chips as they are now (the screen calls
     *  it once they are laid out on return). Nothing without a follow. */
    Q_INVOKABLE void recentreFollow();

#ifdef DSD_NEO_TEST_HOOKS
    /** @brief The node tree updatePaintNode() builds on an RHI window, built in @p window without one (as the
     *  spectrum's test hook does). Pass the previous result back as @p old. */
    QSGNode* testUpdateGpuTree(QSGNode* old, double pixel_ratio);
    /** @brief The software path's picture. */
    QImage testSoftwareImage(double pixel_ratio);

    /** @brief Deliver a tile as the worker would (generation ignored); a null image withdraws it. */
    void
    testDeliverTile(int z, int x, int y, const QImage& image, bool reusable = true) {
        onTileReady(0, z, x, y, image, false, reusable);
    }

    /** @brief The serial of the held image of (@p z, @p x, @p y) (its texture's identity), 0 when none is held. */
    quint64
    testTileSerial(int z, int x, int y) const {
        const auto found = m_tileImages.constFind(MapTileKey{z, x, y});
        return found != m_tileImages.cend() ? found.value().serial : 0U;
    }

    /** @brief Keys of the tiles asked for last, in order. */
    QList<MapTileKey>
    testRequestedTiles() const {
        return m_requested;
    }

    /** @brief Run the polish step now (headless: no window does it). */
    void
    testPolish() {
        updatePolish();
    }

    /** @brief Scene rebuilds so far (a hidden canvas defers them). */
    quint64
    testSceneRebuilds() const {
        return m_sceneRebuilds;
    }

    /** @brief Whether the camera's animation is running. */
    bool testCameraMoving() const;

    /** @brief End the zoom in progress now, as its settle timer would. */
    void
    testSettleZoom() {
        m_zoomSettle.stop();
        zoomSettled();
    }

    bool
    testZooming() const {
        return m_zooming;
    }

    /** @brief Deliver an application state change as QGuiApplication would. */
    void
    testApplicationState(Qt::ApplicationState state) {
        onApplicationStateChanged(state);
    }

    int
    testHeldTiles() const {
        return static_cast<int>(m_tileImages.size());
    }

    /** @brief Label images rendered so far (on the GUI thread, ahead of any sync). */
    int
    testLabelImages() const {
        return static_cast<int>(m_labelImages.size());
    }
#endif

  Q_SIGNALS:
    void modelChanged();
    void tileSourceChanged();
    void activeChanged();
    void viewportChanged();
    void selectedTrackIdChanged();
    void followChanged();
    void deviceChanged();
    void cameraUsedDevicePositionChanged();
    void markerCountChanged();
    void colorsChanged();
    void styleChanged();
    void behaviourChanged();

  protected:
    QSGNode* updatePaintNode(QSGNode* old, UpdatePaintNodeData* data) override;
    void updatePolish() override;
    void geometryChange(const QRectF& new_geometry, const QRectF& old_geometry) override;
    void itemChange(ItemChange change, const ItemChangeData& value) override;

  public:
    /** @brief One track as the canvas draws it, in world coordinates (rebuilt on the GUI thread from the model). */
    struct TrackDraw {
        quint64 id = 0;
        std::vector<map_math::WorldPoint> trail; /**< oldest first; a non-finite point is a break */
        std::vector<uint8_t> dashedTo;           /**< per trail point: the segment ending there is dashed */
        bool hasMarker = false;
        map_math::WorldPoint marker;
        map_scene::MarkerStyle markerStyle;
        double errM = 0.0; /**< accuracy circle radius, metres; 0: none (unknown, or only a lower bound) */
        double latDeg = 0.0;
        bool hasRing = false;
        map_math::WorldPoint ring;
        map_scene::MarkerStyle ringStyle;
        QString label;
        QString labelKey;  /**< its image in the label cache: colour role and text */
        int labelRole = 0; /**< 0 labelColor, 1 selectedColor, 2 alertColor (an emergency outranks the selection) */
        bool emergency = false;
        bool selected = false;
        qint64 ageS = -1;     /**< of the current position when drawn, else of the ring; -1 unknown */
        qint64 ringAgeS = -1; /**< of the open ring; -1 unknown */
    };

    /** @brief A marker in draw order, for hit testing and the sprite pass. */
    struct MarkerDraw {
        map_math::WorldPoint at;
        int cell = 0;
        int track = 0; /**< index into m_scene */
    };

    /** @brief A label placed for this frame. */
    struct LabelDraw {
        QString key; /**< its image: text and colour */
        float left = 0.0F;
        float top = 0.0F;
        float width = 0.0F;
        float height = 0.0F;
    };

  private:
    class GpuRoot;

    /** @brief A held tile to draw, and where. */
    struct TileDraw {
        MapTileKey key;
        QImage image;
        quint64 serial = 0;
        map_scene::ScreenRect rect;
    };

    /** @brief A sprite to draw: an atlas cell centred on a screen point. */
    struct SpriteQuad {
        int cell = 0;
        double x = 0.0;
        double y = 0.0;
    };

    /** @brief An accuracy circle on screen. */
    struct Circle {
        double x = 0.0;
        double y = 0.0;
        double r = 0.0;
    };

    void onTracksChanged();
    void onAgesChanged();
    void onModelDestroyed();
    void onTileSourceDestroyed();
    void onTileReady(quint64 generation, int z, int x, int y, const QImage& image, bool stale, bool reusable);
    void onTileSettingsChanged();
    void onApplicationStateChanged(Qt::ApplicationState state);
    void rebuildScene();
    bool canvasShown() const;
    void ensureScene();
    void catchUpScene();
    void orderLabels();
    void sceneChanged();
    bool assignColor(QColor& member, const QColor& next);
    bool assignSize(double& member, double next);
    void setViewport(const map_math::Viewport& next);
    void zoomSettled();
    void moveCamera(const map_math::Viewport& target);
    void stopCamera();
    void stopFollowing();
    void followNow();
    void markUsedDevice();
    bool deviceWorld(map_math::WorldPoint* out) const;
    bool tilesWanted() const;
    void dropTileRequests();
    /** @brief Whether the view now comes from the device's position (the one place the tile requests read it). */
    bool tilesFromDevice() const;
    void releaseTiles();
    void keepStandIns(const QSet<MapTileKey>& inView, bool complete);
    /** @brief Whether every key in view is held or answered missing (drops misses out of view or of another source). */
    bool tilesSettled(const QSet<MapTileKey>& inView);
    void dropUnreusableTiles(const QSet<MapTileKey>& inView);
    map_math::Viewport cameraBase() const;
    /** @brief itemChange(): the canvas left the screen or its window. */
    void hidden();
    /** @brief itemChange(): the canvas is shown again. */
    void shownAgain();
    void retoneTiles();
    QImage toneTile(const QImage& image) const;
    map_math::Viewport viewportForSize() const;

    std::vector<map_math::WorldPoint> markerPoints() const;
    /** @brief Fit @p points and the device's dot when valid (fitAll()'s body). */
    bool fitPoints(std::vector<map_math::WorldPoint> points, bool takeDevice = true);
    /** @brief @p vp with its centre moved so what it centres sits in the middle of the part below topInset. */
    map_math::Viewport belowTopInset(map_math::Viewport vp) const;
    map_math::Viewport centredOn(map_math::Viewport vp, map_math::WorldPoint at, bool raiseZoom = true) const;
    bool following() const;
    QPointF followAnchor(const map_math::Viewport& base) const;
    void zoomStep(double steps);
    void rearmFollow();
    bool followTarget(map_math::WorldPoint* at);
    /** @brief Where belowTopInset() puts a centred point on screen: the zoom buttons' and a followed zoom's anchor. */
    QPointF focusPoint() const;
    const TrackDraw* findTrack(qulonglong trackId) const;
    std::vector<TileDraw> tileDraws(double ratio) const;
    const std::vector<SpriteQuad>& spriteQuads();
    const std::vector<Circle>& circles();
    void wrapCopies(const map_math::Viewport& vp, std::vector<long long>& out) const;
    template <typename Visit>
    void forEachTrail(Visit&& visit);
    void buildAtlas(double ratio);
    void buildLabels(double ratio);

    /** @brief Where a track's label stands, and the age of the mark it stands by (its priority among equals). */
    struct LabelSlot {
        int scene = 0;
        map_math::ScreenPoint at;
        qint64 ageS = -1;
        int rank = 0;
    };

    LabelSlot labelAnchor(const TrackDraw& t) const;
    void prepareLabels();
    void ensureLabelCache(double ratio);
    const QImage& labelImage(const TrackDraw& t, double ratio);
    QString plateLabelKey(const TrackDraw& t, double ratio);
    const QColor& labelColorFor(int role) const;

    QSGNode* updateGpuTree(QSGNode* old, QQuickWindow* win, double ratio);
    void gpuGrid(GpuRoot* root, double ratio);
    void gpuTiles(GpuRoot* root, QQuickWindow* win, double ratio);
    void gpuOverlay(GpuRoot* root, double ratio);
    void gpuSprites(GpuRoot* root, QQuickWindow* win, double ratio);
    void gpuLabels(GpuRoot* root, QQuickWindow* win, double ratio);
    QImage paintSoftwareImage(double ratio);

    QPointer<PositionModel> m_model;
    QPointer<MapTileWorker> m_tiles;
    bool m_active = true;
    bool m_appActive = true;
    map_math::Viewport m_vp;
    /* The view last asked for (setViewport()), before clamping to the canvas: a size change clamps from it again. */
    map_math::Viewport m_asked;
    /* A follow's camera move was cut short by the canvas hiding: its next step moves even to the same mark. */
    bool m_followCatchUp = false;
    /* The follow's latest step was its first centring (one cut short is done again as a first). */
    bool m_followStepFirst = false;
    /* The camera move running is a follow step's (not a zoom button's). */
    bool m_animFollow = false;
    /* The canvas's size when it hid: a different one on return (a rotation) re-centres a follow. */
    QSizeF m_hiddenSize;
    qulonglong m_selected = 0;
    qulonglong m_followTrack = 0;
    bool m_followDevice = false;
    /* What followNow() last centred: it moves the camera only when the target changes (or rearmFollow()). */
    bool m_followedSet = false;
    map_math::WorldPoint m_followedAt;
    bool m_deviceValid = false;
    double m_deviceLat = 0.0;
    double m_deviceLon = 0.0;
    double m_deviceAccuracyM = 0.0;
    bool m_usedDevice = false;
    int m_markerCount = 0;

    QColor m_markerColor = QColor(0xE6, 0xEA, 0xF2);
    QColor m_haloColor = QColor(0x0E, 0x11, 0x16);
    QColor m_alertColor = QColor(0xFF, 0xAC, 0x88);
    QColor m_selectedColor = QColor(0x22, 0xDC, 0xF5);
    QColor m_trailColor = QColor(0x9A, 0xA5, 0xBD);
    QColor m_accuracyColor = QColor(0x9A, 0xA5, 0xBD, 0x26);
    QColor m_deviceColor = QColor(0x6E, 0xA8, 0xFF);
    QColor m_labelColor = QColor(0xE6, 0xEA, 0xF2);
    QColor m_gridColor = QColor(0x23, 0x2B, 0x3A);
    QColor m_tileInk = QColor(0x6B, 0x76, 0x90);
    QColor m_tilePaper = QColor(0x12, 0x16, 0x1E);
    double m_markerSize = 16.0;
    double m_trailWidth = 2.0;
    double m_dashLength = 6.0;
    double m_dashGap = 4.0;
    QFont m_labelFont;
    QString m_emergencyText = QStringLiteral("Emergency");
    int m_freshS = 120;
    int m_fadedS = 900;
    double m_hitRadius = 24.0;
    double m_fitPadding = 48.0;
    double m_fitMaxZoom = 16.0;
    double m_topInset = 0.0;
    int m_cameraMs = 250;

    /* The scene, in world coordinates: rebuilt when the model's tracks, ages or the selection change. */
    std::vector<TrackDraw> m_scene;
    std::vector<MarkerDraw> m_markers; /* draw order: last on top */
    quint64 m_sceneSerial = 1;
    bool m_sceneStale = false; /* tracks or style changed while the canvas was hidden: build at the next show */
    quint64 m_sceneRebuilds = 0; /* for tests */ /* bumped by every change that alters trails, circles or colours */
    quint64 m_atlasSerial = 1;                   /* bumped by every change to the sprites' look */
    quint64 m_labelSerial = 1;                   /* bumped by every change to the labels' look */

    /* The camera's animation, from m_animFrom to m_animTo. */
    QVariantAnimation* m_camera = nullptr;
    /* A zoom in progress: trails and circles are scaled, not rebuilt, until the zoom holds still this long. */
    static constexpr int kZoomSettleMs = 150;
    QTimer m_zoomSettle;
    bool m_zooming = false;
    map_math::Viewport m_animFrom;
    map_math::Viewport m_animTo;

    /* Tiles: the keys asked for last, and the toned images held (only while in view, or standing in). */
    QList<MapTileKey> m_requested;
    /* The origin m_requested was asked with: a change asks again, so a device-placed view never rides a hand one. */
    bool m_requestedFromDevice = false;
    QList<map_math::VisibleTile> m_visible;

    struct HeldTile {
        QImage source; /* as delivered, for re-toning on a colour change */
        QImage toned;
        quint64 serial = 0;
        bool current = false; /* at the view's tile zoom; else standing in */
        /* The cache rules allow showing it again (MapTileWorker::tileReady): if not, it goes when it leaves the view
         * and never stands in or comes back. */
        bool reusable = true;
    };

    QHash<MapTileKey, HeldTile> m_tileImages;
    /* Keys in view the server has no tile for (withdrawn): settled, so no stand-in covers them; for one source. */
    QSet<MapTileKey> m_tileMissing;
    QString m_tileMissingSource;
    quint64 m_tileSerial = 0;

    /* Sync-time buffers, reused (the GUI thread is blocked while they are used). */
    QImage m_atlas;
    double m_atlasRatio = 0.0;
    quint64 m_atlasBuilt = 0;   /* m_atlasSerial it was drawn at */
    quint64 m_atlasVersion = 0; /* bumped by every redraw of it */
    int m_atlasCellPx = 0;
    std::vector<LabelDraw> m_labels;
    std::vector<int> m_labelOrder; /* scene indices of the labelled tracks, highest priority first */
    std::vector<LabelSlot> m_labelSlots;
    std::vector<map_scene::LabelRequest> m_labelRequests;
    std::vector<map_scene::LabelBox> m_labelBoxes;
    map_scene::LabelSpace m_labelSpace;
    QVariantList m_labelObstacleList;
    std::vector<map_scene::LabelBox> m_labelObstacles;
    std::vector<QString> m_labelKeys;
    std::vector<uint8_t> m_labelKeep;
    QSet<QString> m_usedLabels;
    QHash<QString, QImage> m_labelImages;
    double m_labelRatio = 0.0;
    quint64 m_labelBuilt = 0;   /* m_labelSerial the images were drawn at */
    quint64 m_labelVersion = 0; /* bumped by every clear of them */
    std::vector<float> m_xs;
    std::vector<float> m_ys;
    map_scene::TrailRuns m_runs;
    std::vector<SpriteQuad> m_quads;
    std::vector<Circle> m_circles;
    std::vector<long long> m_copies;
    std::vector<map_geometry::StrokeVertex> m_discs;
    std::vector<map_geometry::StrokeVertex> m_plainTrails;
    std::vector<map_geometry::StrokeVertex> m_chosenTrails;
    std::vector<map_geometry::SpriteVertex> m_sprites;
};

} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_MAP_VIEW_ITEM_H_ */
