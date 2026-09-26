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
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QTemporaryFile>
#include <QVariant>
#include <QVariantList>
#include <dsd-neo/app_control/trunk_scan_validate.h>
#include <dsd-neo/core/csv_validate.h>
#include <dsd-neo/runtime/analog_channel.h>
#include <initializer_list>
#include <stdint.h>
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
QString
generatedInputError(const QString& uid, const QStringList& paths) {
    if (!QRegularExpression(QStringLiteral("^[A-Za-z0-9_-]{1,63}$")).match(uid).hasMatch()) {
        return QStringLiteral("Invalid scan-list ID.");
    }
    for (const auto& path : paths) {
        const QFileInfo info(path);
        if (!info.isFile() || !info.isReadable()) {
            return QStringLiteral("A CSV file is missing or unreadable. Select an existing imported CSV.");
        }
    }
    return {};
}

SessionArgPrefs
sessionPreferences(const AppPrefs* source) {
    SessionArgPrefs prefs;
    if (source) {
        prefs.gainDb = source->gainDb();
        prefs.ppm = source->ppm();
        prefs.bandwidthKhz = source->bandwidthKhz();
        prefs.biasTee = source->biasTee();
        prefs.skipEncrypted = source->skipEncrypted();
        prefs.persistTgLockouts = source->persistTgLockouts();
        prefs.autoPpm = source->autoPpm();
        prefs.hangtimeSec = source->hangtimeSec();
        prefs.extraArgs = source->extraArgs();
    }
    return prefs;
}

/* One analog target a list scans (issue #526): how the validation names it, its demodulator, and its own width in Hz
   (0 when it sets none). */
struct AnalogTarget {
    QString label;
    int kind = DSD_ANALOG_DEMOD_FM;
    int widthHz = 0;
};

void
collectCsvAnalogTarget(const dsd_app_scan_csv_target* target, void* context) {
    const QString type = QString::fromUtf8(target->type);
    const bool am = type == QStringLiteral("am-conventional");
    if (!am && type != QStringLiteral("nfm-conventional")) {
        return;
    }
    static_cast<QList<AnalogTarget>*>(context)->append({QStringLiteral("Target %1").arg(QString::fromUtf8(target->id)),
                                                        am ? DSD_ANALOG_DEMOD_AM : DSD_ANALOG_DEMOD_FM,
                                                        target->bandwidth_hz > 0 ? target->bandwidth_hz : 0});
}

/*
 * Issue #526: the editor's own DSP-rate row diagnostic. An RTL-SDR or rtl_tcp list runs every target at the rate its
 * DSP bandwidth sets -- the list's own, else the app's, read as the engine reads the spec's bandwidth field -- so an
 * analog target whose width that rate cannot filter is skipped at every visit: its own width, or the AM default an am
 * target without one runs, which always runs its channel filter. The unset NFM default runs at any rate. A SoapySDR or
 * Airspy device sets its own rate, which the engine checks once the scan starts. Said beside the targets ready, as the
 * engine names such a row when a map loads, not as a refusal: the rest of the list still scans.
 */
QString
analogRateWarning(const QVariantMap& list, const SessionArgPrefs& prefs, const QList<AnalogTarget>& targets) {
    const QString source = list.value("sourceType").toString();
    if (targets.isEmpty() || (source != QStringLiteral("usb") && source != QStringLiteral("rtltcp"))) {
        return {};
    }
    const int listKhz = list.value("bandwidthKhz", -1).toInt();
    const int khz = listKhz > 0 ? listKhz : prefs.bandwidthKhz;
    const int rateHz = (dsd_analog_rtl_dsp_bw_is_selectable(khz) ? khz : DSD_ANALOG_RTL_DSP_BW_MAX_KHZ) * 1000;
    int skipped = 0;
    QString first;
    for (const auto& target : targets) {
        const int heldHz = target.widthHz > 0                   ? target.widthHz
                           : target.kind == DSD_ANALOG_DEMOD_AM ? dsd_analog_width_default_hz(DSD_ANALOG_DEMOD_AM)
                                                                : 0;
        char why[DSD_ANALOG_ERROR_TEXT_MAX] = {};
        if (heldHz <= 0 || dsd_analog_width_check(target.kind, heldHz, rateHz, why, sizeof why) == 0) {
            continue;
        }
        if (skipped++ == 0) {
            first = target.label + QStringLiteral(": ") + QString::fromUtf8(why);
        }
    }
    if (skipped == 0) {
        return {};
    }
    return QStringLiteral("Skipped at every visit at this bandwidth: %1%2.")
        .arg(first, skipped > 1 ? QStringLiteral(" (and %1 more)").arg(skipped - 1) : QString());
}

/* The analog demodulator a list entry runs, or -1 (scan_list_entry_analog_kind()), a saved system's by its saved decode
   flags. An analog entry (issue #526) carries no decryption: the editor hides the choice, so whatever it holds is
   resolved, retained and checked for no analog entry. */
int
entryAnalogKind(const QVariantMap& entry, const SavedSystemsModel* systems) {
    const QString flag = entry.value("kind").toString() == QStringLiteral("system") && systems
                             ? systems->getByUid(entry.value("systemUid").toString()).value("decodeFlag").toString()
                             : QString();
    return scan_list_entry_analog_kind(entry, flag);
}

/* The enabled analog entries of a manual list, each named as the editor shows it: a saved system by its name, a
   frequency entry by its name, else its frequency. None sets a width of its own. */
