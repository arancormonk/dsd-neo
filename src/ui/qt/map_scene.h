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

/** @brief The area @p a and @p b share; 0 when they do not overlap. */
inline float
intersection_area(const LabelBox& a, const LabelBox& b) {
    const float w = std::min(a.left + a.width, b.left + b.width) - std::max(a.left, b.left);
    const float h = std::min(a.top + a.height, b.top + b.height) - std::max(a.top, b.top);
    return (w > 0.0F && h > 0.0F) ? w * h : 0.0F;
}

/** @brief Whether @p inner lies wholly inside @p outer. */
inline bool
box_inside(const LabelBox& inner, const LabelBox& outer) {
    return inner.left >= outer.left && inner.top >= outer.top && inner.left + inner.width <= outer.left + outer.width
           && inner.top + inner.height <= outer.top + outer.height;
}

/**
 * @brief Boxes binned by the cells of a uniform grid over the view, so "does this box hit any of them" looks only at
 *        its neighbours. Reused frame to frame: reset() keeps every allocation.
 */
class BoxGrid {
  public:
    /** @brief Empty the grid and cover a @p width x @p height view with @p cell pixel cells. */
    void
    reset(float width, float height, float cell) {
        cell_ = (cell > 1.0F) ? cell : 1.0F;
        cols_ = std::max(1, static_cast<int>(std::ceil(std::max(width, 1.0F) / cell_)));
        rows_ = std::max(1, static_cast<int>(std::ceil(std::max(height, 1.0F) / cell_)));
        boxes_.clear();
        stamps_.clear();
        const size_t cells = static_cast<size_t>(cols_) * static_cast<size_t>(rows_);
        if (cells_.size() < cells) {
            cells_.resize(cells);
        }
        for (size_t i = 0; i < cells; i++) {
            cells_[i].clear();
        }
    }

    /** @brief Add @p box; a box with no area, or wholly outside the view, can hit nothing there and is not kept. */
    void
    add(const LabelBox& box) {
        int c0 = 0;
        int c1 = 0;
        int r0 = 0;
        int r1 = 0;
        if (!span(box, &c0, &c1, &r0, &r1)) {
            return;
        }
        const int index = static_cast<int>(boxes_.size());
        boxes_.push_back(box);
        stamps_.push_back(0U);
        for (int r = r0; r <= r1; r++) {
            for (int c = c0; c <= c1; c++) {
                cells_[(static_cast<size_t>(r) * static_cast<size_t>(cols_)) + static_cast<size_t>(c)].push_back(index);
            }
        }
    }

    /** @brief The area of @p box that boxes added cover, each counted once (overlaps between them counted twice). */
    float
    overlap_area(const LabelBox& box) {
        int c0 = 0;
        int c1 = 0;
        int r0 = 0;
        int r1 = 0;
        if (!span(box, &c0, &c1, &r0, &r1)) {
            return 0.0F;
        }
        query_++;
        float area = 0.0F;
        for (int r = r0; r <= r1; r++) {
            for (int c = c0; c <= c1; c++) {
                for (const int i :
                     cells_[(static_cast<size_t>(r) * static_cast<size_t>(cols_)) + static_cast<size_t>(c)]) {
                    if (stamps_[static_cast<size_t>(i)] != query_) {
                        stamps_[static_cast<size_t>(i)] = query_;
                        area += intersection_area(box, boxes_[static_cast<size_t>(i)]);
                    }
                }
            }
        }
        return area;
    }

    /** @brief Whether @p box overlaps any box added (inside the view). */
    bool
    hits(const LabelBox& box) const {
        int c0 = 0;
        int c1 = 0;
        int r0 = 0;
        int r1 = 0;
        if (!span(box, &c0, &c1, &r0, &r1)) {
            return false;
        }
        for (int r = r0; r <= r1; r++) {
            for (int c = c0; c <= c1; c++) {
                const std::vector<int>& cell =
                    cells_[(static_cast<size_t>(r) * static_cast<size_t>(cols_)) + static_cast<size_t>(c)];
                if (std::any_of(cell.begin(), cell.end(),
                                [&](int i) { return boxes_overlap(box, boxes_[static_cast<size_t>(i)]); })) {
                    return true;
                }
            }
        }
        return false;
    }

