// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "spectrum_trace_geometry.h"

#include <cmath>

namespace dsd_qt {
namespace spectrum_geometry {

namespace {

struct Vec {
    float x;
    float y;
};

/* Below this two points are one: there is no direction to stroke along. */
constexpr float kMinSegment = 1.0e-6F;
/* sin(0.5 degrees). A turn this shallow leaves no gap a join would show. */
constexpr float kMinTurn = 0.0087F;
/* A round join is a fan of slices no wider than this: 22.5 degrees, so the
 * widest turn, a full reversal, takes eight. A chord falls short of its arc by
 * 2% of the radius — a twentieth of a device pixel at a phone's 2.6x, which
 * the fringe turns into a twentieth less coverage at most. (At 45 degrees it
 * was 8%, and up to 15% less coverage on the outside of a turn.) */
constexpr float kJoinSliceAngle = 0.39269908F;
constexpr int kMaxJoinSlices = 8;

Vec
add(Vec a, Vec b, float k) {
    return Vec{a.x + (b.x * k), a.y + (b.y * k)};
}

Vec
point_at(const float* xs, const float* ys, int i, float fallback_y) {
    return Vec{xs[i], std::isfinite(ys[i]) ? ys[i] : fallback_y};
}

/** @brief Index of the first point from @p from that is a usable step away from @p p, or -1. */
int
next_usable(const float* xs, const float* ys, int points, int from, Vec p, float fallback_y) {
    for (int i = from; i < points; i++) {
        if (!std::isfinite(xs[i])) {
            continue;
        }
        const Vec q = point_at(xs, ys, i, fallback_y);
        if (std::hypot(q.x - p.x, q.y - p.y) >= kMinSegment) {
            return i;
        }
    }
    return -1;
}

int
emit(StrokeVertex* out, int at, Vec v, float coverage) {
    out[at] = StrokeVertex{v.x, v.y, coverage};
    return at + 1;
}

/**
 * @brief How one end of a segment is cut where it meets the next one.
 *
 * Inside a turn the two segments would overlap. Each is cut on its inner side
 * along the turn's bisector instead, so they meet edge to edge, as the inside
 * of a stroked outline does: a point @c o off the center line on the inner side
 * moves |o| * k along the segment, k = tan(turn / 2). @c inner_side is +1 or -1
 * for which normal points into the turn; 0 leaves the end square.
 */
struct Trim {
    int inner_side;
    float k;
};

/** @brief A segment's two ends (caps included), its unit direction and normal, and its end cuts. */
struct Segment {
    Vec a;
    Vec b;
    Vec d;
    Vec n;
    Trim start;
    Trim end;
};

/**
 * @brief The ramp across a stroke's edge: opaque out to @c inner, nothing past @c outer.
 *
 * @c core is the coverage at and inside @c inner. It is 1 unless the feather is
 * wider than the pen (a very low pixel ratio): then @c inner is 0 and the ramp
 * never reaches full coverage, so the center gets what the ramp gives there,
 * outer / feather, rather than being forced opaque.
 */
struct Pen {
    float inner;
    float outer;
    float core;
};

/** @brief The cut for the joint between two segments of lengths @p len_in and @p len_out. */
Trim
joint_trim(Vec d_in, Vec d_out, float len_in, float len_out, float outer) {
    const Trim none{0, 0.0F};
    const float cross = (d_in.x * d_out.y) - (d_in.y * d_out.x);
    const float dot = (d_in.x * d_out.x) + (d_in.y * d_out.y);
    if (std::fabs(cross) < kMinTurn || !((1.0F + dot) > kMinTurn)) {
        return none; /* straight on, or a reversal with no bisector to cut along */
    }
    const float k = std::fabs(cross) / (1.0F + dot);
    /* A cut reaching back past half a segment would cross the cut at its other
     * end. That only happens at turns so sharp that each segment's core already
     * covers the other's inner fringe, so the overlap left there is not seen. */
    if (outer * k > 0.5F * std::fmin(len_in, len_out)) {
        return none;
    }
    /* With y down and n = (-d.y, d.x), a positive cross turns towards +n. */
    return Trim{(cross > 0.0F) ? 1 : -1, k};
}

/** @brief The corner @p offset off the center line at one end of @p s, after that end's cut. */
Vec
corner(const Segment& s, bool at_end, float offset) {
    const Trim& trim = at_end ? s.end : s.start;
    float shift = 0.0F;
    if ((trim.inner_side > 0 && offset > 0.0F) || (trim.inner_side < 0 && offset < 0.0F)) {
        shift = std::fabs(offset) * trim.k;
    }
    return add(add(at_end ? s.b : s.a, s.n, offset), s.d, at_end ? -shift : shift);
}

/** @brief A quad along @p s between offsets o1 and o2, coverage c1 at o1 and c2 at o2. */
int
emit_band(StrokeVertex* out, int at, const Segment& s, float o1, float c1, float o2, float c2) {
    const Vec a1 = corner(s, false, o1);
    const Vec b1 = corner(s, true, o1);
    const Vec a2 = corner(s, false, o2);
    const Vec b2 = corner(s, true, o2);
    at = emit(out, at, a1, c1);
    at = emit(out, at, b1, c1);
    at = emit(out, at, a2, c2);
    at = emit(out, at, a2, c2);
    at = emit(out, at, b1, c1);
    return emit(out, at, b2, c2);
}

/**
 * @brief One full-width segment: left fringe, the core as two halves, right fringe. 24 vertices.
 *
 * The core is split at the center line because its two sides end differently
 * at a cut joint: square on the outside of the turn, along the bisector on the
 * inside. One quad across both would cut straight from corner to corner and
 * miss the joint point itself.
 */
int
emit_segment(StrokeVertex* out, int at, const Segment& s, const Pen& pen) {
    at = emit_band(out, at, s, -pen.outer, 0.0F, -pen.inner, pen.core);
    at = emit_band(out, at, s, -pen.inner, pen.core, 0.0F, pen.core);
    at = emit_band(out, at, s, 0.0F, pen.core, pen.inner, pen.core);
    return emit_band(out, at, s, pen.inner, pen.core, pen.outer, 0.0F);
}

Vec
rotate(Vec v, float angle) {
    const float c = std::cos(angle);
    const float s = std::sin(angle);
    return Vec{(v.x * c) - (v.y * s), (v.x * s) + (v.y * c)};
}

/** @brief One slice of a round join between unit directions @p u0 and @p u1. 9 vertices. */
int
emit_join_slice(StrokeVertex* out, int at, Vec p, Vec u0, Vec u1, const Pen& pen) {
    const Vec core0 = add(p, u0, pen.inner);
    const Vec core1 = add(p, u1, pen.inner);
    const Vec edge0 = add(p, u0, pen.outer);
    const Vec edge1 = add(p, u1, pen.outer);
    at = emit(out, at, p, pen.core);
    at = emit(out, at, core0, pen.core);
    at = emit(out, at, core1, pen.core);
    at = emit(out, at, core0, pen.core);
    at = emit(out, at, core1, pen.core);
    at = emit(out, at, edge0, 0.0F);
    at = emit(out, at, edge0, 0.0F);
    at = emit(out, at, core1, pen.core);
    return emit(out, at, edge1, 0.0F);
}

/**
 * @brief The round join that fills the outside of a turn at @p p, or nothing for a straight run.
 *
 * Two full-width segments meeting at an angle overlap on the inside of the turn
 * and leave a wedge uncovered on the outside. A fan around @p p covers exactly
 * that wedge, from the incoming segment's outer edge to the outgoing one's, so
 * on the outside nothing is covered twice; and its fringe runs radially, so the
 * tip of a spike keeps a full one-pixel ramp. (A bevel's edge across a spike
 * tip is a few hundredths of a pixel long, and its fringe with it: the tip
 * would be aliased.) QPainter bevels; at a spike tip this reaches half a pen
 * width further, round instead of flat.
 */
int
emit_join(StrokeVertex* out, int at, Vec p, Vec d_in, Vec d_out, const Pen& pen) {
    const float cross = (d_in.x * d_out.y) - (d_in.y * d_out.x);
    const float dot = (d_in.x * d_out.x) + (d_in.y * d_out.y);
    if (std::fabs(cross) < kMinTurn && dot > 0.0F) {
        return at; /* straight on: the butt ends meet exactly */
    }
    const float turn = std::atan2(std::fabs(cross), dot); /* 0 .. pi */
    /* With y down and n = (-d.y, d.x), a positive cross turns towards +n, so
     * the outside is -n. A full reversal has no inside; either side will do,
     * as long as the fan sweeps through the direction the line arrived in. */
    const float side = (cross > 0.0F) ? -1.0F : 1.0F;
    const Vec start{-d_in.y * side, d_in.x * side};
    const float ahead = (-start.y * d_in.x) + (start.x * d_in.y); /* start turned +90 degrees, against d_in */
    const float sweep = (ahead > 0.0F) ? turn : -turn;
    int slices = static_cast<int>(std::ceil(turn / kJoinSliceAngle));
    slices = (slices < 1) ? 1 : ((slices > kMaxJoinSlices) ? kMaxJoinSlices : slices);
    Vec from = start;
    for (int i = 1; i <= slices; i++) {
        const Vec to = rotate(start, (sweep * static_cast<float>(i)) / static_cast<float>(slices));
        at = emit_join_slice(out, at, p, from, to, pen);
        from = to;
    }
    return at;
}

/** @brief A polyline's points and the y that stands in for a non-finite one. */
struct Polyline {
    const float* xs;
    const float* ys;
    int points;
    float fallback_y;
};

Pen
make_pen(float half_width, float feather) {
    const float ramp = (feather > 0.0F) ? feather : 0.0F;
    const float outer = half_width + (ramp / 2.0F);
    return Pen{std::fmax(0.0F, half_width - (ramp / 2.0F)), outer,
               (ramp > 0.0F) ? std::fmin(1.0F, outer / ramp) : 1.0F};
}

/** @brief Index of the first point with a finite x, or -1. */
int
first_usable(const Polyline& line) {
    for (int i = 0; i < line.points; i++) {
        if (std::isfinite(line.xs[i])) {
            return i;
        }
    }
    return -1;
}

/**
 * @brief The segment from @p p to point @p to, its start cut @p start and, when a point @p after follows, its end cut.
 */
Segment
segment_between(const Polyline& line, Vec p, int to, int after, Trim start, const Pen& pen) {
    const Vec q = point_at(line.xs, line.ys, to, line.fallback_y);
    const float len = std::hypot(q.x - p.x, q.y - p.y);
    Segment s{p, q, Vec{(q.x - p.x) / len, (q.y - p.y) / len}, Vec{0.0F, 0.0F}, start, Trim{0, 0.0F}};
    s.n = Vec{-s.d.y, s.d.x};
    if (after >= 0) {
        const Vec r = point_at(line.xs, line.ys, after, line.fallback_y);
        const float len_next = std::hypot(r.x - q.x, r.y - q.y);
        s.end = joint_trim(s.d, Vec{(r.x - q.x) / len_next, (r.y - q.y) / len_next}, len, len_next, pen.outer);
    }
    return s;
}

} // namespace

int
stroke_vertex_capacity(int points) {
    if (points < 2) {
        return 0;
    }
    return ((points - 1) * 24) + ((points - 2) * 9 * kMaxJoinSlices);
}

int
build_stroke(const float* xs, const float* ys, int points, float half_width, float feather, float fallback_y,
             StrokeVertex* out) {
    if (xs == nullptr || ys == nullptr || out == nullptr || points < 2 || !(half_width > 0.0F)
        || !std::isfinite(fallback_y)) {
        return 0;
    }
    const Polyline line{xs, ys, points, fallback_y};
    const Pen pen = make_pen(half_width, feather);
    const int first = first_usable(line);
    if (first < 0) {
        return 0;
    }
    Vec p = point_at(xs, ys, first, fallback_y);
    int next = next_usable(xs, ys, points, first + 1, p, fallback_y);
    int at = 0;
    bool have_prev = false;
    Vec d_prev{0.0F, 0.0F};
    Trim start{0, 0.0F};
    while (next >= 0) {
        const int after = next_usable(xs, ys, points, next + 1, point_at(xs, ys, next, fallback_y), fallback_y);
        Segment s = segment_between(line, p, next, after, start, pen);
        if (have_prev) {
            at = emit_join(out, at, p, d_prev, s.d, pen);
        }
        /* Square caps: the two ends reach half a pen width past their points. */
        if (!have_prev) {
            s.a = add(s.a, s.d, -half_width);
        }
        if (after < 0) {
            s.b = add(s.b, s.d, half_width);
        }
        at = emit_segment(out, at, s, pen);
        d_prev = s.d;
        start = s.end;
        have_prev = true;
        p = point_at(xs, ys, next, fallback_y);
        next = after;
    }
    return at;
}

int
build_area_strip(const float* ys, int columns, float baseline, AreaVertex* out) {
    if (ys == nullptr || out == nullptr || columns < 2 || !std::isfinite(baseline)) {
        return 0;
    }
    int at = 0;
    for (int x = 0; x < columns; x++) {
        const float fx = static_cast<float>(x);
        out[at++] = AreaVertex{fx, std::isfinite(ys[x]) ? ys[x] : baseline};
        out[at++] = AreaVertex{fx, baseline};
    }
    return at;
}

} // namespace spectrum_geometry
} // namespace dsd_qt
