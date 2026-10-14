// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "backup_archive.h"

#include <QByteArrayView>
#include <QChar>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QLatin1String>
#include <QObject>
#include <QSet>
#include <QUuid>
#include <QVariant>
#include <QtEndian>
#include <algorithm>
#include <dsd-neo/app_control/signal_search.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/crypto/backup_seal.h>
#include <dsd-neo/runtime/git_ver.h>
#include <dsd-neo/runtime/log.h>
#include <dsd-neo/runtime/signal_search.h>
#include <initializer_list> // IWYU pragma: keep -- braced range-for lists require this header.
#include <iterator>
#include <qflags.h>
#include <stddef.h>
#include <stdint.h>
#include <utility>
#include <vector>
#include "app_prefs.h"
#include "csv_bundle_import.h"
#include "decryption_profiles_model.h"
#include "imported_files_model.h"
#include "json_store.h"
#include "realtime_clock.h"
#include "saved_systems_model.h"
#include "signal_search_model.h"

namespace dsd_qt {
namespace backup {

namespace {

constexpr char kMagic[] = "DNBA";
constexpr qsizetype kHeaderBytes = 8;
constexpr char kFormatName[] = "dsd-neo-backup";
constexpr int kMaxPathComponents = 6;
constexpr int kMaxSourceRoots = 2;
constexpr int kMaxAppTagChars = 256;

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

QString
uuid() {
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

void
wipe(QByteArray& bytes) {
    if (!bytes.isEmpty()) {
        DSD_SECURE_ZERO(bytes.data(), static_cast<size_t>(bytes.size()));
    }
    bytes.clear();
}

/** A JSON value read the way the models read a stored field: through QVariant, so a number becomes its text. */
QString
variant_string(const QJsonValue& value) {
    return value.toVariant().toString();
}

/** Whether @p id may name an owned bundle directory (csv_bundle_import.cpp's ownedRoot() rule). */
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

/** The comparison key for archive paths: equal for paths equal exactly, by case folding or by normalization. */
QString
path_key(const QString& path) {
    return path.normalized(QString::NormalizationForm_D).toCaseFolded().normalized(QString::NormalizationForm_D);
}

/** Whether @p text holds a backslash or a control character, or is not valid UTF-16. */
bool
unsafe_text(const QString& text) {
    if (QString::fromUtf8(text.toUtf8()) != text) {
        return true;
    }
    return std::any_of(text.cbegin(), text.cend(),
                       [](QChar c) { return c == QLatin1Char('\\') || c.category() == QChar::Other_Control; });
}

/** Why @p path may not name a carried file, or empty when it may. */
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

/** The carried file table's paths, refusing anything a restore could not write as one distinct file. */
class CarriedSet {
  public:
    enum Outcome { Added, Present, Refused };

    Outcome
    add(const QString& path, QString* why) {
        if (m_exact.contains(path)) {
            *why = QStringLiteral("listed twice");
            return Present;
        }
        *why = archive_path_problem(path);
        if (!why->isEmpty()) {
            return Refused;
        }
        const QString key = path_key(path);
        QStringList directories;
        for (qsizetype at = key.indexOf(QLatin1Char('/')); at > 0; at = key.indexOf(QLatin1Char('/'), at + 1)) {
            directories.append(key.left(at));
        }
        const bool fileClash = std::any_of(directories.cbegin(), directories.cend(),
                                           [this](const QString& d) { return m_files.contains(d); });
        if (m_files.contains(key) || m_directories.contains(key) || fileClash) {
            *why = QStringLiteral("collides with another carried path");
            return Refused;
        }
        m_exact.insert(path);
        m_files.insert(key);
        for (const QString& directory : directories) {
            m_directories.insert(directory);
        }
        m_paths.append(path);
        return Added;
    }

    bool
    contains(const QString& path) const {
        return m_exact.contains(path);
    }

    /** Whether some carried file lies below the directory @p path. */
    bool
    holdsFilesUnder(const QString& path) const {
        return m_directories.contains(path_key(path));
    }

    const QStringList&
    paths() const {
        return m_paths;
    }

  private:
    QStringList m_paths;
    QSet<QString> m_exact, m_files, m_directories;
};

bool
is_typed_field(const QString& store, const TypedPathField& field) {
    return store == QLatin1String(field.store);
}

/* ---- Store identity: the loaders' own rules. Export repairs what a loader would repair; restore refuses it. ---- */

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
            uid = uuid();
        }
        seen.insert(uid);
        row.insert(QLatin1String("uid"), uid);
        out.append(row);
    }
    return out;
}

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
            uid = uuid();
        }
        seen.insert(uid);
        entry.insert(QLatin1String("uid"), uid);
        out.append(entry);
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
            uid = uuid();
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
    return objects_only(rows, why)
           && unique_uids(
               rows, [](const QString& uid) { return !uid.isEmpty() && SavedSystemsModel::canonicalUid(uid) == uid; },
               why);
}