  private:
    /** @brief The cells @p box covers, clamped to the grid; false when it has no area or lies outside it. */
    bool
    span(const LabelBox& box, int* c0, int* c1, int* r0, int* r1) const {
        if (!(box.width > 0.0F) || !(box.height > 0.0F) || !std::isfinite(box.left) || !std::isfinite(box.top)) {
            return false;
        }
        const float grid_w = static_cast<float>(cols_) * cell_;
        const float grid_h = static_cast<float>(rows_) * cell_;
        /* Clamped to the grid before any cast: a width of any size (an infinite one too) reaches its last cell. */
        const float left = std::max(0.0F, box.left);
        const float top = std::max(0.0F, box.top);
        const float right = std::min(grid_w, box.left + box.width);
        const float bottom = std::min(grid_h, box.top + box.height);
        if (!(right > 0.0F) || !(bottom > 0.0F) || left >= grid_w || top >= grid_h) {
            return false;
        }
        *c0 = std::min(cols_ - 1, static_cast<int>(std::floor(left / cell_)));
        *r0 = std::min(rows_ - 1, static_cast<int>(std::floor(top / cell_)));
        *c1 = std::min(cols_ - 1, static_cast<int>(std::floor(right / cell_)));
        *r1 = std::min(rows_ - 1, static_cast<int>(std::floor(bottom / cell_)));
        return true;
    }

    float cell_ = 48.0F;
    int cols_ = 1;
    int rows_ = 1;
    std::vector<LabelBox> boxes_;
    std::vector<uint32_t> stamps_; /* overlap_area()'s mark: a box in several cells is counted once */
    uint32_t query_ = 0U;
    std::vector<std::vector<int>> cells_;
};

/** @brief A label to place: the centre of its marker, and the label's size, logical pixels. */
struct LabelRequest {
    float x = 0.0F;
    float y = 0.0F;
    float width = 0.0F;
    float height = 0.0F;
    /** An emergency's label (it carries the word): never dropped, see place_labels(). */
    bool urgent = false;
};

/** @brief Sides a label may take around its marker, in the order tried: right, left, above, below. */
constexpr int kLabelSides = 4;

/** @brief The box of a @p r label on side @p side of its marker, @p reach from the marker's centre. */
inline LabelBox
label_side_box(const LabelRequest& r, float reach, int side) {
    switch (side) {
        case 1: /* left */ return LabelBox{r.x - reach - r.width, r.y - (r.height / 2.0F), r.width, r.height};
        case 2: /* above */ return LabelBox{r.x - (r.width / 2.0F), r.y - reach - r.height, r.width, r.height};
        case 3: /* below */ return LabelBox{r.x - (r.width / 2.0F), r.y + reach, r.width, r.height};
        default: /* right */ return LabelBox{r.x + reach, r.y - (r.height / 2.0F), r.width, r.height};
    }
}

/** @brief What a label must keep clear of: the markers drawn, the screen's keep-outs, and the labels placed so far. */
struct LabelSpace {
    BoxGrid markers;
    BoxGrid keep_out;
    BoxGrid labels;
    /** Half a marker's drawn size: a marker whose centre is this close past the view's edge still shows, and is
     *  labelled. */
    float marker_half = 0.0F;

    void
    reset(float width, float height, float cell) {
        markers.reset(width, height, cell);
        keep_out.reset(width, height, cell);
        labels.reset(width, height, cell);
    }

    bool
    clear(const LabelBox& box) const {
        return !markers.hits(box) && !keep_out.hits(box) && !labels.hits(box);
    }
};

/** @brief @p box moved the least distance to lie inside @p area (its top left kept in when it is the larger). */
inline LabelBox
moved_inside(LabelBox box, const LabelBox& area) {
    box.left = std::max(area.left, std::min(box.left, (area.left + area.width) - box.width));
    box.top = std::max(area.top, std::min(box.top, (area.top + area.height) - box.height));
    return box;
}

