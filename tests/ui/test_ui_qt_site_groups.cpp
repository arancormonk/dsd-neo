// SPDX-License-Identifier: GPL-3.0-or-later
#include <QCoreApplication>
#include <QDir>
#include <QList>
#include <QMap>
#include <QStandardPaths>
#include <QString>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>
#include <cmath>
#include <cstdio>
#include <limits>
#include <utility>
#include "../test_support/qt_test_paths.h"
#include "saved_systems_model.h"
#include "site_groups.h"

namespace {

int g_failures = 0;

void
check(bool ok, const char* label) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", label);
        ++g_failures;
    }
}

/* One RadioReference system saved for several receivers is one group per receiver, as a scan list tells its
 * receivers apart: a USB dongle, an rtl_tcp server by host (any case, blanks trimmed) and port, an Airspy by serial. */
void
check_receiver_groups() {
    dsd_qt::SavedSystemsModel model;
    while (model.count() > 0 && model.remove(0)) {}
    const auto site = [&model](int siteId, double lat, const QVariantMap& receiver) {
        QVariantMap row{
            {"name", "SARA"}, {"rrSid", 20},      {"rrSiteId", siteId}, {"siteName", QString::number(siteId)},
            {"siteLat", lat}, {"siteLon", -91.0}, {"hasSitePos", true}, {"freqMhz", QString::number(850 + siteId)}};
        for (auto it = receiver.cbegin(); it != receiver.cend(); ++it) {
            row.insert(it.key(), it.value());
        }
        return model.add(row);
    };
    const QVariantMap usb{{"sourceType", "usb"}};
    // Rows 0..5: USB, rtl_tcp "A " and "a" on one port, Airspy B1 and B2, USB again.
    check(site(1, 42.0, usb) && site(2, 41.1, {{"sourceType", "rtltcp"}, {"host", "A "}, {"port", 1234}})
              && site(3, 40.0, {{"sourceType", "rtltcp"}, {"host", "a"}, {"port", 1234}})
              && site(4, 41.0, {{"sourceType", "airspy"}, {"airspy", QVariantMap{{"serial", "B1"}}}})
              && site(5, 41.0, {{"sourceType", "airspy"}, {"airspy", QVariantMap{{"serial", "B2"}}}})
              && site(6, 41.5, usb),
          "receivers: add the sites");
    check(model.siblingRows(0) == QVariantList{0, 5} && model.siteCount(5) == 2,
          "receivers: a USB site groups with USB sites only");
    check(model.siblingRows(1) == QVariantList{1, 2} && model.siblingRows(2) == QVariantList{1, 2},
          "receivers: one rtl_tcp server, its host in any case and blanks trimmed");
    check(model.siblingRows(3) == QVariantList{3} && model.siblingRows(4) == QVariantList{4},
          "receivers: two Airspy serials are two groups");
    // The rtl_tcp site at 41.1 is the closest to the fix; the USB group's nearest is still its own.
    check(model.nearestRow(0, 41.1, -91) == 5, "receivers: nearest never crosses to another receiver");
    check(model.nearestRow(3, 41.1, -91) == 3, "receivers: nearest of a one-site group is itself");
    QVariantList maps;
    for (int row = 0; row < model.count(); ++row) {
        maps.append(model.get(row));
    }
    check(dsd_qt::site_sibling_rows(maps, 0) == model.siblingRows(0)
              && dsd_qt::site_sibling_rows(maps, 1) == model.siblingRows(1)
              && dsd_qt::site_nearest_row(maps, 0, 41.1, -91) == 5,
          "receivers: the field-map helpers group as the model does");
    // The model keys a row from its fields as site_group_key() keys its field map.
    check(model.add({{"name", "Manual"}, {"sourceType", "file"}, {"filePath", "/x.wav"}})
              && model.add({{"name", "Replay"},
                            {"rrSid", 20},
                            {"rrSiteId", 7},
                            {"sourceType", "file"},
                            {"filePath", "/x/Cap.wav"}})
              && model.add({{"name", "UDP"}, {"rrSid", 20}, {"rrSiteId", 8}, {"sourceType", "udp"}, {"port", 7355}}),
          "receivers: add rows of other sources");
    bool same = model.groupKey(-1).isEmpty() && model.groupKey(model.count()).isEmpty();
    for (int row = 0; row < model.count(); ++row) {
        same = same && model.groupKey(row) == dsd_qt::site_group_key(model.get(row));
    }
    check(same && model.groupKey(6).isEmpty() && model.groupKey(7) == "20|file|/x/Cap.wav"
              && model.groupKey(8) == "20|udp|7355",
          "receivers: the model's key is site_group_key() of its row");
    while (model.count() > 0 && model.remove(0)) {}
}

