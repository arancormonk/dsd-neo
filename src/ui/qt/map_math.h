// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Web Mercator arithmetic for the map view: positions, viewport, tiles, taps and fitting.
 *
 * Qt-free and header-only, like spectrum_math.h, so the parts that are easy to
 * get subtly wrong — which tile covers a corner of the screen, which side of
 * the antimeridian a radio is drawn on, which marker a tap means, what zoom
 * shows every marker — are testable without a window or a scene graph.
 *
 * Coordinates:
 *  - A position arrives as fixed point, degrees x 1e7 (dsd_position_fix::lat_e7
 *    and lon_e7), and is checked before it is converted.
 *  - The world is the unit square: x = 0 at 180 degrees west, growing east and
 *    wrapping at 1; y = 0 at the north edge of the projection (85.0511 degrees
 *    north), 1 at the south edge. Latitudes beyond the edge are clamped to it.
 *  - At zoom z the world is 256 * 2^z logical pixels square. The zoom is
 *    fractional; tiles come from the nearest whole zoom and are scaled to fit.
 *  - Screen coordinates are logical pixels, y down, origin at the view's top
 *    left. The world repeats east and west; a point is drawn once, at the copy
 *    nearest the view's centre (screen_from_world()), and taps test that copy.
 *    The view is never wider than the world (clamp_viewport() raises the zoom
 *    until the world fills it), so that copy is the only one on screen.
 *  - Every function taking a Viewport expects one from clamp_viewport(); every
 *    function here that returns one has already clamped it. Tiles and points
 *    placed with the same clamped viewport line up.
 *  - A non-finite world point is a break in a track: project_track() passes it
 *    through as non-finite screen coordinates, which map_geometry.h's
 *    append_trail() splits on. Fitting skips such points.
 */

#ifndef DSD_NEO_SRC_UI_QT_MAP_MATH_H_
#define DSD_NEO_SRC_UI_QT_MAP_MATH_H_

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stddef.h>
#include <vector>