/**
 * @brief The side of an emergency's label when none is clear: the one that covers the least, keep-outs avoided
 *        first.
 *
 * A side clear of every keep-out (the screen's controls and chips) wins over any side that is not; among those, the
 * one overlapping the least marker and label area. With no side clear of the keep-outs, the least overlap of all.
 */
inline LabelBox
least_covering_side(const LabelRequest& r, float reach, const LabelBox& area, LabelSpace& space) {
    LabelBox best{};
    bool best_clear_of_keep_out = false;
    float best_cover = 0.0F;
    for (int side = 0; side < kLabelSides; side++) {
        const LabelBox box = moved_inside(label_side_box(r, reach, side), area);
        const float keep_out = space.keep_out.overlap_area(box);
        const bool clear_of_keep_out = !(keep_out > 0.0F);
        const float cover = space.markers.overlap_area(box) + space.labels.overlap_area(box) + keep_out;
        const bool better = side == 0 || (clear_of_keep_out && !best_clear_of_keep_out)
                            || (clear_of_keep_out == best_clear_of_keep_out && cover < best_cover);
        if (better) {
            best = box;
            best_clear_of_keep_out = clear_of_keep_out;
            best_cover = cover;
        }
    }
    return best;
}

/** @brief The first side of @p r that lies inside @p area and is clear in @p space; false when none is. */
inline bool
clear_side(const LabelRequest& r, float reach, const LabelBox& area, const LabelSpace& space, LabelBox* out) {
    for (int side = 0; side < kLabelSides; side++) {
        const LabelBox box = label_side_box(r, reach, side);
        if (box_inside(box, area) && space.clear(box)) {
            *out = box;
            return true;
        }
    }
    return false;
}

/** @brief Whether @p r can be labelled at all: a label with size, for a marker at least partly inside @p area
 *         (its centre at most @p marker_half past an edge). */
inline bool
label_wanted(const LabelRequest& r, const LabelBox& area, float marker_half) {
    const float g = std::max(0.0F, marker_half);
    const bool anchored = r.x >= area.left - g && r.x <= area.left + area.width + g && r.y >= area.top - g
                          && r.y <= area.top + area.height + g;
    return anchored && r.width > 0.0F && r.height > 0.0F;
}

/** @brief LabelPlacement values in place_labels()'s @c keep. */
enum LabelPlacement : uint8_t {
    kLabelDropped = 0U, /**< not drawn */
    kLabelClear = 1U,   /**< on a clear side */
    kLabelCovering = 2U /**< an emergency's label with no clear side: it covers something, so it is drawn on a plate */
};

/**
 * @brief Place labels beside their markers: in the order given (highest priority first), each takes the first side
 *        (right, left, above, below) where it lies wholly inside @p area and covers no marker, keep-out or label
 *        already placed.
 *
 * An emergency's label (LabelRequest::urgent) is never dropped (spec 4.6: never hue alone): emergencies are placed
 * first, so every other label yields to them; one with no clear side takes least_covering_side() (it may cover a
 * marker, or another emergency's label: both are drawn). Any other label with no clear side is not drawn.
 * Only labels whose marker shows at least partly inside @p area are considered (LabelSpace::marker_half).
 *
 * @param reach  From the marker's centre to the label's near edge: half the marker and a gap, so a label never
 *               covers its own marker.
 * @param space  Markers and keep-outs already added; each placed label is added to its labels.
 * @param placed @p count entries: the box of each placed label.
 * @param keep   @p count entries, a LabelPlacement: kLabelClear, kLabelCovering (drawn on a plate) or
 *               kLabelDropped.
 * @return Labels placed.
 */
