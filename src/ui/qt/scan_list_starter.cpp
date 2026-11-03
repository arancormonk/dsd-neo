// SPDX-License-Identifier: GPL-3.0-or-later
#include <QByteArray>
#include <QChar>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QLatin1String>
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
#include <Qt>
#include <QtGlobal>
#include <algorithm>
#include <dsd-neo/app_control/trunk_scan_validate.h>
#include <dsd-neo/core/csv_validate.h>
#include <dsd-neo/core/trunk_scan_lists.h>
#include <dsd-neo/runtime/analog_channel.h>
#include <dsd-neo/runtime/log.h>
#include <stddef.h>
#include <stdint.h>
#include <utility>
#include <vector>
#include "decoder_host.h"
#include "decryption_profile_provider.h"
#include "json_store.h"
#include "recording_sessions.h"
#include "saved_systems_model.h"
#include "scan_list_starter.h"
#include "scan_list_targets.h"
#include "session_args.h"
#include "stored_int.h"

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
 * The width of analog demodulator @p kind the session is configured with (issue #526): the last --nfm-bandwidth-hz or
 * --am-bandwidth-hz among the app's Extra arguments, in the separate or the = form, as the engine reads them; 0 without
 * one (the kind's default). A value the engine would not parse is its to refuse, and counts as none here.
 */
int
configuredAnalogWidthHz(const QString& extraArgs, int kind) {
    static const QRegularExpression wholeHz(QStringLiteral("^[0-9]{1,9}$"));
    const QString option =
        kind == DSD_ANALOG_DEMOD_AM ? QStringLiteral("--am-bandwidth-hz") : QStringLiteral("--nfm-bandwidth-hz");
    const QStringList tokens = extraArgs.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    int widthHz = 0;
    for (qsizetype i = 0; i < tokens.size(); ++i) {
        QString value;
        if (tokens.at(i) == option) {
            value = i + 1 < tokens.size() ? tokens.at(i + 1) : QString();
        } else if (tokens.at(i).startsWith(option + QLatin1Char('='))) {
            value = tokens.at(i).mid(option.size() + 1);
        } else {
            continue;
        }
        widthHz = wholeHz.match(value).hasMatch() ? value.toInt() : 0;
    }
    return widthHz;
}

/*
 * Issue #526: the editor's own DSP-rate row diagnostic. An RTL-SDR or rtl_tcp list runs every target at the rate its
 * DSP bandwidth sets -- the list's own, else the app's, read as the engine reads the spec's bandwidth field -- so an
 * analog target whose width that rate cannot filter is skipped at every visit: its own width, else the configured width
 * of its kind the session runs (configuredAnalogWidthHz()), else the AM default an am target runs, which always runs
 * its channel filter. The unset NFM default runs at any rate. A SoapySDR or Airspy device sets its own rate, which the
 * engine checks once the scan starts. Said beside the targets ready, as the engine names such a row when a map loads,
 * not as a refusal: the rest of the list still scans.
 */
QString
analogRateWarning(const QVariantMap& list, const SessionArgPrefs& prefs, const QList<AnalogTarget>& targets) {
    const QString source = list.value("sourceType").toString();
    if (targets.isEmpty() || (source != QStringLiteral("usb") && source != QStringLiteral("rtltcp"))) {
        return {};
    }
    const int listKhz = stored_int_or(list.value("bandwidthKhz"), -1);
    const int khz = listKhz > 0 ? listKhz : prefs.bandwidthKhz;
    const int rateHz = dsd_analog_rtl_dsp_bw_or_default(khz) * 1000;
    const int nfmHz = configuredAnalogWidthHz(prefs.extraArgs, DSD_ANALOG_DEMOD_FM);
    const int amHz = configuredAnalogWidthHz(prefs.extraArgs, DSD_ANALOG_DEMOD_AM);
    int skipped = 0;
    QString first;
    for (const auto& target : targets) {
        const int inheritedHz = target.kind == DSD_ANALOG_DEMOD_AM
                                    ? (amHz > 0 ? amHz : dsd_analog_width_default_hz(DSD_ANALOG_DEMOD_AM))
                                    : nfmHz;
        const int heldHz = target.widthHz > 0 ? target.widthHz : inheritedHz;
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
    const dsd_app_scan_csv_callbacks callbacks{collectCsvAnalogTarget, nullptr, &analog, nullptr};
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

/* The CSVs an earlier name of list @p uid left in its folder, and the one older versions wrote beside it
   (scan_lists/<uid>.csv): every one but @p keep goes. */
void
remove_other_list_csvs(const QString& uid, const QString& keep) {
    QFile::remove(json_store_path(QStringLiteral("scan_lists/") + uid + QStringLiteral(".csv")));
    const QDir folder(QFileInfo(keep).absolutePath());
    for (const QFileInfo& file : folder.entryInfoList({QStringLiteral("*.csv")}, QDir::Files)) {
        if (file.absoluteFilePath() != QFileInfo(keep).absoluteFilePath()) {
            QFile::remove(file.absoluteFilePath());
        }
    }
}

/* One companion file a target CSV names: its reference index and where it resolves. */
struct TargetCompanion {
    size_t index = 0;
    QString resolved;
};

void
collectTargetCompanion(const dsd_app_scan_csv_reference* ref, void* context) {
    static_cast<QList<TargetCompanion>*>(context)->append({ref->index, QString::fromUtf8(ref->resolved_path)});
}

/* Issue #697: copy an imported target CSV to @p destination, so the decoder names the list after its own file there,
   with every companion it names (channel maps, talkgroup and key files) rewritten to the same file, relative to the
   copy's folder. The imported original is never written. Empty on success, else why no copy was written (a companion
   path that cannot be written as a replacement token, among others). */
QString
copy_targets_for_list(const QString& source, const QString& destination) {
    QList<TargetCompanion> refs;
    const dsd_app_scan_csv_callbacks callbacks{nullptr, collectTargetCompanion, &refs, nullptr};
    char error[512] = {};
    if (dsd_app_scan_csv_inspect(source.toUtf8().constData(), nullptr, 0, &callbacks, error, sizeof error) != 0) {
        return QString::fromUtf8(error);
    }
    if (!QDir().mkpath(QFileInfo(destination).absolutePath())) {
        return QStringLiteral("cannot create its folder");
    }
    const QDir folder(QFileInfo(destination).absolutePath());
    QList<QByteArray> paths;
    QList<dsd_app_scan_csv_replacement> mapping;
    for (const auto& ref : refs) {
        paths << folder.relativeFilePath(ref.resolved).toUtf8();
        mapping << dsd_app_scan_csv_replacement{ref.index, nullptr};
    }
    for (qsizetype i = 0; i < mapping.size(); ++i) {
        mapping[i].relative_path = paths.at(i).constData();
    }
    if (dsd_app_scan_csv_rewrite(source.toUtf8().constData(), nullptr, 0, destination.toUtf8().constData(),
                                 mapping.constData(), static_cast<size_t>(mapping.size()), error, sizeof error)
        != 0) {
        return QString::fromUtf8(error);
    }
    return {};
}

/* Characters a file name cannot hold on Android or Windows, or that would make it a path. */
bool
unsafe_in_file_name(QChar c) {
    static const QString refused = QStringLiteral("/\\<>:\"|?*");
    return c.unicode() < 0x20 || c.unicode() == 0x7F || refused.contains(c);
}

/* @p text cut to at most @p bytes UTF-8 bytes, never inside a character. */
QString
cut_utf8(const QString& text, int bytes) {
    QString out;
    int used = 0;
    for (qsizetype i = 0; i < text.size();) {
        const qsizetype step = text.at(i).isHighSurrogate() && i + 1 < text.size() ? 2 : 1;
        const QString ch = text.mid(i, step);
        const int size = static_cast<int>(ch.toUtf8().size());
        if (used + size > bytes) {
            break;
        }
        out += ch;
        used += size;
        i += step;
    }
    return out;
}

/* A CSV list's own fallback talkgroup and source-alias files, where it names them, load. */
bool
fallbackFilesValid(const QVariantMap& prepared) {
    const QStringList fields{QStringLiteral("groupCsvPath"), QStringLiteral("srcCsvPath")};
    return std::all_of(fields.cbegin(), fields.cend(), [&prepared](const QString& field) {
        const QByteArray fallback = prepared.value(field).toString().toUtf8();
        if (fallback.isEmpty()) {
            return true;
        }
        dsd_csv_validation stats{};
        return (field == QLatin1String("groupCsvPath") ? dsd_csv_validate_group_file(fallback.constData(), &stats)
                                                       : dsd_csv_validate_src_file(fallback.constData(), &stats))
               == 0;
    });
}

} // namespace

QString
scan_list_csv_stem(const QString& name, int listNumber) {
    QString stem = name;
    std::replace_if(stem.begin(), stem.end(), unsafe_in_file_name, QChar('_'));
    /* The name the engine shows is cut there anyway; cut here, whole characters only, so the file says the same. */
    stem = cut_utf8(stem.trimmed(), DSD_TRUNK_SCAN_LIST_NAME_SIZE - 1);
    /* No file name ends in a dot or a blank on Windows, and "." or ".." is not a file. */
    while (!stem.isEmpty() && (stem.endsWith(QLatin1Char('.')) || stem.endsWith(QLatin1Char(' ')))) {
        stem.chop(1);
    }
    static const QRegularExpression reserved(QStringLiteral("^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])$"),
                                             QRegularExpression::CaseInsensitiveOption);
    if (reserved.match(stem).hasMatch()) {
        stem += QLatin1Char('_');
    }
    return stem.isEmpty() ? QStringLiteral("list %1").arg(listNumber) : stem;
}

QString
scan_list_csv_path(const QString& uid, const QString& name, int listNumber) {
    return json_store_path(QStringLiteral("scan_lists/") + uid + QLatin1Char('/') + scan_list_csv_stem(name, listNumber)
                           + QStringLiteral(".csv"));
}

ScanListStarter::ScanListStarter(const AppPrefs* prefs, const SavedSystemsModel* systems, QObject* parent)
    : QObject(parent), m_prefs(prefs), m_systems(systems) {}

QVariantMap
ScanListStarter::build(const QVariantMap& list) const {
    return prepare(list, true);
}

QVariantMap
ScanListStarter::prepareForStart(const QVariantMap& list, const DecoderHost* host, bool* recording,
                                 const QStringList& companionPaths) const {
    // The list's call recording folder goes into its argv (issue #708), so a start that can record checks the list
    // first and asks for the folder only for a list that can start; the build that writes the CSV then names it. A
    // draft never can (only that build refuses it), so it asks for none.
    const bool canRecord = host && m_recordingSessions && !list.value("isDraft").toBool();
    auto result = prepare(list, !canRecord, QString(), false, companionPaths);
    *recording = false;
    if (canRecord && result.value("ok").toBool()) {
        *recording = true;
        const QString folder = m_recordingSessions->beginSession(QStringLiteral("scan"), list.value("uid").toString(),
                                                                 list.value("name").toString());
        result = prepare(list, true, folder, folder.isEmpty(), companionPaths);
    }
    return result;
}

void
ScanListStarter::endUnstartedRecording(bool recording, bool started, const DecoderHost* host) const {
    if (recording && !started && !host->sessionActive()) {
        m_recordingSessions->endSession();
    }
}

void
ScanListStarter::retainProfiles(const QVariantMap& list) const {
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

QVariantMap
ScanListStarter::startPrepared(const QVariantList& lists, DecoderHost* host, const QStringList& companionPaths) const {
    const QVariantMap list = lists.value(0).toMap();
    bool recording = false;
    auto result = prepareForStart(list, host, &recording, companionPaths);
    const auto args = result.take("args").toStringList();
    if (result.value("ok").toBool() && host) {
        host->noteStartAttempt(list.value("sourceType").toString());
    }
    const bool started = result.value("ok").toBool() && host && host->start(args);
    endUnstartedRecording(recording, started, host);
    result.insert("started", started);
    if (started && host->sessionActive() && m_profiles) {
        for (const auto& value : lists) {
            retainProfiles(value.toMap());
        }
    }
    return result;
}

QVariantMap
ScanListStarter::start(const QVariantMap& list, DecoderHost* host) const {
    return startPrepared({list}, host, {});
}

namespace {
QString
quotedName(const QVariantMap& list) {
    return QStringLiteral("\u201c%1\u201d").arg(list.value("name").toString());
}

QVariantMap
listsRefusal(const QString& error) {
    return {{"ok", false},      {"args", QStringList()}, {"error", error}, {"warnings", QStringList()},
            {"targetCount", 0}, {"started", false}};
}
} // namespace

QString
ScanListStarter::listCopyOf(const QVariantMap& list, const QString& source, int listNumber) {
    const QString uid = list.value("uid").toString();
    QString copy = scan_list_csv_path(uid, list.value("name").toString(), listNumber);
    /* A copy that cannot be written, or that does not check out as the original does, leaves the list on the
       imported file itself: it still runs, named after that file, and the log says so. */
    QString why = copy_targets_for_list(source, copy);
    char detail[512] = {};
    if (why.isEmpty()
        && dsd_app_trunk_scan_validate_bundle(copy.toUtf8().constData(), nullptr, nullptr, detail, sizeof detail)
               != 0) {
        why = QString::fromUtf8(detail);
    }
    if (!why.isEmpty()) {
        QFile::remove(copy);
        LOG_INFO("NOTICE: Scan list \"%s\" runs from its imported file, named after that file: no copy under the "
                 "list's name (%s)\n",
                 qUtf8Printable(list.value("name").toString()), qUtf8Printable(why));
        return source;
    }
    remove_other_list_csvs(uid, copy);
    return copy;
}

int
ScanListStarter::maxLists() {
    return DSD_TRUNK_SCAN_MAX_LISTS;
}

QString
ScanListStarter::joinConflict(const QVariantMap& primary, const QVariantMap& other) const {
    return session_args_scan_join_conflict(primary, other, session_args_prefs(m_prefs));
}

QString
ScanListStarter::checkLists(const QVariantList& lists, QStringList* warnings) const {
    if (lists.size() > DSD_TRUNK_SCAN_MAX_LISTS) {
        return QStringLiteral("Scan at most %1 lists at once.").arg(DSD_TRUNK_SCAN_MAX_LISTS);
    }
    const QVariantMap primary = lists.value(0).toMap();
    for (const auto& value : lists) {
        const QVariantMap list = value.toMap();
        const auto checked = prepare(list, false);
        if (!checked.value("ok").toBool()) {
            return quotedName(list) + QStringLiteral(": ") + checked.value("error").toString();
        }
        for (const auto& warning : checked.value("warnings").toStringList()) {
            *warnings << quotedName(list) + QStringLiteral(": ") + warning;
        }
        const QString conflict = joinConflict(primary, list);
        if (!conflict.isEmpty()) {
            return QStringLiteral("%1 can't scan with %2: %3.")
                .arg(quotedName(list), quotedName(primary), conflict.left(1).toLower() + conflict.mid(1));
        }
    }
    return {};
}

QString
ScanListStarter::materializeLists(const QVariantList& lists, QStringList* paths, int* total) const {
    QList<QByteArray> pathBytes;
    QList<QByteArray> nameBytes;
    for (const auto& value : lists) {
        const QVariantMap list = value.toMap();
        const auto built = prepare(list, true, QString(), false, QStringList(), static_cast<int>(paths->size()) + 1);
        if (!built.value("ok").toBool()) {
            return quotedName(list) + QStringLiteral(": ") + built.value("error").toString();
        }
        *paths << built.value("csvPath").toString();
        pathBytes << paths->constLast().toUtf8();
        nameBytes << list.value("name").toString().toUtf8();
    }
    std::vector<const char*> cpaths;
    std::vector<const char*> cnames;
    for (qsizetype i = 0; i < pathBytes.size(); ++i) {
        cpaths.push_back(pathBytes.at(i).constData());
        cnames.push_back(nameBytes.at(i).constData());
    }
    char detail[512] = {};
    if (dsd_app_trunk_scan_validate_target_lists(cpaths.data(), cnames.data(), cpaths.size(), total, detail,
                                                 sizeof detail)
        != 0) {
        return QStringLiteral("Scan lists rejected: ") + QString::fromUtf8(detail);
    }
    return {};
}

QVariantMap
ScanListStarter::startLists(const QVariantList& lists, DecoderHost* host) const {
    if (lists.size() <= 1) {
        return start(lists.value(0).toMap(), host);
    }
    QStringList warnings;
    QString error = checkLists(lists, &warnings);
    QStringList paths;
    int total = 0;
    if (error.isEmpty()) {
        error = materializeLists(lists, &paths, &total);
    }
    if (!error.isEmpty()) {
        return listsRefusal(error);
    }
    auto result = startPrepared(lists, host, paths.mid(1));
    result.insert("warnings", warnings);
    result.insert("targetCount", total);
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
                    *error = std::move(profileError);
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
                *error = std::move(profileError);
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
ScanListStarter::prepare(const QVariantMap& list, bool materialize, const QString& recordingFolder,
                         bool recordingUnavailable, const QStringList& companionPaths, int listNumber) const {
    const auto fail = [](const QString& error, const QStringList& warnings = QStringList()) {
        return QVariantMap{
            {"ok", false}, {"args", QStringList()}, {"error", error}, {"warnings", warnings}, {"targetCount", 0}};
    };
    if (materialize && list.value("isDraft").toBool()) {
        return fail(QStringLiteral("This scan list is a draft. Edit and save it before listening."));
    }
    const QString source = list.value("targetSource", "entries").toString();
    if (source == "csv") {
        return prepareCsv(list, materialize, recordingFolder, recordingUnavailable, companionPaths, listNumber);
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
        analogRateWarning(prepared, session_args_prefs(m_prefs), manualAnalogTargets(prepared, m_systems));
    if (!rateWarning.isEmpty()) {
        generated.warnings << rateWarning;
    }
    const QString uid = list.value("uid").toString();
    const QString inputError = generatedInputError(uid, generated.paths);
    if (!inputError.isEmpty()) {
        return fail(inputError, generated.warnings);
    }
    auto prefs = session_args_prefs(m_prefs);
    prefs.recordingFolder = recordingFolder;
    prefs.recordingUnavailable = recordingUnavailable;
    const QString path = scan_list_csv_path(uid, list.value("name").toString(), listNumber);
    QString error;
    const auto args = session_args_scan_build_lists(prepared, generated.firstFreqMhz,
                                                    QStringList{path} + companionPaths, prefs, &error);
    if (args.isEmpty()) {
        return fail(error, generated.warnings);
    }
    int count = 0;
    if (!validateAndStoreCsv(generated.csv, path, materialize, &count, &error)) {
        return fail(error, generated.warnings);
    }
    if (materialize) {
        remove_other_list_csvs(uid, path);
    }

    return {{"ok", true},           {"args", materialize ? args : QStringList()},
            {"error", QString()},   {"warnings", generated.warnings},
            {"targetCount", count}, {"csvPath", path}};
}

QVariantMap
ScanListStarter::prepareCsv(const QVariantMap& list, bool materialize, const QString& recordingFolder,
                            bool recordingUnavailable, const QStringList& companionPaths, int listNumber) const {
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
    if (!fallbackFilesValid(prepared)) {
        return fail(QStringLiteral("The list's fallback talkgroup or source-alias CSV is missing or invalid."));
    }
    char detail[512] = {};
    int count = 0;
    uint32_t first = 0;
    if (dsd_app_trunk_scan_validate_bundle(path.toUtf8().constData(), &count, &first, detail, sizeof detail)) {
        return fail(QString::fromUtf8(detail));
    }
    const QString runPath = materialize ? listCopyOf(list, path, listNumber) : path;
    QString error;
    auto prefs = session_args_prefs(m_prefs);
    prefs.recordingFolder = recordingFolder;
    prefs.recordingUnavailable = recordingUnavailable;
    const auto args = session_args_scan_build_lists(prepared, QString::number(first / 1e6, 'f', 6),
                                                    QStringList{runPath} + companionPaths, prefs, &error);
    if (args.isEmpty()) {
        return fail(error);
    }
    return {{"ok", true},           {"args", materialize ? args : QStringList()},
            {"error", QString()},   {"warnings", csvAnalogRateWarnings(prepared, path, session_args_prefs(m_prefs))},
            {"targetCount", count}, {"csvPath", runPath}};
}
} // namespace dsd_qt