namespace dsd_qt {
namespace map_math {

/** @brief Edge of a map tile, in pixels. */
constexpr double kTileSize = 256.0;

/** @brief Zoom range the view allows; tiles exist for every whole zoom in it. */
constexpr double kMinZoom = 0.0;
constexpr double kMaxZoom = 19.0;
constexpr int kMaxTileZoom = 19;

/** @brief The projection's north and south edge: atan(sinh(pi)), 85.0511 degrees. */
constexpr double kMaxLatitudeDeg = 85.05112877980659;

/** @brief Fixed-point ranges of a position, degrees x 1e7. */
constexpr int32_t kMaxLatE7 = 900000000;
constexpr int32_t kMaxLonE7 = 1800000000;

/** @brief WGS 84 equatorial circumference, metres: the scale the projection is drawn at. */
constexpr double kEarthCircumferenceM = 40075016.68557849;

constexpr double kPi = 3.14159265358979323846;

/** @brief A point in the unit world square (see the file comment). */
struct WorldPoint {
    double x = 0.0;
    double y = 0.0;
};

/** @brief A point on screen, logical pixels. */
struct ScreenPoint {
    double x = 0.0;
    double y = 0.0;
};

/** @brief A position in degrees. */
struct LatLon {
    double lat = 0.0;
    double lon = 0.0;
};

/** @brief A position in degrees x 1e7. */
struct LatLonE7 {
    int32_t lat_e7 = 0;
    int32_t lon_e7 = 0;
};

/**
 * @brief What the view shows: the world point at its centre, the zoom, and its size.
 *
 * Keep it normalised through clamp_viewport(); every function here that
 * returns a viewport does.
 */
struct Viewport {
    double center_x = 0.5;
    double center_y = 0.5;
    double zoom = 0.0;
    double width = 0.0;  /**< logical pixels */
    double height = 0.0; /**< logical pixels */
};

/** @brief Clamp a requested zoom into the supported range; NaN becomes the minimum. */
inline double
clamp_zoom(double zoom) {
    if (!(zoom > kMinZoom)) { // also catches NaN
        return kMinZoom;
    }
    return (zoom > kMaxZoom) ? kMaxZoom : zoom;
}

/**
 * @brief The whole zoom whose tiles draw a view at @p zoom: the nearest one.
 *
 * Rounding, not flooring: a tile is drawn between 0.71x and 1.41x its own size,
 * rather than up to 2x, so it is never blurred by more than half a zoom step.
 */
inline int
tile_zoom(double zoom) {
    const int z = static_cast<int>(std::floor(clamp_zoom(zoom) + 0.5));
    return (z > kMaxTileZoom) ? kMaxTileZoom : z;
}

/** @brief Edge of the world at @p zoom, logical pixels. */
inline double
world_size_px(double zoom) {
    return kTileSize * std::exp2(clamp_zoom(zoom));
}

/** @brief @p x moved into [0, 1) by whole worlds; non-finite becomes 0.5. */
inline double
wrap_x(double x) {
    if (!std::isfinite(x)) {
        return 0.5;
    }
    const double w = x - std::floor(x);
    return (w >= 1.0) ? 0.0 : w; /* floor of a value a hair below an integer */
}

/** @brief A latitude clamped to the projection's edges; NaN becomes 0. */
inline double
clamp_latitude(double lat_deg) {
    if (!(lat_deg > -kMaxLatitudeDeg)) {
        return std::isnan(lat_deg) ? 0.0 : -kMaxLatitudeDeg;
    }
    return (lat_deg > kMaxLatitudeDeg) ? kMaxLatitudeDeg : lat_deg;
}

/** @brief Whether a fixed-point position is inside |lat| <= 90, |lon| <= 180 degrees. */
inline bool
lat_lon_e7_in_range(int32_t lat_e7, int32_t lon_e7) {
    return lat_e7 >= -kMaxLatE7 && lat_e7 <= kMaxLatE7 && lon_e7 >= -kMaxLonE7 && lon_e7 <= kMaxLonE7;
}

/** @brief The world point of a position in degrees; latitude clamped, longitude wrapped. */
inline WorldPoint
world_from_lat_lon(double lat_deg, double lon_deg) {
    const double lat_rad = clamp_latitude(lat_deg) * (kPi / 180.0);
    WorldPoint p;
    p.x = wrap_x((lon_deg + 180.0) / 360.0);
    p.y = (1.0 - (std::asinh(std::tan(lat_rad)) / kPi)) / 2.0;
    p.y = std::min(1.0, std::max(0.0, p.y));
    return p;
}

/**
 * @brief The world point of a fixed-point position.
 *
 * @return false, leaving @p out alone, when the position is out of range
 *         (lat_lon_e7_in_range()); nothing is clamped into range silently.
 */
inline bool
world_from_e7(int32_t lat_e7, int32_t lon_e7, WorldPoint* out) {
    if (out == nullptr || !lat_lon_e7_in_range(lat_e7, lon_e7)) {
        return false;
    }
    *out = world_from_lat_lon(static_cast<double>(lat_e7) / 1.0e7, static_cast<double>(lon_e7) / 1.0e7);
    return true;
}

/** @brief The position of a world point, degrees; x wrapped, y clamped to the world. */
inline LatLon
lat_lon_from_world(WorldPoint p) {
    const double y = std::isfinite(p.y) ? std::min(1.0, std::max(0.0, p.y)) : 0.5;
    LatLon out;
    out.lon = (wrap_x(p.x) * 360.0) - 180.0;
    out.lat = std::atan(std::sinh(kPi * (1.0 - (2.0 * y)))) * (180.0 / kPi);
    return out;
}

/** @brief The position of a world point, degrees x 1e7, rounded to the nearest unit. */
inline LatLonE7
e7_from_world(WorldPoint p) {
    const LatLon d = lat_lon_from_world(p);
    LatLonE7 out;
    out.lat_e7 = static_cast<int32_t>(std::llround(d.lat * 1.0e7));
    const long long lon = std::llround(d.lon * 1.0e7);
    out.lon_e7 = static_cast<int32_t>(std::min<long long>(kMaxLonE7, std::max<long long>(-kMaxLonE7, lon)));
    return out;
}

/**
 * @brief Ground distance one logical pixel spans at @p lat_deg and @p zoom, metres.
 *
 * Mercator stretches the map by 1/cos(latitude), so a pixel covers less ground
 * away from the equator.
 */
inline double
metres_per_px(double lat_deg, double zoom) {
    const double lat_rad = clamp_latitude(lat_deg) * (kPi / 180.0);
    return (kEarthCircumferenceM * std::cos(lat_rad)) / world_size_px(zoom);
}

/** @brief A ground distance at @p lat_deg drawn at @p zoom, logical pixels; 0 for a non-positive or NaN distance. */
inline double
metres_to_px(double metres, double lat_deg, double zoom) {
    if (!(metres > 0.0)) {
        return 0.0;
    }
    return metres / metres_per_px(lat_deg, zoom);
}

/** @brief The zoom at which the world is exactly @p width logical pixels wide; kMinZoom for a view narrower than 256. */
inline double
min_zoom_for_width(double width) {
    if (!(width > kTileSize)) {
        return kMinZoom;
    }
    return clamp_zoom(std::log2(width / kTileSize));
}

/**
 * @brief A viewport made valid: zoom clamped, centre x wrapped, centre y kept so the world fills the view vertically.
 *
 * The zoom is also raised until the world is at least as wide as the view, so
 * no copy of the world beside the one drawn is ever on screen: each point and
 * each tile appears once (a tile cut by the view's two side edges is the
 * exception, listed at each edge).
 *
 * North and south there is nothing past the projection's edge, so the centre
 * stops where the edge meets the view's edge; when the whole world is shorter
 * than the view, it sits in the middle. A non-finite centre becomes the world's
 * centre and a negative or non-finite size becomes 0.
 */
inline Viewport
clamp_viewport(Viewport vp) {
    vp.width = (vp.width > 0.0 && std::isfinite(vp.width)) ? vp.width : 0.0;
    vp.height = (vp.height > 0.0 && std::isfinite(vp.height)) ? vp.height : 0.0;
    vp.zoom = std::max(clamp_zoom(vp.zoom), min_zoom_for_width(vp.width));
    vp.center_x = wrap_x(vp.center_x);
    if (!std::isfinite(vp.center_y)) {
        vp.center_y = 0.5;
    }
    const double world = world_size_px(vp.zoom);
    const double half = (vp.height / 2.0) / world;
    if (half >= 0.5) {
        vp.center_y = 0.5;
    } else {
        vp.center_y = std::min(1.0 - half, std::max(half, vp.center_y));
    }
    return vp;
}

/** @brief @p dx moved by whole worlds into [-0.5, 0.5): the shortest way east or west. */
inline double
nearest_wrap_delta(double dx) {
    return dx - std::floor(dx + 0.5);
}

/** @brief Where a world point is drawn: the copy nearest the view's centre. @p vp as clamp_viewport() returns it. */
inline ScreenPoint
screen_from_world(const Viewport& vp, WorldPoint p) {
    const double world = world_size_px(vp.zoom);
    ScreenPoint s;
    s.x = (vp.width / 2.0) + (nearest_wrap_delta(p.x - vp.center_x) * world);
    s.y = (vp.height / 2.0) + ((p.y - vp.center_y) * world);
    return s;
}

/** @brief The world point under a screen point, x not wrapped and y not clamped (see world_from_screen()). */
inline WorldPoint
world_from_screen_raw(const Viewport& vp, double sx, double sy) {
    const double world = world_size_px(vp.zoom);
    WorldPoint p;
    p.x = vp.center_x + ((sx - (vp.width / 2.0)) / world);
    p.y = vp.center_y + ((sy - (vp.height / 2.0)) / world);
    return p;
}

/** @brief The world point under a screen point, x wrapped and y clamped to the world. */
inline WorldPoint
world_from_screen(const Viewport& vp, double sx, double sy) {
    WorldPoint p = world_from_screen_raw(vp, sx, sy);
    p.x = wrap_x(p.x);
    p.y = std::isfinite(p.y) ? std::min(1.0, std::max(0.0, p.y)) : 0.5;
    return p;
}

/** @brief The view dragged by @p dx, @p dy logical pixels: the content follows the finger. */
inline Viewport
pan(Viewport vp, double dx, double dy) {
    const double world = world_size_px(vp.zoom);
    if (std::isfinite(dx)) {
        vp.center_x -= dx / world;
    }
    if (std::isfinite(dy)) {
        vp.center_y -= dy / world;
    }
    return clamp_viewport(vp);
}

/**
 * @brief The view at @p new_zoom with the world point under (@p ax, @p ay) still under it.
 *
 * A pinch or a wheel step that slides the map out from under the fingers reads
 * as broken. The zoom stops at the same range clamp_viewport() allows, its
 * floor included (the world filling the view's width), and the anchor holds
 * there too. Near the north or south edge the clamp may still move the anchor:
 * there is nothing past the edge to keep it over.
 */
inline Viewport
zoom_about(Viewport vp, double new_zoom, double ax, double ay) {
    vp = clamp_viewport(vp);
    if (!std::isfinite(ax) || !std::isfinite(ay)) {
        ax = vp.width / 2.0;
        ay = vp.height / 2.0;
    }
    const WorldPoint anchor = world_from_screen_raw(vp, ax, ay);
    /* The same floor clamp_viewport() applies, before the centre is solved: raised after, it would move the
     * anchor. */
    vp.zoom = std::max(clamp_zoom(new_zoom), min_zoom_for_width(vp.width));
    const double world = world_size_px(vp.zoom);
    vp.center_x = anchor.x - ((ax - (vp.width / 2.0)) / world);
    vp.center_y = anchor.y - ((ay - (vp.height / 2.0)) / world);
    return clamp_viewport(vp);
}

/** @brief Most tiles visible_tiles() lists: a view needing more (a size no screen has) gets none. */
constexpr long long kMaxVisibleTiles = 1024;

/** @brief One tile the view needs and where to draw it. */
struct VisibleTile {
    int z = 0;
    int x = 0;         /**< wrapped into [0, 2^z) */
    int y = 0;         /**< within [0, 2^z) */
    double left = 0.0; /**< screen rectangle, logical pixels */
    double top = 0.0;
    double size = 0.0;
};

/** @brief A range of whole tile indices, inclusive; empty when hi < lo. */
struct TileSpan {
    long long lo;
    long long hi;
};

/** @brief Tile indices whose cells overlap [@p start, @p start + @p length) at cell edge @p cell. */
inline TileSpan
tile_span(double start, double length, double cell) {
    TileSpan span{0, -1};
    if (!(length > 0.0) || !(cell > 0.0) || !std::isfinite(start)) {
        return span;
    }
    span.lo = static_cast<long long>(std::floor(start / cell));
    /* The last cell is the one holding the last covered pixel, not the one
     * starting at the far edge: a view that ends on a tile edge needs no
     * tile past it. */
    span.hi = static_cast<long long>(std::ceil((start + length) / cell)) - 1;
    return span;
}

/**
 * @brief Every tile the view shows at its tile zoom, nearest the view's centre first.
 *
 * Tiles come from tile_zoom(), scaled to the view's fractional zoom. East and
 * west the world wraps: a view showing 180 degrees lists the tiles either side
 * of it, and one the width of the whole world can list the tile its two side
 * edges cut at each edge, with its own rectangle each time (a fetcher keys on
 * z, x, y and fetches it once). North and south nothing past the edge is
 * listed. Empty for an empty view, or one needing more than kMaxVisibleTiles.
 * The order is the fetch order: the centre of the view fills in first.
 */
inline std::vector<VisibleTile>
visible_tiles(const Viewport& view) {
    std::vector<VisibleTile> tiles;
    const Viewport vp = clamp_viewport(view);
    if (!(vp.width > 0.0) || !(vp.height > 0.0)) {
        return tiles;
    }
    const int tz = tile_zoom(vp.zoom);
    const long long count = 1LL << tz;
    const double world = world_size_px(vp.zoom);
    const double cell = world / static_cast<double>(count);
    /* Checked before any cast: a size no screen has would overflow the tile indices. */
    if ((vp.width / cell) + 2.0 > static_cast<double>(kMaxVisibleTiles)
        || (vp.height / cell) + 2.0 > static_cast<double>(kMaxVisibleTiles)) {
        return tiles;
    }
    const double left = (vp.center_x * world) - (vp.width / 2.0);
    const double top = (vp.center_y * world) - (vp.height / 2.0);
    const TileSpan xs = tile_span(left, vp.width, cell);
    TileSpan ys = tile_span(top, vp.height, cell);
    ys.lo = std::max(0LL, ys.lo);
    ys.hi = std::min(count - 1, ys.hi);
    if (xs.hi < xs.lo || ys.hi < ys.lo || ((xs.hi - xs.lo) + 1) * ((ys.hi - ys.lo) + 1) > kMaxVisibleTiles) {
        return tiles;
    }
    for (long long ty = ys.lo; ty <= ys.hi; ty++) {
        for (long long tx = xs.lo; tx <= xs.hi; tx++) {
            VisibleTile t;
            t.z = tz;
            t.x = static_cast<int>(((tx % count) + count) % count);
            t.y = static_cast<int>(ty);
            t.left = (static_cast<double>(tx) * cell) - left;
            t.top = (static_cast<double>(ty) * cell) - top;
            t.size = cell;
            tiles.push_back(t);
        }
    }
    const double cx = vp.width / 2.0;
    const double cy = vp.height / 2.0;
    const auto distance = [cx, cy](const VisibleTile& t) {
        const double dx = (t.left + (t.size / 2.0)) - cx;
        const double dy = (t.top + (t.size / 2.0)) - cy;
        return (dx * dx) + (dy * dy);
    };
    std::stable_sort(tiles.begin(), tiles.end(),
                     [&distance](const VisibleTile& a, const VisibleTile& b) { return distance(a) < distance(b); });
    return tiles;
}

/**
 * @brief A track's points on screen, kept continuous across the antimeridian.
 *
 * The last usable point, the track's current position, is placed where its
 * marker is drawn (screen_from_world()), so the trail ends at the marker.
 * Every earlier point takes the copy nearest the usable point after it: a radio
 * crossing 180 degrees draws a short step east, not a line back across the
 * whole world.
 *
 * A point with a non-finite x or y is a break: it is written as NaN (both
 * coordinates), which append_trail() splits the trail on, and the points on
 * either side keep the places they would have without it.
 *
 * @param vp    As clamp_viewport() returns it.
 * @param xs,ys At least @p count entries each.
 */
inline void
project_track(const Viewport& vp, const WorldPoint* points, int count, float* xs, float* ys) {
    if (points == nullptr || xs == nullptr || ys == nullptr || count <= 0) {
        return;
    }
    const double world = world_size_px(vp.zoom);
    bool placed = false;
    double screen_x = 0.0;
    double next_world_x = 0.0;
    for (int i = count - 1; i >= 0; i--) {
        if (!std::isfinite(points[i].x) || !std::isfinite(points[i].y)) {
            xs[i] = std::numeric_limits<float>::quiet_NaN();
            ys[i] = std::numeric_limits<float>::quiet_NaN();
            continue;
        }
        if (placed) {
            screen_x += nearest_wrap_delta(points[i].x - next_world_x) * world;
        } else {
            screen_x = screen_from_world(vp, points[i]).x;
            placed = true;
        }
        next_world_x = points[i].x;
        xs[i] = static_cast<float>(screen_x);
        ys[i] = static_cast<float>((vp.height / 2.0) + ((points[i].y - vp.center_y) * world));
    }
}

/**
 * @brief The marker a tap at (@p sx, @p sy) means, or -1.
 *
 * The nearest marker whose centre is within @p radius logical pixels; between
 * markers at the same distance the later one wins, because it is drawn on
 * top. Markers are tested where screen_from_world() draws them, with @p vp as
 * clamp_viewport() returns it; a marker with a non-finite coordinate is never
 * hit.
 */
inline int
hit_test(const Viewport& vp, const WorldPoint* markers, int count, double sx, double sy, double radius) {
    if (markers == nullptr || count <= 0 || !(radius > 0.0) || !std::isfinite(sx) || !std::isfinite(sy)) {
        return -1;
    }
    int best = -1;
    double best_d2 = radius * radius;
    for (int i = 0; i < count; i++) {
        const ScreenPoint s = screen_from_world(vp, markers[i]);
        const double d2 = ((s.x - sx) * (s.x - sx)) + ((s.y - sy) * (s.y - sy));
        if (d2 <= best_d2) {
            best = i;
            best_d2 = d2;
        }
    }
    return best;
}

/** @brief The shortest east-west arc holding every x: its west end and its width, both in world units. */
struct XArc {
    double west;
    double width;
};

/** @brief Whether a world point can be placed: both coordinates finite. A non-finite one is a break, not a place. */
inline bool
placeable(WorldPoint p) {
    return std::isfinite(p.x) && std::isfinite(p.y);
}

/**
 * @brief The shortest arc of the wrapped x axis that holds every placeable point.
 *
 * Sorted around the circle, the points leave gaps between neighbours; the arc
 * is everything but the widest gap. Markers on both sides of 180 degrees are a
 * short arc across it, not the whole world between them. Points that are not
 * placeable() are skipped, never moved somewhere.
 *
 * @return false, leaving @p out alone, when no point is placeable.
 */
inline bool
shortest_x_arc(const WorldPoint* points, int count, XArc* out) {
    if (points == nullptr || out == nullptr || count <= 0) {
        return false;
    }
    std::vector<double> xs;
    xs.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; i++) {
        if (placeable(points[i])) {
            xs.push_back(wrap_x(points[i].x));
        }
    }
    if (xs.empty()) {
        return false;
    }
    std::sort(xs.begin(), xs.end());
    /* The gap that wraps from the last point round to the first. */
    double widest = (xs.front() + 1.0) - xs.back();
    double west = xs.front();
    for (size_t i = 1; i < xs.size(); i++) {
        const double gap = xs[i] - xs[i - 1U];
        if (gap > widest) {
            widest = gap;
            west = xs[i];
        }
    }
    *out = XArc{west, std::max(0.0, 1.0 - widest)};
    return true;
}

/** @brief The zoom at which @p span world units fill @p avail pixels; @p fallback for a span too small to measure. */
inline double
zoom_to_fill(double span, double avail, double fallback) {
    if (!(span > 1.0e-12)) {
        return fallback;
    }
    return std::log2(avail / (span * kTileSize));
}

/**
 * @brief The view that shows every point with @p padding pixels to spare, as close as @p max_zoom allows.
 *
 * Fit-to-markers: the shortest arc east-west (shortest_x_arc()), the full
 * north-south extent, and the largest zoom at which both fit inside the view
 * less the padding. One point, or points too close to tell apart, centre at
 * @p max_zoom. The zoom never goes below the one where the world fills the
 * view's width (clamp_viewport()). The size of @p view is kept; its centre and
 * zoom are replaced.
 *
 * Points that are not placeable() (a track's breaks) are skipped.
 *
 * @return false, leaving @p out alone, when there is no placeable point or no room.
 */
inline bool
fit_points(const Viewport& view, const WorldPoint* points, int count, double padding, double max_zoom, Viewport* out) {
    if (out == nullptr || points == nullptr || count <= 0 || !(view.width > 0.0) || !(view.height > 0.0)) {
        return false;
    }
    XArc arc{0.0, 0.0};
    if (!shortest_x_arc(points, count, &arc)) {
        return false;
    }
    double north = 1.0;
    double south = 0.0;
    for (int i = 0; i < count; i++) {
        if (placeable(points[i])) {
            const double y = std::min(1.0, std::max(0.0, points[i].y));
            north = std::min(north, y);
            south = std::max(south, y);
        }
    }
    const double pad = (padding > 0.0) ? padding : 0.0;
    const double avail_w = std::max(1.0, view.width - (2.0 * pad));
    const double avail_h = std::max(1.0, view.height - (2.0 * pad));
    const double cap = clamp_zoom(max_zoom);
    const double zx = zoom_to_fill(arc.width, avail_w, cap);
    const double zy = zoom_to_fill(south - north, avail_h, cap);
    Viewport vp = view;
    vp.zoom = std::min(cap, std::min(zx, zy));
    vp.center_x = arc.west + (arc.width / 2.0);
    vp.center_y = (north + south) / 2.0;
    *out = clamp_viewport(vp);
    return true;
}

} // namespace map_math
} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_MAP_MATH_H_ */
