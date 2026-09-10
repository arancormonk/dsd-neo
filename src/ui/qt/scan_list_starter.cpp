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
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTemporaryFile>
#include <QVariant>
#include <QVariantList>
#include <dsd-neo/app_control/trunk_scan_validate.h>
#include <initializer_list>
#include <utility>
#include "app_prefs.h"
#include "decoder_host.h"
#include "decryption_profile_provider.h"
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
    return prepare(list, true);
}

QVariantMap
ScanListStarter::start(const QVariantMap& list, DecoderHost* host) const {
    auto result = prepare(list, true);
    const auto args = result.take("args").toStringList();
    const bool started = result.value("ok").toBool() && host && host->start(args);
    result.insert("started", started);
    if (started && host->sessionActive() && m_profiles) {
        for (const auto& value : list.value("entries").toList()) {
            const auto entry = value.toMap();
            if (!entry.value("enabled", true).toBool()) {
                continue;
            }
            const QString mode = entry.value("decryptionMode", "inherit").toString();
            if (mode == "profile") {
                m_profiles->retainForSession(entry.value("decryptionProfileUid").toString());
            } else if (mode == "inherit" && entry.value("kind").toString() == "system" && m_systems) {
                m_profiles->retainForSession(
                    m_systems->getByUid(entry.value("systemUid").toString()).value("decryptionProfileUid").toString());
            }
        }
    }
    return result;
}

QVariantMap
ScanListStarter::validate(const QVariantMap& list) const {
    return prepare(list, false);
}

QVariantList
ScanListStarter::resolveSystems(const QVariantMap& list, QString* error) const {
    QSet<QString> inheritedSystems;
    for (const auto& value : list.value("entries").toList()) {
        const auto entry = value.toMap();
        if (entry.value("kind").toString() == "system" && entry.value("enabled", true).toBool()
            && entry.value("decryptionMode", "inherit").toString() == "inherit") {
            inheritedSystems.insert(entry.value("systemUid").toString());
        }
    }
    QVariantList systems;
    if (m_systems) {
        for (int row = 0; row < m_systems->count(); ++row) {
            auto system = m_systems->get(row);
            // INT-3 keeps secrets out of QML maps. Resolve them only for the private session input.
            system.insert(QStringLiteral("encKeyValue"),
                          m_systems->keyValueForUid(system.value(QStringLiteral("uid")).toString()));
            const QString profile = system.value("decryptionProfileUid").toString();
            if (!profile.isEmpty() && inheritedSystems.contains(system.value("uid").toString())) {
                QString profileError;
                if (!m_profiles) {
                    *error = QStringLiteral("Decryption profiles are unavailable.");
                    return {};
                }
                const auto configuration = m_profiles->configuration(profile, &profileError);
                if (!profileError.isEmpty()) {
                    *error = profileError;
                    return {};
                }
                for (auto i = configuration.cbegin(); i != configuration.cend(); ++i) {
                    system.insert(i.key(), i.value());
                }
            }
            systems << absolutePaths(system);
        }
    }
    return systems;
}

bool
ScanListStarter::resolveEntryProfiles(QVariantMap& prepared, QString* error) const {
    QVariantList entries;
    for (const auto& value : prepared.value("entries").toList()) {
        auto entry = value.toMap();
        if (!entry.value("enabled", true).toBool()) {
            entries.append(entry);
            continue;
        }
        const auto mode = entry.value("decryptionMode", "inherit").toString();
        if (mode != "inherit" && mode != "profile" && mode != "none") {
            *error = QStringLiteral("Choose an inherited profile, a selected profile, or no keys for this entry.");
            return false;
        }
        if (mode == "profile") {
            QString profileError;
            if (!m_profiles) {
                *error = QStringLiteral("Decryption profiles are unavailable.");
                return false;
            }
            const auto configuration =
                m_profiles->configuration(entry.value("decryptionProfileUid").toString(), &profileError);
            if (!profileError.isEmpty()) {
                *error = profileError;
                return false;
            }
            entry.insert("decryptionConfiguration", configuration);
        } else if (mode == "none") {
            entry.insert("decryptionConfiguration", QVariantMap{{"decryptionClearKeys", true},
                                                                {"encForceKey", 0},
                                                                {"decryptionForce", 0},
                                                                {"encKeyType", ""},
                                                                {"encKeyValue", ""},
                                                                {"keyCsvPath", ""},
                                                                {"keysHexCsvPath", ""},
                                                                {"keysDecCsvPath", ""},
                                                                {"dmrTgKeyCsvPath", ""},
                                                                {"dmrTgKeyClear", true}});
        }
        entries.append(entry);
    }
    prepared.insert("entries", entries);
    return true;
}

static bool
validateAndStoreCsv(const QByteArray& csv, const QString& path, bool materialize, int* count, QString* error) {
    // Validate a private temporary file before touching a previous runnable CSV.
    QTemporaryFile staged(QDir::tempPath() + QStringLiteral("/dsdneo-scan-XXXXXX.csv"));
    if (!staged.open() || !staged.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)
        || staged.write(csv) != csv.size() || !staged.flush()) {
        *error = QStringLiteral("Cannot prepare the scan-list validation.");
        return false;
    }
    char detail[512] = {};
    if (dsd_app_trunk_scan_validate_targets_csv(staged.fileName().toUtf8().constData(), count, detail, sizeof detail)
        != 0) {
        *error = QStringLiteral("Scan list rejected: ") + QString::fromUtf8(detail);
        return false;
    }
    if (materialize) {
        QDir().mkpath(QFileInfo(path).absolutePath());
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly) || !file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)
            || file.write(csv) != csv.size() || !file.commit()) {
            *error = QStringLiteral("Cannot save the scan-list CSV.");
            return false;
        }
    }
    return true;
}

QVariantMap
ScanListStarter::prepare(const QVariantMap& list, bool materialize) const {
    const auto fail = [](const QString& error, const QStringList& warnings = QStringList()) {
        return QVariantMap{
            {"ok", false}, {"args", QStringList()}, {"error", error}, {"warnings", warnings}, {"targetCount", 0}};
    };
    if (materialize && list.value("isDraft").toBool()) {
        return fail(QStringLiteral("This scan list is a draft. Edit and save it before listening."));
    }
    QString preparationError;
    const auto systems = resolveSystems(list, &preparationError);
    if (!preparationError.isEmpty()) {
        return fail(preparationError);
    }
    auto prepared = absolutePaths(list);
    if (!resolveEntryProfiles(prepared, &preparationError)) {
        return fail(preparationError);
    }
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
        prefs.hangtimeSec = m_prefs->hangtimeSec();
        prefs.extraArgs = m_prefs->extraArgs();
    }
    const QString path = json_store_path(QStringLiteral("scan_lists/") + uid + QStringLiteral(".csv"));
    QString error;
    const auto args = session_args_scan_build(prepared, generated.firstFreqMhz, path, prefs, &error);
    if (args.isEmpty()) {
        return fail(error, generated.warnings);
    }
    int count = 0;
    if (!validateAndStoreCsv(generated.csv, path, materialize, &count, &error)) {
        return fail(error, generated.warnings);
    }

    return {{"ok", true},
            {"args", materialize ? args : QStringList()},
            {"error", QString()},
            {"warnings", generated.warnings},
            {"targetCount", count}};
}
} // namespace dsd_qt
