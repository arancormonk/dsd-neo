// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The two drawn halves of the spectrum view: live trace and waterfall.
 *
 * QQuickPaintedItem rather than QML Canvas or a scene-graph node. Canvas would
 * marshal a thousand bins into JavaScript fifteen times a second, which is the
 * slow path on a phone; a QSGNode would be faster still but is a great deal
 * more code for a workload that is one image blit and one polyline per frame.
 * These are the first qmlRegisterType'd items in the tree — every other C++
 * object QML sees is a context property.
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
#include <QPointF>
#include <QQuickPaintedItem>
#include <QtGlobal>
#include <qrgb.h>

#include "spectrum_math.h"

class QQuickItem;

namespace dsd_qt {

class SpectrumModel;

/** @brief The instantaneous spectrum, drawn as a filled trace over a faint grid. */
class SpectrumTraceItem : public QQuickPaintedItem {
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
        return m_model;
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

    void paint(QPainter* painter) override;

  Q_SIGNALS:
    void modelChanged();
    void colorsChanged();

  private:
    void onFrame();
    void onRetuned();

    SpectrumModel* m_model = nullptr;
    QColor m_line_color = QColor(0x22, 0xDC, 0xF5);
    QColor m_area_color = QColor(0x22, 0xDC, 0xF5, 0x33);
    QColor m_grid_color = QColor(0x23, 0x2B, 0x3A);
    spectrum_math::AutoRange m_range;
    /* Reused across frames: a polyline this wide would otherwise allocate on
     * every repaint. */
    QVector<QPointF> m_points;
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
        return m_model;
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

  Q_SIGNALS:
    void modelChanged();
    void colorsChanged();

  private:
    void onFrame();
    void clearHistory();
    QRgb rampColor(double t) const;

    SpectrumModel* m_model = nullptr;
    QColor m_cold_color = QColor(0x12, 0x16, 0x1E);
    QColor m_mid_color = QColor(0x22, 0xDC, 0xF5);
    QColor m_hot_color = QColor(0xEC, 0x1F, 0xDC);
    spectrum_math::AutoRange m_range;
    /* Preallocated once. Rows are always written full-span, so zoom and pan are
     * a source rectangle at paint time and old rows stay correct under both;
     * only a retune can invalidate them. */
    QImage m_image;
    int m_cursor = 0;
    quint64 m_last_frame_index = 0;
};

} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_SPECTRUM_VIEW_ITEM_H_ */
