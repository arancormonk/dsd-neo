// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Regression test: the triangles the map hands the GPU for trails, accuracy
 * circles and marker sprites (map_geometry.h).
 *
 * Headless Qt has no RHI backend, so nothing in CI can grab the real
 * rendering; what can be checked is the shape. A trail must never be drawn
 * across a break in it (that line would claim a radio went somewhere nobody
 * saw it go), a provisional trail's dashes must really be dashes, a faint
 * accuracy circle must cover each pixel once (a translucent fill drawn twice
 * reads as a second, smaller circle) and stay cheap when it is far larger than
 * the screen, and a marker must land on whole device pixels. The checks sample
 * the coverage the triangles composite to, as the GPU blends them.
 *
 * Also what the map view decides per track before any triangle (map_scene.h):
 * each state of a fix is its own sprite on its own axis, a trail's solid and
 * dashed runs never bridge a break, labels never overlap, tiles land where the
 * tile list says (and a parent tile stands in over its children), and a pan
 * reuses the trails built for the view before it only by the offset the
 * markers moved.
 */

// LLVM 22/GCC 16 misclassifies these runtime test oracles as compile-time assertions.
// NOLINTBEGIN(cert-dcl03-c,misc-static-assert)

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <limits>
#include <numeric>
#include <vector>

#include <dsd-neo/core/safe_api.h>

#include "map_geometry.h"
#include "map_math.h"
#include "map_scene.h"
#include "spectrum_trace_geometry.h"

namespace mg = dsd_qt::map_geometry;
namespace mm = dsd_qt::map_math;
namespace ms = dsd_qt::map_scene;
namespace sg = dsd_qt::spectrum_geometry;

