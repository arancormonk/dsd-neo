// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Regression test: the map's Web Mercator arithmetic (map_math.h).
 *
 * These are the parts of a map that are wrong in ways nobody notices until a
 * radio is drawn in the wrong place: which tile covers a corner of the screen
 * at a fractional zoom, which side of 180 degrees a point is drawn on, whether
 * a pinch keeps the place under the fingers, which marker a tap means, and
 * whether "fit all" really shows every marker. The tile numbers are checked
 * against the published slippy-map formula, computed independently here.
 * Positions are made up.
 */

// LLVM 22/GCC 16 misclassifies these runtime test oracles as compile-time assertions.
// NOLINTBEGIN(cert-dcl03-c,misc-static-assert)

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <vector>

#include <dsd-neo/core/safe_api.h>

#include "map_math.h"

namespace mm = dsd_qt::map_math;

namespace {

constexpr double kTol = 1.0e-9;
constexpr double kPxTol = 1.0e-6;

bool
near(double got, double want, double tol) {
    return std::fabs(got - want) <= tol;
}

mm::WorldPoint
at_e7(int32_t lat_e7, int32_t lon_e7) {
    mm::WorldPoint p;
    const bool ok = mm::world_from_e7(lat_e7, lon_e7, &p);
    assert(ok);
    return p;
}

/* Whether a fixed-point position is converted at all. */
bool
admitted(int32_t lat_e7, int32_t lon_e7) {
    mm::WorldPoint p;
    return mm::world_from_e7(lat_e7, lon_e7, &p);
}

mm::Viewport
view(double cx, double cy, double zoom, double w, double h) {
    mm::Viewport vp;
    vp.center_x = cx;
    vp.center_y = cy;
    vp.zoom = zoom;
    vp.width = w;
    vp.height = h;
    return vp;
}

/* The published slippy-map tile formula, written out independently of map_math.h. */
void
osm_tile(double lat, double lon, int z, double* tx, double* ty) {
    const double n = std::exp2(static_cast<double>(z));
    const double lat_rad = lat * 3.14159265358979323846 / 180.0;
    *tx = n * ((lon + 180.0) / 360.0);
    *ty = n * (1.0 - (std::log(std::tan(lat_rad) + (1.0 / std::cos(lat_rad))) / 3.14159265358979323846)) / 2.0;
}

void
test_projection_matches_the_slippy_map_formula(void) {
    const double positions[][2] = {{22.5, 45.0}, {10.0, 20.0}, {-40.25, -130.5}, {60.0, -35.0}, {-5.0, 175.0}};
    for (const auto* pos : positions) {
        const mm::WorldPoint p = mm::world_from_lat_lon(pos[0], pos[1]);
        for (int z = 0; z <= mm::kMaxTileZoom; z++) {
            double tx = 0.0;
            double ty = 0.0;
            osm_tile(pos[0], pos[1], z, &tx, &ty);
            const double n = std::exp2(static_cast<double>(z));
            assert(near(p.x * n, tx, 1.0e-6));
            assert(near(p.y * n, ty, 1.0e-6));
        }
    }
    /* The origin is the middle of the world. */
    const mm::WorldPoint origin = mm::world_from_lat_lon(0.0, 0.0);
    assert(near(origin.x, 0.5, kTol) && near(origin.y, 0.5, kTol));
}

void
test_fixed_point_input_is_checked_not_clamped(void) {
    /* The same position as degrees and as fixed point. */
    const mm::WorldPoint deg = mm::world_from_lat_lon(22.5, 45.0);
    const mm::WorldPoint e7 = at_e7(225000000, 450000000);
    assert(near(deg.x, e7.x, kTol) && near(deg.y, e7.y, kTol));

    /* The extremes are in range; one unit past them is refused, and the output is left alone. */
    assert(admitted(900000000, 1800000000));
    assert(admitted(-900000000, -1800000000));
    assert(!admitted(900000001, 0));
    assert(!admitted(-900000001, 0));
    assert(!admitted(0, 1800000001));
    assert(!admitted(0, -1800000001));
    assert(!admitted(INT32_MIN, INT32_MIN));
    assert(!admitted(0, INT32_MAX));
    const bool to_nowhere = mm::world_from_e7(0, 0, nullptr);
    assert(!to_nowhere);
    mm::WorldPoint p{0.25, 0.75};
    const bool refused = !mm::world_from_e7(0, INT32_MAX, &p);
    assert(refused);
    assert(near(p.x, 0.25, kTol) && near(p.y, 0.75, kTol));
}

void
test_latitude_clamp_and_longitude_wrap(void) {
    /* The projection's edge is 85.0511 degrees: there y is 0 (north) and 1 (south). */
    assert(near(mm::kMaxLatitudeDeg, 85.0511, 1.0e-4));
    const mm::WorldPoint north = mm::world_from_lat_lon(mm::kMaxLatitudeDeg, 0.0);
    const mm::WorldPoint south = mm::world_from_lat_lon(-mm::kMaxLatitudeDeg, 0.0);
    assert(near(north.y, 0.0, 1.0e-12) && near(south.y, 1.0, 1.0e-12));
    /* Past it, and at the poles, a point sits on the edge rather than off the world. */
    assert(near(at_e7(900000000, 0).y, 0.0, kTol));
    assert(near(at_e7(-900000000, 0).y, 1.0, kTol));
    assert(near(at_e7(870000000, 0).y, 0.0, kTol));
    assert(near(mm::clamp_latitude(89.0), mm::kMaxLatitudeDeg, kTol));
    assert(near(mm::clamp_latitude(-89.0), -mm::kMaxLatitudeDeg, kTol));
    assert(near(mm::clamp_latitude(std::numeric_limits<double>::quiet_NaN()), 0.0, kTol));

    /* 180 east and 180 west are the same meridian, x = 0; x always lies in [0, 1). */
    assert(near(at_e7(0, 1800000000).x, 0.0, kTol));
    assert(near(at_e7(0, -1800000000).x, 0.0, kTol));
    assert(near(mm::wrap_x(1.25), 0.25, kTol));
    assert(near(mm::wrap_x(-0.25), 0.75, kTol));
    assert(near(mm::wrap_x(-1.0e-18), 0.0, kTol) && mm::wrap_x(-1.0e-18) < 1.0);
    assert(near(mm::wrap_x(std::numeric_limits<double>::infinity()), 0.5, kTol));
}

void
test_round_trip_to_fixed_point(void) {
    const int32_t positions[][2] = {
        {225000000, 450000000},  {100000000, 200000000},    {-402500000, -1305000000}, {0, 0},
        {850000000, 1799999999}, {-850000000, -1799999999}, {123456789, -98765432},
    };
    for (const auto* pos : positions) {
        const mm::LatLonE7 back = mm::e7_from_world(at_e7(pos[0], pos[1]));
        assert(std::abs(back.lat_e7 - pos[0]) <= 1);
        assert(std::abs(back.lon_e7 - pos[1]) <= 1);
    }
    /* Degrees too. */
    const mm::LatLon d = mm::lat_lon_from_world(mm::world_from_lat_lon(-12.5, -110.25));
    assert(near(d.lat, -12.5, 1.0e-9) && near(d.lon, -110.25, 1.0e-9));
    /* The edges come back as the projection's edges. */
    const mm::LatLon top = mm::lat_lon_from_world(mm::WorldPoint{0.0, 0.0});
    const mm::LatLon past_bottom = mm::lat_lon_from_world(mm::WorldPoint{0.0, 2.0});
    assert(near(top.lat, mm::kMaxLatitudeDeg, 1.0e-9) && near(past_bottom.lat, -mm::kMaxLatitudeDeg, 1.0e-9));
}

void
test_zoom_clamp_and_tile_zoom(void) {
    assert(near(mm::clamp_zoom(-3.0), 0.0, kTol));
    assert(near(mm::clamp_zoom(25.0), 19.0, kTol));
    assert(near(mm::clamp_zoom(std::numeric_limits<double>::quiet_NaN()), 0.0, kTol));
    assert(near(mm::clamp_zoom(7.25), 7.25, kTol));
    /* Nearest whole zoom, so a tile is drawn at 0.71x to 1.41x. */
    assert(mm::tile_zoom(2.49) == 2);
    assert(mm::tile_zoom(2.5) == 3);
    assert(mm::tile_zoom(2.0) == 2);
    assert(mm::tile_zoom(-1.0) == 0);
    assert(mm::tile_zoom(19.0) == 19);
    assert(mm::tile_zoom(30.0) == 19);
    assert(near(mm::world_size_px(0.0), 256.0, kTol));
    assert(near(mm::world_size_px(3.0), 2048.0, kTol));
}

/* How many listed tiles hold the screen point (sx, sy). */
int
tiles_holding(const std::vector<mm::VisibleTile>& tiles, double sx, double sy) {
    return static_cast<int>(std::count_if(tiles.begin(), tiles.end(), [sx, sy](const mm::VisibleTile& t) {
        return sx >= t.left && sx < t.left + t.size && sy >= t.top && sy < t.top + t.size;
    }));
}

/* Every point of the view lies in exactly one listed tile, and every listed tile shows part of the view. */
void
check_tiles_cover_view(const mm::Viewport& vp, const std::vector<mm::VisibleTile>& tiles) {
    const int tz = mm::tile_zoom(vp.zoom);
    const int count = 1 << tz;
    for (const mm::VisibleTile& t : tiles) {
        assert(t.z == tz);
        assert(t.x >= 0 && t.x < count && t.y >= 0 && t.y < count);
        assert(t.left < vp.width && t.left + t.size > 0.0);
        assert(t.top < vp.height && t.top + t.size > 0.0);
        /* The tile is at its place in the world: its corner maps back to its own z/x/y. */
        const mm::WorldPoint corner = mm::world_from_screen(vp, t.left + (t.size / 2.0), t.top + (t.size / 2.0));
        assert(static_cast<int>(std::floor(corner.x * count)) == t.x);
        assert(static_cast<int>(std::floor(corner.y * count)) == t.y);
    }
    const double world = mm::world_size_px(vp.zoom);
    const double top_edge = (vp.height / 2.0) - (vp.center_y * world);
    const double bottom_edge = top_edge + world;
    for (int row = 0; (7.3 * row) + 0.5 < vp.height; row++) {
        const double sy = (7.3 * row) + 0.5;
        if (sy < top_edge || sy >= bottom_edge) {
            continue; /* off the north or south edge of the world: nothing to draw */
        }
        for (int column = 0; (7.3 * column) + 0.5 < vp.width; column++) {
            assert(tiles_holding(tiles, (7.3 * column) + 0.5, sy) == 1);
        }
    }
}

void
test_visible_tiles_whole_zoom(void) {
    /* Zoom 2, 512 x 512, centred: exactly the middle four tiles, edges on tile edges. */
    const mm::Viewport vp = view(0.5, 0.5, 2.0, 512.0, 512.0);
    const std::vector<mm::VisibleTile> tiles = mm::visible_tiles(vp);
    assert(tiles.size() == 4U);
    for (const mm::VisibleTile& t : tiles) {
        assert(t.x == 1 || t.x == 2);
        assert(t.y == 1 || t.y == 2);
        assert(near(t.size, 256.0, kPxTol));
        assert(near(t.left, (t.x - 1) * 256.0, kPxTol));
        assert(near(t.top, (t.y - 1) * 256.0, kPxTol));
    }
    check_tiles_cover_view(vp, tiles);
}

void
test_visible_tiles_fractional_zoom(void) {
    /* Below the half step the tiles come from the lower zoom, scaled up; at and above it from the higher, scaled down. */
    /* Off every tile edge at both zooms, so the tile holding the centre is unambiguous. */
    const mm::WorldPoint p = mm::world_from_lat_lon(22.6, 45.3);
    const mm::Viewport low = mm::clamp_viewport(view(p.x, p.y, 9.4, 411.0, 731.0));
    const std::vector<mm::VisibleTile> lt = mm::visible_tiles(low);
    assert(!lt.empty() && lt.front().z == 9);
    assert(near(lt.front().size, 256.0 * std::exp2(0.4), 1.0e-6));
    check_tiles_cover_view(low, lt);

    const mm::Viewport high = mm::clamp_viewport(view(p.x, p.y, 9.6, 411.0, 731.0));
    const std::vector<mm::VisibleTile> ht = mm::visible_tiles(high);
    assert(!ht.empty() && ht.front().z == 10);
    assert(near(ht.front().size, 256.0 * std::exp2(-0.4), 1.0e-6));
    check_tiles_cover_view(high, ht);

    /* Nearest the centre first: the first tile holds the view's centre. */
    const mm::VisibleTile& first = ht.front();
    assert(first.left <= 411.0 / 2.0 && first.left + first.size > 411.0 / 2.0);
    assert(first.top <= 731.0 / 2.0 && first.top + first.size > 731.0 / 2.0);
    double tx = 0.0;
    double ty = 0.0;
    osm_tile(22.6, 45.3, 10, &tx, &ty);
    assert(first.x == static_cast<int>(std::floor(tx)) && first.y == static_cast<int>(std::floor(ty)));
}

void
test_visible_tiles_wrap_the_antimeridian(void) {
    /* Centred on 180 degrees at zoom 3: tile 7 (just west) sits left of tile 0 (just east). */
    const mm::Viewport vp = mm::clamp_viewport(view(0.0, 0.5, 3.0, 600.0, 400.0));
    const std::vector<mm::VisibleTile> tiles = mm::visible_tiles(vp);
    check_tiles_cover_view(vp, tiles);
    bool saw_west = false;
    bool saw_east = false;
    for (const mm::VisibleTile& t : tiles) {
        if (t.x == 7 && t.top < 190.0 && t.top + t.size > 190.0) {
            assert(near(t.left + t.size, 300.0, kPxTol));
            saw_west = true;
        }
        if (t.x == 0 && t.top < 190.0 && t.top + t.size > 190.0) {
            assert(near(t.left, 300.0, kPxTol));
            saw_east = true;
        }
    }
    assert(saw_west && saw_east);
}

void
test_visible_tiles_low_zoom_wide_view(void) {
    /* Zoom 0 asked of a view wider than the world: the zoom rises until the world fills the width, so no
     * second copy of the world (where markers, drawn once, would be missing) is ever on screen. */
    const mm::Viewport wide = mm::clamp_viewport(view(0.5, 0.5, 0.0, 1000.0, 700.0));
    assert(near(wide.zoom, std::log2(1000.0 / 256.0), kTol));
    assert(near(mm::world_size_px(wide.zoom), 1000.0, 1.0e-6));
    const std::vector<mm::VisibleTile> wide_tiles = mm::visible_tiles(wide);
    assert(wide_tiles.size() == 16U); /* zoom 2, 250 px tiles: 4 across, 4 rows over y 150..850 of the world */
    for (size_t i = 0; i < wide_tiles.size(); i++) {
        for (size_t j = i + 1U; j < wide_tiles.size(); j++) {
            assert(wide_tiles[i].x != wide_tiles[j].x || wide_tiles[i].y != wide_tiles[j].y);
        }
    }
    check_tiles_cover_view(wide, wide_tiles);
    /* Every point of the world is drawn on screen there. */
    const mm::WorldPoint corners[] = {mm::WorldPoint{0.0, 0.5}, mm::WorldPoint{0.999, 0.5}};
    for (const mm::WorldPoint& c : corners) {
        const mm::ScreenPoint at = mm::screen_from_world(wide, c);
        assert(at.x >= 0.0 && at.x <= 1000.0);
    }
    assert(near(mm::min_zoom_for_width(200.0), 0.0, kTol));
    assert(near(mm::min_zoom_for_width(1.0e12), mm::kMaxZoom, kTol));

    /* A tall, narrow view at the lowest zoom: the world is shorter than the view and sits in the middle, one tile,
     * nothing north or south. */
    const mm::Viewport vp = mm::clamp_viewport(view(0.5, 0.2, 0.0, 300.0, 900.0));
    assert(near(vp.center_y, 0.5, kTol));
    const std::vector<mm::VisibleTile> tiles = mm::visible_tiles(vp);
    assert(tiles.size() == 1U);
    assert(tiles[0].z == 0 && tiles[0].x == 0 && tiles[0].y == 0);
    assert(near(tiles[0].top, 450.0 - 150.0, kPxTol) && near(tiles[0].size, 300.0, kPxTol));
    check_tiles_cover_view(vp, tiles);

    /* An empty view needs nothing, and a size no screen has gets nothing rather than a million tiles. */
    const size_t empty_view = mm::visible_tiles(view(0.5, 0.5, 5.0, 0.0, 400.0)).size();
    const size_t huge_view = mm::visible_tiles(view(0.5, 0.5, 5.0, 1.0e7, 1.0e7)).size();
    const size_t nan_view =
        mm::visible_tiles(view(0.5, 0.5, 5.0, std::numeric_limits<double>::quiet_NaN(), 400.0)).size();
    assert(empty_view == 0U && huge_view == 0U && nan_view == 0U);
    /* Sizes far past any screen are refused before a tile index is computed, so nothing overflows. */
    const size_t absurd_wide = mm::visible_tiles(view(0.5, 0.5, 5.0, 1.0e30, 400.0)).size();
    const size_t absurd_tall = mm::visible_tiles(view(0.5, 0.5, 5.0, 400.0, 1.0e30)).size();
    const size_t past_double = mm::visible_tiles(view(0.5, 0.5, 5.0, 1.0e300, 1.0e300)).size();
    assert(absurd_wide == 0U && absurd_tall == 0U && past_double == 0U);
}

void
test_viewport_clamp_and_pan(void) {
    /* Panning north past the edge stops with the edge on the view's top. */
    mm::Viewport vp = mm::clamp_viewport(view(0.5, 0.5, 4.0, 400.0, 800.0));
    vp = mm::pan(vp, 0.0, 1.0e6);
    const double world = mm::world_size_px(4.0);
    assert(near(vp.center_y, 400.0 / world, kTol));
    const mm::ScreenPoint north_edge = mm::screen_from_world(vp, mm::WorldPoint{0.5, 0.0});
    assert(near(north_edge.y, 0.0, kPxTol));
    vp = mm::pan(vp, 0.0, -1.0e6);
    const mm::ScreenPoint south_edge = mm::screen_from_world(vp, mm::WorldPoint{0.5, 1.0});
    assert(near(south_edge.y, 800.0, kPxTol));

    /* East-west wraps: a pan by one whole world lands where it started. */
    vp = mm::clamp_viewport(view(0.3, 0.5, 4.0, 400.0, 800.0));
    const mm::Viewport round = mm::pan(vp, world, 0.0);
    assert(near(round.center_x, 0.3, 1.0e-12));
    /* The content follows the finger: dragging right by 40 px moves the centre 40 px west. */
    const mm::Viewport dragged = mm::pan(vp, 40.0, 0.0);
    assert(near(mm::nearest_wrap_delta(vp.center_x - dragged.center_x) * world, 40.0, kPxTol));
    /* A non-finite drag is ignored; a non-finite centre resets to the world's centre. */
    const mm::Viewport ignored = mm::pan(vp, std::numeric_limits<double>::quiet_NaN(), 0.0);
    assert(near(ignored.center_x, vp.center_x, kTol));
    const mm::Viewport reset = mm::clamp_viewport(
        view(std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(), 4.0, 400.0, 400.0));
    assert(near(reset.center_x, 0.5, kTol) && near(reset.center_y, 0.5, kTol));
}

void
test_screen_world_round_trip(void) {
    const mm::WorldPoint p = mm::world_from_lat_lon(-40.25, -130.5);
    const mm::Viewport vp = mm::clamp_viewport(view(p.x, p.y, 12.7, 411.0, 731.0));
    const mm::ScreenPoint s = mm::screen_from_world(vp, p);
    assert(near(s.x, 205.5, kPxTol) && near(s.y, 365.5, kPxTol));
    for (int column = 0; column <= 11; column++) {
        const double sx = 37.0 * column;
        for (int row = 0; row <= 13; row++) {
            const double sy = 53.0 * row;
            const mm::ScreenPoint back = mm::screen_from_world(vp, mm::world_from_screen(vp, sx, sy));
            assert(near(back.x, sx, kPxTol) && near(back.y, sy, kPxTol));
        }
    }
    /* A point just east of 180 seen from just west of it is drawn to the right, not a world away. */
    const mm::Viewport west = mm::clamp_viewport(view(0.99, 0.5, 5.0, 400.0, 400.0));
    const mm::ScreenPoint east = mm::screen_from_world(west, mm::WorldPoint{0.01, 0.5});
    assert(near(east.x, 200.0 + (0.02 * mm::world_size_px(5.0)), kPxTol));
}

void
test_zoom_keeps_the_anchor(void) {
    const mm::WorldPoint p = mm::world_from_lat_lon(10.0, 20.0);
    const mm::Viewport vp = mm::clamp_viewport(view(p.x, p.y, 11.0, 411.0, 731.0));
    const double ax = 80.0;
    const double ay = 600.0;
    const mm::WorldPoint under = mm::world_from_screen(vp, ax, ay);
    const double zooms[] = {11.5, 13.25, 9.0, 19.0};
    for (const double z : zooms) {
        const mm::Viewport zoomed = mm::zoom_about(vp, z, ax, ay);
        assert(near(zoomed.zoom, z, kTol));
        const mm::ScreenPoint s = mm::screen_from_world(zoomed, under);
        assert(near(s.x, ax, 1.0e-5) && near(s.y, ay, 1.0e-5));
    }
    /* Across 180 degrees too. */
    const mm::Viewport edge = mm::clamp_viewport(view(0.001, 0.5, 6.0, 400.0, 400.0));
    const mm::WorldPoint west_of_edge = mm::world_from_screen(edge, 10.0, 200.0);
    assert(west_of_edge.x > 0.9);
    const mm::Viewport in = mm::zoom_about(edge, 8.0, 10.0, 200.0);
    const mm::ScreenPoint s = mm::screen_from_world(in, west_of_edge);
    assert(near(s.x, 10.0, 1.0e-5) && near(s.y, 200.0, 1.0e-5));
    /* The zoom is clamped; a non-finite anchor zooms about the centre. */
    const mm::Viewport too_far = mm::zoom_about(vp, 40.0, ax, ay);
    assert(near(too_far.zoom, mm::kMaxZoom, kTol));
    const mm::Viewport centred = mm::zoom_about(vp, 12.0, std::numeric_limits<double>::quiet_NaN(), 0.0);
    assert(near(centred.center_x, vp.center_x, 1.0e-12) && near(centred.center_y, vp.center_y, 1.0e-12));
}

void
test_track_projection_crosses_the_antimeridian(void) {
    /* A track heading east across 180 degrees, seen from far west of it. */
    const mm::WorldPoint track[] = {
        mm::world_from_lat_lon(-5.0, 179.8),
        mm::world_from_lat_lon(-5.0, 179.95),
        mm::world_from_lat_lon(-5.0, -179.95),
        mm::world_from_lat_lon(-5.0, -179.8),
    };
    const mm::Viewport vp = mm::clamp_viewport(view(0.95, track[0].y, 9.0, 400.0, 400.0));
    float xs[4];
    float ys[4];
    mm::project_track(vp, track, 4, xs, ys);
    /* The last point is where its marker is drawn. */
    const mm::ScreenPoint last = mm::screen_from_world(vp, track[3]);
    assert(near(xs[3], last.x, 1.0e-2) && near(ys[3], last.y, 1.0e-2));
    /* Every step is a short step east, 0.15 or 0.1 degrees, never a world back. */
    const double px_per_deg = mm::world_size_px(9.0) / 360.0;
    assert(near(static_cast<double>(xs[1] - xs[0]), 0.15 * px_per_deg, 0.05));
    assert(near(static_cast<double>(xs[2] - xs[1]), 0.10 * px_per_deg, 0.05));
    assert(near(static_cast<double>(xs[3] - xs[2]), 0.15 * px_per_deg, 0.05));
    /* Nothing written for nothing asked. */
    float untouched = 7.0F;
    mm::project_track(vp, track, 0, &untouched, &untouched);
    assert(near(untouched, 7.0, kTol));
}

/*
 * A break in a track (a non-finite point, the convention map_geometry.h's
 * append_trail() splits on) stays a break through the projection: it comes out
 * non-finite, and the points on both sides of it are kept and placed as they
 * would be without it, across 180 degrees too. Without this a break blanked
 * every older point, and the trail before the gap vanished.
 */
void
test_track_projection_keeps_both_sides_of_a_break(void) {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const mm::WorldPoint whole[] = {
        mm::world_from_lat_lon(-5.0, 179.8),
        mm::world_from_lat_lon(-5.0, 179.95),
        mm::world_from_lat_lon(-5.0, -179.95),
        mm::world_from_lat_lon(-5.0, -179.8),
    };
    const mm::WorldPoint broken[] = {whole[0], whole[1], mm::WorldPoint{nan, nan}, whole[2], whole[3]};
    const mm::Viewport vp = mm::clamp_viewport(view(0.9998, whole[0].y, 9.0, 400.0, 400.0));
    float wx[4];
    float wy[4];
    mm::project_track(vp, whole, 4, wx, wy);
    float bx[5];
    float by[5];
    mm::project_track(vp, broken, 5, bx, by);
    assert(!std::isfinite(bx[2]) && !std::isfinite(by[2]));
    const int kept[] = {0, 1, 3, 4};
    for (int i = 0; i < 4; i++) {
        assert(std::isfinite(bx[kept[i]]) && std::isfinite(by[kept[i]]));
        assert(near(bx[kept[i]], wx[i], 1.0e-3) && near(by[kept[i]], wy[i], 1.0e-3));
    }
    /* Only x or only y non-finite is a break as well, and breaks at either end leave the rest in place. */
    const mm::WorldPoint half[] = {
        mm::WorldPoint{nan, 0.5}, whole[0], mm::WorldPoint{whole[1].x, nan}, whole[2], whole[3],
        mm::WorldPoint{nan, nan},
    };
    float hx[6];
    float hy[6];
    mm::project_track(vp, half, 6, hx, hy);
    assert(!std::isfinite(hx[0]) && !std::isfinite(hx[2]) && !std::isfinite(hx[5]));
    assert(near(hx[1], wx[0], 1.0e-3) && near(hx[3], wx[2], 1.0e-3) && near(hx[4], wx[3], 1.0e-3));
    assert(near(hy[1], wy[0], 1.0e-3) && near(hy[4], wy[3], 1.0e-3));
}

void
test_hit_testing(void) {
    const mm::WorldPoint centre = mm::world_from_lat_lon(22.5, 45.0);
    const mm::Viewport vp = mm::clamp_viewport(view(centre.x, centre.y, 14.0, 400.0, 400.0));
    const mm::WorldPoint a = mm::world_from_screen(vp, 200.0, 200.0);
    const mm::WorldPoint b = mm::world_from_screen(vp, 220.0, 200.0);
    const mm::WorldPoint c = mm::world_from_screen(vp, 200.0, 200.0); /* on top of a */
    const mm::WorldPoint markers[] = {a, b, c};
    /* Nearest wins. */
    assert(mm::hit_test(vp, markers, 2, 214.0, 201.0, 24.0) == 1);
    assert(mm::hit_test(vp, markers, 2, 203.0, 199.0, 24.0) == 0);
    /* Two at the same place: the later one, drawn on top. */
    assert(mm::hit_test(vp, markers, 3, 201.0, 200.0, 24.0) == 2);
    /* Outside the radius, nothing. */
    assert(mm::hit_test(vp, markers, 3, 260.0, 200.0, 24.0) == -1);
    assert(mm::hit_test(vp, markers, 3, 200.0, 200.0, 0.0) == -1);
    assert(mm::hit_test(vp, markers, 3, std::numeric_limits<double>::quiet_NaN(), 200.0, 24.0) == -1);
    assert(mm::hit_test(vp, nullptr, 3, 200.0, 200.0, 24.0) == -1);

    /* A marker just east of 180, from a view just west of it, is hit where it is drawn. */
    const mm::Viewport west = mm::clamp_viewport(view(0.9999, 0.5, 11.0, 400.0, 400.0));
    const mm::WorldPoint over[] = {mm::WorldPoint{0.0001, 0.5}};
    const mm::ScreenPoint drawn = mm::screen_from_world(west, over[0]);
    assert(drawn.x > 200.0 && drawn.x < 400.0);
    assert(mm::hit_test(west, over, 1, drawn.x, drawn.y, 10.0) == 0);
}

/* Every point drawn inside the view less the padding. */
void
check_fit_shows(const mm::Viewport& vp, const mm::WorldPoint* points, int count, double padding) {
    for (int i = 0; i < count; i++) {
        const mm::ScreenPoint s = mm::screen_from_world(vp, points[i]);
        assert(s.x >= padding - 1.0e-6 && s.x <= vp.width - padding + 1.0e-6);
        assert(s.y >= padding - 1.0e-6 && s.y <= vp.height - padding + 1.0e-6);
    }
}

void
test_fit_points(void) {
    const mm::Viewport base = view(0.5, 0.5, 3.0, 400.0, 600.0);
    const mm::WorldPoint pts[] = {
        mm::world_from_lat_lon(10.0, 20.0),
        mm::world_from_lat_lon(22.5, 45.0),
        mm::world_from_lat_lon(15.0, 30.0),
    };
    mm::Viewport fit;
    const bool fitted = mm::fit_points(base, pts, 3, 32.0, 15.0, &fit);
    assert(fitted);
    assert(near(fit.width, 400.0, kTol) && near(fit.height, 600.0, kTol));
    check_fit_shows(fit, pts, 3, 32.0);
    /* As close as it can be: a tenth of a zoom step more and a point leaves the padded view. */
    mm::Viewport closer = fit;
    closer.zoom += 0.1;
    bool left_view = false;
    for (const mm::WorldPoint& p : pts) {
        const mm::ScreenPoint s = mm::screen_from_world(closer, p);
        left_view = left_view || s.x < 32.0 || s.x > 368.0 || s.y < 32.0 || s.y > 568.0;
    }
    assert(left_view);

    /* One point, or several in the same place: centred at the zoom cap. */
    mm::Viewport one;
    const bool fitted_one = mm::fit_points(base, pts + 1, 1, 32.0, 15.0, &one);
    assert(fitted_one);
    assert(near(one.zoom, 15.0, kTol));
    const mm::ScreenPoint s = mm::screen_from_world(one, pts[1]);
    assert(near(s.x, 200.0, kPxTol) && near(s.y, 300.0, kPxTol));
    const mm::WorldPoint same[] = {pts[0], pts[0]};
    const bool fitted_same = mm::fit_points(base, same, 2, 32.0, 15.0, &one);
    assert(fitted_same && near(one.zoom, 15.0, kTol));

    /* Nothing to fit, or no room: refused, output untouched. */
    mm::Viewport untouched = view(0.25, 0.25, 4.0, 1.0, 1.0);
    const bool fitted_none = mm::fit_points(base, pts, 0, 32.0, 15.0, &untouched);
    const bool fitted_no_room = mm::fit_points(view(0.5, 0.5, 3.0, 0.0, 600.0), pts, 3, 32.0, 15.0, &untouched);
    assert(!fitted_none && !fitted_no_room);
    assert(near(untouched.center_x, 0.25, kTol) && near(untouched.zoom, 4.0, kTol));
}

void
test_fit_across_the_antimeridian(void) {
    /* Radios either side of 180 degrees: a short arc across it, not the world between them. */
    const mm::WorldPoint pts[] = {
        mm::world_from_lat_lon(-35.0, 179.0),
        mm::world_from_lat_lon(-36.0, -179.0),
        mm::world_from_lat_lon(-34.5, 178.5),
    };
    const mm::Viewport base = view(0.5, 0.5, 2.0, 400.0, 600.0);
    mm::Viewport fit;
    const bool fitted = mm::fit_points(base, pts, 3, 32.0, 15.0, &fit);
    assert(fitted);
    check_fit_shows(fit, pts, 3, 32.0);
    assert(fit.zoom > 6.0);
    /* Centred near 179.75 east, the middle of the 178.5 .. 181 (= -179) arc. */
    const double lon = mm::lat_lon_from_world(mm::WorldPoint{fit.center_x, fit.center_y}).lon;
    assert(near(lon, 179.75, 1.0e-6));

    mm::XArc arc{0.0, 0.0};
    const bool have_arc = mm::shortest_x_arc(pts, 3, &arc);
    assert(have_arc && near(arc.width, 2.5 / 360.0, 1.0e-12));
}

/*
 * A track's breaks are non-finite points. Fitting skips them rather than
 * reading them as a place (a NaN made 0.5, the middle of the world, would pull
 * the fit towards 0 degrees), and a set with no placeable point is refused.
 */
void
test_fit_skips_breaks(void) {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const mm::WorldPoint real = mm::world_from_lat_lon(60.0, -35.0);
    const mm::WorldPoint pts[] = {
        mm::WorldPoint{nan, nan},
        real,
        mm::WorldPoint{nan, 0.1},
        mm::WorldPoint{0.1, std::numeric_limits<double>::infinity()},
    };
    const mm::Viewport base = view(0.5, 0.5, 3.0, 400.0, 600.0);
    mm::Viewport fit;
    const bool fitted = mm::fit_points(base, pts, 4, 32.0, 15.0, &fit);
    assert(fitted && near(fit.zoom, 15.0, kTol));
    assert(near(fit.center_x, real.x, 1.0e-12) && near(fit.center_y, real.y, 1.0e-12));

    mm::XArc arc{0.25, 0.5};
    const bool arc_of_breaks = mm::shortest_x_arc(pts, 1, &arc);
    const bool arc_of_none = mm::shortest_x_arc(pts, 0, &arc);
    const bool arc_of_null = mm::shortest_x_arc(nullptr, 3, &arc);
    assert(!arc_of_breaks && !arc_of_none && !arc_of_null);
    assert(near(arc.west, 0.25, kTol) && near(arc.width, 0.5, kTol));

    mm::Viewport untouched = view(0.25, 0.25, 4.0, 1.0, 1.0);
    const mm::WorldPoint breaks[] = {mm::WorldPoint{nan, nan}, mm::WorldPoint{nan, 0.5}};
    const bool fitted_breaks = mm::fit_points(base, breaks, 2, 32.0, 15.0, &untouched);
    assert(!fitted_breaks && near(untouched.center_x, 0.25, kTol) && near(untouched.zoom, 4.0, kTol));

    /* A marker that is a break is never hit. */
    const mm::Viewport vp = mm::clamp_viewport(view(0.5, 0.5, 3.0, 400.0, 400.0));
    const mm::WorldPoint markers[] = {mm::WorldPoint{nan, 0.5}};
    assert(mm::hit_test(vp, markers, 1, 200.0, 200.0, 1.0e9) == -1);
}

void
test_ground_scale(void) {
    /* At the equator, zoom 0: the circumference over 256 px. */
    assert(near(mm::metres_per_px(0.0, 0.0), 40075016.68557849 / 256.0, 1.0e-6));
    /* At 60 degrees a pixel spans half that, and each zoom step halves it again. */
    assert(near(mm::metres_per_px(60.0, 0.0), 40075016.68557849 / 512.0, 1.0e-6));
    assert(near(mm::metres_per_px(60.0, 1.0), 40075016.68557849 / 1024.0, 1.0e-6));
    /* A 100 m error at zoom 17 on the equator. */
    const double px = mm::metres_to_px(100.0, 0.0, 17.0);
    assert(near(px, 100.0 / (40075016.68557849 / (256.0 * 131072.0)), 1.0e-9));
    assert(near(mm::metres_to_px(0.0, 0.0, 17.0), 0.0, kTol));
    assert(near(mm::metres_to_px(std::numeric_limits<double>::quiet_NaN(), 0.0, 17.0), 0.0, kTol));
}

} // namespace

int
main(void) {
    test_projection_matches_the_slippy_map_formula();
    test_fixed_point_input_is_checked_not_clamped();
    test_latitude_clamp_and_longitude_wrap();
    test_round_trip_to_fixed_point();
    test_zoom_clamp_and_tile_zoom();
    test_visible_tiles_whole_zoom();
    test_visible_tiles_fractional_zoom();
    test_visible_tiles_wrap_the_antimeridian();
    test_visible_tiles_low_zoom_wide_view();
    test_viewport_clamp_and_pan();
    test_screen_world_round_trip();
    test_zoom_keeps_the_anchor();
    test_track_projection_crosses_the_antimeridian();
    test_track_projection_keeps_both_sides_of_a_break();
    test_hit_testing();
    test_fit_points();
    test_fit_across_the_antimeridian();
    test_fit_skips_breaks();
    test_ground_scale();
    (void)DSD_FPRINTF(stdout, "UI_QT_MAP_MATH: ok\n");
    return 0;
}

// NOLINTEND(cert-dcl03-c,misc-static-assert)
