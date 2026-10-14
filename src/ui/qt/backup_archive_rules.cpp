// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "backup_archive_rules.h"

#include <QByteArray>
#include <QChar>
#include <QLatin1String>
#include <QObject>
#include <QUuid>
#include <QVariant>
#include <algorithm>
#include <dsd-neo/core/safe_api.h>
#include <functional>
#include <initializer_list> // IWYU pragma: keep -- braced range-for lists require this header.
#include <iterator>
#include <stddef.h>
#include "decode_mode_flag.h"
#include "decryption_profiles_model.h"
#include "imported_files_model.h"
#include "saved_systems_model.h"
#include "signal_search_model.h"

namespace dsd_qt {
namespace backup {
namespace detail {

namespace {

constexpr TypedPathField kTypedPathFields[] = {
    {kSavedSystemsStore, "chanCsvPath", PathKind::File},
    {kSavedSystemsStore, "groupCsvPath", PathKind::File},
    {kSavedSystemsStore, "keyCsvPath", PathKind::File},
    {kSavedSystemsStore, "p25BandplanCsvPath", PathKind::File},
    {kSavedSystemsStore, "srcCsvPath", PathKind::File},
    {kSavedSystemsStore, "filePath", PathKind::Recording},
    {kScanListsStore, "groupCsvPath", PathKind::File},
    {kScanListsStore, "srcCsvPath", PathKind::File},
    {kScanListsStore, "targetsCsvPath", PathKind::File},
    {kImportedFilesStore, "path", PathKind::File},
    {kImportedFilesStore, "bundleRoot", PathKind::BundleRoot},
    {kDecryptionProfilesStore, "legacyCsvPath", PathKind::File},
    {kDecryptionProfilesStore, "mappingCsvPath", PathKind::File},
    {kDecryptionProfilesStore, "vendorCsvPath", PathKind::File},
};

QJsonArray
repair_scan_entries(const QJsonValue& entries) {
    QJsonArray out;
    QSet<QString> seen;
    for (const auto& value : entries.toArray()) {
        if (!value.isObject()) {
            continue;
        }
        QJsonObject entry = value.toObject();
        QString uid = variant_string(entry.value(QLatin1String("uid")));
        if (uid.isEmpty() || seen.contains(uid)) {
            uid = new_uid();
        }
        seen.insert(uid);
        entry.insert(QLatin1String("uid"), uid);
        out.append(entry);
    }
    return out;
}

bool
objects_only(const QJsonArray& rows, QString* why) {
    const bool ok = std::all_of(rows.cbegin(), rows.cend(), [](const QJsonValue& v) { return v.isObject(); });
    if (!ok) {
        *why = QStringLiteral("a row is not an object");
    }
    return ok;
}

/** Whether every row has a string "uid" that @p valid accepts, none twice. */
bool
unique_uids(const QJsonArray& rows, const std::function<bool(const QString&)>& valid, QString* why) {
    QSet<QString> seen;
    for (const auto& value : rows) {
        const QJsonValue uid = value.toObject().value(QLatin1String("uid"));
        if (!uid.isString() || !valid(uid.toString()) || seen.contains(uid.toString())) {
            *why = QStringLiteral("a row's uid is missing, malformed or repeated");
            return false;
        }
        seen.insert(uid.toString());
    }
    return true;
}

bool
validate_systems(const QJsonArray& rows, QString* why) {
    const auto canonical = [](const QString& uid) {
        return !uid.isEmpty() && SavedSystemsModel::canonicalUid(uid) == uid;
    };
    return objects_only(rows, why) && unique_uids(rows, canonical, why);
}

bool
validate_scan_lists(const QJsonArray& rows, QString* why) {
    const auto nonEmpty = [](const QString& uid) { return !uid.isEmpty(); };
    if (!objects_only(rows, why) || !unique_uids(rows, nonEmpty, why)) {
        return false;
    }
    return std::all_of(rows.cbegin(), rows.cend(), [&](const QJsonValue& value) {
        const QJsonValue entries = value.toObject().value(QLatin1String("entries"));
        if (entries.isUndefined()) {
            return true;
        }
        if (!entries.isArray() || !objects_only(entries.toArray(), why)
            || !unique_uids(entries.toArray(), nonEmpty, why)) {
            *why = QStringLiteral("scan entries are malformed or repeat a uid");
            return false;
        }
        return true;
    });
}

bool
validate_library(const QJsonArray& rows, QString* why) {
    if (!objects_only(rows, why)) {
        return false;
    }
    const QStringList types = ImportedFilesModel::knownTypes();
    QSet<QString> paths;
    for (const auto& value : rows) {
        const QJsonObject row = value.toObject();
        const QJsonValue path = row.value(QLatin1String("path"));
        if (!types.contains(row.value(QLatin1String("type")).toString())) {
            *why = QStringLiteral("a library row has an unknown type");
            return false;
        }
        if (path.isString() && paths.contains(path.toString())) {
            *why = QStringLiteral("two library rows name the same file");
            return false;
        }
        paths.insert(path.toString());
    }
    return true;
}

bool
validate_profiles(QJsonArray& rows, QString* why) {
    if (!objects_only(rows, why) || !unique_uids(rows, &DecryptionProfilesModel::validUid, why)) {
        return false;
    }
    for (qsizetype i = 0; i < rows.size(); ++i) {
        QJsonObject row = rows.at(i).toObject();
        strip_compiled(row);
        rows.replace(i, row);
    }
    return true;
}

bool
validate_ranges(QJsonArray& rows, QString* why) {
    for (qsizetype i = 0; i < rows.size(); ++i) {
        QVariantMap range;
        if (!rows.at(i).isObject()
            || !SignalSearchModel::storedRangeRow(rows.at(i).toObject().toVariantMap(), &range)) {
            *why = QStringLiteral("a signal-search range is out of shape");
            return false;
        }
        rows.replace(i, QJsonObject::fromVariantMap(range));
    }
    return true;
}

} // namespace

const QList<TypedPathField>&
typed_fields() {
    static const QList<TypedPathField> fields(std::begin(kTypedPathFields), std::end(kTypedPathFields));
    return fields;
}

bool
is_typed_field(const QString& store, const TypedPathField& field) {
    return store == QLatin1String(field.store);
}

QString
new_uid() {
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

void
wipe(QByteArray& bytes) {
    if (!bytes.isEmpty()) {
        DSD_SECURE_ZERO(bytes.data(), static_cast<size_t>(bytes.size()));
    }
    bytes.clear();
}

QString
variant_string(const QJsonValue& value) {
    return value.toVariant().toString();
}

bool
owned_name(const QString& id) {
    if (id.isEmpty() || id.size() > 63) {
        return false;
    }
    return std::all_of(id.cbegin(), id.cend(), [](QChar c) {
        return (c >= QLatin1Char('A') && c <= QLatin1Char('Z')) || (c >= QLatin1Char('a') && c <= QLatin1Char('z'))
               || (c >= QLatin1Char('0') && c <= QLatin1Char('9')) || c == QLatin1Char('_') || c == QLatin1Char('-');
    });
}

bool
staging_name(const QString& name) {
    return name.startsWith(QLatin1String(".staged-")) || name.startsWith(QLatin1String(".import"));
}

QString
path_key(const QString& path) {
    return path.normalized(QString::NormalizationForm_D).toCaseFolded().normalized(QString::NormalizationForm_D);
}

bool
unsafe_text(const QString& text) {
    if (QString::fromUtf8(text.toUtf8()) != text) {
        return true;
    }
    return std::any_of(text.cbegin(), text.cend(),
                       [](QChar c) { return c == QLatin1Char('\\') || c.category() == QChar::Other_Control; });
}

QString
archive_path_problem(const QString& path) {
    if (path.isEmpty()) {
        return QStringLiteral("empty path");
    }
    if (unsafe_text(path)) {
        return QStringLiteral("backslash, control character or invalid Unicode");
    }
    const QStringList parts = path.split(QLatin1Char('/'));
    if (parts.first() != QLatin1String("imports")) {
        return QStringLiteral("not under imports/");
    }
    if (parts.size() < 2 || parts.size() > kMaxPathComponents) {
        return QStringLiteral("too shallow or too deep");
    }
    for (const QString& part : parts) {
        if (part.isEmpty() || part == QLatin1String(".") || part == QLatin1String("..")) {
            return QStringLiteral("empty, '.' or '..' component");
        }
    }
    return staging_name(parts.last()) ? QStringLiteral("staging leftover") : QString();
}

QString
bundle_of(const QString& path) {
    static const QString bundles = QStringLiteral("imports/bundles/");
    if (!path.startsWith(bundles)) {
        return {};
    }
    const qsizetype end = path.indexOf(QLatin1Char('/'), bundles.size());
    return end > bundles.size() ? path.left(end) : QString();
}

CarriedSet::Outcome
CarriedSet::add(const QString& path, QString* why) {
    if (m_exact.contains(path)) {
        *why = QStringLiteral("listed twice");
        return Present;
    }
    *why = archive_path_problem(path);
    if (!why->isEmpty()) {
        return Refused;
    }
    const QString key = path_key(path);
    QStringList directories, directoryKeys;
    for (qsizetype at = path.indexOf(QLatin1Char('/')); at > 0; at = path.indexOf(QLatin1Char('/'), at + 1)) {
        directories.append(path.left(at));
        directoryKeys.append(path_key(directories.last()));
    }
    const bool fileClash = std::any_of(directoryKeys.cbegin(), directoryKeys.cend(),
                                       [this](const QString& d) { return m_fileKeys.contains(d); });
    if (m_fileKeys.contains(key) || m_directoryKeys.contains(key) || fileClash) {
        *why = QStringLiteral("collides with another carried path");
        return Refused;
    }
    m_exact.insert(path);
    m_fileKeys.insert(key);
    for (qsizetype i = 0; i < directories.size(); ++i) {
        m_directories.insert(directories.at(i));
        m_directoryKeys.insert(directoryKeys.at(i));
    }
    m_paths.append(path);
    return Added;
}

QJsonArray
repair_systems(const QJsonArray& rows) {
    QJsonArray out;
    QSet<QString> seen;
    for (const auto& value : rows) {
        if (!value.isObject()) {
            continue;
        }
        QJsonObject row = value.toObject();
        QString uid = SavedSystemsModel::canonicalUid(variant_string(row.value(QLatin1String("uid"))));
        if (uid.isEmpty() || seen.contains(uid)) {
            uid = new_uid();
        }
        seen.insert(uid);
        row.insert(QLatin1String("uid"), uid);
        out.append(row);
    }
    return out;
}

QJsonArray
repair_scan_lists(const QJsonArray& rows) {
    QJsonArray out;
    QSet<QString> seen;
    for (const auto& value : rows) {
        if (!value.isObject()) {
            continue;
        }
        QJsonObject row = value.toObject();
        QString uid = variant_string(row.value(QLatin1String("uid")));
        if (uid.isEmpty() || seen.contains(uid)) {
            uid = new_uid();
        }
        seen.insert(uid);
        row.insert(QLatin1String("uid"), uid);
        if (row.contains(QLatin1String("entries"))) {
            row.insert(QLatin1String("entries"), repair_scan_entries(row.value(QLatin1String("entries"))));
        }
        out.append(row);
    }
    return out;
}

void
strip_compiled(QJsonObject& row) {
    row.remove(QLatin1String("compiled"));
    row.remove(QLatin1String("runtimeRef"));
    row.remove(QLatin1String("pastRuntimeRefs"));
}

QJsonArray
repair_profiles(const QJsonArray& rows) {
    QJsonArray out;
    QSet<QString> seen;
    for (const auto& value : rows) {
        QJsonObject row = value.toObject();
        const QString uid = variant_string(row.value(QLatin1String("uid")));
        if (!value.isObject() || !DecryptionProfilesModel::validUid(uid) || seen.contains(uid)) {
            continue;
        }
        seen.insert(uid);
        row.insert(QLatin1String("uid"), uid);
        strip_compiled(row);
        out.append(row);
    }
    return out;
}

QJsonArray
repair_ranges(const QJsonArray& rows) {
    QJsonArray out;
    for (const auto& value : rows) {
        QVariantMap range;
        if (value.isObject() && SignalSearchModel::storedRangeRow(value.toObject().toVariantMap(), &range)) {
            out.append(QJsonObject::fromVariantMap(range));
        }
    }
    return out;
}

bool
validate_store(const QString& store, QJsonArray& rows, QString* why) {
    if (store == QLatin1String(kSavedSystemsStore)) {
        return validate_systems(rows, why);
    }
    if (store == QLatin1String(kScanListsStore)) {
        return validate_scan_lists(rows, why);
    }
    if (store == QLatin1String(kImportedFilesStore)) {
        return validate_library(rows, why);
    }
    if (store == QLatin1String(kDecryptionProfilesStore)) {
        return validate_profiles(rows, why);
    }
    if (store == QLatin1String(kSignalSearchRangesStore)) {
        return validate_ranges(rows, why);
    }
    *why = QStringLiteral("not a store a backup carries");
    return false;
}

Summary
summarize(const QMap<QString, QJsonArray>& stores, const QVariantMap& prefs, int avoids) {
    Summary summary;
    const QJsonArray systems = stores.value(QLatin1String(kSavedSystemsStore));
    summary.systems = static_cast<int>(systems.size());
    summary.scanLists = static_cast<int>(stores.value(QLatin1String(kScanListsStore)).size());
    summary.libraryFiles = static_cast<int>(stores.value(QLatin1String(kImportedFilesStore)).size());
    summary.profiles = static_cast<int>(stores.value(QLatin1String(kDecryptionProfilesStore)).size());
    summary.ranges = static_cast<int>(stores.value(QLatin1String(kSignalSearchRangesStore)).size());
    summary.avoids = avoids;
    for (const auto& value : systems) {
        const QJsonObject row = value.toObject();
        if (row.value(QLatin1String("sourceType")).toString() == QLatin1String("file")) {
            summary.recordingsToChoose++;
        }
        // The session builder splits a decode flag into argv like the extra arguments, so a flag carrying a token the
        // app itself never writes passes decoder options too.
        const bool extra = !variant_string(row.value(QLatin1String("extraArgs"))).trimmed().isEmpty()
                           || !decode_flag_is_standard(variant_string(row.value(QLatin1String("decodeFlag"))));
        if (extra) {
            summary.systemsWithExtraArgs++;
        }
    }
    summary.appExtraArgs = !prefs.value(QStringLiteral("decode/extraArgs")).toString().trimmed().isEmpty();
    return summary;
}

QString
row_label(const QJsonObject& row) {
    for (const char* key : {"name", "label"}) {
        const QString label = row.value(QLatin1String(key)).toString().trimmed();
        if (!label.isEmpty()) {
            return label;
        }
    }
    return QObject::tr("Unnamed");
}

} // namespace detail
} // namespace backup
} // namespace dsd_qt