bool
validate_scan_lists(const QJsonArray& rows, QString* why) {
    const auto nonEmpty = [](const QString& uid) { return !uid.isEmpty(); };
    if (!objects_only(rows, why) || !unique_uids(rows, nonEmpty, why)) {
        return false;
    }
    for (const auto& value : rows) {
        const QJsonValue entries = value.toObject().value(QLatin1String("entries"));
        if (entries.isUndefined()) {
            continue;
        }
        if (!entries.isArray() || !objects_only(entries.toArray(), why)
            || !unique_uids(entries.toArray(), nonEmpty, why)) {
            *why = QStringLiteral("scan entries are malformed or repeat a uid");
            return false;
        }
    }
    return true;
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

/** Validate @p rows of @p store against its model's load rules, normalizing them as the model would store them. */
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
    return validate_ranges(rows, why);
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
        if (!variant_string(row.value(QLatin1String("extraArgs"))).trimmed().isEmpty()) {
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

/* ---- Restore side ---- */

class Parser {
  public:
    Parser(const QByteArray& archive, const QString& targetRoot) : m_archive(archive), m_target(targetRoot) {}

    ParseResult
    run() {
        const bool ok = checkTarget() && readManifest() && readIdentity() && readSourceRoots() && readOrigin()
                        && readFiles() && readStores() && readPrefs() && readAvoids() && checkTypedPaths()
                        && checkBundles() && verifyHashes();
        if (!ok) {
            ParseResult failed;
            failed.error = m_result.error;
            failed.detail = m_result.detail;
            return failed;
        }
        const QString app = m_result.summary.app;
        const qint64 createdAt = m_result.summary.createdAt;
        const QStringList notIncluded = m_result.summary.notIncluded;
        m_result.summary = summarize(m_result.stores, m_result.prefs, static_cast<int>(m_result.avoids.size()));
        m_result.summary.app = app;
        m_result.summary.createdAt = createdAt;
        m_result.summary.notIncluded = notIncluded;
        return m_result;
    }

  private:
    bool
    fail(BackupError error, const QString& detail) {
        m_result.error = error;
        m_result.detail = detail;
        return false;
    }

    /** Every rebased path is built on the target root, so it must be a clean absolute directory. */
    bool
    checkTarget() {
        if (!QDir::isAbsolutePath(m_target) || QDir::cleanPath(m_target) != m_target
            || m_target.endsWith(QLatin1Char('/')) || unsafe_text(m_target)) {
            return fail(BackupError::Io, QStringLiteral("target: not a data root"));
        }
        return true;
    }

    bool
    readManifest() {
        if (m_archive.size() < kHeaderBytes || !m_archive.startsWith(kMagic)) {
            return fail(BackupError::Damaged, QStringLiteral("archive: no archive header"));
        }
        const quint32 length = qFromBigEndian<quint32>(m_archive.constData() + 4);
        if (length > kMaxManifestBytes) {
            return fail(BackupError::TooLarge, QStringLiteral("manifest: larger than the manifest cap"));
        }
        if (static_cast<qint64>(length) > m_archive.size() - kHeaderBytes) {
            return fail(BackupError::Damaged, QStringLiteral("archive: the manifest runs past the end"));
        }
        // A view, not a copy: the manifest carries key material.
        const QByteArray json = QByteArray::fromRawData(m_archive.constData() + kHeaderBytes, length);
        QJsonParseError error{};
        const QJsonDocument document = QJsonDocument::fromJson(json, &error);
        if (error.error != QJsonParseError::NoError || !document.isObject()) {
            return fail(BackupError::Damaged, QStringLiteral("manifest: not a JSON object"));
        }
        m_manifest = document.object();
        m_dataOffset = kHeaderBytes + static_cast<qint64>(length);
        return true;
    }

    bool
    readIdentity() {
        if (m_manifest.value(QLatin1String("format")).toString() != QLatin1String(kFormatName)) {
            return fail(BackupError::NotBackup, QStringLiteral("manifest: not a DSD-neo backup"));
        }
        const QJsonValue version = m_manifest.value(QLatin1String("version"));
        const qint64 number = version.isDouble() ? version.toInteger(-1) : -1;
        if (number > kFormatVersion) {
            return fail(BackupError::NewerVersion, QStringLiteral("manifest: made by a newer DSD-neo"));
        }
        if (number != kFormatVersion) {
            return fail(BackupError::Damaged, QStringLiteral("manifest: malformed version"));
        }
        return true;
    }

    /** An absolute, clean directory other than the filesystem root: rebasing strips it as a prefix. */
    static bool
    validSourceRoot(const QJsonValue& value) {
        const QString root = value.toString();
        return value.isString() && QDir::isAbsolutePath(root) && QDir::cleanPath(root) == root
               && !root.endsWith(QLatin1Char('/')) && !unsafe_text(root);
    }

    bool
    readSourceRoots() {
        const QJsonArray roots = m_manifest.value(QLatin1String("sourceRoots")).toArray();
        if (roots.isEmpty() || roots.size() > kMaxSourceRoots
            || !std::all_of(roots.cbegin(), roots.cend(), validSourceRoot)) {
            return fail(BackupError::Damaged, QStringLiteral("manifest: malformed source roots"));
        }
        for (const auto& root : roots) {
            m_sourceRoots.append(root.toString());
        }
        // The longest root first, so a root nested in another rebases by its own prefix.
        std::sort(m_sourceRoots.begin(), m_sourceRoots.end(),
                  [](const QString& a, const QString& b) { return a.size() > b.size(); });
        return true;
    }

    bool
    readOrigin() {
        const QJsonValue createdAt = m_manifest.value(QLatin1String("createdAt"));
        const QJsonValue app = m_manifest.value(QLatin1String("app"));
        const QJsonValue notIncluded = m_manifest.value(QLatin1String("notIncluded"));
        const QJsonArray lines = notIncluded.toArray();
        if (!createdAt.isDouble() || createdAt.toInteger(-1) < 0 || !app.isString()
            || app.toString().size() > kMaxAppTagChars || !(notIncluded.isUndefined() || notIncluded.isArray())
            || lines.size() > kMaxRowsPerStore
            || !std::all_of(lines.cbegin(), lines.cend(), [](const QJsonValue& v) { return v.isString(); })) {
            return fail(BackupError::Damaged, QStringLiteral("manifest: malformed origin"));
        }
        m_result.summary.createdAt = createdAt.toInteger();
        m_result.summary.app = app.toString();
        for (const auto& line : lines) {
            m_result.summary.notIncluded.append(line.toString());
        }
        return true;
    }

    static bool
    validSha256(const QJsonValue& value) {
        const QString hex = value.toString();
        return value.isString() && hex.size() == 64 && std::all_of(hex.cbegin(), hex.cend(), [](QChar c) {
                   return (c >= QLatin1Char('0') && c <= QLatin1Char('9'))
                          || (c >= QLatin1Char('a') && c <= QLatin1Char('f'));
               });
    }

    bool
    readFile(const QJsonValue& value, qint64* offset) {
        const QJsonObject entry = value.toObject();
        const QJsonValue size = entry.value(QLatin1String("size"));
        const qint64 bytes = size.isDouble() ? size.toInteger(-1) : -1;
        if (!value.isObject() || !entry.value(QLatin1String("path")).isString() || bytes < 0
            || !validSha256(entry.value(QLatin1String("sha256")))) {
            return fail(BackupError::Damaged, QStringLiteral("files: malformed file entry"));
        }
        if (bytes > kMaxFileBytes) {
            return fail(BackupError::TooLarge, QStringLiteral("files: a file is larger than the file cap"));
        }
        const QString path = entry.value(QLatin1String("path")).toString();
        QString why;
        if (m_carried.add(path, &why) != CarriedSet::Added) {
            return fail(BackupError::Damaged, QStringLiteral("path: ") + why);
        }
        if (bytes > m_archive.size() - *offset) {
            return fail(BackupError::Damaged, QStringLiteral("files: file data runs past the end"));
        }
        m_result.files.append({path, *offset, bytes, entry.value(QLatin1String("sha256")).toString()});
        *offset += bytes;
        return true;
    }

    bool
    readFiles() {
        const QJsonValue files = m_manifest.value(QLatin1String("files"));
        if (!files.isArray()) {
            return fail(BackupError::Damaged, QStringLiteral("files: no file table"));
        }
        const QJsonArray table = files.toArray();
        if (table.size() > kMaxFiles) {
            return fail(BackupError::TooLarge, QStringLiteral("files: more files than the file count cap"));
        }
        qint64 offset = m_dataOffset;
        for (const auto& value : table) {
            if (!readFile(value, &offset)) {
                return false;
            }
        }
        if (offset != m_archive.size()) {
            return fail(BackupError::Damaged, QStringLiteral("files: file data does not fill the archive"));
        }
        return true;
    }

    QString
    rebase(const QString& value) const {
        for (const QString& root : m_sourceRoots) {
            if (value == root) {
                return m_target;
            }
            if (value.startsWith(root + QLatin1Char('/'))) {
                return m_target + value.mid(root.size());
            }
        }
        return value;
    }

    bool
    readStores() {
        const QJsonValue stores = m_manifest.value(QLatin1String("stores"));
        if (!stores.isObject()) {
            return fail(BackupError::Damaged, QStringLiteral("store: no stores"));
        }
        const QJsonObject object = stores.toObject();
        for (const QString& store : backup_store_names()) {
            if (!object.contains(store)) {
                continue;
            }
            if (!object.value(store).isArray()) {
                return fail(BackupError::Damaged, QStringLiteral("store: %1 is not an array").arg(store));
            }
            QJsonArray rows = object.value(store).toArray();
            if (rows.size() > kMaxRowsPerStore) {
                return fail(BackupError::TooLarge, QStringLiteral("store: %1 has too many rows").arg(store));
            }
            rows = map_typed_path_values(store, rows, [this](const TypedPathValue& v) { return rebase(v.value); });
            QString why;
            if (!validate_store(store, rows, &why)) {
                return fail(BackupError::Damaged, QStringLiteral("store: %1: %2").arg(store, why));
            }
            m_result.stores.insert(store, rows);
        }
        return true;
    }

    bool
    readPrefs() {
        const QJsonValue prefs = m_manifest.value(QLatin1String("prefs"));
        if (!prefs.isObject()) {
            return fail(BackupError::Damaged, QStringLiteral("prefs: not an object"));
        }
        const QJsonObject object = prefs.toObject();
        for (const QString& key : AppPrefs::portableKeys()) {
            if (!object.contains(key)) {
                continue;
            }
            const QJsonValue value = object.value(key);
            if (!value.isString() && !value.isDouble() && !value.isBool()) {
                return fail(BackupError::Damaged, QStringLiteral("prefs: %1 is not a scalar").arg(key));
            }
            m_result.prefs.insert(key, value.toVariant());
        }
        return true;
    }

    bool
    readAvoids() {
        const QJsonValue avoids = m_manifest.value(QLatin1String("avoids"));
        const QJsonArray list = avoids.toArray();
        if (!avoids.isArray() || list.size() > DSD_SEARCH_MAX_AVOIDS) {
            return fail(BackupError::Damaged, QStringLiteral("avoids: not a list the avoid store takes"));
        }
        for (const auto& value : list) {
            const qint64 frequency = value.isDouble() ? value.toInteger(-1) : -1;
            if (frequency < 1 || frequency > static_cast<qint64>(UINT32_MAX)) {
                return fail(BackupError::Damaged, QStringLiteral("avoids: frequency out of range"));
            }
            m_result.avoids.append(static_cast<quint32>(frequency));
        }
        return true;
    }

    /** Whether @p value names a carried file under the target root. */
    bool
    names_carried_file(const QString& value) const {
        return value.startsWith(m_target + QLatin1Char('/')) && m_carried.contains(value.mid(m_target.size() + 1));
    }

    bool
    checkTypedValue(const QString& store, const TypedPathField& field, const QJsonValue& value) {
        if (value.isUndefined()) {
            return true;
        }
        const QString path = value.toString();
        if (!value.isString()) {
            return fail(BackupError::Damaged,
                        QStringLiteral("typed: %1 %2 is not a string").arg(store, QLatin1String(field.field)));
        }
        if (field.kind == PathKind::Recording && !path.isEmpty()) {
            return fail(BackupError::Damaged, QStringLiteral("typed: a recording is never carried"));
        }
        if (field.kind == PathKind::File && !path.isEmpty() && !names_carried_file(path)) {
            return fail(BackupError::Damaged, QStringLiteral("typed: %1 %2 names a file the backup does not carry")
                                                  .arg(store, QLatin1String(field.field)));
        }
        return true;
    }

    bool
    checkTypedPaths() {
        for (auto it = m_result.stores.cbegin(); it != m_result.stores.cend(); ++it) {
            for (const auto& value : it.value()) {
                const QJsonObject row = value.toObject();
                for (const auto& field : kTypedPathFields) {
                    if (is_typed_field(it.key(), field)
                        && !checkTypedValue(it.key(), field, row.value(QLatin1String(field.field)))) {
                        return false;
                    }
                }
            }
        }
        return true;
    }

    /** Validate one library row's bundle root; @p roots collects the claimed roots' keys and their rows. */
    bool
    checkBundle(const QJsonObject& row, int index, QMap<QString, int>* roots) {
        const QString path = row.value(QLatin1String("path")).toString();
        if (path.isEmpty()) {
            return fail(BackupError::Damaged, QStringLiteral("typed: a library row names no file"));
        }
        const QString root = row.value(QLatin1String("bundleRoot")).toString();
        if (root.isEmpty()) {
            return true;
        }
        const QString bundles = m_target + QStringLiteral("/imports/bundles/");
        if (!root.startsWith(bundles) || !owned_name(root.mid(bundles.size()))) {
            return fail(BackupError::Damaged, QStringLiteral("typed: a bundle root is not an owned bundle directory"));
        }
        const QString relative = root.mid(m_target.size() + 1);
        if (!m_carried.holdsFilesUnder(relative)) {
            return fail(BackupError::Damaged, QStringLiteral("typed: a bundle root holds no carried file"));
        }
        if (roots->contains(path_key(relative))) {
            return fail(BackupError::Damaged, QStringLiteral("typed: two library rows share a bundle root"));
        }
        if (path.startsWith(bundles) && !path.startsWith(root + QLatin1Char('/'))) {
            return fail(BackupError::Damaged, QStringLiteral("typed: a bundled file lies outside its row's bundle"));
        }
        roots->insert(path_key(relative), index);
        return true;
    }

    bool
    checkBundles() {
        const QJsonArray library = m_result.stores.value(QLatin1String(kImportedFilesStore));
        QMap<QString, int> roots;
        for (int i = 0; i < library.size(); ++i) {
            if (!checkBundle(library.at(i).toObject(), i, &roots)) {
                return false;
            }
        }
        // Removing a row deletes its whole bundle, so no other row's file may live inside one.
        for (int i = 0; i < library.size(); ++i) {
            const QString file =
                path_key(library.at(i).toObject().value(QLatin1String("path")).toString().mid(m_target.size() + 1));
            for (auto root = roots.cbegin(); root != roots.cend(); ++root) {
                if (root.value() != i && file.startsWith(root.key() + QLatin1Char('/'))) {
                    return fail(BackupError::Damaged,
                                QStringLiteral("typed: a library file lies in another row's bundle"));
                }
            }
        }
        return true;
    }

    bool
    verifyHashes() {
        for (const ArchiveFile& file : m_result.files) {
            const QByteArrayView bytes(m_archive.constData() + file.offset, file.size);
            const QByteArray hash = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
            if (QString::fromLatin1(hash) != file.sha256) {
                return fail(BackupError::Damaged, QStringLiteral("files: a file does not match its sha256"));
            }
        }
        return true;
    }

    const QByteArray& m_archive;
    QString m_target;
    QJsonObject m_manifest;
    qint64 m_dataOffset = 0;
    QStringList m_sourceRoots;
    CarriedSet m_carried;
    ParseResult m_result;
};

/* ---- Export side ---- */

class Builder {
  public:
    Builder(const Snapshot& snapshot, const std::atomic<bool>* cancel)
        : m_snapshot(snapshot), m_cancel(cancel), m_root(snapshot.root), m_canonicalRoot(snapshot.canonicalRoot) {}

    BuildResult
    run() {
        if (m_root.isEmpty()) {
            fail(BackupError::Io, QStringLiteral("snapshot: no data root"));
            return m_result;
        }
        normalizeStores();
        if (!checkRowCaps()) {
            return m_result;
        }
        sanitizeLibrary();
        for (const char* store : {kSavedSystemsStore, kScanListsStore, kDecryptionProfilesStore}) {
            sanitizeStore(QLatin1String(store));
        }
        QByteArray archive;
        if (!readFiles() || !assemble(&archive) || !selfCheck(archive)) {
            wipe(archive);
            return m_result;
        }
        m_result.archive = std::move(archive);
        return m_result;
    }

  private:
    bool
    fail(BackupError error, const QString& detail) {
        m_result.error = error;
        m_result.detail = detail;
        return false;
    }

    bool
    cancelled() {
        if (m_cancel != nullptr && m_cancel->load()) {
            m_result.cancelled = true;
            return true;
        }
        return false;
    }

    void
    notInclude(const QString& line) {
        // Bounded like every manifest list; the first lines say enough.
        if (m_notIncluded.size() < kMaxRowsPerStore && !m_notIncluded.contains(line)) {
            m_notIncluded.append(line);
        }
    }

    void
    normalizeStores() {
        for (const QString& store : backup_store_names()) {
            m_stores.insert(store, m_snapshot.stores.value(store));
        }
        auto& stores = m_stores;
        stores[QLatin1String(kSavedSystemsStore)] = repair_systems(stores.value(QLatin1String(kSavedSystemsStore)));
        stores[QLatin1String(kScanListsStore)] = repair_scan_lists(stores.value(QLatin1String(kScanListsStore)));
        stores[QLatin1String(kDecryptionProfilesStore)] =
            repair_profiles(stores.value(QLatin1String(kDecryptionProfilesStore)));
        stores[QLatin1String(kSignalSearchRangesStore)] =
            repair_ranges(stores.value(QLatin1String(kSignalSearchRangesStore)));
        for (const QString& key : AppPrefs::portableKeys()) {
            const auto it = m_snapshot.prefs.constFind(key);
            const QJsonValue value = it == m_snapshot.prefs.constEnd() ? QJsonValue() : QJsonValue::fromVariant(*it);
            if (value.isString() || value.isDouble() || value.isBool()) {
                m_prefs.insert(key, value);
            }
        }
    }

    bool
    checkRowCaps() {
        for (auto it = m_stores.cbegin(); it != m_stores.cend(); ++it) {
            if (it.value().size() > kMaxRowsPerStore) {
                return fail(BackupError::TooLarge, QStringLiteral("store: %1 has too many rows").arg(it.key()));
            }
        }
        return true;
    }

    /** Carry @p canonical (inside the canonical root) as an archive path; empty when it cannot be carried. */
    QString
    carryCanonical(const QString& canonical) {
        QString relative = canonical.mid(m_canonicalRoot.size() + 1);
        QString why;
        if (m_carried.add(relative, &why) == CarriedSet::Refused) {
            return {};
        }
        m_sources.insert(relative, canonical);
        return relative;
    }

    /** The archive path for a typed File value, carrying it; empty when it is not a file a backup may carry. */
    QString
    carryFile(const QString& value) {
        if (value.isEmpty() || !QDir::isAbsolutePath(value) || m_canonicalRoot.isEmpty()) {
            return {};
        }
        const QFileInfo info(value);
        if (info.isSymLink() || info.isJunction() || !info.isFile() || staging_name(info.fileName())) {
            return {};
        }
        const QString canonical = info.canonicalFilePath();
        if (!canonical.startsWith(m_canonicalRoot + QStringLiteral("/imports/"))) {
            return {};
        }
        return carryCanonical(canonical);
    }

    /** Whether @p info is an owned bundle root (csv_bundle_import.cpp's ownedRoot(), with symlinks resolved). */
    bool
    ownedBundle(const QFileInfo& info, const QString& relative) const {
        const QString parent = info.absolutePath();
        const bool underBundles = parent == m_root + QStringLiteral("/imports/bundles")
                                  || parent == m_canonicalRoot + QStringLiteral("/imports/bundles");
        return underBundles && owned_name(info.fileName()) && !info.isSymLink() && !info.isJunction() && info.isDir()
               && info.canonicalFilePath() == m_canonicalRoot + QLatin1Char('/') + relative;
    }

    /** Carry an owned bundle root's regular files; the root's archive path, or empty when it carries none. */
    QString
    carryBundle(const QString& root, const QString& label) {
        const QFileInfo info(root);
        QString relative = QStringLiteral("imports/bundles/") + info.fileName();
        if (m_canonicalRoot.isEmpty() || !ownedBundle(info, relative) || m_claimedRoots.contains(path_key(relative))) {
            return {};
        }
        const QString directory = info.canonicalFilePath();
        int carried = 0;
        QDirIterator it(directory, QDir::Files | QDir::Hidden | QDir::NoSymLinks, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QFileInfo file(it.next());
            const QString canonical = file.canonicalFilePath();
            if (!file.isFile() || file.isSymLink() || staging_name(file.fileName())
                || !canonical.startsWith(directory + QLatin1Char('/'))) {
                continue;
            }
            if (carryCanonical(canonical).isEmpty()) {
                notInclude(QObject::tr("%1: %2").arg(label, file.fileName()));
            } else {
                carried++;
            }
        }
        if (carried == 0) {
            return {};
        }
        m_claimedRoots.insert(path_key(relative), label);
        return relative;
    }

    /** A library row with its file and bundle carried, or false (listed) when the row cannot be. */
    bool
    sanitizeLibraryRow(QJsonObject& row, QSet<QString>* paths) {
        const QString label = row_label(row);
        const bool known = ImportedFilesModel::knownTypes().contains(row.value(QLatin1String("type")).toString());
        const QString file = known ? carryFile(variant_string(row.value(QLatin1String("path")))) : QString();
        if (file.isEmpty()) {
            notInclude(QObject::tr("Imported file %1").arg(label));
            return false;
        }
        if (paths->contains(file)) {
            return false; // a second row for the same file: the loader would show it twice
        }
        paths->insert(file);
        row.insert(QLatin1String("path"), m_root + QLatin1Char('/') + file);
        const QString root = variant_string(row.value(QLatin1String("bundleRoot")));
        QString carried;
        if (!root.isEmpty()) {
            // A bundled file belongs to its own row's bundle; one inside some other bundle keeps none.
            const QString own = QStringLiteral("imports/bundles/") + QFileInfo(root).fileName() + QLatin1Char('/');
            const bool fits = !file.startsWith(QStringLiteral("imports/bundles/")) || file.startsWith(own);
            carried = fits ? carryBundle(root, label) : QString();
            if (carried.isEmpty()) {
                notInclude(QObject::tr("%1: companion files").arg(label));
            }
        }
        row.insert(QLatin1String("bundleRoot"), carried.isEmpty() ? QString() : m_root + QLatin1Char('/') + carried);
        return true;
    }

    void
    sanitizeLibrary() {
        QJsonArray kept;
        QSet<QString> paths;
        for (const auto& value : m_stores.value(QLatin1String(kImportedFilesStore))) {
            QJsonObject row = value.toObject();
            if (value.isObject() && sanitizeLibraryRow(row, &paths)) {
                kept.append(row);
            }
        }
        // Removing a row deletes its whole bundle, so a file inside another row's bundle cannot keep a row of its own.
        QJsonArray library;
        for (const auto& value : kept) {
            const QJsonObject row = value.toObject();
            const QString file = path_key(row.value(QLatin1String("path")).toString().mid(m_root.size() + 1));
            const QString own = path_key(row.value(QLatin1String("bundleRoot")).toString().mid(m_root.size() + 1));
            bool foreign = false;
            for (auto root = m_claimedRoots.cbegin(); root != m_claimedRoots.cend(); ++root) {
                foreign = foreign || (root.key() != own && file.startsWith(root.key() + QLatin1Char('/')));
            }
            if (foreign) {
                notInclude(QObject::tr("Imported file %1").arg(row_label(row)));
                continue;
            }
            if (row.value(QLatin1String("type")).toString() == QLatin1String("chan")
                && row.value(QLatin1String("bundleRoot")).toString().isEmpty()) {
                m_channelMaps.insert(file, row_label(row));
            }
            library.append(row);
        }
        m_stores.insert(QLatin1String(kImportedFilesStore), library);
    }

    QString
    sanitizedValue(const TypedPathField& field, const QString& value, const QString& label) {
        if (field.kind != PathKind::File || value.isEmpty()) {
            return QString(); // recordings are never carried
        }
        const QString file = carryFile(value);
        if (file.isEmpty()) {
            const QString name = QFileInfo(value).fileName();
            notInclude(QObject::tr("%1: %2").arg(label, name.isEmpty() ? value : name));
            return QString();
        }
        return m_root + QLatin1Char('/') + file;
    }

    void
    sanitizeStore(const QString& store) {
        QJsonArray rows = m_stores.value(store);
        for (qsizetype i = 0; i < rows.size(); ++i) {
            QJsonObject row = rows.at(i).toObject();
            for (const auto& field : kTypedPathFields) {
                const QString key = QLatin1String(field.field);
                if (is_typed_field(store, field) && row.contains(key)) {
                    row.insert(key, sanitizedValue(field, variant_string(row.value(key)), row_label(row)));
                }
            }
            rows.replace(i, row);
        }
        m_stores.insert(store, rows);
    }

    void
    listAbsoluteReferences(const QString& relative, const QByteArray& bytes) {
        const auto map = m_channelMaps.constFind(path_key(relative));
        if (map == m_channelMaps.constEnd()) {
            return;
        }
        for (const QString& reference : csv_companion_references(QString::fromUtf8(bytes))) {
            if (QDir::isAbsolutePath(reference)) {
                notInclude(QObject::tr("%1: %2 (named by full path)").arg(*map, QFileInfo(reference).fileName()));
            }
        }
    }

    bool
    readFile(const QString& relative, qint64* total) {
        QFile file(m_sources.value(relative));
        if (!file.open(QIODevice::ReadOnly)) {
            return fail(BackupError::Io, QStringLiteral("read: %1").arg(relative));
        }
        if (file.size() > kMaxFileBytes) {
            return fail(BackupError::TooLarge, QStringLiteral("files: %1 is larger than the file cap").arg(relative));
        }
        QByteArray bytes = file.read(kMaxFileBytes + 1);
        if (file.error() != QFileDevice::NoError || bytes.size() > kMaxFileBytes) {
            wipe(bytes);
            return fail(file.error() != QFileDevice::NoError ? BackupError::Io : BackupError::TooLarge,
                        QStringLiteral("read: %1").arg(relative));
        }
        *total += bytes.size();
        listAbsoluteReferences(relative, bytes);
        m_fileTable.append(
            QJsonObject{{QStringLiteral("path"), relative},
                        {QStringLiteral("size"), static_cast<qint64>(bytes.size())},
                        {QStringLiteral("sha256"),
                         QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex())}});
        m_bytes.append(bytes);
        return true;
    }

    bool
    readFiles() {
        if (m_carried.paths().size() > kMaxFiles) {
            return fail(BackupError::TooLarge, QStringLiteral("files: more files than the file count cap"));
        }
        qint64 total = 0;
        for (const QString& relative : m_carried.paths()) {
            if (cancelled()) {
                return false;
            }
            if (!readFile(relative, &total)) {
                return false;
            }
            if (total > kMaxArchiveBytes) {
                return fail(BackupError::TooLarge, QStringLiteral("files: larger than the archive cap"));
            }
        }
        return !cancelled();
    }

    QJsonObject
    manifest() const {
        QJsonArray roots{m_root};
        if (!m_canonicalRoot.isEmpty() && m_canonicalRoot != m_root) {
            roots.append(m_canonicalRoot);
        }
        QJsonObject stores;
        for (auto it = m_stores.cbegin(); it != m_stores.cend(); ++it) {
            stores.insert(it.key(), it.value());
        }
        QJsonArray avoids;
        for (const quint32 frequency : m_snapshot.avoids) {
            avoids.append(static_cast<qint64>(frequency));
        }
        return QJsonObject{{QStringLiteral("format"), QLatin1String(kFormatName)},
                           {QStringLiteral("version"), kFormatVersion},
                           {QStringLiteral("app"), m_snapshot.app},
                           {QStringLiteral("createdAt"), m_snapshot.createdAt},
                           {QStringLiteral("sourceRoots"), roots},
                           {QStringLiteral("prefs"), m_prefs},
                           {QStringLiteral("stores"), stores},
                           {QStringLiteral("avoids"), avoids},
                           {QStringLiteral("files"), m_fileTable},
                           {QStringLiteral("notIncluded"), QJsonArray::fromStringList(m_notIncluded)}};
    }

    bool
    assemble(QByteArray* archive) {
        QByteArray json = QJsonDocument(manifest()).toJson(QJsonDocument::Compact);
        qint64 total = kHeaderBytes + json.size();
        for (const QByteArray& bytes : m_bytes) {
            total += bytes.size();
        }
        if (json.size() > kMaxManifestBytes || total > kMaxArchiveBytes) {
            wipe(json);
            return fail(BackupError::TooLarge, QStringLiteral("archive: larger than the archive cap"));
        }
        archive->reserve(static_cast<qsizetype>(total));
        archive->append(kMagic, 4);
        char length[4];
        qToBigEndian<quint32>(static_cast<quint32>(json.size()), length);
        archive->append(length, 4);
        archive->append(json);
        wipe(json);
        for (QByteArray& bytes : m_bytes) {
            archive->append(bytes);
            wipe(bytes);
        }
        return true;
    }

    /** Every backup must pass its own restore validation; a mismatch here is a bug, not a user error. */
    bool
    selfCheck(const QByteArray& archive) {
        if (cancelled()) {
            return false;
        }
        const ParseResult check = backup_parse_archive(archive, m_root);
        if (check.error != BackupError::None) {
            LOG_WARN("Backup export failed its own restore validation: %s\n", qUtf8Printable(check.detail));
            return fail(BackupError::Damaged, QStringLiteral("self-check: ") + check.detail);
        }
        m_result.summary = check.summary;
        return true;
    }

    const Snapshot& m_snapshot;
    const std::atomic<bool>* m_cancel;
    QString m_root, m_canonicalRoot;
    QMap<QString, QJsonArray> m_stores;
    QJsonObject m_prefs;
    CarriedSet m_carried;
    QMap<QString, QString> m_sources;      // archive path → canonical source path
    QMap<QString, QString> m_claimedRoots; // carried bundle root key → its row's label
    QMap<QString, QString> m_channelMaps;  // key of a carried, unbundled channel map → its row's label
    QStringList m_notIncluded;
    QJsonArray m_fileTable;
    QList<QByteArray> m_bytes;
    BuildResult m_result;
};

} // namespace