inline int
place_labels(const LabelRequest* requests, int count, float reach, const LabelBox& area, LabelSpace& space,
             LabelBox* placed, uint8_t* keep) {
    if (requests == nullptr || placed == nullptr || keep == nullptr || count <= 0) {
        return 0;
    }
    int kept = 0;
    for (int pass = 0; pass < 2; pass++) {
        const bool urgent = pass == 0;
        for (int i = 0; i < count; i++) {
            const LabelRequest& r = requests[i];
            if (r.urgent != urgent) {
                continue;
            }
            keep[i] = 0U;
            if (!label_wanted(r, area, space.marker_half)) {
                continue;
            }
            LabelBox box{};
            uint8_t how = kLabelClear;
            if (!clear_side(r, reach, area, space, &box)) {
                if (!urgent) {
                    continue;
                }
                box = least_covering_side(r, reach, area, space);
                how = kLabelCovering;
            }
            placed[i] = box;
            keep[i] = how;
            space.labels.add(box);
            kept++;
        }
    }
    return kept;
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

/** @brief How far content built for one zoom may be scaled to stand in for another: half a zoom step, sqrt(2). */
constexpr double kZoomScaleReach = 1.4142135623730951;

/** @brief How content drawn for one view maps onto another: screen(now, p) = scale * screen(built, p) + (dx, dy). */
struct ViewTransform {
    double scale = 1.0;
    double dx = 0.0;
    double dy = 0.0;
};

/**
 * @brief The scale and offset that carry content drawn for @p built onto @p now (any zoom, any size).
 *
 * Exact for every point drawn on the same copy of the world in both (wrap_copy()); east-west the shortest way round.
 * At the same zoom it is pan_offset() with a scale of 1.
 */
inline ViewTransform
view_transform(const map_math::Viewport& built, const map_math::Viewport& now) {
    const double world = map_math::world_size_px(now.zoom);
    ViewTransform t;
    t.scale = world / map_math::world_size_px(built.zoom);
    t.dx = (now.width / 2.0) - (t.scale * built.width / 2.0)
           - (map_math::nearest_wrap_delta(now.center_x - built.center_x) * world);
    t.dy = (now.height / 2.0) - (t.scale * built.height / 2.0) - ((now.center_y - built.center_y) * world);
    return t;
}

/**
 * @brief Whether content built for @p built (clipped to its view grown by @p margin) can stand in for @p now, scaled.
 *
 * For a zoom in progress (a pinch, a zoom animation), at the same view size: the scale stays within
 * 1 / @p max_scale .. @p max_scale (lines and edges are drawn that much thicker or thinner until the zoom settles and
 * the content is built again), and the built area, carried by view_transform(), still covers the whole of @p now's
 * view.
 */
inline bool
reusable_scaled(const map_math::Viewport& built, const map_math::Viewport& now, double margin, double max_scale) {
    /* The same size only: @p margin is the one the content was clipped with, which a resize would change. */
    if (std::fabs(built.width - now.width) > kSameZoom || std::fabs(built.height - now.height) > kSameZoom) {
        return false;
    }
    const ViewTransform t = view_transform(built, now);
    if (!(t.scale <= max_scale) || !(t.scale * max_scale >= 1.0)) {
        return false;
    }
    const double left = (t.scale * -margin) + t.dx;
    const double right = (t.scale * (built.width + margin)) + t.dx;
    const double top = (t.scale * -margin) + t.dy;
    const double bottom = (t.scale * (built.height + margin)) + t.dy;
    return left <= 0.0 && top <= 0.0 && right >= now.width && bottom >= now.height;
}

/**
 * @brief @p now with its centre carried on from @p built the shortest way round, not wrapped back into [0, 1).
 *
 * view_transform() moves content the short way; wrap_copy() must count copies against the same camera, or a camera
 * that crossed its own wrap (0.99 to 0.01) would look as if every point kept its copy.
 */
inline map_math::Viewport
continued_viewport(const map_math::Viewport& built, const map_math::Viewport& now) {
    map_math::Viewport v = now;
    v.center_x = built.center_x + map_math::nearest_wrap_delta(now.center_x - built.center_x);
    return v;
}

/**
 * @brief Which copy of the world @p p is drawn on in @p vp (screen_from_world()): whole worlds from the raw one.
 *
 * Content built for one view may be moved to show a pan only while every point it was placed by is still drawn on
 * the same copy; across the flip line, half a world from the centre, the marker jumps a world width and its trail
 * must jump with it.
 */
inline long long
wrap_copy(const map_math::Viewport& vp, map_math::WorldPoint p) {
    if (!std::isfinite(p.x)) {
        return 0;
    }
    return static_cast<long long>(std::floor((p.x - vp.center_x) + 0.5));
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
