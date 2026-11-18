// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief What the map view draws for each track, decided without Qt: marker styles, trail runs, labels, tile places.
 *
 * Qt-free and header-only, like map_math.h and map_geometry.h, so the rules
 * that keep the map honest are testable without a window:
 *
 *  - One visual variable per axis (spec 4.6). Freshness is the marker's fill
 *    strength; an unconfirmed source is a dashed outline; a fix whose position
 *    may be old or invalid is an open ring with no fill; a format no published
 *    specification confirms is a double outline (and a dashed trail); an
 *    emergency is the alert colour. Each combination is one cell of the sprite
 *    atlas, so a marker is one textured quad.
 *  - A trail is split into a solid run list and a dashed run list, segment by
 *    segment, and a break is never bridged.
 *  - Labels never overlap: they are placed greedily in priority order.
 *  - Content built for one viewport is reused for a nearby pan by a
 *    translation, and rebuilt when the zoom or size changes or the pan leaves
 *    the margin it was built with.
 */

#ifndef DSD_NEO_SRC_UI_QT_MAP_SCENE_H_
#define DSD_NEO_SRC_UI_QT_MAP_SCENE_H_

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#include "map_math.h"

namespace dsd_qt {
namespace map_scene {

// ------------------------------------------------------------- freshness ----

/** @brief Fill strengths a marker can have: 0 is the freshest (solid), kFillLevels - 1 the oldest. */
constexpr int kFillLevels = 4;

/**
 * @brief The fill level of a fix @p age_s seconds old.
 *
 * Up to @p fresh_s it is solid (0); from @p faded_s on, or with no age (-1, a
 * stopped replay's fix), it is the faintest (kFillLevels - 1); between them it
 * steps through the levels in equal parts. A @p faded_s not above @p fresh_s
 * has no middle: older than @p fresh_s is the faintest.
 */
inline int
fill_level(int64_t age_s, int64_t fresh_s, int64_t faded_s) {
    if (age_s < 0) {
        return kFillLevels - 1;
    }
    if (age_s <= fresh_s) {
        return 0;
    }
    if (age_s >= faded_s || faded_s <= fresh_s) {
        return kFillLevels - 1;
    }
    const int64_t middle = kFillLevels - 2; /* levels strictly between solid and faintest */
    const int64_t step = ((age_s - fresh_s) * middle) / (faded_s - fresh_s);
    return 1 + static_cast<int>(step);
}

/** @brief How strongly a marker of fill @p level is filled, 0..1: solid, then fading, never quite gone. */
inline float
fill_opacity(int level) {
    static const float kOpacity[kFillLevels] = {1.0F, 0.66F, 0.4F, 0.18F};
    if (level <= 0) {
        return kOpacity[0];
    }
    return (level >= kFillLevels) ? kOpacity[kFillLevels - 1] : kOpacity[level];
}

// --------------------------------------------------------------- markers ----

/** @brief How one marker looks; each field is one axis of spec 4.6. */
struct MarkerStyle {
    bool ring = false;           /**< position validity unknown: open ring, no fill (fill_level is ignored) */
    bool dashed_outline = false; /**< source not confirmed (anonymous: hashed or unverified) */
    bool double_outline = false; /**< format not confirmed by a published specification */
    bool alert = false;          /**< the report carried an emergency */
    int fill_level = 0;          /**< freshness, 0..kFillLevels - 1 */
};

/** @brief Atlas cells: the filled markers, then the rings, then the selection halo and the device's own dot. */
constexpr int kFilledCells = 2 * 2 * 2 * kFillLevels;
constexpr int kRingCells = 2 * 2 * 2;
constexpr int kSelectionHaloCell = kFilledCells + kRingCells;
constexpr int kDeviceDotCell = kSelectionHaloCell + 1;
constexpr int kMarkerCellCount = kDeviceDotCell + 1;
/** @brief Cells per atlas row. */
constexpr int kAtlasColumns = 8;
constexpr int kAtlasRows = (kMarkerCellCount + kAtlasColumns - 1) / kAtlasColumns;

/** @brief The three outline/colour bits of a style, 0..7. */
inline int
outline_bits(const MarkerStyle& style) {
    return ((style.alert ? 1 : 0) * 4) + ((style.double_outline ? 1 : 0) * 2) + (style.dashed_outline ? 1 : 0);
}

/** @brief The atlas cell drawing @p style; a fill level out of range is clamped. */
inline int
marker_cell(const MarkerStyle& style) {
    if (style.ring) {
        return kFilledCells + outline_bits(style);
    }
    const int fill = std::min(kFillLevels - 1, std::max(0, style.fill_level));
    return (outline_bits(style) * kFillLevels) + fill;
}

/**
 * @brief The style atlas cell @p cell draws (the inverse of marker_cell()).
 *
 * @return false for the halo, the device dot and any cell out of range (@p out untouched).
 */
inline bool
marker_style_of_cell(int cell, MarkerStyle* out) {
    if (out == nullptr || cell < 0 || cell >= kSelectionHaloCell) {
        return false;
    }
    MarkerStyle style;
    int bits = 0;
    if (cell >= kFilledCells) {
        style.ring = true;
        bits = cell - kFilledCells;
    } else {
        bits = cell / kFillLevels;
        style.fill_level = cell % kFillLevels;
    }
    style.alert = (bits & 4) != 0;
    style.double_outline = (bits & 2) != 0;
    style.dashed_outline = (bits & 1) != 0;
    *out = style;
    return true;
}

// ---------------------------------------------------------------- trails ----

/** @brief A trail split by style: each list is a run list for map_geometry::append_trail() (NaN between runs). */
struct TrailRuns {
    std::vector<float> solid_x;
    std::vector<float> solid_y;
    std::vector<float> dashed_x;
    std::vector<float> dashed_y;
};

namespace detail {

inline void
run_point(std::vector<float>& xs, std::vector<float>& ys, float x, float y) {
    xs.push_back(x);
    ys.push_back(y);
}

/** @brief Add segment (a -> b) to a run list, continuing its run when its last point is @p a (index @p from). */
inline void
run_segment(std::vector<float>& xs, std::vector<float>& ys, int& last, int from, const float* px, const float* py) {
    if (last != from) {
        if (!xs.empty()) {
            const float nan = std::numeric_limits<float>::quiet_NaN();
            run_point(xs, ys, nan, nan);
        }
        run_point(xs, ys, px[from], py[from]);
    }
    run_point(xs, ys, px[from + 1], py[from + 1]);
    last = from + 1;
}

} // namespace detail

/**
 * @brief Split projected trail points into solid and dashed runs.
 *
 * Segment i runs from point i - 1 to point i and is dashed when
 * @p dashed_to[i] is set (spec: a segment ending at a provisional point). A
 * segment with a non-finite end (a break, map_math::project_track()) is not
 * drawn and ends the run in both lists, so no line crosses a break; a style
 * change ends the run in one list and starts it in the other at the shared
 * point, so the trail stays continuous. @p out is cleared first.
 *
 * @param dashed_to @p count entries; entry 0 is ignored. Null: every segment solid.
 */
inline void
split_trail(const float* xs, const float* ys, const uint8_t* dashed_to, int count, TrailRuns& out) {
    out.solid_x.clear();
    out.solid_y.clear();
    out.dashed_x.clear();
    out.dashed_y.clear();
    if (xs == nullptr || ys == nullptr || count < 2) {
        return;
    }
    int last_solid = -1;
    int last_dashed = -1;
    for (int i = 1; i < count; i++) {
        const bool finite =
            std::isfinite(xs[i - 1]) && std::isfinite(ys[i - 1]) && std::isfinite(xs[i]) && std::isfinite(ys[i]);
        if (!finite) {
            continue;
        }
        if (dashed_to != nullptr && dashed_to[i] != 0U) {
            detail::run_segment(out.dashed_x, out.dashed_y, last_dashed, i - 1, xs, ys);
        } else {
            detail::run_segment(out.solid_x, out.solid_y, last_solid, i - 1, xs, ys);
        }
    }
}

// ---------------------------------------------------------------- labels ----

/** @brief A label's rectangle, logical pixels. */
struct LabelBox {
    float left;
    float top;
    float width;
    float height;
};

inline bool
boxes_overlap(const LabelBox& a, const LabelBox& b) {
    return a.left < b.left + b.width && b.left < a.left + a.width && a.top < b.top + b.height
           && b.top < a.top + a.height;
}

/**
 * @brief Choose the labels to draw: in the order given (highest priority first), each one that is on screen and
 *        overlaps no label already chosen.
 *
 * @param keep   @p count entries, set to 1 for a chosen label and 0 otherwise.
 * @param width,height The view, logical pixels: a label wholly outside it is not chosen.
 * @return Labels chosen.
 */
inline int
place_labels(const LabelBox* boxes, int count, float width, float height, uint8_t* keep) {
    if (boxes == nullptr || keep == nullptr || count <= 0) {
        return 0;
    }
    const LabelBox view{0.0F, 0.0F, width, height};
    std::vector<int> chosen;
    for (int i = 0; i < count; i++) {
        keep[i] = 0U;
        const LabelBox& b = boxes[i];
        if (!(b.width > 0.0F) || !(b.height > 0.0F) || !std::isfinite(b.left) || !std::isfinite(b.top)
            || !boxes_overlap(b, view)) {
            continue;
        }
        const bool blocked =
            std::any_of(chosen.begin(), chosen.end(), [&](int j) { return boxes_overlap(b, boxes[j]); });
        if (!blocked) {
            keep[i] = 1U;
            chosen.push_back(i);
        }
    }
    return static_cast<int>(chosen.size());
}

// ----------------------------------------------------------------- tiles ----

/** @brief A screen rectangle, logical pixels. */
struct ScreenRect {
    double left = 0.0;
    double top = 0.0;
    double right = 0.0;
    double bottom = 0.0;
};

/**
 * @brief Where tile (@p z, @p x, @p y) lies on screen in @p vp: the copy nearest the view's centre.
 *
 * Any whole zoom, not only the view's tile zoom, so a tile kept from the zoom
 * before is drawn in its place, scaled, while the new zoom's tiles load. For
 * a tile the view's tile zoom lists, the rectangle is visible_tiles()'s.
 */
inline ScreenRect
tile_rect(const map_math::Viewport& vp, int z, int x, int y) {
    ScreenRect r;
    if (z < 0 || z > 30) {
        return r;
    }
    const double n = std::ldexp(1.0, z);
    const double world = map_math::world_size_px(vp.zoom);
    const double cell = world / n;
    const double centre_x = (static_cast<double>(x) + 0.5) / n;
    r.left = (vp.width / 2.0) + (map_math::nearest_wrap_delta(centre_x - vp.center_x) * world) - (cell / 2.0);
    r.top = (vp.height / 2.0) + (((static_cast<double>(y) / n) - vp.center_y) * world);
    r.right = r.left + cell;
    r.bottom = r.top + cell;
    return r;
}

/** @brief @p r with every edge on the device pixel grid, so neighbouring tiles share their edges exactly. */
inline ScreenRect
snap_rect(const ScreenRect& r, double device_pixel_ratio) {
    if (!(device_pixel_ratio > 0.0) || !std::isfinite(device_pixel_ratio)) {
        return r;
    }
    ScreenRect s;
    s.left = std::round(r.left * device_pixel_ratio) / device_pixel_ratio;
    s.top = std::round(r.top * device_pixel_ratio) / device_pixel_ratio;
    s.right = std::round(r.right * device_pixel_ratio) / device_pixel_ratio;
    s.bottom = std::round(r.bottom * device_pixel_ratio) / device_pixel_ratio;
    return s;
}

/** @brief Whether @p r meets the view (0, 0)..(@p width, @p height). */
inline bool
rect_in_view(const ScreenRect& r, double width, double height) {
    return r.right > 0.0 && r.left < width && r.bottom > 0.0 && r.top < height;
}

// ----------------------------------------------------- viewport re-use ----

/** @brief Below this two zooms (or sizes) are the same: content built at one is valid at the other. */
constexpr double kSameZoom = 1.0e-9;

/**
 * @brief How far content drawn for @p built has to move to show @p now: screen(now, p) - screen(built, p).
 *
 * Only meaningful when both have the same zoom (same_scale()). East-west the
 * shortest way round, as markers are placed.
 */
inline map_math::ScreenPoint
pan_offset(const map_math::Viewport& built, const map_math::Viewport& now) {
    const double world = map_math::world_size_px(now.zoom);
    map_math::ScreenPoint d;
    d.x = ((now.width - built.width) / 2.0) - (map_math::nearest_wrap_delta(now.center_x - built.center_x) * world);
    d.y = ((now.height - built.height) / 2.0) - ((now.center_y - built.center_y) * world);
    return d;
}

/** @brief Whether @p a and @p b draw the world at the same scale and size. */
inline bool
same_scale(const map_math::Viewport& a, const map_math::Viewport& b) {
    return std::fabs(a.zoom - b.zoom) <= kSameZoom && std::fabs(a.width - b.width) <= kSameZoom
           && std::fabs(a.height - b.height) <= kSameZoom;
}

/**
 * @brief Whether content built for @p built (clipped to its view grown by @p margin) can show @p now by a translation.
 *
 * Same scale and size, and a pan of at most @p margin each way: past it the
 * clipped parts (accuracy circles, dashes) would show their cut edges.
 */
inline bool
reusable(const map_math::Viewport& built, const map_math::Viewport& now, double margin) {
    if (!same_scale(built, now)) {
        return false;
    }
    const map_math::ScreenPoint d = pan_offset(built, now);
    return std::fabs(d.x) <= margin && std::fabs(d.y) <= margin;
}

/**
 * @brief The viewport @p t of the way from @p from to @p to (0..1): zoom and centre linear, east-west the short way.
 *
 * The camera's animation (fit, follow, the zoom buttons). Returned clamped.
 */
inline map_math::Viewport
interpolate_viewport(const map_math::Viewport& from, const map_math::Viewport& to, double t) {
    if (!(t > 0.0)) {
        return map_math::clamp_viewport(from);
    }
    if (t >= 1.0) {
        return map_math::clamp_viewport(to);
    }
    map_math::Viewport v = to;
    v.zoom = from.zoom + ((to.zoom - from.zoom) * t);
    v.center_x = from.center_x + (map_math::nearest_wrap_delta(to.center_x - from.center_x) * t);
    v.center_y = from.center_y + ((to.center_y - from.center_y) * t);
    return map_math::clamp_viewport(v);
}

} // namespace map_scene
} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_MAP_SCENE_H_ */