QStringList
backup_store_names() {
    return {QLatin1String(kSavedSystemsStore), QLatin1String(kScanListsStore), QLatin1String(kImportedFilesStore),
            QLatin1String(kDecryptionProfilesStore), QLatin1String(kSignalSearchRangesStore)};
}

QString
backup_error_name(BackupError error) {
    switch (error) {
        case BackupError::None: return QStringLiteral("None");
        case BackupError::NotBackup: return QStringLiteral("NotBackup");
        case BackupError::NewerVersion: return QStringLiteral("NewerVersion");
        case BackupError::WrongPassphrase: return QStringLiteral("WrongPassphrase");
        case BackupError::TooLarge: return QStringLiteral("TooLarge");
        case BackupError::Damaged: return QStringLiteral("Damaged");
        case BackupError::NoSpace: return QStringLiteral("NoSpace");
        case BackupError::SessionActive: return QStringLiteral("SessionActive");
        case BackupError::Io: return QStringLiteral("Io");
    }
    return QStringLiteral("Io");
}

BackupError
backup_error_from_seal_status(int status) {
    switch (status) {
        case DSD_BACKUP_OK: return BackupError::None;
        case DSD_BACKUP_ERR_FORMAT: return BackupError::NotBackup;
        case DSD_BACKUP_ERR_VERSION: return BackupError::NewerVersion;
        case DSD_BACKUP_ERR_AUTH: return BackupError::WrongPassphrase;
        case DSD_BACKUP_ERR_TOO_LARGE: return BackupError::TooLarge;
        case DSD_BACKUP_ERR_PARAMS: return BackupError::Damaged;
        default: return BackupError::Io;
    }
}

