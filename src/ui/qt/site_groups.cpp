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

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kRadians = kPi / 180.;

// The great-circle distance on a sphere of the mean Earth radius: the fallback where Vincenty does not converge.
double
haversine_km(double lat, double lon, double siteLat, double siteLon) {
    const double a = std::sin((siteLat - lat) * kRadians / 2);
    const double b = std::sin((siteLon - lon) * kRadians / 2);
    const double h =
        std::max(0., std::min(1., a * a + std::cos(lat * kRadians) * std::cos(siteLat * kRadians) * b * b));
    return 6371.0088 * 2 * std::atan2(std::sqrt(h), std::sqrt(1 - h));
}

// The WGS84 ellipsoid.
constexpr double kWgs84A = 6378137.0;
constexpr double kWgs84F = 1 / 298.257223563;
constexpr double kWgs84B = (1 - kWgs84F) * kWgs84A;

// One iteration of Vincenty's inverse on the auxiliary sphere; the converged one gives the length.
struct VincentyPoint {
    double sinSigma = 0;
    double cosSigma = 0;
    double sigma = 0;
    double cosSqAlpha = 0;
    double cos2SigmaM = 0;
};

double
vincenty_length_km(const VincentyPoint& p) {
    const double uSq = p.cosSqAlpha * (kWgs84A * kWgs84A - kWgs84B * kWgs84B) / (kWgs84B * kWgs84B);
    const double bigA = 1 + uSq / 16384 * (4096 + uSq * (-768 + uSq * (320 - 175 * uSq)));
    const double bigB = uSq / 1024 * (256 + uSq * (-128 + uSq * (74 - 47 * uSq)));
    const double c2m = p.cos2SigmaM;
    const double deltaSigma =
        bigB * p.sinSigma
        * (c2m
           + bigB / 4
                 * (p.cosSigma * (-1 + 2 * c2m * c2m)
                    - bigB / 6 * c2m * (-3 + 4 * p.sinSigma * p.sinSigma) * (-3 + 4 * c2m * c2m)));
    return kWgs84B * bigA * (p.sigma - deltaSigma) / 1000;
}

/*
 * Vincenty's inverse on WGS84 into @p km; false where it does not converge (near-antipodal points). Convergence is an
 * absolute test on lambda, so a meridian (a zero longitude difference) needs no division by it.
 */
bool
vincenty_km(double lat, double lon, double siteLat, double siteLon, double& km) {
    double lonDiff = (siteLon - lon) * kRadians; // across the dateline the short way
    if (lonDiff > kPi) {
        lonDiff -= 2 * kPi;
    } else if (lonDiff < -kPi) {
        lonDiff += 2 * kPi;
    }
    const double tanU1 = (1 - kWgs84F) * std::tan(lat * kRadians);
    const double cosU1 = 1 / std::sqrt(1 + tanU1 * tanU1);
    const double sinU1 = tanU1 * cosU1;
    const double tanU2 = (1 - kWgs84F) * std::tan(siteLat * kRadians);
    const double cosU2 = 1 / std::sqrt(1 + tanU2 * tanU2);
    const double sinU2 = tanU2 * cosU2;
    double lambda = lonDiff;
    for (int iteration = 0; iteration < 200; ++iteration) {
        const double sinLambda = std::sin(lambda);
        const double cosLambda = std::cos(lambda);
        const double x = cosU2 * sinLambda;
        const double y = cosU1 * sinU2 - sinU1 * cosU2 * cosLambda;
        VincentyPoint p;
        p.sinSigma = std::sqrt(x * x + y * y);
        p.cosSigma = sinU1 * sinU2 + cosU1 * cosU2 * cosLambda;
        if (!(p.sinSigma > 0)) {
            km = 0;
            return p.cosSigma > 0; // the same point; else exactly antipodal
        }
        p.sigma = std::atan2(p.sinSigma, p.cosSigma);
        const double sinAlpha = cosU1 * cosU2 * sinLambda / p.sinSigma;
        p.cosSqAlpha = 1 - sinAlpha * sinAlpha;
        p.cos2SigmaM = p.cosSqAlpha > 0 ? p.cosSigma - 2 * sinU1 * sinU2 / p.cosSqAlpha : 0; // 0 on the equator
        const double c = kWgs84F / 16 * p.cosSqAlpha * (4 + kWgs84F * (4 - 3 * p.cosSqAlpha));
        const double previous = lambda;
        lambda = lonDiff
                 + (1 - c) * kWgs84F * sinAlpha
                       * (p.sigma
                          + c * p.sinSigma * (p.cos2SigmaM + c * p.cosSigma * (-1 + 2 * p.cos2SigmaM * p.cos2SigmaM)));
        if (std::fabs(lambda - previous) < 1e-12) {
            km = vincenty_length_km(p);
            return true;
        }
    }
    return false;
}
} // namespace

double
site_distance_km(double lat, double lon, double siteLat, double siteLon) {
    if (!site_position_valid(lat, lon) || !site_position_valid(siteLat, siteLon)) {
        return -1;
    }
    double km = 0;
    return vincenty_km(lat, lon, siteLat, siteLon, km) ? km : haversine_km(lat, lon, siteLat, siteLon);
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
    QString name = lead.value(QStringLiteral("name")).toString();
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
    double best = 0;
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
        if (distance >= 0 && (nearest < 0 || distance < best)) {
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
