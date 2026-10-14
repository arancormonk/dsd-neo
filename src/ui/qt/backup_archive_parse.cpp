// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Restore side of the backup archive (issue #580): validate an untrusted archive and rebase it onto this device's data
 * root, before anything is written. Pure: it reads no files. */

#include "backup_archive.h"

#include <QByteArray>
#include <QByteArrayView>
#include <QChar>
#include <QCryptographicHash>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QLatin1String>
#include <QList>
#include <QMap>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QtEndian>
#include <QtGlobal>
#include <algorithm>
#include <dsd-neo/runtime/signal_search.h>
#include <stdint.h>
#include "app_prefs.h"
#include "backup_archive_rules.h"

namespace dsd_qt {
namespace backup {

namespace {

using namespace detail;

class Parser {
  public:
    Parser(const QByteArray& archive, const QString& targetRoot) : m_archive(archive), m_target(targetRoot) {}

    ParseResult
    run() {
        const bool ok = checkTarget() && readManifest() && readIdentity() && readSourceRoots() && readOrigin()
                        && readFiles() && readStores() && readPrefs() && readAvoids() && checkTypedPaths()
                        && checkBundles() && checkCoverage() && verifyHashes();
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

    /** The archive path @p value names under the target root, or empty when it names none. */
    QString
    archivePath(const QString& value) const {
        return value.startsWith(m_target + QLatin1Char('/')) ? value.mid(m_target.size() + 1) : QString();
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
        if (field.kind != PathKind::File || path.isEmpty()) {
            return true;
        }
        const QString carried = archivePath(path);
        if (!m_carried.contains(carried)) {
            return fail(BackupError::Damaged, QStringLiteral("typed: %1 %2 names a file the backup does not carry")
                                                  .arg(store, QLatin1String(field.field)));
        }
        m_named.insert(carried);
        return true;
    }

    bool
    checkTypedPaths() {
        for (auto it = m_result.stores.cbegin(); it != m_result.stores.cend(); ++it) {
            for (const auto& value : it.value()) {
                const QJsonObject row = value.toObject();
                for (const auto& field : typed_fields()) {
                    if (is_typed_field(it.key(), field)
                        && !checkTypedValue(it.key(), field, row.value(QLatin1String(field.field)))) {
                        return false;
                    }
                }
            }
        }
        return true;
    }

    /** Validate one library row's bundle root; @p roots collects the claimed roots' folded keys and their rows. */
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
        // Whether the file lies under imports/bundles/ is a clash question, so it is folded: a case-insensitive
        // filesystem puts imports/Bundles/ there too. Whether it lies in its own root is answered exactly.
        const bool bundled = path_key(archivePath(path)).startsWith(QStringLiteral("imports/bundles/"));
        if (bundled && !path.startsWith(root + QLatin1Char('/'))) {
            return fail(BackupError::Damaged, QStringLiteral("typed: a bundled file lies outside its row's bundle"));
        }
        roots->insert(path_key(relative), index);
        m_claimed.insert(relative);
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
            // Folded, like the roots: imports/Bundles/<id>/ is the same directory on case-insensitive storage.
            const QString bundle =
                bundle_of(path_key(archivePath(library.at(i).toObject().value(QLatin1String("path")).toString())));
            const auto owner = bundle.isEmpty() ? roots.constEnd() : roots.constFind(bundle);
            if (owner != roots.constEnd() && owner.value() != i) {
                return fail(BackupError::Damaged, QStringLiteral("typed: a library file lies in another row's bundle"));
            }
        }
        return true;
    }

    /**
     * Every carried file is one a path field names or one inside a claimed bundle, exactly as a case-sensitive
     * filesystem lays them out. Those are the files a later restore counts as this configuration and replaces; a file
     * in neither would be written now and never removed again.
     */
    bool
    checkCoverage() {
        for (const QString& path : m_carried.paths()) {
            if (!m_named.contains(path) && !m_claimed.contains(bundle_of(path))) {
                return fail(BackupError::Damaged,
                            QStringLiteral("typed: a carried file is named by no path field and lies in no bundle"));
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
    QSet<QString> m_named;   // archive paths a File value names
    QSet<QString> m_claimed; // exact archive paths of the claimed bundle roots
    ParseResult m_result;
};

} // namespace

ParseResult
backup_parse_archive(const QByteArray& archive, const QString& targetRoot) {
    Parser parser(archive, targetRoot);
    return parser.run();
}

} // namespace backup
} // namespace dsd_qt