QList<TypedPathField>
typed_path_fields() {
    return QList<TypedPathField>(std::begin(kTypedPathFields), std::end(kTypedPathFields));
}

QList<TypedPathValue>
typed_path_values(const QString& store, const QJsonArray& rows) {
    QList<TypedPathValue> values;
    for (int i = 0; i < rows.size(); ++i) {
        const QJsonObject row = rows.at(i).toObject();
        for (const auto& field : kTypedPathFields) {
            const QJsonValue value = row.value(QLatin1String(field.field));
            if (is_typed_field(store, field) && value.isString() && !value.toString().isEmpty()) {
                values.append({i, field, value.toString()});
            }
        }
    }
    return values;
}

QJsonArray
map_typed_path_values(const QString& store, const QJsonArray& rows,
                      const std::function<QString(const TypedPathValue&)>& map) {
    QJsonArray out = rows;
    for (const TypedPathValue& value : typed_path_values(store, rows)) {
        const QString mapped = map(value);
        if (mapped != value.value) {
            QJsonObject row = out.at(value.row).toObject();
            row.insert(QLatin1String(value.field.field), mapped);
            out.replace(value.row, row);
        }
    }
    return out;
}

QVariantMap
summary_to_map(const Summary& summary) {
    return {{QStringLiteral("systems"), summary.systems},
            {QStringLiteral("scanLists"), summary.scanLists},
            {QStringLiteral("libraryFiles"), summary.libraryFiles},
            {QStringLiteral("profiles"), summary.profiles},
            {QStringLiteral("ranges"), summary.ranges},
            {QStringLiteral("avoids"), summary.avoids},
            {QStringLiteral("recordingsToChoose"), summary.recordingsToChoose},
            {QStringLiteral("systemsWithExtraArgs"), summary.systemsWithExtraArgs},
            {QStringLiteral("appExtraArgs"), summary.appExtraArgs},
            {QStringLiteral("notIncluded"), summary.notIncluded},
            {QStringLiteral("createdAt"), summary.createdAt},
            {QStringLiteral("app"), summary.app}};
}

