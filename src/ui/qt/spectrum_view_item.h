// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The two drawn halves of the spectrum view: live trace and waterfall.
 *
 * The trace is scene-graph geometry. It was a QQuickPaintedItem, and on a phone
 * QPainter's antialiased stroke of a thousand-point spiky polyline took 100-150
 * ms a frame (measured on the emulator with a real dongle; 75 ms of it the
 * stroke alone) — inside the scene graph's sync, while the GUI thread waits, so
 * a pan stuttered at about 8 frames a second. As triangles the stroke costs the
 * CPU a few thousand vertices; the GPU draws them, antialiased by a coverage
 * ramp across each edge (spectrum_trace_geometry.h).
 *
 * Qt's software scene graph cannot draw such geometry: it is what
 * QT_QUICK_BACKEND=software selects, and also what Qt falls back to by itself
 * on a platform with no RHI. There the trace paints itself with QPainter into
 * an image, as before.
 *
 * The waterfall stays a QQuickPaintedItem: its paint is one scaled image blit,
 * measured at 2-3 ms, and drawing it at its own size changed nothing.
 *
 * QML Canvas would marshal a thousand bins into JavaScript fifteen times a
 * second, which is the slow path on a phone. These are the first
 * qmlRegisterType'd items in the tree — every other C++ object QML sees is a
 * context property.
 *
 * Colors arrive as properties so the screen can bind them to Theme tokens; no
 * palette decisions are made here.
 */

#ifndef DSD_NEO_SRC_UI_QT_SPECTRUM_VIEW_ITEM_H_
#define DSD_NEO_SRC_UI_QT_SPECTRUM_VIEW_ITEM_H_

#include <QColor>
#include <QImage>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QQuickItem>
#include <QQuickPaintedItem>
#include <QtGlobal>
#include <qrgb.h>
#include <vector>

#include "spectrum_math.h"
#include "spectrum_model.h"
#include "spectrum_trace_geometry.h" // IWYU pragma: keep -- C++14 vector members need complete vertex types.

class QSGNode;

namespace dsd_qt {

/**
 * @brief The instantaneous spectrum, drawn as a filled trace over a faint grid.
 *
 * Content is clipped to the item, as a painted item's was: the stroke's caps,
 * bevels and fringes reach past the points they surround.
 */
class SpectrumTraceItem : public QQuickItem {
    Q_OBJECT
    Q_PROPERTY(dsd_qt::SpectrumModel* model READ model WRITE setModel NOTIFY modelChanged)
    Q_PROPERTY(QColor lineColor READ lineColor WRITE setLineColor NOTIFY colorsChanged)
    Q_PROPERTY(QColor areaColor READ areaColor WRITE setAreaColor NOTIFY colorsChanged)
    Q_PROPERTY(QColor gridColor READ gridColor WRITE setGridColor NOTIFY colorsChanged)

  public:
    explicit SpectrumTraceItem(QQuickItem* parent = nullptr);
    ~SpectrumTraceItem() override;

    SpectrumModel*
    model() const {
        return m_model.data();
    }

    void setModel(SpectrumModel* next);

    QColor
    lineColor() const {
        return m_line_color;
    }

    void setLineColor(const QColor& color);

    QColor
    areaColor() const {
        return m_area_color;
    }

    void setAreaColor(const QColor& color);

    QColor
    gridColor() const {
        return m_grid_color;
    }

    void setGridColor(const QColor& color);

#ifdef DSD_NEO_TEST_HOOKS
    /**
     * @brief The node tree updatePaintNode() returns on an RHI window, built without one.
     *
     * Headless Qt only has the software scene graph, so this is how a test sees
     * the geometry, materials and flags the GPU path hands Qt. Same rules as
     * updatePaintNode(): pass the previous result back as @p old.
     */
    QSGNode* testUpdateGpuTree(QSGNode* old, double width, double height, double pixel_ratio);

    /** @brief The software path's picture at @p pixel_ratio, as its image node would show it. */
    QImage testSoftwareImage(double width, double height, double pixel_ratio);

    /** @brief The size, in item coordinates, the software path's image node shows that picture at (from 0, 0). */
    void testSoftwareRectSize(double width, double height, double pixel_ratio, double* shown_width,
                              double* shown_height);

    /** @brief The auto-range, so a test can tell a fold from a repaint. */
    const spectrum_math::AutoRange&
    testRange() const {
        return m_range;
    }
#endif

  Q_SIGNALS:
    void modelChanged();
    void colorsChanged();

  protected:
    QSGNode* updatePaintNode(QSGNode* old, UpdatePaintNodeData* /*data*/) override;
    void geometryChange(const QRectF& new_geometry, const QRectF& old_geometry) override;
    void itemChange(ItemChange change, const ItemChangeData& value) override;

