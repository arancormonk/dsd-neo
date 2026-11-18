// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Triangles for the map's trails, accuracy circles and marker sprites.
 *
 * Qt-free, like spectrum_trace_geometry.h, whose stroke builder draws the
 * trails: headless Qt has no RHI backend, so the shape the GPU draws can only
 * be checked here. Everything is a triangle list in logical pixels, y down, in
 * whatever frame the caller projected into (map_math.h's screen coordinates,
 * or those shifted by a fixed origin to keep floats small).
 *
 * Stroke and circle vertices carry a coverage in [0, 1] that the material
 * multiplies into its colour, with a one-device-pixel ramp at every edge.
 * Pieces never overlap one another within one trail or one circle, so a
 * translucent colour (the faint accuracy circles) draws each pixel once; where
 * a trail's dashes or segments meet at a sharp turn the stroke builder's own
 * limits apply, which is why trails are drawn opaque.
 *
 * Builders append to a std::vector and return how many vertices they added.
 * They run on the GUI thread, never on the decode path.
 */

#ifndef DSD_NEO_SRC_UI_QT_MAP_GEOMETRY_H_
#define DSD_NEO_SRC_UI_QT_MAP_GEOMETRY_H_

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

#include "spectrum_trace_geometry.h"

namespace dsd_qt {
namespace map_geometry {

/** @brief A stroke or fill vertex: position, logical pixels, and coverage in [0, 1]. */
using spectrum_geometry::StrokeVertex;

/** @brief A sprite vertex: position, logical pixels, and texture coordinate in the atlas, 0..1. */
struct SpriteVertex {
    float x;
    float y;
    float u;
    float v;
};

/**
 * @brief How a trail is drawn.
 *
 * A solid trail leaves @c dash at 0. A dashed one (a format not confirmed by
 * a published specification) gives the visible length of each dash and each
 * gap; the dashes run on through the trail's turns.
 */
struct TrailStyle {
    float half_width = 1.0F; /**< half the pen width, logical pixels */
    float feather = 0.5F;    /**< coverage ramp, logical pixels: one device pixel */
    float dash = 0.0F;       /**< visible dash length, logical pixels; 0 or less: solid */
    float gap = 0.0F;        /**< visible gap length, logical pixels */
};

/** @brief A rectangle, logical pixels: the part of the frame the view shows. */
struct ClipRect {
    float left;
    float top;
    float right;
    float bottom;
};

/** @brief Below this two trail points are one: there is no direction to stroke along. */
constexpr float kMinStep = 1.0e-6F;

/** @brief Most slices an accuracy circle is cut into: the chord error stays within kCircleChordError up to a radius
 * of 340,000 px, and grows past it. */
constexpr int kMaxCircleSlices = 4096;

/** @brief Fewest slices of a small circle, so it still reads as round. */
constexpr int kMinCircleSlices = 16;

/** @brief Largest gap between a circle's chord and its arc, logical pixels: a fifth of a one-device-pixel ramp at
 * a 2x screen, so the edge's ramp keeps its width all round. */
constexpr float kCircleChordError = 0.1F;

/** @brief Stroke one run of points with spectrum_geometry::build_stroke(), appending; returns vertices added. */
inline int
append_stroke(const float* xs, const float* ys, int points, const TrailStyle& style, std::vector<StrokeVertex>& out) {
    const int capacity = spectrum_geometry::stroke_vertex_capacity(points);
    if (capacity <= 0) {
        return 0;
    }
    const size_t base = out.size();
    out.resize(base + static_cast<size_t>(capacity));
    const int added =
        spectrum_geometry::build_stroke(xs, ys, points, style.half_width, style.feather, 0.0F, out.data() + base);
    out.resize(base + static_cast<size_t>(added > 0 ? added : 0));
    return (added > 0) ? added : 0;
}

/** @brief One dash being collected while a dashed trail is walked. */
struct DashWalk {
    std::vector<float> xs;
    std::vector<float> ys;
    bool on = true;         /* in a dash rather than a gap */
    float remaining = 0.0F; /* centre-line length left in the current dash or gap */
    float dash = 0.0F;      /* centre-line lengths: see append_dashed() */
    float gap = 0.0F;
};

/** @brief End the dash being collected at (@p x, @p y) and stroke it; returns vertices added. */
inline int
dash_finish(DashWalk& walk, float x, float y, const TrailStyle& style, std::vector<StrokeVertex>& out) {
    walk.xs.push_back(x);
    walk.ys.push_back(y);
    const int added = append_stroke(walk.xs.data(), walk.ys.data(), static_cast<int>(walk.xs.size()), style, out);
    walk.xs.clear();
    walk.ys.clear();
    return added;
}

/** @brief Toggle between dash and gap at (@p x, @p y): end the dash being collected, or start a new one. */
inline int
dash_toggle(DashWalk& walk, float x, float y, const TrailStyle& style, std::vector<StrokeVertex>& out) {
    int added = 0;
    if (walk.on) {
        added = dash_finish(walk, x, y, style, out);
    } else {
        walk.xs.push_back(x);
        walk.ys.push_back(y);
    }
    walk.on = !walk.on;
    walk.remaining = walk.on ? walk.dash : walk.gap;
    return added;
}

/** @brief Move the dash pattern @p length along without drawing: whole periods are skipped in one step. */
inline void
dash_skip(DashWalk& walk, float length) {
    if (length <= walk.remaining) {
        walk.remaining -= length;
        return;
    }
    length -= walk.remaining;
    length = std::fmod(length, walk.dash + walk.gap);
    walk.on = !walk.on;
    walk.remaining = walk.on ? walk.dash : walk.gap;
    if (length > walk.remaining) {
        length -= walk.remaining;
        walk.on = !walk.on;
        walk.remaining = walk.on ? walk.dash : walk.gap;
    }
    walk.remaining -= length;
}

/** @brief The part of a segment inside a rectangle, as fractions of its length; empty when @c t1 < @c t0. */
struct SegmentSpan {
    float t0;
    float t1;
};

/** @brief Narrow @p span to where p + t * d is on the inner side of one edge: p.d * t <= limit. */
inline void
clip_edge(float start, float delta, float limit, SegmentSpan& span) {
    if (std::fabs(delta) < kMinStep) {
        if (start > limit) {
            span.t1 = -1.0F; /* parallel to the edge and outside it */
        }
        return;
    }
    const float t = (limit - start) / delta;
    if (delta > 0.0F) {
        span.t1 = std::min(span.t1, t);
    } else {
        span.t0 = std::max(span.t0, t);
    }
}

/** @brief The part of the segment from (@p ax, @p ay) to (@p bx, @p by) inside @p clip grown by @p margin. */
inline SegmentSpan
clip_segment(float ax, float ay, float bx, float by, float margin, const ClipRect& clip) {
    SegmentSpan span{0.0F, 1.0F};
    clip_edge(-ax, -(bx - ax), -(clip.left - margin), span);
    clip_edge(ax, bx - ax, clip.right + margin, span);
    clip_edge(-ay, -(by - ay), -(clip.top - margin), span);
    clip_edge(ay, by - ay, clip.bottom + margin, span);
    return span;
}

/**
 * @brief Walk a stretch of a trail the view does not show: keep the pattern's phase, draw nothing on it.
 *
 * Both ends of such a stretch lie outside the clip by at least the pen's
 * reach, so a dash running into it is ended at its start and a dash on at its
 * far end starts again there: the cut ends' caps fall outside the view.
 */
inline int
dash_hidden_segment(DashWalk& walk, float px, float py, float qx, float qy, float len, const TrailStyle& style,
                    std::vector<StrokeVertex>& out) {
    int added = 0;
    if (walk.on) {
        added = dash_finish(walk, px, py, style, out);
    }
    dash_skip(walk, len);
    if (walk.on) {
        walk.xs.push_back(qx);
        walk.ys.push_back(qy);
    }
    return added;
}

/** @brief Walk a segment the view shows, cutting it into dashes. */
inline int
dash_shown_segment(DashWalk& walk, float px, float py, float qx, float qy, float len, const TrailStyle& style,
                   std::vector<StrokeVertex>& out) {
    int added = 0;
    float t = 0.0F;
    while ((len - t) > walk.remaining) {
        t += walk.remaining;
        const float k = t / len;
        added += dash_toggle(walk, px + ((qx - px) * k), py + ((qy - py) * k), style, out);
    }
    walk.remaining -= (len - t);
    if (walk.on) {
        walk.xs.push_back(qx);
        walk.ys.push_back(qy);
    }
    return added;
}

/**
 * @brief Walk one segment of a dashed trail: what @p clip shows is cut into dashes, the rest only moves the pattern on.
 *
 * However long the segment, the parts outside @p clip (grown by the pen's
 * reach) cost nothing per dash they would have held.
 */
inline int
dash_segment(DashWalk& walk, float px, float py, float qx, float qy, float len, const ClipRect* clip,
             const TrailStyle& style, std::vector<StrokeVertex>& out) {
    if (clip == nullptr) {
        return dash_shown_segment(walk, px, py, qx, qy, len, style, out);
    }
    const SegmentSpan span = clip_segment(px, py, qx, qy, style.half_width + std::max(style.feather, 0.0F), *clip);
    if (!(span.t1 > span.t0)) {
        return dash_hidden_segment(walk, px, py, qx, qy, len, style, out);
    }
    const float ax = px + ((qx - px) * span.t0);
    const float ay = py + ((qy - py) * span.t0);
    const float bx = px + ((qx - px) * span.t1);
    const float by = py + ((qy - py) * span.t1);
    int added = 0;
    if (span.t0 > 0.0F) {
        added += dash_hidden_segment(walk, px, py, ax, ay, len * span.t0, style, out);
    }
    added += dash_shown_segment(walk, ax, ay, bx, by, len * (span.t1 - span.t0), style, out);
    if (span.t1 < 1.0F) {
        added += dash_hidden_segment(walk, bx, by, qx, qy, len * (1.0F - span.t1), style, out);
    }
    return added;
}

/**
 * @brief Stroke one run of finite points as dashes, appending; returns vertices added.
 *
 * Square caps reach half a pen width past each end of a dash, so the centre
 * line of each dash is a pen width shorter than the visible dash, and of each
 * gap a pen width longer. The ends are cut square with no ramp along the line,
 * as the stroke's caps are. A dash that bends round a turn is one stroke, with
 * the turn's round join. Where @p clip is given, the parts of segments it does
 * not show only move the pattern on (dash_segment()).
 */
inline int
append_dashed(const float* xs, const float* ys, int points, const TrailStyle& style, const ClipRect* clip,
              std::vector<StrokeVertex>& out) {
    DashWalk walk;
    const float pen = 2.0F * style.half_width;
    walk.dash = std::max(style.dash - pen, 0.01F);
    walk.gap = std::max(style.gap, 0.0F) + pen;
    walk.remaining = walk.dash;
    walk.xs.push_back(xs[0]);
    walk.ys.push_back(ys[0]);
    int added = 0;
    for (int i = 1; i < points; i++) {
        const float px = xs[i - 1];
        const float py = ys[i - 1];
        const float len = std::hypot(xs[i] - px, ys[i] - py);
        if (!(len >= kMinStep)) {
            continue;
        }
        added += dash_segment(walk, px, py, xs[i], ys[i], len, clip, style, out);
    }
    if (walk.on && walk.xs.size() >= 2U) {
        added += append_stroke(walk.xs.data(), walk.ys.data(), static_cast<int>(walk.xs.size()), style, out);
    }
    return added;
}

/** @brief Whether point @p i has both coordinates finite: a trail runs through it rather than breaking there. */
inline bool
finite_point(const float* xs, const float* ys, int i) {
    return std::isfinite(xs[i]) && std::isfinite(ys[i]);
}

/** @brief Whether @p style can stroke at all: a positive, finite pen and a finite ramp. */
inline bool
usable_pen(const TrailStyle& style) {
    return style.half_width > 0.0F && std::isfinite(style.half_width) && std::isfinite(style.feather);
}

/**
 * @brief Stroke a trail, appending its triangles; returns vertices added.
 *
 * A point with a non-finite coordinate breaks the trail there: the runs either
 * side are stroked apart, never joined across it. Callers mark the gaps the
 * position model reports (lost reports, a new stream) the same way, so no line
 * is drawn across missing positions. A run of fewer than two usable points
 * draws nothing.
 *
 * A solid trail costs a bounded number of vertices per point wherever it lies.
 * A dashed one costs one stroke per dash, and a long segment can hold any
 * number of dashes; @p clip (the view's rectangle grown by whatever margin the
 * caller pans within before rebuilding, or null for none) keeps the parts of
 * segments outside it to their phase only.
 *
 * @param xs,ys @p points coordinates, logical pixels, oldest first.
 */
inline int
append_trail(const float* xs, const float* ys, int points, const TrailStyle& style, const ClipRect* clip,
             std::vector<StrokeVertex>& out) {
    if (xs == nullptr || ys == nullptr || points < 2 || !usable_pen(style)) {
        return 0;
    }
    const bool dashed = (style.dash > 0.0F) && std::isfinite(style.dash) && std::isfinite(style.gap);
    int added = 0;
    int start = 0;
    while (start < points) {
        while (start < points && !finite_point(xs, ys, start)) {
            start++;
        }
        int end = start;
        while (end < points && finite_point(xs, ys, end)) {
            end++;
        }
        const int run = end - start;
        if (run >= 2) {
            added += dashed ? append_dashed(xs + start, ys + start, run, style, clip, out)
                            : append_stroke(xs + start, ys + start, run, style, out);
        }
        start = end;
    }
    return added;
}

/** @brief Slices that keep a circle of @p radius within kCircleChordError of round. */
inline int
circle_slices(float radius) {
    if (!(radius > kCircleChordError)) {
        return kMinCircleSlices;
    }
    /* A slice of angle a falls short of its arc by r * (1 - cos(a / 2)). */
    const double half = std::acos(1.0 - (static_cast<double>(kCircleChordError) / static_cast<double>(radius)));
    const double slices = std::ceil(3.14159265358979323846 / half);
    if (!(slices < static_cast<double>(kMaxCircleSlices))) {
        return kMaxCircleSlices;
    }
    return std::max(kMinCircleSlices, static_cast<int>(slices));
}

/** @brief Whether the triangle (a, b, c)'s bounding box meets @p clip. */
inline bool
box_meets(float ax, float ay, float bx, float by, float cx, float cy, const ClipRect& clip) {
    const float left = std::min(ax, std::min(bx, cx));
    const float right = std::max(ax, std::max(bx, cx));
    const float top = std::min(ay, std::min(by, cy));
    const float bottom = std::max(ay, std::max(by, cy));
    return right >= clip.left && left <= clip.right && bottom >= clip.top && top <= clip.bottom;
}

/** @brief Whether every corner of @p clip is within @p r of (@p cx, @p cy). */
inline bool
circle_holds(float cx, float cy, float r, const ClipRect& clip) {
    const float dx = std::max(std::fabs(clip.left - cx), std::fabs(clip.right - cx));
    const float dy = std::max(std::fabs(clip.top - cy), std::fabs(clip.bottom - cy));
    return std::hypot(dx, dy) <= r;
}

/** @brief A disc's solid core and its edge ramp. */
struct Disc {
    float cx;
    float cy;
    float inner; /* coverage is @c core out to here */
    float outer; /* and 0 here */
    float core;
};

inline void
push_vertex(std::vector<StrokeVertex>& out, float x, float y, float coverage) {
    out.push_back(StrokeVertex{x, y, coverage});
}

/** @brief One slice of a disc between angles @p a0 and @p a1: the core wedge and the edge band. 9 vertices. */
inline void
push_disc_slice(const Disc& d, double a0, double a1, std::vector<StrokeVertex>& out) {
    const float c0 = static_cast<float>(std::cos(a0));
    const float s0 = static_cast<float>(std::sin(a0));
    const float c1 = static_cast<float>(std::cos(a1));
    const float s1 = static_cast<float>(std::sin(a1));
    const float ix0 = d.cx + (c0 * d.inner);
    const float iy0 = d.cy + (s0 * d.inner);
    const float ix1 = d.cx + (c1 * d.inner);
    const float iy1 = d.cy + (s1 * d.inner);
    const float ox0 = d.cx + (c0 * d.outer);
    const float oy0 = d.cy + (s0 * d.outer);
    const float ox1 = d.cx + (c1 * d.outer);
    const float oy1 = d.cy + (s1 * d.outer);
    push_vertex(out, d.cx, d.cy, d.core);
    push_vertex(out, ix0, iy0, d.core);
    push_vertex(out, ix1, iy1, d.core);
    push_vertex(out, ix0, iy0, d.core);
    push_vertex(out, ox0, oy0, 0.0F);
    push_vertex(out, ix1, iy1, d.core);
    push_vertex(out, ix1, iy1, d.core);
    push_vertex(out, ox0, oy0, 0.0F);
    push_vertex(out, ox1, oy1, 0.0F);
}

/** @brief The whole of @p clip at coverage 1, as two triangles. */
inline int
append_clip_quad(const ClipRect& clip, std::vector<StrokeVertex>& out) {
    push_vertex(out, clip.left, clip.top, 1.0F);
    push_vertex(out, clip.right, clip.top, 1.0F);
    push_vertex(out, clip.left, clip.bottom, 1.0F);
    push_vertex(out, clip.left, clip.bottom, 1.0F);
    push_vertex(out, clip.right, clip.top, 1.0F);
    push_vertex(out, clip.right, clip.bottom, 1.0F);
    return 6;
}

/**
 * @brief A filled accuracy circle, appending its triangles; returns vertices added.
 *
 * Coverage is solid out to `radius - feather/2` and falls to zero at
 * `radius + feather/2`, so the edge sits on the radius. A position error is
 * often far larger than the view at street zoom, so only what @p clip shows is
 * built: a circle holding the whole of @p clip is two triangles covering it, a
 * circle wholly outside it is nothing, and otherwise only the slices whose
 * extent meets @p clip are kept (the ones left out cannot touch it). Pass the
 * view's rectangle, grown by whatever margin the caller pans within before
 * rebuilding.
 *
 * @param radius Logical pixels (map_math::metres_to_px()); nothing for 0, less or NaN.
 */
inline int
append_accuracy_circle(float cx, float cy, float radius, float feather, const ClipRect& clip,
                       std::vector<StrokeVertex>& out) {
    if (!(radius > 0.0F) || !std::isfinite(radius) || !std::isfinite(cx) || !std::isfinite(cy)
        || !std::isfinite(feather) || !(clip.right > clip.left) || !(clip.bottom > clip.top)) {
        return 0;
    }
    const float ramp = std::max(feather, 0.0F);
    const float outer = radius + (ramp / 2.0F);
    const Disc disc{
        cx, cy, std::max(0.0F, radius - (ramp / 2.0F)), outer, (ramp > 0.0F) ? std::min(1.0F, outer / ramp) : 1.0F,
    };
    if (!box_meets(cx - outer, cy - outer, cx + outer, cy + outer, cx, cy, clip)) {
        return 0;
    }
    if (disc.core >= 1.0F && circle_holds(cx, cy, disc.inner, clip)) {
        return append_clip_quad(clip, out);
    }
    const size_t base = out.size();
    const int slices = circle_slices(radius);
    const double step = (2.0 * 3.14159265358979323846) / static_cast<double>(slices);
    /* A slice's wedge lies inside the triangle from the centre through its
     * outer edge's two ends pushed out to the tangent lines; that triangle's
     * box is the test, so a kept wedge is never missing a part the clip shows. */
    const float reach = outer / static_cast<float>(std::cos(step / 2.0));
    for (int i = 0; i < slices; i++) {
        const double a0 = step * static_cast<double>(i);
        const double a1 = step * static_cast<double>(i + 1);
        const float ex0 = cx + (static_cast<float>(std::cos(a0)) * reach);
        const float ey0 = cy + (static_cast<float>(std::sin(a0)) * reach);
        const float ex1 = cx + (static_cast<float>(std::cos(a1)) * reach);
        const float ey1 = cy + (static_cast<float>(std::sin(a1)) * reach);
        if (box_meets(cx, cy, ex0, ey0, ex1, ey1, clip)) {
            push_disc_slice(disc, a0, a1, out);
        }
    }
    return static_cast<int>(out.size() - base);
}

/** @brief One cell of a sprite atlas, as texture coordinates. */
struct AtlasCell {
    float u0;
    float v0;
    float u1;
    float v1;
};

/**
 * @brief Texture coordinates of the cell at @p column, @p row in an atlas of @p cell_px square cells.
 *
 * Inset by half a texel on every side, so linear filtering never reaches into
 * the neighbouring cell. A cell outside the atlas, or a bad size, is the empty
 * cell at the origin.
 */
inline AtlasCell
atlas_cell(int column, int row, int cell_px, int atlas_width_px, int atlas_height_px) {
    const AtlasCell none{0.0F, 0.0F, 0.0F, 0.0F};
    if (cell_px <= 0 || atlas_width_px <= 0 || atlas_height_px <= 0 || column < 0 || row < 0
        || (column + 1) * cell_px > atlas_width_px || (row + 1) * cell_px > atlas_height_px) {
        return none;
    }
    const float w = static_cast<float>(atlas_width_px);
    const float h = static_cast<float>(atlas_height_px);
    const float x0 = static_cast<float>(column * cell_px);
    const float y0 = static_cast<float>(row * cell_px);
    const float edge = static_cast<float>(cell_px);
    return AtlasCell{(x0 + 0.5F) / w, (y0 + 0.5F) / h, (x0 + edge - 0.5F) / w, (y0 + edge - 0.5F) / h};
}

/**
 * @brief A marker sprite centred on (@p cx, @p cy), @p size logical pixels square, appending 6 vertices.
 *
 * The quad's top left corner is snapped to the device pixel grid at
 * @p device_pixel_ratio so a sprite drawn at its own resolution stays sharp
 * rather than smeared across two pixels. Nothing is added for a non-positive
 * size, a non-finite position or ratio.
 *
 * @return Vertices added: 6 or 0.
 */
inline int
append_marker_quad(float cx, float cy, float size, float device_pixel_ratio, const AtlasCell& cell,
                   std::vector<SpriteVertex>& out) {
    if (!(size > 0.0F) || !std::isfinite(size) || !std::isfinite(cx) || !std::isfinite(cy)
        || !(device_pixel_ratio > 0.0F) || !std::isfinite(device_pixel_ratio)) {
        return 0;
    }
    const float left = std::round((cx - (size / 2.0F)) * device_pixel_ratio) / device_pixel_ratio;
    const float top = std::round((cy - (size / 2.0F)) * device_pixel_ratio) / device_pixel_ratio;
    const float right = left + size;
    const float bottom = top + size;
    out.push_back(SpriteVertex{left, top, cell.u0, cell.v0});
    out.push_back(SpriteVertex{right, top, cell.u1, cell.v0});
    out.push_back(SpriteVertex{left, bottom, cell.u0, cell.v1});
    out.push_back(SpriteVertex{left, bottom, cell.u0, cell.v1});
    out.push_back(SpriteVertex{right, top, cell.u1, cell.v0});
    out.push_back(SpriteVertex{right, bottom, cell.u1, cell.v1});
    return 6;
}

} // namespace map_geometry
} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_MAP_GEOMETRY_H_ */
