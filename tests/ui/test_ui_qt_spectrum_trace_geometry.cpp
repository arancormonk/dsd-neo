// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Regression test: the triangles the spectrum trace hands the GPU
 * (spectrum_trace_geometry.{h,cpp}).
 *
 * Headless Qt has no RHI backend, so nothing in CI can grab the real
 * rendering; what can be checked is the shape. The case that matters is the
 * spectrum itself: one point per pixel column, often a full panel height
 * apart. A stroke that narrows on steep sides or leaves a spike tip aliased is
 * exactly what a mitered or bevelled strip does there. The checks sample the
 * coverage the triangles composite to, as the GPU blends them.
 */

// LLVM 22/GCC 16 misclassifies these runtime test oracles as compile-time assertions.
// NOLINTBEGIN(cert-dcl03-c,misc-static-assert)

#include <cassert>
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

#include <dsd-neo/core/safe_api.h>

#include "spectrum_trace_geometry.h"

namespace sg = dsd_qt::spectrum_geometry;

namespace {

constexpr float kHalf = 0.75F;   /* the trace's 1.5 px pen */
constexpr float kFeather = 0.5F; /* one device pixel at a DPR of 2 */
constexpr float kInner = kHalf - (kFeather / 2.0F);
constexpr float kOuter = kHalf + (kFeather / 2.0F);
constexpr float kTol = 1.0e-4F;

bool
near(float got, float want) {
    return std::fabs(got - want) <= kTol;
}

std::vector<sg::StrokeVertex>
stroke(const std::vector<float>& xs, const std::vector<float>& ys, float fallback_y = 0.0F) {
    const int n = static_cast<int>(xs.size());
    std::vector<sg::StrokeVertex> out(static_cast<size_t>(sg::stroke_vertex_capacity(n)) + 1U);
    const int got = sg::build_stroke(xs.data(), ys.data(), n, kHalf, kFeather, fallback_y, out.data());
    assert(got >= 0 && got <= sg::stroke_vertex_capacity(n));
    assert(got % 3 == 0);
    out.resize(static_cast<size_t>(got));
    return out;
}

/*
 * Coverage a pixel sampled at (px, py) ends up with: every triangle containing
 * it blended over the last, in drawing order, as the GPU composites them. This
 * is what the eye sees, so a fringe drawn twice shows up here as coverage
 * above the outline's.
 */
float
coverage_at(const std::vector<sg::StrokeVertex>& v, float px, float py) {
    /* A GPU's fill rule draws a pixel on an edge two triangles share exactly
     * once; a sample nudged off the edge does the same here. The nudge moves
     * a ramp by about a thousandth of its height. */
    px += 1.37e-3F;
    py += 0.71e-3F;
    float dst = 0.0F;
    for (size_t i = 0; i + 2U < v.size(); i += 3U) {
        const sg::StrokeVertex& a = v[i];
        const sg::StrokeVertex& b = v[i + 1U];
        const sg::StrokeVertex& c = v[i + 2U];
        const float det = ((b.y - c.y) * (a.x - c.x)) + ((c.x - b.x) * (a.y - c.y));
        if (std::fabs(det) < 1.0e-12F) {
            continue;
        }
        const float wa = (((b.y - c.y) * (px - c.x)) + ((c.x - b.x) * (py - c.y))) / det;
        const float wb = (((c.y - a.y) * (px - c.x)) + ((a.x - c.x) * (py - c.y))) / det;
        const float wc = 1.0F - wa - wb;
        if (wa < -1.0e-6F || wb < -1.0e-6F || wc < -1.0e-6F) {
            continue;
        }
        const float src = (wa * a.coverage) + (wb * b.coverage) + (wc * c.coverage);
        dst = src + (dst * (1.0F - src));
    }
    return dst;
}

/*
 * The edge profile along a unit direction (ux, uy) from (x, y): opaque inside
 * the core, half covered on the pen's nominal edge, clear past the fringe.
 */
void
assert_edge_profile(const std::vector<sg::StrokeVertex>& v, float x, float y, float ux, float uy) {
    auto at = [&](float r) { return coverage_at(v, x + (ux * r), y + (uy * r)); };
    assert(near(at(0.0F), 1.0F));
    assert(near(at(kInner - 0.01F), 1.0F));
    assert(std::fabs(at(kHalf) - 0.5F) < 0.02F);
    assert(at(kOuter + 0.01F) < 1.0e-4F);
}

void
test_a_flat_line_covers_exactly_the_pen(void) {
    const auto v = stroke({0.0F, 10.0F, 20.0F}, {10.0F, 10.0F, 10.0F});
    /* Two segments and no joint: a straight run needs none. */
    assert(v.size() == 48U);
    assert_edge_profile(v, 5.0F, 10.0F, 0.0F, 1.0F);
    assert_edge_profile(v, 15.0F, 10.0F, 0.0F, -1.0F);
    /* Where the two segments meet, nothing is drawn twice. */
    assert_edge_profile(v, 10.0F, 10.0F, 0.0F, 1.0F);
    /* Square caps reach half a pen width past each end. */
    assert(coverage_at(v, -kHalf + 0.05F, 10.0F) > 0.99F);
    assert(coverage_at(v, 20.0F + kHalf - 0.05F, 10.0F) > 0.99F);
    assert(coverage_at(v, 20.0F + kHalf + 0.05F, 10.0F) < 1.0e-4F);
}

/*
 * One column apart and a hundred pixels tall: the spectrum's own shape. Every
 * segment keeps its full width (a clamped miter squeezes these sides to a few
 * hundredths of a pixel), and every tip ends in an antialiased round cap (a
 * bevel across it is too short to carry a ramp, so the tip would be aliased).
 */
void
test_spikes_keep_their_width_and_their_tips(void) {
    const std::vector<float> xs{0.0F, 1.0F, 2.0F, 3.0F};
    const std::vector<float> ys{0.0F, 100.0F, 0.0F, 100.0F};
    const auto v = stroke(xs, ys);
    assert(v.size() <= static_cast<size_t>(sg::stroke_vertex_capacity(4)));

    /* Halfway down the first side, on its outside (away from the next one). */
    const float len = std::hypot(1.0F, 100.0F);
    const float nx = -100.0F / len; /* the side's normal, pointing left */
    const float ny = 1.0F / len;
    assert_edge_profile(v, 0.5F, 50.0F, nx, ny);

    /* Past the bottom tip at (1, 100), straight down, and past the top one at (2, 0), up. */
    assert_edge_profile(v, 1.0F, 100.0F, 0.0F, 1.0F);
    assert_edge_profile(v, 2.0F, 0.0F, 0.0F, -1.0F);
}

void
test_a_right_angle_is_round_on_the_outside(void) {
    /* Right, then down: the outside of the turn is up and to the right. */
    const auto v = stroke({0.0F, 10.0F, 10.0F}, {0.0F, 0.0F, 10.0F});
    const float diag = 1.0F / std::sqrt(2.0F);
    assert_edge_profile(v, 10.0F, 0.0F, diag, -diag); /* the corner itself */
    assert_edge_profile(v, 5.0F, 0.0F, 0.0F, -1.0F);  /* the first leg */
    assert_edge_profile(v, 10.0F, 5.0F, 1.0F, 0.0F);  /* the second */
    /* Inside the corner both legs overlap: still opaque, never above 1. */
    const float inside = coverage_at(v, 9.6F, 0.4F);
    assert(inside > 0.999F && inside <= 1.0F);
}

/* Distance from (x, y) to the polyline. */
float
distance_to_polyline(const std::vector<float>& xs, const std::vector<float>& ys, float x, float y) {
    float best = 1.0e9F;
    for (size_t i = 0; i + 1U < xs.size(); i++) {
        const float dx = xs[i + 1U] - xs[i];
        const float dy = ys[i + 1U] - ys[i];
        const float len2 = (dx * dx) + (dy * dy);
        float t = (len2 > 0.0F) ? ((((x - xs[i]) * dx) + ((y - ys[i]) * dy)) / len2) : 0.0F;
        t = std::fmax(0.0F, std::fmin(1.0F, t));
        best = std::fmin(best, std::hypot(x - (xs[i] + (t * dx)), y - (ys[i] + (t * dy))));
    }
    return best;
}

/*
 * The pieces are blended one over another rather than filled as one outline,
 * so wherever two overlap coverage adds up above the outline's, and wherever
 * they leave a gap it falls below. Compared on a fine grid against the exact
 * ramped outline (round joins: coverage is a function of the distance to the
 * polyline; QPainter's own bevels and analytic coverage are a different shape
 * again), away from the square end caps. Never more than the fan's chord
 * error below it. Above it, @p max_over: around one turn, only the sampling
 * nudge, since the cuts inside it leave no overlap; along a trace drawn one
 * column per pixel with a 1.5 px pen, pieces that are not neighbours also
 * overlap, and where two of their fringes cross outside every core a pixel
 * gets up to a quarter more coverage than the outline gives it.
 */
void
assert_matches_the_outline(const std::vector<float>& xs, const std::vector<float>& ys, float max_over) {
    const auto v = stroke(xs, ys);
    float min_y = 1.0e9F;
    float max_y = -1.0e9F;
    for (const float y : ys) {
        min_y = std::fmin(min_y, y);
        max_y = std::fmax(max_y, y);
    }
    float over = 0.0F;
    float under = 0.0F;
    constexpr float kStep = 0.043F;
    const int columns = static_cast<int>((xs.back() - xs.front() - 3.0F) / kStep);
    const int rows = static_cast<int>((max_y - min_y + 3.0F) / kStep);
    for (int i = 0; i <= columns; i++) {
        const float x = xs.front() + 1.5F + (static_cast<float>(i) * kStep);
        for (int j = 0; j <= rows; j++) {
            const float y = min_y - 1.5F + (static_cast<float>(j) * kStep);
            const float exact =
                std::fmax(0.0F, std::fmin(1.0F, (kOuter - distance_to_polyline(xs, ys, x, y)) / kFeather));
            const float got = coverage_at(v, x, y);
            over = std::fmax(over, got - exact);
            under = std::fmax(under, exact - got);
        }
    }
    assert(over < max_over);
    assert(under < 0.06F);
}

void
test_turns_match_the_outline(void) {
    for (const float drop : {0.2F, 1.0F, 5.0F, 10.0F, 25.0F}) {
        assert_matches_the_outline({0.0F, 10.0F, 20.0F}, {10.0F, 10.0F, 10.0F + drop}, 0.01F);
        assert_matches_the_outline({0.0F, 10.0F, 20.0F}, {10.0F, 10.0F, 10.0F - drop}, 0.01F);
    }
    /* One column apart, as the spectrum is drawn: gentle, steep and spiky runs. */
    assert_matches_the_outline({0.0F, 1.0F, 2.0F, 3.0F, 4.0F, 5.0F, 6.0F}, {5.0F, 5.4F, 5.1F, 6.0F, 5.2F, 5.5F, 5.0F},
                               0.3F);
    assert_matches_the_outline({0.0F, 1.0F, 2.0F, 3.0F, 4.0F, 5.0F, 6.0F}, {0.0F, 3.0F, 1.0F, 6.0F, 0.0F, 4.0F, 2.0F},
                               0.3F);
    assert_matches_the_outline({0.0F, 1.0F, 2.0F, 3.0F, 4.0F, 5.0F, 6.0F},
                               {0.0F, 40.0F, 2.0F, 39.0F, 0.0F, 40.0F, 1.0F}, 0.3F);
}

/*
 * At a very low pixel ratio the one-pixel feather is wider than the pen (the
 * 1 px grid at DPR 0.5: half width 0.5, feather 2). The ramp then never
 * reaches full coverage: the center gets what it gives there, outer / feather,
 * not a forced 1 that would draw the line a third brighter than its ramp.
 */
void
test_a_pen_thinner_than_its_feather(void) {
    const std::vector<float> xs{0.0F, 10.0F, 20.0F};
    const std::vector<float> ys{10.0F, 10.0F, 10.0F};
    std::vector<sg::StrokeVertex> v(static_cast<size_t>(sg::stroke_vertex_capacity(3)));
    const int n = sg::build_stroke(xs.data(), ys.data(), 3, 0.5F, 2.0F, 0.0F, v.data());
    v.resize(static_cast<size_t>(n));
    /* outer = 0.5 + 1 = 1.5; coverage = (1.5 - d) / 2 */
    assert(std::fabs(coverage_at(v, 5.0F, 10.0F) - 0.75F) < 0.01F);
    assert(std::fabs(coverage_at(v, 5.0F, 11.0F) - 0.25F) < 0.01F);
    assert(coverage_at(v, 5.0F, 11.51F) < 1.0e-3F);
    /* A corner keeps the same peak. */
    const std::vector<float> cx{0.0F, 10.0F, 10.0F};
    const std::vector<float> cy{0.0F, 0.0F, 10.0F};
    std::vector<sg::StrokeVertex> c(static_cast<size_t>(sg::stroke_vertex_capacity(3)));
    c.resize(static_cast<size_t>(sg::build_stroke(cx.data(), cy.data(), 3, 0.5F, 2.0F, 0.0F, c.data())));
    assert(std::fabs(coverage_at(c, 10.0F, 0.0F) - 0.75F) < 0.01F);
}

void
test_degenerate_input(void) {
    std::vector<sg::StrokeVertex> out(64);
    const float one = 1.0F;
    assert(sg::stroke_vertex_capacity(0) == 0);
    assert(sg::stroke_vertex_capacity(1) == 0);
    assert(sg::stroke_vertex_capacity(2) == 24);
    assert(sg::build_stroke(&one, &one, 1, kHalf, kFeather, 0.0F, out.data()) == 0);
    assert(sg::build_stroke(nullptr, &one, 2, kHalf, kFeather, 0.0F, out.data()) == 0);
    assert(stroke({0.0F, 5.0F}, {0.0F, 0.0F}).size() == 24U);

    /* Repeated points have no direction: they collapse into one, and nothing turns NaN. */
    const auto dup = stroke({0.0F, 0.0F, 5.0F, 5.0F}, {0.0F, 0.0F, 0.0F, 0.0F});
    assert(dup.size() == 24U);
    for (const auto& p : dup) {
        assert(std::isfinite(p.x) && std::isfinite(p.y));
    }
    /* All one point: nothing to draw. */
    assert(stroke({3.0F, 3.0F, 3.0F}, {3.0F, 3.0F, 3.0F}).empty());
    /* A zero or negative pen draws nothing rather than an inside-out strip. */
    const float xs[2] = {0.0F, 5.0F};
    const float ys[2] = {0.0F, 0.0F};
    assert(sg::build_stroke(xs, ys, 2, 0.0F, kFeather, 0.0F, out.data()) == 0);
}

void
test_non_finite_points(void) {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    /* A bad y sits on the fallback line; a bad x is dropped outright. */
    const auto v = stroke({0.0F, 1.0F, nan, 2.0F}, {7.0F, inf, 3.0F, 7.0F}, 7.0F);
    assert(v.size() == static_cast<size_t>(2 * 24)); /* (0,7) (1,7) (2,7): straight, no joint */
    for (const auto& p : v) {
        assert(std::isfinite(p.x) && std::isfinite(p.y));
        assert(std::fabs(p.y - 7.0F) <= kOuter + kTol);
    }
}

void
test_random_traces_stay_bounded(void) {
    unsigned state = 12345U;
    auto next = [&state]() {
        state = (state * 1103515245U) + 12345U;
        return static_cast<float>((state >> 8) & 0xFFFFU) / 65535.0F;
    };
    for (int trial = 0; trial < 500; trial++) {
        const int n = 2 + static_cast<int>(next() * 60.0F);
        std::vector<float> xs(static_cast<size_t>(n));
        std::vector<float> ys(static_cast<size_t>(n));
        for (int i = 0; i < n; i++) {
            xs[static_cast<size_t>(i)] = static_cast<float>(i);
            /* Now and then a repeat of the column before, as a flat run of bins gives. */
            const bool repeat = i > 0 && next() < 0.2F;
            ys[static_cast<size_t>(i)] = repeat ? ys[static_cast<size_t>(i) - 1U] : next() * 120.0F;
        }
        const auto v = stroke(xs, ys);
        const float reach = kHalf + kOuter; /* a cap plus a fringe, or a bevel's fringe */
        for (const auto& p : v) {
            assert(std::isfinite(p.x) && std::isfinite(p.y));
            assert(p.coverage >= 0.0F && p.coverage <= 1.0F);
            assert(p.x >= -reach && p.x <= static_cast<float>(n - 1) + reach);
            assert(p.y >= -reach && p.y <= 120.0F + reach);
        }
    }
}

void
test_area_strip_matches_the_old_polygon(void) {
    const std::vector<float> ys{10.0F, 20.0F, std::numeric_limits<float>::quiet_NaN(), 5.0F};
    std::vector<sg::AreaVertex> out(8);
    assert(sg::build_area_strip(ys.data(), 4, 50.0F, out.data()) == 8);
    /* Top and baseline per column; the corners are the old polygon's (0, h) and (w - 1, h). */
    assert(near(out[0].x, 0.0F) && near(out[0].y, 10.0F));
    assert(near(out[1].x, 0.0F) && near(out[1].y, 50.0F));
    assert(near(out[4].y, 50.0F)); /* the NaN column sits on the baseline */
    assert(near(out[6].x, 3.0F) && near(out[6].y, 5.0F));
    assert(near(out[7].x, 3.0F) && near(out[7].y, 50.0F));
    assert(sg::build_area_strip(ys.data(), 1, 50.0F, out.data()) == 0);
    assert(sg::build_area_strip(nullptr, 4, 50.0F, out.data()) == 0);
}

} // namespace

int
main(void) {
    test_a_flat_line_covers_exactly_the_pen();
    test_spikes_keep_their_width_and_their_tips();
    test_a_right_angle_is_round_on_the_outside();
    test_turns_match_the_outline();
    test_a_pen_thinner_than_its_feather();
    test_degenerate_input();
    test_non_finite_points();
    test_random_traces_stay_bounded();
    test_area_strip_matches_the_old_polygon();
    (void)DSD_FPRINTF(stdout, "UI_QT_SPECTRUM_TRACE_GEOMETRY: ok\n");
    return 0;
}

// NOLINTEND(cert-dcl03-c,misc-static-assert)