  private:
    void onFrame();
    void onRetuned();
    void onModelDestroyed();
    void forgetFrames();
    bool buildTrace(int w, int h);
    QSGNode* updateGpuTree(QSGNode* old, int w, int h, double pixel_ratio, bool have_trace);
    QImage paintSoftwareImage(int w, int h, double pixel_ratio, bool have_trace) const;

    QPointer<SpectrumModel> m_model;
    QColor m_line_color = QColor(0x22, 0xDC, 0xF5);
    QColor m_area_color = QColor(0x22, 0xDC, 0xF5, 0x33);
    QColor m_grid_color = QColor(0x23, 0x2B, 0x3A);
    spectrum_math::AutoRange m_range;
    /* All reused across frames: a trace this wide would otherwise allocate on
     * every sync, fifteen times a second. m_xs/m_ys are the trace's points, one
     * per pixel column; the stroke and area buffers are their triangles. */
    QVector<float> m_columns;
    std::vector<float> m_xs;
    std::vector<float> m_ys;
    std::vector<spectrum_geometry::StrokeVertex> m_stroke;
    std::vector<spectrum_geometry::AreaVertex> m_area;
    /* Which frame the auto-range last folded in. The fold is stateful (a 5%
     * relaxation per call), so it has to happen once per frame and not once per
     * repaint: viewChanged repaints at touch rate during a pan, which would
     * advance the range four times faster than the waterfall's copy of it. */
    quint64 m_last_frame_index = 0;
    bool m_have_frame = false;
};

/** @brief Scrolling history of the spectrum, newest row at the top. */
class WaterfallItem : public QQuickPaintedItem {
    Q_OBJECT
    Q_PROPERTY(dsd_qt::SpectrumModel* model READ model WRITE setModel NOTIFY modelChanged)
    Q_PROPERTY(QColor coldColor READ coldColor WRITE setColdColor NOTIFY colorsChanged)
    Q_PROPERTY(QColor midColor READ midColor WRITE setMidColor NOTIFY colorsChanged)
    Q_PROPERTY(QColor hotColor READ hotColor WRITE setHotColor NOTIFY colorsChanged)

  public:
    explicit WaterfallItem(QQuickItem* parent = nullptr);
    ~WaterfallItem() override;

    SpectrumModel*
    model() const {
        return m_model.data();
    }

    void setModel(SpectrumModel* next);

    QColor
    coldColor() const {
        return m_cold_color;
    }

    void setColdColor(const QColor& color);

    QColor
    midColor() const {
        return m_mid_color;
    }

    void setMidColor(const QColor& color);

    QColor
    hotColor() const {
        return m_hot_color;
    }

    void setHotColor(const QColor& color);

    void paint(QPainter* painter) override;

#ifdef DSD_NEO_TEST_HOOKS
    /**
     * @brief Rows written since the last history clear.
     *
     * The history is a private image, so this is the only way a test can say
     * whether a frame became a row or was dropped — which is the difference
     * between a waterfall and a waterfall missing the moment it retuned.
     */
    quint64
    testRowsWritten() const {
        return m_rows_written;
    }
#endif

  Q_SIGNALS:
    void modelChanged();
    void colorsChanged();

  private:
    void onFrame();
    void onModelDestroyed();
    void clearHistory();
    void rebuildRamp();
    QRgb rampColor(double t) const;

    QPointer<SpectrumModel> m_model;
    QColor m_cold_color = QColor(0x12, 0x16, 0x1E);
    QColor m_mid_color = QColor(0x22, 0xDC, 0xF5);
    QColor m_hot_color = QColor(0xEC, 0x1F, 0xDC);
    /* The cold-mid-hot ramp evaluated once per palette change rather than once
     * per bin. Every row is kWaterfallWidth bins and rows arrive at the frame
     * rate, so the six-channel interpolation was running tens of thousands of
     * times a second — on a phone — to produce colors drawn from a set this
     * small. 256 steps is finer than the 8-bit channels can show. */
    static constexpr int kRampSteps = 256;
    QRgb m_ramp[kRampSteps] = {};
    spectrum_math::AutoRange m_range;
    /* Preallocated once. Rows are always written full-span, so zoom and pan are
     * a source rectangle at paint time and old rows stay correct under both;
     * only a retune can invalidate them. */
    QImage m_image;
    /* One row's worth of columns, reused rather than allocated per frame. */
    QVector<float> m_columns;
    int m_cursor = 0;
    /* Which frame the newest row was drawn from, and whether there is one at
     * all. The flag is what keeps clearHistory() from claiming the frame that
     * is about to arrive: a retune bumps the model's index before the new frame
     * is announced, so recording that index here would swallow the first row at
     * the new center — the one row that is certain to be worth seeing. */
    quint64 m_last_frame_index = 0;
    bool m_have_row = false;
    quint64 m_rows_written = 0;
};

} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_SPECTRUM_VIEW_ITEM_H_ */