QList<AnalogTarget>
manualAnalogTargets(const QVariantMap& list, const SavedSystemsModel* systems) {
    QList<AnalogTarget> analog;
    for (const auto& value : list.value("entries").toList()) {
        const auto entry = value.toMap();
        const int kind = entry.value("enabled", true).toBool() ? entryAnalogKind(entry, systems) : -1;
        if (kind < 0) {
            continue;
        }
        const QString name = entry.value("kind").toString() == QStringLiteral("system") && systems
                                 ? systems->getByUid(entry.value("systemUid").toString()).value("name").toString()
                                 : entry.value("name").toString();
        analog.append({name.isEmpty() ? entry.value("freqMhz").toString() + QStringLiteral(" MHz") : name, kind, 0});
    }
    return analog;
}

/* The DSP-rate warning for a CSV-backed list (analogRateWarning()), each analog target held at its own width. The file
   was validated already; one the inspection cannot read says nothing more. */
QStringList
csvAnalogRateWarnings(const QVariantMap& list, const QString& path, const SessionArgPrefs& prefs) {
    QList<AnalogTarget> analog;
    const dsd_app_scan_csv_callbacks callbacks{collectCsvAnalogTarget, nullptr, &analog};
    char detail[512] = {};
    if (dsd_app_scan_csv_inspect(path.toUtf8().constData(), nullptr, 0, &callbacks, detail, sizeof detail) != 0) {
        return {};
    }
    const QString warning = analogRateWarning(list, prefs, analog);
    return warning.isEmpty() ? QStringList() : QStringList{warning};
}

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
            if (!entry.value("enabled", true).toBool() || entryAnalogKind(entry, m_systems) >= 0) {
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
            && entry.value("decryptionMode", "inherit").toString() == "inherit"
            && entryAnalogKind(entry, m_systems) < 0) {
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
        if (!entry.value("enabled", true).toBool() || entryAnalogKind(entry, m_systems) >= 0) {
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
    // Android need not have a writable global temp directory. Stage in the
    // app's cache, privately, before touching a previous runnable CSV.
    const QString directory = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (directory.isEmpty() || !QDir().mkpath(directory)) {
        *error = QStringLiteral("Cannot prepare the scan-list validation.");
        return false;
    }
    QTemporaryFile staged(directory + QStringLiteral("/dsdneo-scan-XXXXXX.csv"));
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
    const QString source = list.value("targetSource", "entries").toString();
    if (source == "csv") {
        return prepareCsv(list, materialize);
    }
    if (source != "entries" || !list.value("targetsCsvPath").toString().isEmpty()) {
        return fail(QStringLiteral("Choose either manual entries or an imported target CSV."));
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
    const QString rateWarning =
        analogRateWarning(prepared, sessionPreferences(m_prefs), manualAnalogTargets(prepared, m_systems));
    if (!rateWarning.isEmpty()) {
        generated.warnings << rateWarning;
    }
    const QString uid = list.value("uid").toString();
    const QString inputError = generatedInputError(uid, generated.paths);
    if (!inputError.isEmpty()) {
        return fail(inputError, generated.warnings);
    }
    const auto prefs = sessionPreferences(m_prefs);
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

QVariantMap
ScanListStarter::prepareCsv(const QVariantMap& list, bool materialize) const {
    const auto fail = [](const QString& error) {
        return QVariantMap{
            {"ok", false}, {"args", QStringList()}, {"error", error}, {"warnings", QStringList()}, {"targetCount", 0}};
    };
    if (!list.value("entries").toList().isEmpty()) {
        return fail(QStringLiteral("An imported target CSV cannot be combined with manual entries."));
    }
    if (!QRegularExpression(QStringLiteral("^[A-Za-z0-9_-]{1,63}$")).match(list.value("uid").toString()).hasMatch()) {
        return fail(QStringLiteral("Invalid scan-list ID."));
    }
    const auto prepared = absolutePaths(list);
    const QString settingsError = scan_list_settings_error(prepared);
    if (!settingsError.isEmpty()) {
        return fail(settingsError);
    }
    const QString path = prepared.value("targetsCsvPath").toString();
    if (path.isEmpty() || !m_targetFileLookup || !m_targetFileLookup(path) || !QFileInfo(path).isFile()) {
        return fail(QStringLiteral(
            "The target CSV is missing or was removed. Edit this list and select an imported target CSV."));
    }
    for (const auto& field : {"groupCsvPath", "srcCsvPath"}) {
        const QString fallback = prepared.value(field).toString();
        if (fallback.isEmpty()) {
            continue;
        }
        dsd_csv_validation stats{};
        const int rc = QString(field) == "groupCsvPath"
                           ? dsd_csv_validate_group_file(fallback.toUtf8().constData(), &stats)
                           : dsd_csv_validate_src_file(fallback.toUtf8().constData(), &stats);
        if (rc) {
            return fail(QStringLiteral("The list's fallback talkgroup or source-alias CSV is missing or invalid."));
        }
    }
    char detail[512] = {};
    int count = 0;
    uint32_t first = 0;
    if (dsd_app_trunk_scan_validate_bundle(path.toUtf8().constData(), &count, &first, detail, sizeof detail)) {
        return fail(QString::fromUtf8(detail));
    }
    QString error;
    const auto args = session_args_scan_build(prepared, QString::number(first / 1e6, 'f', 6), path,
                                              sessionPreferences(m_prefs), &error);
    if (args.isEmpty()) {
        return fail(error);
    }
    return {{"ok", true},
            {"args", materialize ? args : QStringList()},
            {"error", QString()},
            {"warnings", csvAnalogRateWarnings(prepared, path, sessionPreferences(m_prefs))},
            {"targetCount", count}};
}
} // namespace dsd_qt
