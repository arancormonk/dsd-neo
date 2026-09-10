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
    QDir(dir).removeRecursively();
    return failures ? 1 : 0;
}
