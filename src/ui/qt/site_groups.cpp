// SPDX-License-Identifier: GPL-3.0-or-later
#include "site_groups.h"
#include <QCoreApplication>
#include <QEvent>
#include <QVariantMap>
#include <algorithm>
#include <cmath>
#include <limits>

namespace dsd_qt {
SiteInteractionGuard::SiteInteractionGuard(QObject* parent) : QObject(parent) {
    QCoreApplication::instance()->installEventFilter(this);
}

qint64
SiteInteractionGuard::nextLocationRequestId() {
    // RR browse uses positive IDs. Keep site requests distinct across sheet recreation.
    static qint64 nextId = 0;
    return --nextId;
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

QVariantList
site_sibling_rows(const QVariantList& rows, int row) {
    QVariantList result;
    if (row < 0 || row >= rows.size()) {
        return result;
    }
    const auto selected = rows[row].toMap();
    const int sid = selected.value("rrSid").toInt();
    if (sid <= 0 || selected.value("rrSiteId").toInt() <= 0) {
        return result;
    }
    for (int i = 0; i < rows.size(); ++i) {
        const auto candidate = rows[i].toMap();
        if (candidate.value("rrSid").toInt() == sid && candidate.value("rrSiteId").toInt() > 0) {
            result.append(i);
        }
    }
    return result;
}

int
site_nearest_row(const QVariantList& rows, int row, double lat, double lon) {
    int nearest = -1;
    double best = std::numeric_limits<double>::infinity();
    for (const auto& index : site_sibling_rows(rows, row)) {
        const auto candidate = rows[index.toInt()].toMap();
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
} // namespace dsd_qt
