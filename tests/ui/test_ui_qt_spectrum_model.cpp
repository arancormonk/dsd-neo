// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Regression test: what the spectrum model does with the frames it is handed,
 * and what the waterfall does with the model.
 *
 * The viewport arithmetic itself is UI_QT_SPECTRUM_MATH's job. What lives here
 * is the part that only shows up once a producer and a poll timer are running
 * against each other on separate clocks:
 *
 *  - a poll that re-reads the frame already on screen must not be announced as
 *    a new one, or the waterfall scrolls duplicate rows and reports a signal as
 *    lasting longer than it did;
 *  - the first frame captured at a new center must survive the history clear
 *    that the retune triggers, because it is the one row that is certain to be
 *    worth seeing;
 *  - a tap must never resolve to a frequency outside the visible window, even
 *    with a window edge between two bin centers, and a hop must still find the
 *    carriers inside it once the view has been panned clear of the tuned center;
 *  - the trace's GPU node tree, its software-scene-graph picture, one auto-range
 *    fold per frame, and both items forgetting a model that is destroyed.
 *
 * The producer is stubbed here rather than mocked through the engine: the model
 * only ever sees the app-control getter, so supplying that is the whole of it.
 */

// LLVM 22/GCC 16 misclassifies these runtime test oracles as compile-time assertions.
// NOLINTBEGIN(cert-dcl03-c,misc-static-assert)

#include <QColor>
#include <QGuiApplication>
#include <QImage>
#include <QObject>
#include <QPainter>
#include <QQuickWindow>
#include <QSGFlatColorMaterial>
#include <QSGGeometry>
#include <QSGGeometryNode>
#include <QSGRendererInterface>
#include <QSizeF>
#include <Qt>
#include <QtGlobal>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <qrgb.h>
#include <stdio.h>

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/wideband_spectrum.h>

#include "spectrum_math.h"
#include "spectrum_model.h"
#include "spectrum_trace_geometry.h"
#include "spectrum_view_item.h"

namespace {

/* ------------------------------------------------------------- producer ---- */

constexpr int kBins = DSD_WIDEBAND_SPECTRUM_BINS;
constexpr uint32_t kCenterHz = 851000000U;
constexpr uint32_t kSpanHz = 1536000U;

float g_bins[kBins];
uint32_t g_center_hz = kCenterHz;
uint32_t g_span_hz = kSpanHz;
uint32_t g_serial = 0;
int g_enabled = 0;
int g_get_calls = 0;

/** @brief Flat noise with one strong bin, as a real capture with one carrier. */
void
seed_bins(int peak_bin) {
    for (int i = 0; i < kBins; i++) {
        g_bins[i] = -80.0F;
    }
    if (peak_bin >= 0 && peak_bin < kBins) {
        g_bins[peak_bin] = -20.0F;
    }
}

/** @brief Publish a frame, as the demod thread would. */
void
publish(uint32_t center_hz) {
    g_center_hz = center_hz;
    g_serial++;
}

} // namespace

extern "C" int
dsd_app_frontend_wideband_spectrum_get(float* out_db, int max_bins, uint32_t* out_center_freq_hz, uint32_t* out_span_hz,
                                       uint32_t* out_frame_serial) {
    g_get_calls++;
    if (!out_db || max_bins < kBins || g_enabled == 0 || g_serial == 0) {
        return 0;
    }
    for (int i = 0; i < kBins; i++) {
        out_db[i] = g_bins[i];
    }
    if (out_center_freq_hz) {
        *out_center_freq_hz = g_center_hz;
    }
    if (out_span_hz) {
        *out_span_hz = g_span_hz;
    }
    if (out_frame_serial) {
        *out_frame_serial = g_serial;
    }
    return kBins;
}

extern "C" void
dsd_app_frontend_wideband_spectrum_set_enabled(int on) {
    g_enabled = on ? 1 : 0;
}

