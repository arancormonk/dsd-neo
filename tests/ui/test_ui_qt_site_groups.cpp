// SPDX-License-Identifier: GPL-3.0-or-later
#include <QCoreApplication>
#include <QDir>
#include <QList>
#include <QMap>
#include <QStandardPaths>
#include <QString>
#include <QVariant>
#include <QVariantMap>
#include <cmath>
#include <cstdio>
#include <limits>
#include <utility>
#include "../test_support/qt_test_paths.h"
#include "saved_systems_model.h"
#include "site_groups.h"

namespace {

/* B4: one RadioReference system saved for several receivers is one group per receiver, as a scan list tells its
 * receivers apart: a USB dongle, an rtl_tcp server by host (any case, blanks trimmed) and port, an Airspy by serial.
 * Returns the number of failed checks. */
int
check_receiver_groups() {
    int failures = 0;
    auto check = [&failures](bool ok, const char* label) {
        if (!ok) {
            std::fprintf(stderr, "FAIL: %s\n", label);
            ++failures;
        }
    };
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
    while (model.count() > 0 && model.remove(0)) {}
    return failures;
}

/* The receiver and group keys as written, and a group's title. Returns the number of failed checks. */
int
check_keys_and_titles() {
    using dsd_qt::receiver_identity;
    using dsd_qt::site_group_key;
    using dsd_qt::site_group_title;
    int failures = 0;
    auto check = [&failures](bool ok, const char* label) {
        if (!ok) {
            std::fprintf(stderr, "FAIL: %s\n", label);
            ++failures;
        }
    };
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
    return failures;
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
    int failures = 0;
    auto check = [&](bool ok, const char* label) {
        if (!ok) {
            std::fprintf(stderr, "FAIL: %s\n", label);
            ++failures;
        }
    };
    using namespace dsd_qt;
    check(std::abs(site_distance_km(41, -91, 42, -91) - 111.195) < .01, "haversine km");
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

    failures += check_receiver_groups();
    failures += check_keys_and_titles();
    QDir(dir).removeRecursively();
    return failures ? 1 : 0;
}
