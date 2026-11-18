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
 */

// LLVM 22/GCC 16 misclassifies these runtime test oracles as compile-time assertions.
// NOLINTBEGIN(cert-dcl03-c,misc-static-assert)

#include <cassert>
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

#include <dsd-neo/core/safe_api.h>

#include "map_geometry.h"
#include "spectrum_trace_geometry.h"

namespace mg = dsd_qt::map_geometry;
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
    /* Bad input adds nothing. */
    assert(mg::append_marker_quad(kNaN, 51.9F, 24.0F, dpr, cell, out) == 0);
    assert(mg::append_marker_quad(10.0F, 51.9F, 0.0F, dpr, cell, out) == 0);
    assert(mg::append_marker_quad(10.0F, 51.9F, 24.0F, 0.0F, cell, out) == 0);
    assert(mg::append_marker_quad(10.0F, 51.9F, 24.0F, kNaN, cell, out) == 0);
    assert(out.size() == 6U);
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
    (void)DSD_FPRINTF(stdout, "UI_QT_MAP_GEOMETRY: ok\n");
    return 0;
}

// NOLINTEND(cert-dcl03-c,misc-static-assert)