/* The receiver and group keys as written, and a group's title. */
void
check_keys_and_titles() {
    using dsd_qt::receiver_identity;
    using dsd_qt::site_group_key;
    using dsd_qt::site_group_title;
    check(receiver_identity({{"sourceType", "usb"}, {"host", "x"}, {"port", 1}}) == "usb", "receiver: USB");
    check(receiver_identity({{"sourceType", "rtltcp"}, {"host", " Radio.Lan "}, {"port", 1234}})
              == "rtltcp|radio.lan:1234",
          "receiver: rtl_tcp host and port");
    check(receiver_identity({{"sourceType", "tcp"}, {"host", "h"}, {"port", "7355"}}) == "tcp|h:7355",
          "receiver: TCP audio, a port stored as text");
    check(receiver_identity({{"sourceType", "udp"}, {"port", 7355}}) == "udp|7355", "receiver: UDP by port");
    check(receiver_identity({{"sourceType", "airspy"}, {"airspy", QVariantMap{{"serial", "a1b2"}}}}) == "airspy|A1B2",
          "receiver: Airspy by serial, any case");
    check(receiver_identity({{"sourceType", "file"}, {"filePath", "/x/Cap.wav"}}) == "file|/x/Cap.wav",
          "receiver: a file by its path, case kept");
    const QVariantMap usbSite{{"sourceType", "usb"}, {"rrSid", 20}, {"rrSiteId", 1}};
    check(site_group_key(usbSite) == "20|usb", "group key: system then receiver");
    QVariantMap noSite = usbSite;
    noSite["rrSiteId"] = 0;
    QVariantMap noSystem = usbSite;
    noSystem["rrSid"] = 0;
    check(site_group_key(noSite).isEmpty() && site_group_key(noSystem).isEmpty(), "group key: none without both ids");

    const auto title = [](const QString& name, const QString& siteName) {
        return site_group_title({{"name", name}, {"siteName", siteName}});
    };
    check(title(QStringLiteral("SARA — Johnson Simulcast"), "Johnson Simulcast") == "SARA",
          "title: the generated site suffix goes");
    check(title("My SARA", "Johnson Simulcast") == "My SARA", "title: a custom name stays");
    check(title(QStringLiteral("SARA — Polk"), "Johnson Simulcast") == QStringLiteral("SARA — Polk"),
          "title: only the lead's own site name");
    check(title(QStringLiteral("SARA - Johnson"), "Johnson") == QStringLiteral("SARA - Johnson")
              && title(QStringLiteral("SARA — Johnson "), "Johnson") == QStringLiteral("SARA — Johnson "),
          "title: only the exact separator, at the very end");
    check(title(QStringLiteral("SARA — "), "") == QStringLiteral("SARA — "), "title: no site name, no cut");
    check(title(QStringLiteral(" — Johnson"), "Johnson") == QStringLiteral(" — Johnson"),
          "title: never cut to nothing");
}

/*
 * Reference geodesic distances on WGS84 (GeographicLib, Karney's algorithm), generated once and pinned here, in km.
 * Each is the distance from the first point to the second.
 */
struct GeodesicCase {
    const char* label;
    double lat;
    double lon;
    double siteLat;
    double siteLon;
    double km;
};

constexpr double kMillimeterKm = 1e-6;

constexpr GeodesicCase kGeodesicCases[] = {
    {"WGS84 meridian", 10, 20, 11, 20, 110.611186562},
    {"WGS84 equator", 0, 20, 0, 21, 111.319490793},
    {"WGS84 coincident", 10, 20, 10, 20, 0},
    {"WGS84 at the pole", 90, 0, 89, 20, 111.693864914},
    {"WGS84 over the pole", 89.5, 20, 89.5, -160, 111.693950897},
    {"WGS84 near the pole", 89.2, 20, 89.6, 110, 99.901427378},
    {"WGS84 near the south pole", -90, 20, -89.5, -100, 55.846975448},
    {"WGS84 across the dateline", 10, 179.6, 10.2, -179.7, 79.849410112},
    {"WGS84 from the dateline", 10, 180, 10, -179.5, 54.819676789},
    {"WGS84 reversal east", 0, 30, 0, 30.896, 99.742263751},
    {"WGS84 reversal north", 0, 30, 0.9, 30, 99.516930425},
    // Near-antipodal, but it converges (after 182 iterations): still the ellipsoid, not the fallback.
    {"WGS84 near-antipodal converging", 0, 10, 0.5, -170.5, 19936.288578965},
};