namespace {

constexpr float kTol = 1.0e-4F;
constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();

bool
near(float got, float want, float tol) {
    return std::fabs(got - want) <= tol;
}

mg::TrailStyle
pen(float dash, float gap) {
    mg::TrailStyle style;
    style.half_width = 1.0F;
    style.feather = 0.5F;
    style.dash = dash;
    style.gap = gap;
    return style;
}

/* Barycentric weights of (px, py) in triangle i of @p v; false when it is outside. */
bool
inside(const std::vector<mg::StrokeVertex>& v, size_t i, float px, float py, float* coverage) {
    const mg::StrokeVertex& a = v[i];
    const mg::StrokeVertex& b = v[i + 1U];
    const mg::StrokeVertex& c = v[i + 2U];
    const float det = ((b.y - c.y) * (a.x - c.x)) + ((c.x - b.x) * (a.y - c.y));
    if (std::fabs(det) < 1.0e-12F) {
        return false;
    }
    const float wa = (((b.y - c.y) * (px - c.x)) + ((c.x - b.x) * (py - c.y))) / det;
    const float wb = (((c.y - a.y) * (px - c.x)) + ((a.x - c.x) * (py - c.y))) / det;
    const float wc = 1.0F - wa - wb;
    if (wa < -1.0e-6F || wb < -1.0e-6F || wc < -1.0e-6F) {
        return false;
    }
    *coverage = (wa * a.coverage) + (wb * b.coverage) + (wc * c.coverage);
    return true;
}

/*
 * Coverage a pixel sampled at (px, py) ends up with: every triangle holding it
 * blended over the last, as the GPU composites them. The sample is nudged off
 * shared edges, which a GPU's fill rule draws once.
 */
float
coverage_at(const std::vector<mg::StrokeVertex>& v, float px, float py, int* holders = nullptr) {
    px += 1.37e-3F;
    py += 0.71e-3F;
    float dst = 0.0F;
    int count = 0;
    for (size_t i = 0; i + 2U < v.size(); i += 3U) {
        float src = 0.0F;
        if (inside(v, i, px, py, &src)) {
            dst = src + (dst * (1.0F - src));
            count++;
        }
    }
    if (holders != nullptr) {
        *holders = count;
    }
    return dst;
}

std::vector<mg::StrokeVertex>
trail(const std::vector<float>& xs, const std::vector<float>& ys, const mg::TrailStyle& style,
      const mg::ClipRect* clip = nullptr) {
    std::vector<mg::StrokeVertex> out;
    const int added = mg::append_trail(xs.data(), ys.data(), static_cast<int>(xs.size()), style, clip, out);
    assert(added >= 0 && static_cast<size_t>(added) == out.size());
    assert(out.size() % 3U == 0U);
    for (const mg::StrokeVertex& v : out) {
        assert(std::isfinite(v.x) && std::isfinite(v.y) && v.coverage >= 0.0F && v.coverage <= 1.0F);
    }
    return out;
}

void
test_solid_trail_is_the_spectrum_stroke(void) {
    /* A solid trail is exactly the stroke the spectrum trace already draws and tests. */
    const std::vector<float> xs = {10.0F, 40.0F, 55.0F, 90.0F};
    const std::vector<float> ys = {10.0F, 30.0F, 12.0F, 50.0F};
    const mg::TrailStyle style = pen(0.0F, 0.0F);
    const std::vector<mg::StrokeVertex> got = trail(xs, ys, style);
    std::vector<sg::StrokeVertex> want(static_cast<size_t>(sg::stroke_vertex_capacity(4)));
    want.resize(static_cast<size_t>(sg::build_stroke(xs.data(), ys.data(), 4, 1.0F, 0.5F, 0.0F, want.data())));
    assert(!got.empty() && got.size() == want.size());
    for (size_t i = 0; i < got.size(); i++) {
        assert(near(got[i].x, want[i].x, 0.0F) && near(got[i].y, want[i].y, 0.0F));
        assert(near(got[i].coverage, want[i].coverage, 0.0F));
    }
    /* Appending keeps what was already there. */
    std::vector<mg::StrokeVertex> out(5, mg::StrokeVertex{1.0F, 2.0F, 0.5F});
    const int added = mg::append_trail(xs.data(), ys.data(), 4, style, nullptr, out);
    assert(static_cast<size_t>(added) == want.size() && out.size() == want.size() + 5U);
    assert(near(out[4].x, 1.0F, 0.0F) && near(out[5].x, want[0].x, 0.0F));
}

void
test_a_break_is_never_bridged(void) {
    /* Two runs on one line with a break between them: nothing is drawn in the gap. */
    const std::vector<float> xs = {0.0F, 20.0F, kNaN, 60.0F, 80.0F};
    const std::vector<float> ys = {10.0F, 10.0F, kNaN, 10.0F, 10.0F};
    const std::vector<mg::StrokeVertex> v = trail(xs, ys, pen(0.0F, 0.0F));
    assert(near(coverage_at(v, 10.0F, 10.0F), 1.0F, kTol));
    assert(near(coverage_at(v, 70.0F, 10.0F), 1.0F, kTol));
    assert(coverage_at(v, 40.0F, 10.0F) < kTol);
    assert(coverage_at(v, 25.0F, 10.0F) < kTol);
    /* Either coordinate breaks it; a lone point between breaks draws nothing. */
    const std::vector<float> ys2 = {10.0F, 10.0F, kNaN, 10.0F, 10.0F};
    const std::vector<float> xs2 = {0.0F, 20.0F, 40.0F, 60.0F, 80.0F};
    const std::vector<mg::StrokeVertex> y_break = trail(xs2, ys2, pen(0.0F, 0.0F));
    assert(coverage_at(y_break, 40.0F, 10.0F) < kTol);
    const std::vector<float> lone_x = {0.0F, kNaN, 40.0F, kNaN, 80.0F};
    const std::vector<float> lone_y = {10.0F, 10.0F, 10.0F, 10.0F, 10.0F};
    const size_t lone_solid = trail(lone_x, lone_y, pen(0.0F, 0.0F)).size();
    const size_t lone_dashed = trail(lone_x, lone_y, pen(8.0F, 4.0F)).size();
    assert(lone_solid == 0U && lone_dashed == 0U);
}

void
test_dashes_have_their_visible_lengths(void) {
    /* Dash 8, gap 4 along a straight line, pen 2: visible dashes on [-1 + 12k, 7 + 12k]. */
    const std::vector<float> xs = {0.0F, 100.0F};
    const std::vector<float> ys = {10.0F, 10.0F};
    const std::vector<mg::StrokeVertex> v = trail(xs, ys, pen(8.0F, 4.0F));
    for (int k = 0; k < 8; k++) {
        const float base = 12.0F * static_cast<float>(k);
        assert(near(coverage_at(v, base + 3.0F, 10.0F), 1.0F, kTol)); /* middle of a dash */
        assert(coverage_at(v, base + 9.0F, 10.0F) < kTol);            /* middle of a gap */
        /* A dash's square end sits on its length: covered just inside, clear just outside. */
        assert(near(coverage_at(v, base + 6.95F, 10.0F), 1.0F, kTol));
        assert(coverage_at(v, base + 7.05F, 10.0F) < kTol);
        assert(coverage_at(v, base + 10.95F, 10.0F) < kTol);
        assert(near(coverage_at(v, base + 11.05F, 10.0F), 1.0F, kTol));
        /* Across the pen the ramp is the stroke's: the edge half covered. */
        assert(near(coverage_at(v, base + 3.0F, 11.0F), 0.5F, 0.02F));
        int holders = 0;
        (void)coverage_at(v, base + 3.0F, 10.0F, &holders);
        assert(holders <= 2); /* a dash's core is its two halves, never another dash */
    }
}

void
test_a_dash_bends_round_a_turn(void) {
    /* A dash longer than the first leg is one stroke round the corner, joined like any trail. */
    const std::vector<float> xs = {0.0F, 10.0F, 10.0F};
    const std::vector<float> ys = {0.0F, 0.0F, 40.0F};
    const std::vector<mg::StrokeVertex> v = trail(xs, ys, pen(20.0F, 4.0F));
    /* Centre-line dash 18: from (0, 0) round (10, 0) to (10, 8). */
    const float bent_x[] = {0.0F, 10.0F, 10.0F};
    const float bent_y[] = {0.0F, 0.0F, 8.0F};
    std::vector<sg::StrokeVertex> one(static_cast<size_t>(sg::stroke_vertex_capacity(3)));
    one.resize(static_cast<size_t>(sg::build_stroke(bent_x, bent_y, 3, 1.0F, 0.5F, 0.0F, one.data())));
    assert(v.size() > one.size());
    for (size_t i = 0; i < one.size(); i++) {
        assert(near(v[i].x, one[i].x, 1.0e-4F) && near(v[i].y, one[i].y, 1.0e-4F));
    }
    assert(near(coverage_at(v, 10.4F, -0.4F), 1.0F, kTol)); /* the outside of the turn is filled */
    assert(coverage_at(v, 10.0F, 11.0F) < kTol);            /* then a gap of 4 ... */
    assert(near(coverage_at(v, 10.0F, 16.0F), 1.0F, kTol)); /* ... and the next dash */
}

void
test_clipped_dashes_match_and_stay_cheap(void) {
    /* Inside the clip, a clipped dashed trail draws what the unclipped one does. */
    const std::vector<float> xs = {-300.0F, 150.0F, 180.0F, 700.0F};
    const std::vector<float> ys = {100.0F, 120.0F, 260.0F, 300.0F};
    const mg::ClipRect clip{0.0F, 0.0F, 400.0F, 400.0F};
    const std::vector<mg::StrokeVertex> all = trail(xs, ys, pen(8.0F, 4.0F));
    const std::vector<mg::StrokeVertex> cut = trail(xs, ys, pen(8.0F, 4.0F), &clip);
    assert(cut.size() < all.size());
    for (int step = 0; step <= 73; step++) {
        const float t = static_cast<float>(step) / 73.0F;
        for (int seg = 0; seg < 3; seg++) {
            const float x = xs[seg] + ((xs[seg + 1] - xs[seg]) * t);
            const float y = ys[seg] + ((ys[seg + 1] - ys[seg]) * t);
            if (x < 0.0F || x > 400.0F || y < 0.0F || y > 400.0F) {
                continue;
            }
            assert(near(coverage_at(cut, x, y), coverage_at(all, x, y), 1.0e-3F));
        }
    }

    /* A segment a million pixels long through a phone-sized clip: only the dashes it shows. */
    const std::vector<float> lx = {-1.0e6F, 1.0e6F};
    const std::vector<float> ly = {200.0F, 200.0F};
    const std::vector<mg::StrokeVertex> lv = trail(lx, ly, pen(8.0F, 4.0F), &clip);
    assert(!lv.empty() && lv.size() < size_t{24} * 40U);
    /* Wholly outside: nothing, and no time spent per dash. */
    const std::vector<float> fx = {-1.0e6F, 1.0e6F};
    const std::vector<float> fy = {-500.0F, -500.0F};
    const size_t outside = trail(fx, fy, pen(8.0F, 4.0F), &clip).size();
    assert(outside == 0U);
}

void
test_bad_trail_input(void) {
    const std::vector<float> xs = {0.0F, 10.0F};
    const std::vector<float> ys = {0.0F, 0.0F};
    std::vector<mg::StrokeVertex> out;
    const int one_point = mg::append_trail(xs.data(), ys.data(), 1, pen(0.0F, 0.0F), nullptr, out);
    const int no_xs = mg::append_trail(nullptr, ys.data(), 2, pen(0.0F, 0.0F), nullptr, out);
    assert(one_point == 0 && no_xs == 0);
    mg::TrailStyle thin = pen(0.0F, 0.0F);
    thin.half_width = 0.0F;
    assert(mg::append_trail(xs.data(), ys.data(), 2, thin, nullptr, out) == 0);
    thin.half_width = kNaN;
    assert(mg::append_trail(xs.data(), ys.data(), 2, thin, nullptr, out) == 0);
    /* Repeated points have no direction: nothing, not NaN. */
    const std::vector<float> same = {5.0F, 5.0F, 5.0F};
    const size_t same_solid = trail(same, same, pen(0.0F, 0.0F)).size();
    const size_t same_dashed = trail(same, same, pen(8.0F, 4.0F)).size();
    assert(same_solid == 0U && same_dashed == 0U);
    assert(out.empty());
}

std::vector<mg::StrokeVertex>
circle(float cx, float cy, float r, const mg::ClipRect& clip) {
    std::vector<mg::StrokeVertex> out;
    const int added = mg::append_accuracy_circle(cx, cy, r, 0.5F, clip, out);
    assert(added >= 0 && static_cast<size_t>(added) == out.size() && out.size() % 3U == 0U);
    return out;
}

void
test_circle_profile_and_single_cover(void) {
    const mg::ClipRect clip{0.0F, 0.0F, 400.0F, 400.0F};
    const std::vector<mg::StrokeVertex> v = circle(200.0F, 200.0F, 60.0F, clip);
    assert(!v.empty());
    for (int i = 0; i < 64; i++) {
        const float a = (6.2831853F * static_cast<float>(i)) / 64.0F + 0.031F;
        const float ux = std::cos(a);
        const float uy = std::sin(a);
        const auto at = [&](float r, int* holders) {
            return coverage_at(v, 200.0F + (ux * r), 200.0F + (uy * r), holders);
        };
        int holders = 0;
        assert(near(at(0.5F, &holders), 1.0F, kTol) && holders == 1);
        assert(near(at(30.0F, &holders), 1.0F, kTol) && holders == 1);
        /* Solid to the ramp, less the chord error; half covered on the radius, less what the chord takes. */
        assert(near(at(59.6F, &holders), 1.0F, kTol) && holders == 1);
        const float edge = at(60.0F, &holders);
        assert(edge > 0.28F && edge < 0.52F && holders == 1);
        assert(at(60.26F, &holders) < kTol);
        assert(at(61.0F, &holders) < kTol && holders == 0);
    }
}

void
test_circle_far_larger_than_the_view(void) {
    const mg::ClipRect clip{0.0F, 0.0F, 400.0F, 700.0F};
    /* Holding the whole view: two triangles over it, solid. */
    const std::vector<mg::StrokeVertex> all = circle(200.0F, 350.0F, 5000.0F, clip);
    assert(all.size() == 6U);
    assert(near(coverage_at(all, 1.0F, 1.0F), 1.0F, kTol) && near(coverage_at(all, 399.0F, 699.0F), 1.0F, kTol));

    /* Wholly outside: nothing. */
    assert(circle(-5000.0F, 350.0F, 4000.0F, clip).empty());
    assert(circle(900.0F, 900.0F, 100.0F, clip).empty());

    /* A 600,000 px circle whose edge crosses the view: a few slices, and the edge in the right place. */
    const float r = 600000.0F;
    const float cx = 200.0F - r; /* the edge crosses the view's middle column at y = 350 */
    const std::vector<mg::StrokeVertex> edge = circle(cx, 350.0F, r, clip);
    assert(!edge.empty() && edge.size() < size_t{9} * 64U);
    assert(near(coverage_at(edge, 150.0F, 350.0F), 1.0F, kTol));
    assert(coverage_at(edge, 250.0F, 350.0F) < kTol);
    for (int row = 0; row < 16; row++) {
        const float y = 5.0F + (45.0F * static_cast<float>(row));
        const double dy = static_cast<double>(y) - 350.0;
        const double edge_x = static_cast<double>(cx) + std::sqrt((static_cast<double>(r) * r) - (dy * dy));
        int holders = 0;
        assert(near(coverage_at(edge, static_cast<float>(edge_x - 1.5), y, &holders), 1.0F, kTol) && holders == 1);
        assert(coverage_at(edge, static_cast<float>(edge_x + 1.5), y) < kTol);
    }
}

void
test_circle_bad_input(void) {
    const mg::ClipRect clip{0.0F, 0.0F, 400.0F, 400.0F};
    std::vector<mg::StrokeVertex> out;
    assert(mg::append_accuracy_circle(200.0F, 200.0F, 0.0F, 0.5F, clip, out) == 0);
    assert(mg::append_accuracy_circle(200.0F, 200.0F, -3.0F, 0.5F, clip, out) == 0);
    assert(mg::append_accuracy_circle(200.0F, 200.0F, kNaN, 0.5F, clip, out) == 0);
    assert(mg::append_accuracy_circle(kNaN, 200.0F, 10.0F, 0.5F, clip, out) == 0);
    assert(mg::append_accuracy_circle(200.0F, 200.0F, std::numeric_limits<float>::infinity(), 0.5F, clip, out) == 0);
    const mg::ClipRect empty{10.0F, 10.0F, 10.0F, 40.0F};
    assert(mg::append_accuracy_circle(200.0F, 200.0F, 10.0F, 0.5F, empty, out) == 0);
    assert(out.empty());
    /* Smaller than the ramp: faint all over, never above full. */
    const std::vector<mg::StrokeVertex> tiny = circle(200.0F, 200.0F, 0.1F, clip);
    assert(!tiny.empty());
    const float c = coverage_at(tiny, 200.0F, 200.0F);
    assert(c > 0.0F && c < 1.0F);
}

void
test_slices_bound_the_chord_error(void) {
    assert(mg::circle_slices(1.0F) == mg::kMinCircleSlices);
    assert(mg::circle_slices(0.0F) == mg::kMinCircleSlices);
    assert(mg::circle_slices(1.0e9F) == mg::kMaxCircleSlices);
    const float radii[] = {20.0F, 300.0F, 5000.0F, 80000.0F, 300000.0F};
    for (const float r : radii) {
        const int n = mg::circle_slices(r);
        const double sag = static_cast<double>(r) * (1.0 - std::cos(3.14159265358979323846 / n));
        assert(sag <= static_cast<double>(mg::kCircleChordError) + 1.0e-6);
        /* And not wastefully more: one slice fewer would exceed it (above the floor). */
        if (n > mg::kMinCircleSlices) {
            const double fewer = static_cast<double>(r) * (1.0 - std::cos(3.14159265358979323846 / (n - 1)));
            assert(fewer > static_cast<double>(mg::kCircleChordError));
        }
    }
}

void
test_atlas_cells(void) {
    /* A 4 x 2 atlas of 32 px cells: the cell at (2, 1), inset half a texel. */
    const mg::AtlasCell c = mg::atlas_cell(2, 1, 32, 128, 64);
    assert(near(c.u0, 64.5F / 128.0F, 1.0e-7F) && near(c.u1, 95.5F / 128.0F, 1.0e-7F));
    assert(near(c.v0, 32.5F / 64.0F, 1.0e-7F) && near(c.v1, 63.5F / 64.0F, 1.0e-7F));
    /* Off the atlas, or a bad size: the empty cell. */
    const mg::AtlasCell off = mg::atlas_cell(4, 0, 32, 128, 64);
    assert(near(off.u0, 0.0F, 0.0F) && near(off.u1, 0.0F, 0.0F));
    const mg::AtlasCell bad = mg::atlas_cell(0, 0, 0, 128, 64);
    assert(near(bad.v1, 0.0F, 0.0F));
    assert(near(mg::atlas_cell(-1, 0, 32, 128, 64).u1, 0.0F, 0.0F));
}

void
test_marker_quads(void) {
    const mg::AtlasCell cell = mg::atlas_cell(1, 0, 32, 64, 32);
    std::vector<mg::SpriteVertex> out;
    /* A 24 px marker at a fractional position on a 2.625x screen: the corner lands on a device pixel. */
    const float dpr = 2.625F;
    assert(mg::append_marker_quad(100.37F, 51.9F, 24.0F, dpr, cell, out) == 6);
    assert(out.size() == 6U);
    const float left = out[0].x;
    const float top = out[0].y;
    assert(near(left * dpr, std::round(left * dpr), 1.0e-3F) && near(top * dpr, std::round(top * dpr), 1.0e-3F));
    assert(std::fabs(left - (100.37F - 12.0F)) <= 0.5F / dpr + 1.0e-4F);
    assert(std::fabs(top - (51.9F - 12.0F)) <= 0.5F / dpr + 1.0e-4F);
    /* Two triangles over the square, each corner on its own texture corner. */
    float min_x = left;
    float max_x = left;
    for (const mg::SpriteVertex& v : out) {
        min_x = std::fmin(min_x, v.x);
        max_x = std::fmax(max_x, v.x);
        const bool is_left = near(v.x, left, 1.0e-5F);
        const bool is_top = near(v.y, top, 1.0e-5F);
        assert(near(v.u, is_left ? cell.u0 : cell.u1, 0.0F));
        assert(near(v.v, is_top ? cell.v0 : cell.v1, 0.0F));
    }
    assert(near(max_x - min_x, 24.0F, 1.0e-4F));
    /* A size that is not a whole number of device pixels still lands every edge on the grid. */
    std::vector<mg::SpriteVertex> odd;
    assert(mg::append_marker_quad(100.37F, 51.9F, 23.9F, dpr, cell, odd) == 6);
    for (const mg::SpriteVertex& v : odd) {
        assert(near(v.x * dpr, std::round(v.x * dpr), 1.0e-3F) && near(v.y * dpr, std::round(v.y * dpr), 1.0e-3F));
    }
    assert(near(odd[5].x - odd[0].x, 63.0F / dpr, 1.0e-4F) && near(odd[5].y - odd[0].y, 63.0F / dpr, 1.0e-4F));
    /* Bad input adds nothing. */
    assert(mg::append_marker_quad(kNaN, 51.9F, 24.0F, dpr, cell, out) == 0);
    assert(mg::append_marker_quad(10.0F, 51.9F, 0.0F, dpr, cell, out) == 0);
    assert(mg::append_marker_quad(10.0F, 51.9F, 24.0F, 0.0F, cell, out) == 0);
    assert(mg::append_marker_quad(10.0F, 51.9F, 24.0F, kNaN, cell, out) == 0);
    assert(out.size() == 6U);
}

/* ------------------------------------------------------------------------- */
/* map_scene.h: what the map view draws for each track                       */
/* ------------------------------------------------------------------------- */

/* Freshness is the fill strength: solid while fresh, stepping down, faintest when old or of no known age. */
void
test_fill_levels() {
    assert(ms::fill_level(0, 120, 900) == 0);
    assert(ms::fill_level(120, 120, 900) == 0);
    assert(ms::fill_level(121, 120, 900) == 1);
    assert(ms::fill_level(509, 120, 900) == 1);
    assert(ms::fill_level(510, 120, 900) == 2);
    assert(ms::fill_level(899, 120, 900) == 2);
    assert(ms::fill_level(900, 120, 900) == ms::kFillLevels - 1);
    assert(ms::fill_level(1000000, 120, 900) == ms::kFillLevels - 1);
    /* No age (a stopped replay's fix) is never mistaken for a fresh one. */
    assert(ms::fill_level(-1, 120, 900) == ms::kFillLevels - 1);
    /* No middle when the faded edge is not past the fresh one. */
    assert(ms::fill_level(121, 120, 120) == ms::kFillLevels - 1);
    assert(ms::fill_level(121, 120, 60) == ms::kFillLevels - 1);
    /* Each step is visibly weaker and none is invisible. */
    for (int level = 1; level < ms::kFillLevels; level++) {
        assert(ms::fill_opacity(level) < ms::fill_opacity(level - 1) - 0.1F);
    }
    assert(ms::fill_opacity(0) > 0.99F && ms::fill_opacity(ms::kFillLevels - 1) > 0.1F);
    assert(near(ms::fill_opacity(-5), ms::fill_opacity(0), 0.0F));
    assert(near(ms::fill_opacity(99), ms::fill_opacity(ms::kFillLevels - 1), 0.0F));
}

/* Every combination of the axes is its own atlas cell, and the cell says which combination it draws. */
void
test_marker_cells() {
    std::vector<int> seen(static_cast<size_t>(ms::kMarkerCellCount), 0);
    for (int bits = 0; bits < 16; bits++) {
        for (int fill = 0; fill < ms::kFillLevels; fill++) {
            ms::MarkerStyle style;
            style.ring = (bits & 8) != 0;
            style.alert = (bits & 4) != 0;
            style.double_outline = (bits & 2) != 0;
            style.dashed_outline = (bits & 1) != 0;
            style.fill_level = fill;
            const int cell = ms::marker_cell(style);
            assert(cell >= 0 && cell < ms::kSelectionHaloCell);
            ms::MarkerStyle back;
            const bool known = ms::marker_style_of_cell(cell, &back);
            assert(known);
            assert(back.ring == style.ring && back.alert == style.alert && back.double_outline == style.double_outline
                   && back.dashed_outline == style.dashed_outline);
            /* A ring has no fill, so its freshness does not pick another cell. */
            assert(style.ring ? back.fill_level == 0 : back.fill_level == fill);
            if (!style.ring || fill == 0) {
                seen[static_cast<size_t>(cell)]++;
            }
        }
    }
    for (int cell = 0; cell < ms::kSelectionHaloCell; cell++) {
        assert(seen[static_cast<size_t>(cell)] == 1);
    }
    ms::MarkerStyle none;
    for (const int cell : {ms::kSelectionHaloCell, ms::kDeviceDotCell, -1, ms::kMarkerCellCount}) {
        const bool known = ms::marker_style_of_cell(cell, &none);
        assert(!known);
    }
    const bool to_null = ms::marker_style_of_cell(0, nullptr);
    assert(!to_null);
    assert(ms::kAtlasRows * ms::kAtlasColumns >= ms::kMarkerCellCount);
    /* An out-of-range freshness is clamped, never another axis. */
    ms::MarkerStyle wild;
    wild.fill_level = 42;
    ms::MarkerStyle faint;
    faint.fill_level = ms::kFillLevels - 1;
    assert(ms::marker_cell(wild) == ms::marker_cell(faint));
    wild.fill_level = -3;
    assert(ms::marker_cell(wild) == 0);
}

/* A trail splits into solid and dashed runs that share their meeting points, and a break is never bridged. */
void
test_split_trail() {
    const float xs[6] = {0.0F, 10.0F, 20.0F, 30.0F, kNaN, 50.0F};
    const float ys[6] = {0.0F, 0.0F, 0.0F, 0.0F, kNaN, 0.0F};
    /* Segments ending at 2 and 3 are dashed; 5 follows a break. */
    const uint8_t dashed[6] = {0U, 0U, 1U, 1U, 0U, 0U};
    ms::TrailRuns runs;
    ms::split_trail(xs, ys, dashed, 6, runs);
    assert(runs.solid_x.size() == 2U && near(runs.solid_x[0], 0.0F, 0.0F) && near(runs.solid_x[1], 10.0F, 0.0F));
    assert(runs.dashed_x.size() == 3U);
    assert(near(runs.dashed_x[0], 10.0F, 0.0F) && near(runs.dashed_x[1], 20.0F, 0.0F)
           && near(runs.dashed_x[2], 30.0F, 0.0F));
    /* Solid, dashed, solid again: the solid list starts a new run (a NaN between), sharing the point it resumes at. */
    const uint8_t mixed[4] = {0U, 0U, 1U, 0U};
    ms::split_trail(xs, ys, mixed, 4, runs);
    assert(runs.solid_x.size() == 5U && std::isnan(runs.solid_x[2]));
    assert(near(runs.solid_x[3], 20.0F, 0.0F) && near(runs.solid_x[4], 30.0F, 0.0F));
    assert(runs.dashed_x.size() == 2U);
    /* Drawn, the solid list never bridges the dashed stretch. */
    std::vector<mg::StrokeVertex> out;
    const int drawn = mg::append_trail(runs.solid_x.data(), runs.solid_y.data(), static_cast<int>(runs.solid_x.size()),
                                       pen(0.0F, 0.0F), nullptr, out);
    assert(drawn > 0);
    for (const mg::StrokeVertex& v : out) {
        assert(!(v.x > 12.0F && v.x < 18.0F));
    }
    /* Across a break: nothing joins 30 to 50, whatever the styles. */
    ms::split_trail(xs, ys, nullptr, 6, runs);
    assert(runs.dashed_x.empty());
    for (size_t i = 1; i < runs.solid_x.size(); i++) {
        assert(!(runs.solid_x[i - 1U] < 31.0F && runs.solid_x[i] > 49.0F));
    }
    /* Too short or no points: nothing, and the lists are emptied. */
    ms::split_trail(xs, ys, nullptr, 1, runs);
    assert(runs.solid_x.empty() && runs.dashed_x.empty());
    ms::split_trail(nullptr, ys, nullptr, 6, runs);
    assert(runs.solid_x.empty());
}

/* Place labels for @p n requests in a 400 x 300 view with markers and keep-outs already in @p space. */
int
place(const ms::LabelRequest* requests, int n, ms::LabelSpace& space, ms::LabelBox* placed, uint8_t* keep) {
    return ms::place_labels(requests, n, 11.0F, ms::LabelBox{0.0F, 0.0F, 400.0F, 300.0F}, space, placed, keep);
}

ms::LabelBox
marker_box(float x, float y) {
    return ms::LabelBox{x - 9.0F, y - 9.0F, 18.0F, 18.0F};
}

/* A fresh 400 x 300 space holding markers at @p xs, @p ys. */
void
fresh_space(ms::LabelSpace& space, std::initializer_list<float> xs, std::initializer_list<float> ys) {
    space.reset(400.0F, 300.0F, 48.0F);
    auto y = ys.begin();
    for (const float x : xs) {
        space.markers.add(marker_box(x, *y));
        ++y;
    }
}

/* Labels take the first clear side (right, left, above, below), in priority order: never over another label, a
 * marker or a keep-out box, never past the view's edge, and dropped only when no side is clear. */
void
test_place_labels() {
    ms::LabelSpace space;
    ms::LabelBox placed[4];
    uint8_t keep[4] = {9U, 9U, 9U, 9U};

    /* Alone: to the right, clear of its own marker. */
    fresh_space(space, {100.0F}, {100.0F});
    const ms::LabelRequest alone{100.0F, 100.0F, 60.0F, 16.0F};
    assert(place(&alone, 1, space, placed, keep) == 1 && keep[0] == 1U);
    assert(near(placed[0].left, 111.0F, 0.0F) && near(placed[0].top, 92.0F, 0.0F));
    assert(!ms::boxes_overlap(placed[0], marker_box(100.0F, 100.0F)));

    /* A neighbour's marker to the right: the label goes left instead of covering it. */
    fresh_space(space, {100.0F, 140.0F}, {100.0F, 100.0F});
    assert(place(&alone, 1, space, placed, keep) == 1);
    assert(near(placed[0].left, 100.0F - 11.0F - 60.0F, 0.0F));

    /* At the right edge, or with the control column there (a keep-out box): it goes left too. */
    fresh_space(space, {360.0F}, {200.0F});
    const ms::LabelRequest edge{360.0F, 200.0F, 60.0F, 16.0F};
    assert(place(&edge, 1, space, placed, keep) == 1 && placed[0].left + placed[0].width <= 360.0F);
    fresh_space(space, {300.0F}, {250.0F});
    const ms::LabelBox controls{340.0F, 150.0F, 48.0F, 140.0F};
    space.keep_out.add(controls);
    const ms::LabelRequest by_controls{300.0F, 250.0F, 60.0F, 16.0F};
    assert(place(&by_controls, 1, space, placed, keep) == 1 && !ms::boxes_overlap(placed[0], controls));

    /* Hemmed in on every side: not drawn. */
    fresh_space(space, {200.0F}, {150.0F});
    space.keep_out.add(ms::LabelBox{0.0F, 0.0F, 400.0F, 146.0F});
    space.keep_out.add(ms::LabelBox{0.0F, 154.0F, 400.0F, 146.0F});
    const ms::LabelRequest hemmed{200.0F, 150.0F, 60.0F, 16.0F};
    keep[0] = 9U;
    assert(place(&hemmed, 1, space, placed, keep) == 0 && keep[0] == 0U);

    /* Priority: two markers one above the other, their right-hand labels would overlap. The first label in the
     * order keeps the right; the second takes another side, and neither covers the other or either marker. */
    fresh_space(space, {100.0F, 100.0F}, {100.0F, 112.0F});
    const ms::LabelRequest pair[2] = {{100.0F, 100.0F, 60.0F, 16.0F}, {100.0F, 112.0F, 60.0F, 16.0F}};
    assert(place(pair, 2, space, placed, keep) == 2);
    assert(near(placed[0].left, 111.0F, 0.0F) && !near(placed[1].left, 111.0F, 0.0F));
    assert(!ms::boxes_overlap(placed[0], placed[1]) && !ms::boxes_overlap(placed[1], marker_box(100.0F, 100.0F)));
    const ms::LabelRequest reversed[2] = {pair[1], pair[0]};
    fresh_space(space, {100.0F, 100.0F}, {100.0F, 112.0F});
    assert(place(reversed, 2, space, placed, keep) == 2);
    assert(near(placed[0].left, 111.0F, 0.0F) && near(placed[0].top, 104.0F, 0.0F)
           && !near(placed[1].left, 111.0F, 0.0F));

    /* A marker off the view gets no label; one whose centre is just past the edge still shows (half of it is drawn)
     * and is labelled, as far as half a marker out. */
    const ms::LabelRequest outside{-30.0F, 100.0F, 60.0F, 16.0F};
    fresh_space(space, {}, {});
    space.marker_half = 8.0F;
    assert(place(&outside, 1, space, placed, keep) == 0);
    const ms::LabelRequest edge_on{-3.0F, 100.0F, 60.0F, 16.0F};
    fresh_space(space, {-3.0F}, {100.0F});
    space.marker_half = 8.0F;
    assert(place(&edge_on, 1, space, placed, keep) == 1 && keep[0] == ms::kLabelClear
           && near(placed[0].left, 8.0F, 0.0F));
    fresh_space(space, {-3.0F}, {100.0F});
    space.marker_half = 0.0F;
    assert(place(&edge_on, 1, space, placed, keep) == 0);
}

/* An emergency's label is never dropped (never hue alone): it goes first, every other label yields to it, and with
 * no clear side it takes the side that covers least, keep-outs avoided while any side clears them. */
void
test_place_emergency_labels() {
    ms::LabelSpace space;
    ms::LabelBox placed[3];
    uint8_t keep[3] = {9U, 9U, 9U};
    ms::LabelRequest urgent{200.0F, 150.0F, 60.0F, 16.0F};
    urgent.urgent = true;

    /* Hemmed in by markers on every side: drawn anyway, on the side covering least (right: one marker's corner). */
    fresh_space(space, {200.0F, 232.0F, 140.0F, 200.0F, 200.0F}, {150.0F, 162.0F, 150.0F, 125.0F, 175.0F});
    assert(place(&urgent, 1, space, placed, keep) == 1 && keep[0] == ms::kLabelCovering);
    assert(near(placed[0].left, 211.0F, 0.0F));

    /* Every side touches something, the right only a keep-out: a side clear of keep-outs wins even covering more. */
    fresh_space(space, {200.0F, 140.0F, 200.0F, 200.0F}, {150.0F, 150.0F, 125.0F, 175.0F});
    space.keep_out.add(ms::LabelBox{265.0F, 140.0F, 10.0F, 20.0F});
    assert(place(&urgent, 1, space, placed, keep) == 1
           && !ms::boxes_overlap(placed[0], ms::LabelBox{265.0F, 140.0F, 10.0F, 20.0F}));
    assert(near(placed[0].left, 200.0F - 11.0F - 60.0F, 0.0F) || near(placed[0].top, 150.0F - 11.0F - 16.0F, 0.0F)
           || near(placed[0].top, 161.0F, 0.0F));
    /* Keep-outs on every side: the least overlap of all. */
    fresh_space(space, {200.0F}, {150.0F});
    space.keep_out.add(ms::LabelBox{0.0F, 0.0F, 400.0F, 148.0F});
    space.keep_out.add(ms::LabelBox{0.0F, 152.0F, 400.0F, 148.0F});
    assert(place(&urgent, 1, space, placed, keep) == 1);

    /* A label earlier in the order yields to the emergency: the emergency keeps the right, the other moves. */
    fresh_space(space, {100.0F, 100.0F}, {100.0F, 112.0F});
    ms::LabelRequest first{100.0F, 100.0F, 60.0F, 16.0F};
    ms::LabelRequest second{100.0F, 112.0F, 60.0F, 16.0F};
    second.urgent = true;
    const ms::LabelRequest order[2] = {first, second};
    assert(place(order, 2, space, placed, keep) == 2);
    assert(near(placed[1].left, 111.0F, 0.0F) && near(placed[1].top, 104.0F, 0.0F));
    assert(!ms::boxes_overlap(placed[0], placed[1]));

    /* Two emergencies on one spot, hemmed in: both are drawn. */
    fresh_space(space, {200.0F, 200.0F, 140.0F, 260.0F, 200.0F, 200.0F},
                {150.0F, 150.0F, 150.0F, 150.0F, 125.0F, 175.0F});
    const ms::LabelRequest both[2] = {urgent, urgent};
    assert(place(both, 2, space, placed, keep) == 2 && keep[0] != ms::kLabelDropped && keep[1] != ms::kLabelDropped);
}

/* The grid finds overlaps across cells, ignores what lies outside the view, takes boxes of any size (an infinite
 * keep-out reaches the last cell), measures covered area once per box, and reset keeps nothing. */
void
test_box_grid() {
    ms::BoxGrid grid;
    grid.reset(400.0F, 300.0F, 48.0F);
    grid.add(ms::LabelBox{90.0F, 90.0F, 20.0F, 20.0F}); /* straddles four cells */
    assert(grid.hits(ms::LabelBox{105.0F, 105.0F, 2.0F, 2.0F}) && !grid.hits(ms::LabelBox{111.0F, 90.0F, 5.0F, 5.0F}));
    const float covered = grid.overlap_area(ms::LabelBox{80.0F, 80.0F, 20.0F, 20.0F});
    assert(near(covered, 100.0F, 1.0e-3F));
    grid.add(ms::LabelBox{-50.0F, -50.0F, 10.0F, 10.0F});
    assert(!grid.hits(ms::LabelBox{-45.0F, -45.0F, 2.0F, 2.0F}));
    grid.add(ms::LabelBox{0.0F, 280.0F, std::numeric_limits<float>::infinity(), 10.0F});
    grid.add(ms::LabelBox{0.0F, 250.0F, 1.0e30F, 10.0F});
    assert(grid.hits(ms::LabelBox{395.0F, 285.0F, 2.0F, 2.0F}) && grid.hits(ms::LabelBox{395.0F, 255.0F, 2.0F, 2.0F}));
    grid.reset(400.0F, 300.0F, 48.0F);
    assert(!grid.hits(ms::LabelBox{105.0F, 105.0F, 2.0F, 2.0F}));
    ms::LabelSpace space;
    ms::LabelBox placed[1];
    uint8_t keep[1] = {0U};
    space.reset(400.0F, 300.0F, 48.0F);
    assert(ms::place_labels(nullptr, 2, 11.0F, ms::LabelBox{0.0F, 0.0F, 400.0F, 300.0F}, space, placed, keep) == 0);
}

/* A tile's place on screen, for any zoom: the view's own tiles where visible_tiles() puts them, a parent covering
 * its four children, the copy nearest the centre across 180 degrees, and edges snapped to device pixels. */
void
test_tile_rects() {
    mm::Viewport vp;
    vp.width = 390.0;
    vp.height = 844.0;
    vp.zoom = 12.37;
    vp.center_x = 0.61;
    vp.center_y = 0.47;
    vp = mm::clamp_viewport(vp);
    const std::vector<mm::VisibleTile> tiles = mm::visible_tiles(vp);
    assert(!tiles.empty());
    for (const mm::VisibleTile& t : tiles) {
        const ms::ScreenRect r = ms::tile_rect(vp, t.z, t.x, t.y);
        assert(std::fabs(r.left - t.left) < 1.0e-6 && std::fabs(r.top - t.top) < 1.0e-6);
        assert(std::fabs((r.right - r.left) - t.size) < 1.0e-6 && ms::rect_in_view(r, vp.width, vp.height));
    }
    const mm::VisibleTile& t = tiles.front();
    const ms::ScreenRect parent = ms::tile_rect(vp, t.z - 1, t.x / 2, t.y / 2);
    const ms::ScreenRect child = ms::tile_rect(vp, t.z, t.x, t.y);
    assert(parent.left <= child.left + 1.0e-6 && parent.right >= child.right - 1.0e-6);
    assert(parent.top <= child.top + 1.0e-6 && parent.bottom >= child.bottom - 1.0e-6);
    assert(std::fabs((parent.right - parent.left) - (2.0 * t.size)) < 1.0e-6);
    /* At 180 degrees: tile x = 0 sits just east of the centre, the last column just west. */
    mm::Viewport dateline = vp;
    dateline.center_x = 0.0;
    dateline = mm::clamp_viewport(dateline);
    assert(t.z >= 0 && t.z <= 19);
    const int last = static_cast<int>(std::ldexp(1.0, t.z)) - 1;
    const ms::ScreenRect east = ms::tile_rect(dateline, t.z, 0, t.y);
    const ms::ScreenRect west = ms::tile_rect(dateline, t.z, last, t.y);
    assert(std::fabs(east.left - (dateline.width / 2.0)) < 1.0e-6 && std::fabs(west.right - east.left) < 1.0e-6);
    /* Snapped edges land on device pixels, and two neighbours still share theirs. */
    const ms::ScreenRect a = ms::snap_rect(ms::tile_rect(vp, t.z, t.x, t.y), 2.625);
    const ms::ScreenRect b = ms::snap_rect(ms::tile_rect(vp, t.z, t.x + 1, t.y), 2.625);
    assert(std::fabs(a.right - b.left) < 1.0e-9);
    assert(std::fabs((a.left * 2.625) - std::round(a.left * 2.625)) < 1.0e-6);
    assert(!ms::rect_in_view(
        ms::ScreenRect{
            -20.0,
            0.0,
            -1.0,
            10.0,
        },
        100.0, 100.0));
    const ms::ScreenRect bad = ms::tile_rect(vp, 31, 0, 0);
    assert(near(static_cast<float>(bad.right - bad.left), 0.0F, 0.0F));
}

/* Trails built for one view are reused for a pan by a translation that matches where the markers move. */
void
test_pan_reuse() {
    mm::Viewport built;
    built.width = 400.0;
    built.height = 300.0;
    built.zoom = 11.0;
    built.center_x = 0.999; /* just west of 180 degrees, so the pan crosses it */
    built.center_y = 0.4;
    built = mm::clamp_viewport(built);
    const mm::Viewport now = mm::pan(built, -37.5, 12.25);
    const mm::ScreenPoint d = ms::pan_offset(built, now);
    assert(std::fabs(d.x + 37.5) < 1.0e-6 && std::fabs(d.y - 12.25) < 1.0e-6);
    const mm::WorldPoint p{0.0005, 0.4001};
    const mm::ScreenPoint before = mm::screen_from_world(built, p);
    const mm::ScreenPoint after = mm::screen_from_world(now, p);
    assert(std::fabs((before.x + d.x) - after.x) < 1.0e-6 && std::fabs((before.y + d.y) - after.y) < 1.0e-6);
    assert(ms::reusable(built, now, 200.0));
    assert(!ms::reusable(built, now, 30.0));
    mm::Viewport zoomed = now;
    zoomed.zoom += 0.01;
    assert(!ms::reusable(built, zoomed, 1.0e9));
    mm::Viewport resized = now;
    resized.width += 1.0;
    assert(!ms::reusable(built, resized, 1.0e9));
    /* The copy a point is drawn on: the same while it stays within half a world of the centre, the next one past. */
    mm::Viewport wide = built;
    wide.zoom = 2.0;
    wide.center_x = 0.5;
    wide = mm::clamp_viewport(wide);
    const mm::WorldPoint near_flip{0.99, 0.5};
    assert(ms::wrap_copy(wide, near_flip) == 0);
    const mm::Viewport crossed = mm::pan(wide, 30.0, 0.0); /* 0.49 + 30/1024 > 0.5 */
    assert(ms::wrap_copy(crossed, near_flip) == 1);
    assert(ms::reusable(wide, crossed, 200.0)); /* the pan alone would allow reuse: the copy is what forbids it */
    const mm::ScreenPoint a = mm::screen_from_world(wide, near_flip);
    const mm::ScreenPoint b = mm::screen_from_world(crossed, near_flip);
    assert(std::fabs((b.x - a.x) - (30.0 - 1024.0)) < 1.0e-6);
    assert(ms::wrap_copy(wide, mm::WorldPoint{std::numeric_limits<double>::quiet_NaN(), 0.5}) == 0);
}

/* During a zoom, the trails built for one view stand in for the next by one scale and offset: exact for every point
 * on the same copy, within half a zoom step, and only while the built area still covers the view. */
void
test_scaled_reuse() {
    mm::Viewport built;
    built.width = 412.0;
    built.height = 915.0;
    built.zoom = 10.5;
    built.center_x = 0.555;
    built.center_y = 0.47;
    built = mm::clamp_viewport(built);
    const mm::Viewport now = mm::zoom_about(built, 10.8, 120.0, 610.0);
    const ms::ViewTransform t = ms::view_transform(built, now);
    assert(std::fabs(t.scale - std::exp2(0.3)) < 1.0e-9);
    for (const mm::WorldPoint p : {mm::WorldPoint{0.5551, 0.4701}, mm::WorldPoint{0.5549, 0.4699}}) {
        const mm::ScreenPoint a = mm::screen_from_world(built, p);
        const mm::ScreenPoint b = mm::screen_from_world(now, p);
        assert(std::fabs(((t.scale * a.x) + t.dx) - b.x) < 1.0e-6
               && std::fabs(((t.scale * a.y) + t.dy) - b.y) < 1.0e-6);
    }
    /* The anchor of the zoom stays put. */
    const mm::WorldPoint anchor = mm::world_from_screen_raw(built, 120.0, 610.0);
    const mm::ScreenPoint s = mm::screen_from_world(now, anchor);
    assert(std::fabs(s.x - 120.0) < 1.0e-6 && std::fabs(s.y - 610.0) < 1.0e-6);
    const double margin = 0.5 * 915.0;
    assert(ms::reusable_scaled(built, now, margin, ms::kZoomScaleReach));
    /* Zooming out shrinks the built area: past the margin's cover it must be built again. */
    const mm::Viewport out = mm::zoom_about(built, 10.0, 0.0, 0.0);
    assert(!ms::reusable_scaled(built, out, 0.0, ms::kZoomScaleReach));
    /* Past half a zoom step either way. */
    const mm::Viewport in_far = mm::zoom_about(built, 11.1, 206.0, 457.0);
    const mm::Viewport out_far = mm::zoom_about(built, 9.9, 206.0, 457.0);
    assert(!ms::reusable_scaled(built, in_far, margin, ms::kZoomScaleReach));
    assert(!ms::reusable_scaled(built, out_far, 1.0e6, ms::kZoomScaleReach));
    /* A resize during the zoom: the margin the content was clipped with no longer holds, so it is built again. */
    mm::Viewport resized = now;
    resized.height += 40.0;
    resized = mm::clamp_viewport(resized);
    assert(!ms::reusable_scaled(built, resized, margin, ms::kZoomScaleReach));
    /* At the same zoom it is the pan offset. */
    const mm::Viewport panned = mm::pan(built, 12.0, -7.0);
    const ms::ViewTransform p = ms::view_transform(built, panned);
    const mm::ScreenPoint d = ms::pan_offset(built, panned);
    assert(std::fabs(p.scale - 1.0) < 1.0e-12 && std::fabs(p.dx - d.x) < 1.0e-9 && std::fabs(p.dy - d.y) < 1.0e-9);
}

/* The camera's path: the ends exactly, east-west the short way across 180 degrees. */
void
test_interpolate_viewport() {
    mm::Viewport from;
    from.width = 400.0;
    from.height = 300.0;
    from.zoom = 10.0;
    from.center_x = 0.98;
    from.center_y = 0.45;
    mm::Viewport to = from;
    to.zoom = 12.0;
    to.center_x = 0.02;
    to.center_y = 0.46;
    const mm::Viewport start = ms::interpolate_viewport(from, to, 0.0);
    const mm::Viewport end = ms::interpolate_viewport(from, to, 1.0);
    assert(std::fabs(start.center_x - 0.98) < 1.0e-12 && std::fabs(end.center_x - 0.02) < 1.0e-12);
    assert(std::fabs(end.zoom - 12.0) < 1.0e-12);
    const mm::Viewport mid = ms::interpolate_viewport(from, to, 0.5);
    assert(std::fabs(mid.zoom - 11.0) < 1.0e-12);
    assert(mid.center_x < 1.0e-9 || mid.center_x > 1.0 - 1.0e-9); /* at 180 degrees, not back across the world */
    assert(std::fabs(mid.center_y - 0.455) < 1.0e-12);
    const mm::Viewport nan = ms::interpolate_viewport(from, to, std::numeric_limits<double>::quiet_NaN());
    assert(std::fabs(nan.center_x - 0.98) < 1.0e-12);
}

/* PR2 review: a dash or gap used up exactly at a joint toggles there; the next segment starts the next part instead of
 * being skipped as if hidden. */
void
test_dash_boundary_at_a_joint() {
    const mg::TrailStyle style = pen(6.0F, 4.0F); /* half width 1: a 2 px pen, a 4 px centre-line dash */
    const auto reach = [](const std::vector<mg::StrokeVertex>& v) {
        return std::accumulate(v.begin(), v.end(), -1.0e9F,
                               [](float right, const mg::StrokeVertex& p) { return std::fmax(right, p.x); });
    };
    /* A 100 px line, and the same line with a joint where the first dash ends (x = 4). */
    const float plain_x[2] = {0.0F, 100.0F};
    const float plain_y[2] = {0.0F, 0.0F};
    std::vector<mg::StrokeVertex> plain;
    (void)mg::append_trail(plain_x, plain_y, 2, style, nullptr, plain);
    const float joint_x[3] = {0.0F, 4.0F, 100.0F};
    const float joint_y[3] = {0.0F, 0.0F, 0.0F};
    std::vector<mg::StrokeVertex> joint;
    (void)mg::append_trail(joint_x, joint_y, 3, style, nullptr, joint);
    assert(reach(joint) > 90.0F && std::fabs(reach(joint) - reach(plain)) < 0.5F);
    /* With a clip too (the hidden-stretch path), and with the default style's three points 30, 34, 134. */
    const mg::ClipRect clip{-10.0F, -10.0F, 400.0F, 300.0F};
    std::vector<mg::StrokeVertex> clipped;
    (void)mg::append_trail(joint_x, joint_y, 3, style, &clip, clipped);
    assert(std::fabs(reach(clipped) - reach(plain)) < 0.5F);
    const float far_x[3] = {30.0F, 34.0F, 134.0F};
    const float far_y[3] = {50.0F, 50.0F, 50.0F};
    std::vector<mg::StrokeVertex> far;
    (void)mg::append_trail(far_x, far_y, 3, style, &clip, far);
    assert(reach(far) > 120.0F);
    /* A hidden stretch that ends exactly where a part does: the next shown segment still draws. */
    const mg::ClipRect right_part{50.0F, -10.0F, 400.0F, 300.0F};
    const float hidden_x[3] = {0.0F, 4.0F, 120.0F};
    std::vector<mg::StrokeVertex> hidden;
    (void)mg::append_trail(hidden_x, joint_y, 3, style, &right_part, hidden);
    assert(reach(hidden) > 110.0F);
    /* A hidden stretch that ends exactly at the clip's edge (50 less the pen's reach, 48.5) and exactly where a part
     * ends (a 4 px centre-line dash): the shown segment from there draws on. */
    const float edge_x[3] = {44.5F, 48.5F, 140.0F};
    std::vector<mg::StrokeVertex> at_edge;
    (void)mg::append_trail(edge_x, joint_y, 3, style, &right_part, at_edge);
    assert(reach(at_edge) > 130.0F);
}

/* PR2 review: copies are counted against the camera carried on the short way, as view_transform() moves content. */
void
test_continued_viewport() {
    mm::Viewport built;
    built.width = 256.0;
    built.height = 256.0;
    built.zoom = 0.0;
    built.center_x = 0.99;
    mm::Viewport now = built;
    now.center_x = 0.01;
    const mm::WorldPoint radio{0.5, 0.5};
    assert(ms::wrap_copy(built, radio) == ms::wrap_copy(now, radio)); /* the wrapped camera hides the jump */
    const mm::Viewport carried = ms::continued_viewport(built, now);
    assert(std::fabs(carried.center_x - 1.01) < 1.0e-12);
    assert(ms::wrap_copy(built, radio) != ms::wrap_copy(carried, radio));
    /* An ordinary pan keeps every copy. */
    mm::Viewport panned = built;
    panned.center_x = 0.97;
    const mm::Viewport panned_on = ms::continued_viewport(built, panned);
    assert(ms::wrap_copy(built, radio) == ms::wrap_copy(panned_on, radio));
}

} // namespace

int
main(void) {
    test_solid_trail_is_the_spectrum_stroke();
    test_a_break_is_never_bridged();
    test_dashes_have_their_visible_lengths();
    test_a_dash_bends_round_a_turn();
    test_clipped_dashes_match_and_stay_cheap();
    test_bad_trail_input();
    test_circle_profile_and_single_cover();
    test_circle_far_larger_than_the_view();
    test_circle_bad_input();
    test_slices_bound_the_chord_error();
    test_atlas_cells();
    test_marker_quads();
    test_dash_boundary_at_a_joint();
    test_continued_viewport();
    test_fill_levels();
    test_marker_cells();
    test_split_trail();
    test_place_labels();
    test_place_emergency_labels();
    test_box_grid();
    test_tile_rects();
    test_pan_reuse();
    test_interpolate_viewport();
    test_scaled_reuse();
    (void)DSD_FPRINTF(stdout, "UI_QT_MAP_GEOMETRY: ok\n");
    return 0;
}

// NOLINTEND(cert-dcl03-c,misc-static-assert)
