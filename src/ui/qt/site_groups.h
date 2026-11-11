// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef DSD_NEO_UI_QT_SITE_GROUPS_H_
#define DSD_NEO_UI_QT_SITE_GROUPS_H_
#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <functional>

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
/*
 * The receiver a saved system or scan list runs on, as its argv names it: the source, then the device the source opens
 * (an Airspy by its serial, rtl_tcp and TCP audio by host and port, UDP audio by its port, a file by its path). A host
 * is matched without case or the blanks the argv trims. Scan lists join on it, and site groups split by it.
 */
QString receiver_identity(const QVariantMap& system);
// The same from the fields themselves (sourceType, host, port, the Airspy serial, filePath), for a caller holding them.
QString receiver_identity(const QString& source, const QString& host, int port, const QString& airspySerial,
                          const QString& filePath);
// Whether a row with these ids is a RadioReference site, and so in a site group.
bool site_grouped(int rrSid, int rrSiteId);
/*
 * The site group @p row belongs to, "<rrSid>|<receiver_identity()>" for a RadioReference site (site_grouped()), else
 * empty: a system saved for two receivers is two groups. Every view of a group (siblings, nearest, site count, cards and
 * pins) groups by this key.
 */
QString site_group_key(const QVariantMap& row);
// The key of a grouped row (site_grouped()) of system @p rrSid on @p receiver (receiver_identity()).
QString site_group_key(int rrSid, const QString& receiver);
/*
 * A group's name, from its lead row: the name without the " — <siteName>" an import generated, else the name as it is.
 * The card and its pin both show it.
 */
QString site_group_title(const QVariantMap& lead);
// The rows of @p row's group (site_group_key()) in row order; empty for a row in no group.
QVariantList site_sibling_rows(const QVariantList& rows, int row);
int site_nearest_row(const QVariantList& rows, int row, double lat, double lon);
/*
 * The row of @p indices nearest to @p lat, @p lon that is not avoided and has a valid position, reading each row's
 * fields through @p rowAt; -1 when none can be ranked.
 */
int site_nearest_among(const QVariantList& indices, const std::function<QVariantMap(int)>& rowAt, double lat,
                       double lon);
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