Snapshot
backup_take_snapshot() {
    Snapshot snapshot;
    snapshot.root = json_store_root();
    snapshot.canonicalRoot = QFileInfo(snapshot.root).canonicalFilePath();
    snapshot.prefs = AppPrefs::readPortable();
    for (const QString& store : backup_store_names()) {
        snapshot.stores.insert(store, json_store_load_array(store));
    }
    std::vector<uint32_t> avoids(DSD_SEARCH_MAX_AVOIDS);
    const size_t count = dsd_app_signal_search_avoids_get(avoids.data(), avoids.size());
    for (size_t i = 0; i < count; ++i) {
        snapshot.avoids.append(avoids[i]);
    }
    snapshot.app = QString::fromUtf8(GIT_TAG);
    snapshot.createdAt = realtimeSecsSinceEpoch();
    return snapshot;
}

BuildResult
backup_build_archive(const Snapshot& snapshot, const std::atomic<bool>* cancel) {
    Builder builder(snapshot, cancel);
    return builder.run();
}

ParseResult
backup_parse_archive(const QByteArray& archive, const QString& targetRoot) {
    Parser parser(archive, targetRoot);
    return parser.run();
}

QByteArray
backup_passphrase_bytes(const QString& passphrase) {
    return passphrase.normalized(QString::NormalizationForm_C).toUtf8();
}

