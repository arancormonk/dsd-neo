// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef DSD_NEO_UI_QT_SITE_GROUPS_H_
#define DSD_NEO_UI_QT_SITE_GROUPS_H_
#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

namespace dsd_qt {
// Observe input without consuming it, so any intervening gesture cancels a queued restart.
class SiteInteractionGuard : public QObject {
    Q_OBJECT
  public:
    explicit SiteInteractionGuard(QObject* parent = nullptr);
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
/*
 * @p row with every piece of RadioReference provenance and geography cleared: rrSid, rrSiteId, avoidSite, siteName,
 * siteLat, siteLon, hasSitePos and place. The one rule for a row whose tuning, files or library file no longer
 * describe the site it was imported from.
 */
QVariantMap rr_provenance_cleared(const QVariantMap& row);
/*
 * A name generated from a site, "base — oldSite", renamed to "base — newSite"; any other name, or a site name
 * that is empty on either side, comes back as it is.
 */
QString site_generated_name(const QString& name, const QString& oldSite, const QString& newSite);
} // namespace dsd_qt
#endif
