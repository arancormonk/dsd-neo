// SPDX-License-Identifier: GPL-3.0-or-later
#include <QByteArray>
#include <QChar>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QList>
#include <QMap>
#include <QRegularExpression>
#include <QSaveFile>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantList>
#include <dsd-neo/app_control/trunk_scan_validate.h>
#include <initializer_list>
#include <utility>
#include "app_prefs.h"
#include "json_store.h"
#include "saved_systems_model.h"
#include "scan_list_starter.h"
#include "scan_list_targets.h"
#include "session_args.h"

namespace dsd_qt {
namespace {
QVariantMap
absolutePaths(QVariantMap map) {
    for (const auto& field : {"chanCsvPath", "groupCsvPath", "srcCsvPath", "keyCsvPath", "p25BandplanCsvPath"}) {
        const QString path = map.value(field).toString();
        // Leave invalid text intact for the pure generator's rejection. Resolve
        // legitimate relative saved paths before changing the CSV's directory.
        if (!path.isEmpty() && !path.contains(QRegularExpression(QStringLiteral("[,\"\\r\\n]")))
            && !path.contains(QChar(0))) {
            map[field] = QFileInfo(path).absoluteFilePath();
        }
    }
    return map;
}
} // namespace

ScanListStarter::ScanListStarter(const AppPrefs* prefs, const SavedSystemsModel* systems, QObject* parent)
    : QObject(parent), m_prefs(prefs), m_systems(systems) {}

QVariantMap
ScanListStarter::build(const QVariantMap& list) const {
    const auto fail = [](const QString& error, const QStringList& warnings = QStringList()) {
        return QVariantMap{
            {"ok", false}, {"args", QStringList()}, {"error", error}, {"warnings", warnings}, {"targetCount", 0}};
    };
    QVariantList systems;
    if (m_systems) {
        for (int row = 0; row < m_systems->count(); ++row) {
            auto system = m_systems->get(row);
            // INT-3 keeps secrets out of QML maps. Resolve them only for the private session input.
            system.insert(QStringLiteral("encKeyValue"),
                          m_systems->keyValueForUid(system.value(QStringLiteral("uid")).toString()));
            systems << absolutePaths(system);
        }
    }
    const auto prepared = absolutePaths(list);
    auto generated = scan_list_targets(prepared, systems);
    if (!generated.ok) {
        return fail(generated.error, generated.warnings);
    }
    const QString uid = list.value("uid").toString();
    if (!QRegularExpression(QStringLiteral("^[A-Za-z0-9_-]{1,63}$")).match(uid).hasMatch()) {
        return fail(QStringLiteral("Invalid scan-list ID."));
    }
    for (const auto& path : generated.paths) {
        const QFileInfo info(path);
        if (!info.isFile() || !info.isReadable()) {
            return fail(QStringLiteral("A CSV file is missing or unreadable. Select an existing imported CSV."),
                        generated.warnings);
        }
    }
    SessionArgPrefs prefs;
    if (m_prefs) {
        prefs.gainDb = m_prefs->gainDb();
        prefs.ppm = m_prefs->ppm();
        prefs.bandwidthKhz = m_prefs->bandwidthKhz();
        prefs.biasTee = m_prefs->biasTee();
        prefs.skipEncrypted = m_prefs->skipEncrypted();
        prefs.autoPpm = m_prefs->autoPpm();
        prefs.extraArgs = m_prefs->extraArgs();
    }
    const QString path = json_store_path(QStringLiteral("scan_lists/") + uid + QStringLiteral(".csv"));
    QString error;
    const auto args = session_args_scan_build(prepared, generated.firstFreqMhz, path, prefs, &error);
    if (args.isEmpty()) {
        return fail(error, generated.warnings);
    }
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return fail(QStringLiteral("Cannot write the scan-list CSV."));
    }
    // Generated CSVs can contain direct keys; keep them private to this user.
    if (!file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)
        || file.write(generated.csv) != generated.csv.size() || !file.commit()) {
        return fail(QStringLiteral("Cannot save the scan-list CSV."));
    }
    char detail[512] = {};
    int count = 0;
    if (dsd_app_trunk_scan_validate_targets_csv(path.toUtf8().constData(), &count, detail, sizeof detail) != 0) {
        QFile::remove(path);
        return fail(QStringLiteral("Scan list rejected: ") + QString::fromUtf8(detail), generated.warnings);
    }
    return {
        {"ok", true}, {"args", args}, {"error", QString()}, {"warnings", generated.warnings}, {"targetCount", count}};
}
} // namespace dsd_qt
