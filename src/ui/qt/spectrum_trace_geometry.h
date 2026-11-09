// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Triangles for the spectrum trace's antialiased line, grid and filled area.
 *
 * Qt-free so the shape the GPU draws can be checked without a window or a
 * scene graph: headless Qt has no RHI backend, so no test can grab the real
 * rendering.
 *
 * A line is stroked close to the way QPainter strokes a QPen with its
 * defaults: every segment keeps the full pen width and the two ends get square
 * caps. Joins are round where QPainter bevels, because a bevel across a spike
 * tip is too short to carry an antialiased edge. Antialiasing is a coverage
 * ramp across the nominal edge rather than outside it — opaque out to
 * `half_width - feather/2`, falling to zero at `half_width + feather/2` — so
 * the line covers as much as the pen it replaces.
 *
 * The pieces are blended one over another, not merged into one outline the way
 * QPainter fills a stroke, so they are cut to meet edge to edge instead of
 * overlapping: a round join's fan fills the outside of a turn, and inside it
 * both segments are cut along the turn's bisector. Around a single turn that
 * leaves the coverage of the exact ramped outline, short only by the fan's
 * chord error. Two limits remain, both measured by the test:
 *
 *  - pieces that are not neighbours still overlap when the pen is wider than
 *    the segments are long, as on the spectrum, one point per pixel column with
 *    a 1.5 px pen. Where two of their fringes cross outside every core, a
 *    pixel's edge gets up to a quarter more coverage than the outline gives;
 *  - at turns too sharp to cut (a cut would reach past half a segment) the two
 *    segments overlap, each core covering the other's inner fringe.
 *
 * An opaque line shows neither beyond that; a translucent one would draw every
 * overlap twice. The spectrum's line color is opaque.
 */

#ifndef DSD_NEO_SRC_UI_QT_SPECTRUM_TRACE_GEOMETRY_H_
#define DSD_NEO_SRC_UI_QT_SPECTRUM_TRACE_GEOMETRY_H_

namespace dsd_qt {
namespace spectrum_geometry {

/** @brief One stroke vertex: position in logical pixels and coverage in [0, 1]. */
struct StrokeVertex {
    float x;
    float y;
    float coverage;
};

/** @brief One filled-area vertex in logical pixels. */
struct AreaVertex {
    float x;
    float y;
};

/** @brief Vertices a polyline of @p points can need, at most: 24 per segment and 72 per joint. */
int stroke_vertex_capacity(int points);

/**
 * @brief Stroke a polyline into a triangle list.
 *
 * Consecutive points closer than a millionth of a pixel are one point: they
 * have no direction to stroke along. A point with a non-finite x is dropped; a
 * non-finite y is drawn at @p fallback_y.
 *
 * @param xs,ys      @p points coordinates, logical pixels, y down.
 * @param half_width Half the pen width, logical pixels.
 * @param feather    Width of the coverage ramp, logical pixels (one device pixel).
 * @param out        At least stroke_vertex_capacity(points) entries.
 * @return Vertices written, a multiple of 3; 0 for fewer than two usable points.
 */
int build_stroke(const float* xs, const float* ys, int points, float half_width, float feather, float fallback_y,
                 StrokeVertex* out);

/**
 * @brief The area between a trace and its baseline, as a triangle strip.
 *
 * Column x runs from (x, ys[x]) down to (x, baseline): the same region as the
 * polygon through the trace and the two baseline corners (0, baseline) and
 * (columns - 1, baseline). A non-finite y sits on the baseline.
 *
 * @param out At least 2 * columns entries.
 * @return Vertices written: 2 * columns, or 0 for fewer than two columns.
 */
int build_area_strip(const float* ys, int columns, float baseline, AreaVertex* out);

} // namespace spectrum_geometry
} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_SPECTRUM_TRACE_GEOMETRY_H_ */