void
check_geodesy() {
    using dsd_qt::site_distance_km;
    for (const GeodesicCase& c : kGeodesicCases) {
        check(std::abs(site_distance_km(c.lat, c.lon, c.siteLat, c.siteLon) - c.km) <= kMillimeterKm, c.label);
        check(std::abs(site_distance_km(c.siteLat, c.siteLon, c.lat, c.lon) - c.km) <= kMillimeterKm, c.label);
    }
    /* Vincenty does not converge here: the spherical distance (R = 6371.0088 km) stands in, within the sphere's error
     * of the geodesic (GeographicLib: 19944.127420750 km; exactly antipodal: 20003.931458625 km). */
    const double nearAntipodal = site_distance_km(0, 10, 0.5, -170.3);
    check(std::abs(nearAntipodal - 19950.277343497) <= kMillimeterKm
              && std::abs(nearAntipodal - 19944.127420750) <= 19944.127420750 * 0.006,
          "near-antipodal falls back to the sphere");
    const double antipodal = site_distance_km(10, 20, -10, -160);
    check(std::abs(antipodal - 20015.114307777) <= kMillimeterKm
              && std::abs(antipodal - 20003.931458625) <= 20003.931458625 * 0.006,
          "exactly antipodal falls back to the sphere");
    /* The sphere ranks (0, 30.896) first from (0, 30), at 99.631 km against 100.076 km; on the ellipsoid (0.9, 30) is
     * nearer. */
    const QVariantList indices{0, 1};
    const QList<QVariantMap> sites{
        {{"siteLat", 0.0}, {"siteLon", 30.896}, {"hasSitePos", true}},
        {{"siteLat", 0.9}, {"siteLon", 30.0}, {"hasSitePos", true}},
    };
    check(dsd_qt::site_nearest_among(
              indices, [&sites](int index) { return sites.at(index); }, 0, 30)
              == 1,
          "WGS84 orders the nearer site first");
}

} // namespace

int
main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    app.setOrganizationName("dsd-neo-test");
    app.setApplicationName(QString("site-groups-%1").arg(app.applicationPid()));
    dsd_test_qt_isolate_paths();
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir(dir).removeRecursively();
    using namespace dsd_qt;
    check_geodesy();
    check(site_distance_km(0, 0, 42, -91) < 0, "missing coordinate sentinel");
    check(site_distance_km(91, 0, 42, -91) < 0, "latitude range");
    check(site_distance_km(41, std::numeric_limits<double>::quiet_NaN(), 42, -91) < 0, "finite coordinates");
    SavedSystemsModel model;
    QVariantMap north{{"name", "North"}, {"rrSid", 12},      {"rrSiteId", 16863},  {"siteName", "North"},
                      {"siteLat", 42.0}, {"siteLon", -91.0}, {"hasSitePos", true}, {"freqMhz", "851"}};
    model.add(north);
    auto south = north;
    south["rrSiteId"] = 48391;
    south["siteLat"] = 41.;
    model.add(south);
    model.add({{"name", "Manual"}});
    model.add({{"rrSid", 12}, {"name", "Legacy"}});
    check(model.siteCount(0) == 2 && model.siblingRows(2).isEmpty() && model.siblingRows(3).isEmpty(),
          "only site provenance groups");
    check(model.nearestRow(0, 41.1, -91) == 1, "nearest sibling");
    model.setAvoidSite(1, true);
    check(model.nearestRow(0, 41.1, -91) == 0, "avoided site skipped");
    SavedSystemsModel reload;
    check(reload.get(1).value("avoidSite").toBool() && reload.siteCount(0) == 2, "avoid and grouping persist");
    model.update(0, {{"hasSitePos", false}});
    check(model.nearestRow(0, 41.1, -91) == -1, "no eligible site");
    model.update(1, {{"name", "Rename"}});
    check(model.siteCount(1) == 2, "rename retains provenance");
    model.update(1, {{"freqMhz", "852"}});
    check(model.get(1).value("rrSid").toInt() == 0 && model.get(1).value("rrSiteId").toInt() == 0,
          "tuning edit clears provenance");

    const QVariantMap cleared = rr_provenance_cleared(north);
    check(cleared.value("rrSid").toInt() == 0 && cleared.value("rrSiteId").toInt() == 0
              && !cleared.value("avoidSite").toBool() && cleared.value("siteName").toString().isEmpty()
              && std::abs(cleared.value("siteLat").toDouble()) < 1e-9
              && std::abs(cleared.value("siteLon").toDouble()) < 1e-9 && !cleared.value("hasSitePos").toBool()
              && cleared.contains("place") && cleared.value("place").toMap().isEmpty(),
          "provenance cleared: every RadioReference key, present and empty");
    check(cleared.value("name") == "North" && cleared.value("freqMhz") == "851", "provenance cleared: the rest kept");
    check(site_generated_name(QStringLiteral("SARA — Old"), "Old", "New") == QStringLiteral("SARA — New"),
          "generated name follows its site");
    check(site_generated_name(QStringLiteral("My SARA"), "Old", "New") == QStringLiteral("My SARA"),
          "a custom name stays");
    check(site_generated_name(QStringLiteral("SARA Old"), "Old", "New") == QStringLiteral("SARA Old"),
          "only the generated separator counts");
    check(site_generated_name(QStringLiteral("SARA — Old"), "", "New") == QStringLiteral("SARA — Old")
              && site_generated_name(QStringLiteral("SARA — Old"), "Old", "") == QStringLiteral("SARA — Old"),
          "an empty site name renames nothing");

    check_receiver_groups();
    check_keys_and_titles();
    QDir(dir).removeRecursively();
    return g_failures ? 1 : 0;
}
