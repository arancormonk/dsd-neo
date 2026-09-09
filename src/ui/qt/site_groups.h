// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef DSD_NEO_UI_QT_SITE_GROUPS_H_
#define DSD_NEO_UI_QT_SITE_GROUPS_H_
#include <QObject>
#include <QVariantList>

namespace dsd_qt {
// Observe input without consuming it, so any intervening gesture cancels a queued restart.
class SiteInteractionGuard : public QObject {
    Q_OBJECT
  public:
    explicit SiteInteractionGuard(QObject* parent = nullptr);
    Q_INVOKABLE qint64 nextLocationRequestId();
  Q_SIGNALS:
    void interaction();

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
};

bool site_position_valid(double lat, double lon);
// Negative means unavailable. All distances are kilometers.
double site_distance_km(double lat, double lon, double siteLat, double siteLon);
QVariantList site_sibling_rows(const QVariantList& rows, int row);
int site_nearest_row(const QVariantList& rows, int row, double lat, double lon);
} // namespace dsd_qt
#endif