namespace {

/* ---------------------------------------------------------------- cases ---- */

/*
 * The poll timer and the publish period free-run, so most polls land on a frame
 * that is already drawn. Announcing one as new is what fills the waterfall with
 * rows nothing happened in.
 */
void
test_a_repeated_frame_is_not_a_new_frame(void) {
    seed_bins(kBins / 2);
    g_serial = 0;
    dsd_qt::SpectrumModel model;
    /* A plain counter rather than QSignalSpy: that lives in Qt's testlib, which
     * distributions package separately, and nothing else here needs it. */
    int frames = 0;
    QObject::connect(&model, &dsd_qt::SpectrumModel::frameChanged, &model, [&frames]() { frames++; });

    model.setActive(true);
    publish(kCenterHz);

    model.testPoll();
    assert(model.hasData());
    const quint64 first = model.frameIndex();
    assert(frames == 1);

    /* Three more polls, no new publish: the producer has not replaced the frame. */
    model.testPoll();
    model.testPoll();
    model.testPoll();
    assert(model.frameIndex() == first);
    assert(frames == 1);

    /* ...and a fresh publish is announced again. */
    publish(kCenterHz);
    model.testPoll();
    assert(model.frameIndex() == first + 1);
    assert(frames == 2);

    model.setActive(false);
}

/*
 * A retune clears the waterfall's history, which is right — rows measured at
 * the old center mean nothing at the new one. The frame that arrives *with* the
 * retune was captured at the new center, so it must still become a row.
 */
void
test_the_first_frame_after_a_retune_becomes_a_row(void) {
    seed_bins(kBins / 2);
    g_serial = 0;
    dsd_qt::SpectrumModel model;
    dsd_qt::WaterfallItem waterfall;
    waterfall.setModel(&model);
    int retunes = 0;
    QObject::connect(&model, &dsd_qt::SpectrumModel::retuned, &model, [&retunes]() { retunes++; });

    model.setActive(true);
    publish(kCenterHz);
    model.testPoll();
    assert(waterfall.testRowsWritten() == 1);

    publish(kCenterHz);
    model.testPoll();
    assert(waterfall.testRowsWritten() == 2);

    /* The tap lands, the front end moves, and the next frame is at the new
     * center. History resets to nothing — and then immediately takes that
     * frame, rather than skipping it as already seen. */
    publish(kCenterHz + 1000000U);
    model.testPoll();
    assert(retunes == 1);
    assert(waterfall.testRowsWritten() == 1);

    publish(kCenterHz + 1000000U);
    model.testPoll();
    assert(waterfall.testRowsWritten() == 2);

    model.setActive(false);
}

/*
 * The snap width is derived from the view span, so at high zoom it reaches
 * further than the screen does. A tap must never answer with a carrier the user
 * cannot see — that reads as a tap that landed somewhere random.
 */
void
test_a_tap_never_snaps_outside_the_visible_window(void) {
    /* One loud bin near the top of the capture, quiet everywhere else. */
    const int loud_bin = kBins - 8;
    seed_bins(loud_bin);
    g_serial = 0;
    dsd_qt::SpectrumModel model;

    model.setActive(true);
    publish(kCenterHz);
    model.testPoll();
    assert(model.hasData());

    /* Zoom in and pan so the loud bin sits just past the right-hand edge. */
    model.setZoom(8.0);
    const double bin_width = static_cast<double>(kSpanHz) / static_cast<double>(kBins);
    const double loud_hz =
        static_cast<double>(kCenterHz) + ((static_cast<double>(loud_bin) - (kBins / 2.0)) * bin_width);
    const double view_span = static_cast<double>(kSpanHz) / 8.0;
    /* Put the window's high edge a few bins below the carrier. */
    model.setViewOffsetHz((loud_hz - (4.0 * bin_width)) - (view_span / 2.0) - static_cast<double>(kCenterHz));

    const double high = model.viewHighHz();
    const double low = model.viewLowHz();
    assert(loud_hz > high); /* the carrier really is off screen */

    /* A tap on the right-hand edge, where the unbounded search would reach it. */
    const double tapped = model.tapFrequencyHz(1.0);
    assert(tapped >= low && tapped <= high);

    model.setActive(false);
}

/*
 * The hop is bounded by the visible window but takes its reference from the
 * tuned center, and past 2x the window can be panned until that center is not
 * inside it at all. Seeded outside its own bounds, one of the two directions
 * searches an empty range: the control reports an empty band with carriers
 * plainly on screen, and stays that way until the view is reset.
 */
void
test_a_hop_still_works_when_the_view_is_panned_off_center(void) {
    /* Two carriers in the top of the capture, either side of where the panned
     * window's middle lands. The lower one is the stronger of the two, so an
     * upward hop that searched the whole window would answer with it. */
    const int lower_bin = 940;
    const int upper_bin = 980;
    seed_bins(lower_bin);
    g_bins[lower_bin] = -15.0F;
    g_bins[upper_bin] = -20.0F;
    g_serial = 0;
    dsd_qt::SpectrumModel model;

    model.setActive(true);
    publish(kCenterHz);
    model.testPoll();
    assert(model.hasData());

    /* Past 2x the maximum pan clears the center bin entirely; the offset is
     * deliberately over-large and left to the model's own clamp. */
    model.setZoom(8.0);
    model.setViewOffsetHz(static_cast<double>(kSpanHz));
    assert(model.viewLowHz() > static_cast<double>(kCenterHz));

    const double bin_width = static_cast<double>(kSpanHz) / static_cast<double>(kBins);
    const double lower_hz =
        static_cast<double>(kCenterHz) + ((static_cast<double>(lower_bin) - (kBins / 2.0)) * bin_width);
    const double upper_hz =
        static_cast<double>(kCenterHz) + ((static_cast<double>(upper_bin) - (kBins / 2.0)) * bin_width);
    assert(lower_hz > model.viewLowHz() && upper_hz < model.viewHighHz());

    /* Both carriers are on screen, so both directions have an answer to give. */
    assert(std::fabs(model.nextPeakHz(1) - upper_hz) < (bin_width / 2.0));
    assert(std::fabs(model.nextPeakHz(-1) - lower_hz) < (bin_width / 2.0));

    model.setActive(false);
}

/*
 * Production follows the view, so a closed view costs no FFT at all, and a
 * reopened one never shows the frame the last session left behind.
 */
void
test_production_follows_the_view(void) {
    seed_bins(kBins / 2);
    g_serial = 0;
    dsd_qt::SpectrumModel model;

    assert(g_enabled == 0);
    model.setActive(true);
    assert(g_enabled == 1);
    publish(kCenterHz);
    model.testPoll();
    assert(model.hasData());

    model.setActive(false);
    assert(g_enabled == 0);
    assert(!model.hasData());
    assert(model.binCount() == 0);
}

/* --------------------------------------------------------- window edges ---- */

/** @brief Frequency of bin @p bin, as the model reports it. */
double
bin_hz(int bin) {
    const double bin_width = static_cast<double>(kSpanHz) / static_cast<double>(kBins);
    return static_cast<double>(kCenterHz) + ((static_cast<double>(bin) - (kBins / 2.0)) * bin_width);
}

/*
 * A window edge that falls between two bin centers. Rounding the edge to the
 * nearest bin admits the bin just outside it: zoom 8 and a 375 Hz pan put the
 * low edge at 850,904,375 Hz, a quarter-bin above bin 448 (850,904,000 Hz). A
 * tap at that edge, or a hop downward, then answered with a carrier the screen
 * does not show, and the tuner marker drew it off screen.
 */
void
test_a_fractional_window_edge_admits_no_bin_outside_it(void) {
    struct Case {
        double offset_hz;
        int loud_bin;
        double tap_fraction;
        int hop_direction;
    };

    /* The low edge a quarter-bin above bin 448; the high edge a quarter-bin below bin 576. */
    const Case cases[2] = {{375.0, 448, 0.0, -1}, {-375.0, 576, 1.0, 1}};
    for (const Case& c : cases) {
        seed_bins(c.loud_bin);
        g_serial = 0;
        dsd_qt::SpectrumModel model;
        model.setActive(true);
        publish(kCenterHz);
        model.testPoll();
        model.setZoom(8.0);
        model.setViewOffsetHz(c.offset_hz);
        const double low = model.viewLowHz();
        const double high = model.viewHighHz();
        const double loud = bin_hz(c.loud_bin);
        assert(loud < low || loud > high); /* just outside, by a quarter bin */
        assert(std::fabs(loud - low) < 400.0 || std::fabs(loud - high) < 400.0);

        const double tapped = model.tapFrequencyHz(c.tap_fraction);
        assert(tapped >= low && tapped <= high);
        const double hop = model.nextPeakHz(c.hop_direction);
        assert(!(hop > 0.0) || (hop >= low && hop <= high)); /* 0 is "nothing on screen" */
        model.setActive(false);
    }
}

/* ---------------------------------------------------------------- trace ---- */

/** @brief A model holding one published frame with a carrier at @p peak_bin. */
void
start_model(dsd_qt::SpectrumModel& model, int peak_bin) {
    seed_bins(peak_bin);
    g_serial = 0;
    model.setActive(true);
    publish(kCenterHz);
    model.testPoll();
    assert(model.hasData());
}

QSGGeometryNode*
child_geometry(QSGNode* root, int index) {
    QSGNode* child = root->childAtIndex(index);
    assert(child != nullptr && child->type() == QSGNode::GeometryNodeType);
    return static_cast<QSGGeometryNode*>(child);
}

/*
 * What the GPU path hands Qt. Headless Qt only has the software scene graph,
 * so this is the one place a test sees it: three layers in drawing order, each
 * owning its geometry and material, kept out of Qt's 16-bit merged batches,
 * and colored with premultiplied alpha — a zero-coverage fringe vertex is
 * transparent black, not the line color with its alpha cleared.
 */
void
test_the_gpu_tree(void) {
    dsd_qt::SpectrumModel model;
    start_model(model, kBins / 2);
    dsd_qt::SpectrumTraceItem trace;
    trace.setLineColor(QColor(0x22, 0xDC, 0xF5));
    trace.setAreaColor(QColor(0x22, 0xDC, 0xF5, 0x24));
    trace.setGridColor(QColor(0x23, 0x2B, 0x3A));
    trace.setModel(&model);

    QSGNode* root = trace.testUpdateGpuTree(nullptr, 400.0, 200.0, 2.0);
    assert(root != nullptr && root->childCount() == 3);
    QSGGeometryNode* grid = child_geometry(root, 0);
    QSGGeometryNode* area = child_geometry(root, 1);
    QSGGeometryNode* line = child_geometry(root, 2);
    for (QSGGeometryNode* layer : {grid, area, line}) {
        assert(layer->flags().testFlag(QSGNode::OwnsGeometry) && layer->flags().testFlag(QSGNode::OwnsMaterial));
        assert(layer->geometry()->indexType() == QSGGeometry::UnsignedIntType);
        assert(layer->geometry()->indexCount() == 0);
    }
    assert(grid->geometry()->vertexCount() == 3 * dsd_qt::spectrum_geometry::stroke_vertex_capacity(2));
    assert(area->geometry()->vertexCount() == 2 * 400);
    const int line_vertices = line->geometry()->vertexCount();
    assert(line_vertices > 0 && line_vertices <= dsd_qt::spectrum_geometry::stroke_vertex_capacity(400));
    auto* area_material = static_cast<QSGFlatColorMaterial*>(area->material());
    assert(area_material->color() == QColor(0x22, 0xDC, 0xF5, 0x24));

    bool saw_core = false;
    bool saw_edge = false;
    const QSGGeometry::ColoredPoint2D* v = line->geometry()->vertexDataAsColoredPoint2D();
    for (int i = 0; i < line_vertices; i++) {
        if (v[i].a == 0) {
            assert(v[i].r == 0 && v[i].g == 0 && v[i].b == 0);
            saw_edge = true;
        } else if (v[i].a == 255) {
            assert(v[i].r == 0x22 && v[i].g == 0xDC && v[i].b == 0xF5);
            saw_core = true;
        }
        assert(v[i].x >= -2.0F && v[i].x <= 402.0F);
    }
    assert(saw_core && saw_edge);

    /* Same tree on the next sync; a narrower item reallocates in place. */
    assert(trace.testUpdateGpuTree(root, 300.0, 200.0, 2.0) == root);
    assert(child_geometry(root, 1)->geometry()->vertexCount() == 2 * 300);

    /* A transparent layer draws nothing, and its vertices go with it. */
    trace.setLineColor(QColor(0, 0, 0, 0));
    assert(trace.testUpdateGpuTree(root, 300.0, 200.0, 2.0) == root);
    assert(child_geometry(root, 2)->geometry()->vertexCount() == 0);

    /* No frame: the grid still shows, the trace does not. */
    model.setActive(false);
    assert(trace.testUpdateGpuTree(root, 300.0, 200.0, 2.0) == root);
    assert(child_geometry(root, 0)->geometry()->vertexCount() > 0);
    assert(child_geometry(root, 1)->geometry()->vertexCount() == 0);

    /* ...and back again, on the same tree. */
    start_model(model, kBins / 2);
    trace.setLineColor(QColor(0x22, 0xDC, 0xF5));
    assert(trace.testUpdateGpuTree(root, 300.0, 200.0, 2.0) == root);
    assert(child_geometry(root, 1)->geometry()->vertexCount() == 2 * 300);
    assert(child_geometry(root, 2)->geometry()->vertexCount() > 0);

    /* The fringe is one device pixel whatever the pixel ratio, so a ratio change
     * with no new frame still reshapes it: the 1 px grid line at y = 50 reaches
     * 0.5 + 1/2 logical px at 1x and 0.5 + 1/8 at 4x. */
    auto grid_reach = [&](double ratio) {
        QSGGeometryNode* layer = child_geometry(trace.testUpdateGpuTree(root, 300.0, 200.0, ratio), 0);
        const QSGGeometry::ColoredPoint2D* p = layer->geometry()->vertexDataAsColoredPoint2D();
        float reach = 0.0F;
        for (int i = 0; i < layer->geometry()->vertexCount(); i++) {
            if (p[i].y < 75.0F) {
                reach = std::fmax(reach, std::fabs(p[i].y - 50.0F));
            }
        }
        return reach;
    };
    assert(std::fabs(grid_reach(1.0) - 1.0F) < 1e-4F);
    assert(std::fabs(grid_reach(4.0) - 0.625F) < 1e-4F);

    /* A recreated tree (the scene graph was torn down and rebuilt) folds nothing:
     * the range is the item's, not the tree's. */
    const double before = trace.testRange().max_db;
    QSGNode* fresh = trace.testUpdateGpuTree(nullptr, 300.0, 200.0, 2.0);
    assert(fresh != nullptr && fresh != root);
    assert(std::fabs(trace.testRange().max_db - before) < 1e-12);
    delete fresh;
    model.setActive(false);
    delete root;
}

/*
 * A frame with non-finite bins (a producer glitch) must not put a NaN into the
 * vertices: the GPU would draw garbage triangles across the screen, or none.
 */
void
test_a_bad_frame_draws_finite_geometry(void) {
    seed_bins(kBins / 2);
    g_bins[10] = std::numeric_limits<float>::quiet_NaN();
    g_bins[11] = std::numeric_limits<float>::infinity();
    g_bins[12] = -std::numeric_limits<float>::infinity();
    g_serial = 0;
    dsd_qt::SpectrumModel model;
    model.setActive(true);
    publish(kCenterHz);
    model.testPoll();
    dsd_qt::SpectrumTraceItem trace;
    trace.setModel(&model);
    QSGNode* root = trace.testUpdateGpuTree(nullptr, 400.0, 200.0, 2.0);
    for (int layer = 0; layer < 3; layer++) {
        const QSGGeometry* g = child_geometry(root, layer)->geometry();
        const auto* bytes = static_cast<const char*>(g->vertexData());
        for (int i = 0; i < g->vertexCount(); i++) {
            const auto* xy = reinterpret_cast<const float*>(bytes + (static_cast<ptrdiff_t>(i) * g->sizeOfVertex()));
            assert(std::isfinite(xy[0]) && std::isfinite(xy[1]));
        }
    }
    delete root;
    model.setActive(false);
}

/*
 * The software scene graph (QT_QUICK_BACKEND=software, or Qt's own fallback on
 * a platform without an RHI) skips geometry nodes, so there the trace is
 * painted into an image as before. The peak must land in its own column, at
 * the top, and the grid must show with no frame at all.
 */
void
test_the_software_picture(void) {
    dsd_qt::SpectrumModel model;
    const int peak_bin = kBins / 2;
    start_model(model, peak_bin);
    dsd_qt::SpectrumTraceItem trace;
    trace.setLineColor(QColor(255, 0, 0));
    trace.setAreaColor(QColor(0, 0, 0, 0));
    trace.setGridColor(QColor(0, 0, 255));
    trace.setModel(&model);

    const QImage picture = trace.testSoftwareImage(400.0, 200.0, 1.0);
    assert(picture.width() == 400 && picture.height() == 200);
    /* The middle bin of a full-span view is column 200; the peak tops the range. */
    int top_red = picture.height();
    for (int y = 0; y < picture.height(); y++) {
        if (qRed(picture.pixel(200, y)) > 128 && qBlue(picture.pixel(200, y)) < 64) {
            top_red = y;
            break;
        }
    }
    assert(top_red < 30);

    /* A pixel ratio scales the picture, not the chart. */
    assert(trace.testSoftwareImage(400.0, 200.0, 2.0).width() == 800);
    /* ...at its own pixel scale. At 1.25 a 401 x 201 item rounds up to a 502 x 252
     * picture, 401.6 x 201.6 logical: shown at that size, from the origin, so the
     * sliver past the item is clipped rather than the whole chart squeezed into it. */
    double shown_w = 0.0;
    double shown_h = 0.0;
    trace.testSoftwareRectSize(401.0, 201.0, 1.25, &shown_w, &shown_h);
    assert(std::fabs(shown_w - 401.6) < 1e-9 && std::fabs(shown_h - 201.6) < 1e-9);
    trace.testSoftwareRectSize(400.0, 200.0, 1.0, &shown_w, &shown_h);
    assert(std::fabs(shown_w - 400.0) < 1e-9 && std::fabs(shown_h - 200.0) < 1e-9);

    model.setActive(false);
    const QImage idle = trace.testSoftwareImage(400.0, 200.0, 1.0);
    /* A grid line at h/4: a 1 px antialiased pen centered on y = 50 covers half of rows 49 and 50. */
    assert(qBlue(idle.pixel(200, 49)) + qBlue(idle.pixel(200, 50)) > 200);
    assert(qAlpha(idle.pixel(200, 30)) == 0); /* and no trace */
}

/*
 * The whole path through a real window, on the software scene graph the test
 * runs with: the image node shows up, and an item one pixel tall draws nothing
 * at all, as the painted item's early return did.
 */
void
test_a_window_shows_the_trace(void) {
    dsd_qt::SpectrumModel model;
    start_model(model, kBins / 2);
    QQuickWindow window;
    window.resize(400, 200);
    window.setColor(Qt::white);
    dsd_qt::SpectrumTraceItem trace(window.contentItem());
    trace.setSize(QSizeF(400.0, 200.0));
    trace.setLineColor(QColor(255, 0, 0));
    trace.setAreaColor(QColor(0, 0, 0, 0));
    trace.setModel(&model);
    window.show();
    assert(window.rendererInterface()->graphicsApi() == QSGRendererInterface::Software);

    const QImage shown = window.grabWindow();
    int red = 0;
    for (int y = 0; y < shown.height(); y++) {
        red += (qRed(shown.pixel(200, y)) > 200 && qGreen(shown.pixel(200, y)) < 100) ? 1 : 0;
    }
    assert(red > 0);

    trace.setSize(QSizeF(400.0, 1.0));
    const QImage flat = window.grabWindow();
    for (int x = 0; x < flat.width(); x += 7) {
        assert(flat.pixel(x, 0) == QColor(Qt::white).rgb());
    }
    model.setActive(false);
}

/*
 * The fold that drives the auto-range runs once per published frame. A pan
 * repaints at touch rate, and a model handed over after its first frame is
 * seeded at draw time with that frame's stamp, so the next pan does not fold
 * the same frame a second time.
 */
void
test_one_fold_per_frame(void) {
    dsd_qt::SpectrumModel model;
    start_model(model, kBins / 2);

    dsd_qt::SpectrumTraceItem late;
    late.setModel(&model); /* after the frame: nothing announced yet */
    assert(!late.testRange().seeded);
    delete late.testUpdateGpuTree(nullptr, 400.0, 200.0, 1.0);
    assert(late.testRange().seeded);
    const double seeded_max = late.testRange().max_db;

    /* Pans: viewChanged, no new frame. */
    model.setZoom(2.0);
    model.setViewOffsetHz(1000.0);
    model.setViewOffsetHz(2000.0);
    assert(std::fabs(late.testRange().max_db - seeded_max) < 1e-12);

    /* A new frame with a stronger carrier folds once and moves the range. */
    g_bins[kBins / 2] = 0.0F;
    publish(kCenterHz);
    model.testPoll();
    const double after_frame = late.testRange().max_db;
    assert(after_frame > seeded_max);
    model.setViewOffsetHz(3000.0);
    assert(std::fabs(late.testRange().max_db - after_frame) < 1e-12);
    model.setActive(false);
}

/*
 * The items held a raw model pointer and drew from it on every repaint, so a
 * model destroyed first left both reading freed memory (ASan: use after free).
 * Now they forget it and the state that came from it.
 */
void
test_a_destroyed_model_is_forgotten(void) {
    auto* model = new dsd_qt::SpectrumModel;
    start_model(*model, kBins / 2);
    dsd_qt::SpectrumTraceItem trace;
    dsd_qt::WaterfallItem waterfall;
    trace.setModel(model);
    waterfall.setModel(model);
    waterfall.setSize(QSizeF(200.0, 100.0));
    int trace_changes = 0;
    int waterfall_changes = 0;
    QObject::connect(&trace, &dsd_qt::SpectrumTraceItem::modelChanged, &trace, [&]() { trace_changes++; });
    QObject::connect(&waterfall, &dsd_qt::WaterfallItem::modelChanged, &waterfall, [&]() { waterfall_changes++; });

    model->setActive(false);
    delete model;
    assert(trace.model() == nullptr && waterfall.model() == nullptr);
    assert(trace_changes == 1 && waterfall_changes == 1);
    assert(!trace.testRange().seeded);

    /* Both still draw: the trace's grid, the waterfall's (cleared) history. */
    QSGNode* root = trace.testUpdateGpuTree(nullptr, 200.0, 100.0, 1.0);
    assert(child_geometry(root, 0)->geometry()->vertexCount() > 0);
    assert(child_geometry(root, 2)->geometry()->vertexCount() == 0);
    delete root;
    QImage canvas(200, 100, QImage::Format_ARGB32_Premultiplied);
    QPainter painter(&canvas);
    waterfall.paint(&painter);
    painter.end();

    /* And a replacement model is taken up as usual. */
    dsd_qt::SpectrumModel replacement;
    start_model(replacement, kBins / 2);
    trace.setModel(&replacement);
    assert(trace.model() == &replacement && trace_changes == 2);
    replacement.setActive(false);
}

} // namespace

int
main(int argc, char** argv) {
    /* The waterfall is a QQuickItem, so it needs an application object; the
     * offscreen platform comes from the CTest registration. */
    QGuiApplication app(argc, argv);

    test_a_repeated_frame_is_not_a_new_frame();
    test_the_first_frame_after_a_retune_becomes_a_row();
    test_a_tap_never_snaps_outside_the_visible_window();
    test_a_hop_still_works_when_the_view_is_panned_off_center();
    test_production_follows_the_view();
    test_a_fractional_window_edge_admits_no_bin_outside_it();
    test_the_gpu_tree();
    test_a_bad_frame_draws_finite_geometry();
    test_the_software_picture();
    test_a_window_shows_the_trace();
    test_one_fold_per_frame();
    test_a_destroyed_model_is_forgotten();

    assert(g_get_calls > 0);
    (void)DSD_FPRINTF(stdout, "UI_QT_SPECTRUM_MODEL: ok\n");
    return 0;
}

// NOLINTEND(cert-dcl03-c,misc-static-assert)