bool
backup_passphrase_ok(const QString& passphrase) {
    const QString composed = passphrase.normalized(QString::NormalizationForm_C);
    qsizetype points = 0;
    for (qsizetype i = 0; i < composed.size(); ++i) {
        if (composed.at(i).isHighSurrogate() && i + 1 < composed.size() && composed.at(i + 1).isLowSurrogate()) {
            ++i;
        }
        ++points;
    }
    return points >= kMinPassphraseChars;
}

QByteArray
backup_seal(const QByteArray& archive, const QString& passphrase, const dsd_backup_kdf_params& kdf, int* status) {
    QByteArray pass = backup_passphrase_bytes(passphrase);
    const size_t capacity = dsd_backup_sealed_size(static_cast<size_t>(archive.size()));
    QByteArray sealed;
    int rc = DSD_BACKUP_ERR_TOO_LARGE;
    if (capacity != 0U) {
        sealed.resize(static_cast<qsizetype>(capacity));
        size_t length = 0;
        rc = dsd_backup_seal(reinterpret_cast<const uint8_t*>(pass.constData()), static_cast<size_t>(pass.size()), &kdf,
                             reinterpret_cast<const uint8_t*>(archive.constData()), static_cast<size_t>(archive.size()),
                             reinterpret_cast<uint8_t*>(sealed.data()), capacity, &length);
        sealed.resize(rc == DSD_BACKUP_OK ? static_cast<qsizetype>(length) : 0);
    }
    wipe(pass);
    if (status != nullptr) {
        *status = rc;
    }
    return sealed;
}

QByteArray
backup_open(const QByteArray& sealed, const QString& passphrase, int* status) {
    size_t payload = 0;
    int rc = dsd_backup_parse_header(reinterpret_cast<const uint8_t*>(sealed.constData()),
                                     static_cast<size_t>(sealed.size()), nullptr, &payload);
    QByteArray archive;
    if (rc == DSD_BACKUP_OK) {
        QByteArray pass = backup_passphrase_bytes(passphrase);
        archive.resize(static_cast<qsizetype>(payload));
        size_t length = 0;
        rc = dsd_backup_open(reinterpret_cast<const uint8_t*>(pass.constData()), static_cast<size_t>(pass.size()),
                             reinterpret_cast<const uint8_t*>(sealed.constData()), static_cast<size_t>(sealed.size()),
                             reinterpret_cast<uint8_t*>(archive.data()), payload, &length);
        wipe(pass);
        if (rc == DSD_BACKUP_OK) {
            archive.resize(static_cast<qsizetype>(length));
        } else {
            wipe(archive);
        }
    }
    if (status != nullptr) {
        *status = rc;
    }
    return archive;
}

} // namespace backup
} // namespace dsd_qt
