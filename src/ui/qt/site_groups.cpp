// SPDX-License-Identifier: GPL-3.0-or-later
#include <QCoreApplication>
#include <QEvent>
#include <QLatin1Char>
#include <QLatin1String>
#include <QList>
#include <QMap>
#include <QString>
#include <QVariant>
#include <QVariantMap>
#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include "site_groups.h"
#include "stored_int.h"

namespace dsd_qt {
SiteInteractionGuard::SiteInteractionGuard(QObject* parent) : QObject(parent) {
    QCoreApplication::instance()->installEventFilter(this);
}

bool
SiteInteractionGuard::eventFilter(QObject* watched, QEvent* event) {
    switch (event->type()) {
        case QEvent::MouseButtonPress:
        case QEvent::TouchBegin:
        case QEvent::KeyPress:
        case QEvent::Wheel:
        case QEvent::Shortcut: Q_EMIT interaction(); break;
        default: break;
    }
    return QObject::eventFilter(watched, event);
}

bool
site_position_valid(double lat, double lon) {
    return std::isfinite(lat) && std::isfinite(lon) && lat >= -90 && lat <= 90 && lon >= -180 && lon <= 180
           && !(lat == 0 && lon == 0);
}

double
site_distance_km(double lat, double lon, double siteLat, double siteLon) {
    if (!site_position_valid(lat, lon) || !site_position_valid(siteLat, siteLon)) {
        return -1;
    }
    constexpr double radians = 3.14159265358979323846 / 180.;
    const double a = std::sin((siteLat - lat) * radians / 2);
    const double b = std::sin((siteLon - lon) * radians / 2);
    const double h = std::max(0., std::min(1., a * a + std::cos(lat * radians) * std::cos(siteLat * radians) * b * b));
    return 6371.0088 * 2 * std::atan2(std::sqrt(h), std::sqrt(1 - h));
}

QString
receiver_identity(const QString& source, const QString& host, int port, const QString& airspySerial,
                  const QString& filePath) {
    if (source == QLatin1String("airspy")) {
        return source + QLatin1Char('|') + airspySerial.toUpper();
    }
    if (source == QLatin1String("rtltcp") || source == QLatin1String("tcp")) {
        return source + QLatin1Char('|') + host.trimmed().toLower() + QLatin1Char(':') + QString::number(port);
    }
    if (source == QLatin1String("udp")) {
        return source + QLatin1Char('|') + QString::number(port);
    }
    return source == QLatin1String("file") ? source + QLatin1Char('|') + filePath : source;
}

QString
receiver_identity(const QVariantMap& system) {
    return receiver_identity(system.value(QStringLiteral("sourceType")).toString(),
                             system.value(QStringLiteral("host")).toString(),
                             stored_int_or(system.value(QStringLiteral("port")), 0),
                             system.value(QStringLiteral("airspy")).toMap().value(QStringLiteral("serial")).toString(),
                             system.value(QStringLiteral("filePath")).toString());
}

bool
site_grouped(int rrSid, int rrSiteId) {
    return rrSid > 0 && rrSiteId > 0;
}

QString
site_group_key(int rrSid, const QString& receiver) {
    return QString::number(rrSid) + QLatin1Char('|') + receiver;
}

QString
site_group_key(const QVariantMap& row) {
    const int sid = row.value(QStringLiteral("rrSid")).toInt();
    if (!site_grouped(sid, row.value(QStringLiteral("rrSiteId")).toInt())) {
        return QString();
    }
    return site_group_key(sid, receiver_identity(row));
}

QString
site_group_title(const QVariantMap& lead) {
    const QString name = lead.value(QStringLiteral("name")).toString();
    const QString siteName = lead.value(QStringLiteral("siteName")).toString();
    const QString suffix = QStringLiteral(" — ") + siteName;
    if (siteName.isEmpty() || name.size() <= suffix.size() || !name.endsWith(suffix)) {
        return name;
    }
    return name.left(name.size() - suffix.size());
}

QVariantList
site_sibling_rows(const QVariantList& rows, int row) {
    QVariantList result;
    if (row < 0 || row >= rows.size()) {
        return result;
    }
    const QString key = site_group_key(rows[row].toMap());
    if (key.isEmpty()) {
        return result;
    }
    for (int i = 0; i < rows.size(); ++i) {
        if (site_group_key(rows[i].toMap()) == key) {
            result.append(i);
        }
    }
    return result;
}

int
site_nearest_row(const QVariantList& rows, int row, double lat, double lon) {
    return site_nearest_among(
        site_sibling_rows(rows, row), [&rows](int index) { return rows[index].toMap(); }, lat, lon);
}

int
site_nearest_among(const QVariantList& indices, const std::function<QVariantMap(int)>& rowAt, double lat, double lon) {
    int nearest = -1;
    double best = std::numeric_limits<double>::infinity();
    for (const auto& index : indices) {
        const auto candidate = rowAt(index.toInt());
        if (candidate.value("avoidSite").toBool() || !candidate.value("hasSitePos").toBool()) {
            continue;
        }
        bool latOk = false;
        bool lonOk = false;
        const double siteLat = candidate.value("siteLat").toDouble(&latOk);
        const double siteLon = candidate.value("siteLon").toDouble(&lonOk);
        if (!latOk || !lonOk) {
            continue;
        }
        const double distance = site_distance_km(lat, lon, siteLat, siteLon);
        if (distance >= 0 && distance < best) {
            nearest = index.toInt();
            best = distance;
        }
    }
    return nearest;
}

QVariantMap
rr_provenance_cleared(const QVariantMap& row) {
    QVariantMap cleared = row;
    cleared.insert(QStringLiteral("rrSid"), 0);
    cleared.insert(QStringLiteral("rrSiteId"), 0);
    // Only the group's site chooser lifts an avoid; a site out of its group would stay avoided for good.
    cleared.insert(QStringLiteral("avoidSite"), false);
    cleared.insert(QStringLiteral("siteName"), QString());
    cleared.insert(QStringLiteral("siteLat"), 0.0);
    cleared.insert(QStringLiteral("siteLon"), 0.0);
    cleared.insert(QStringLiteral("hasSitePos"), false);
    cleared.insert(QStringLiteral("place"), QVariantMap());
    return cleared;
}

QString
site_generated_name(const QString& name, const QString& oldSite, const QString& newSite) {
    const QString separator = QStringLiteral(" — ");
    if (oldSite.isEmpty() || newSite.isEmpty() || !name.endsWith(separator + oldSite)) {
        return name;
    }
    return name.left(name.size() - oldSite.size()) + newSite;
}
} // namespace dsd_qt
