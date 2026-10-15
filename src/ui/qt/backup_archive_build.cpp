// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Export side of the backup archive (issue #580): sanitize a copy of the stores so the backup passes its own restore
 * validation, then lay out the archive. Worker-safe: it reads only the filesystem. */

#include "backup_archive.h"

#include <QByteArray>
#include <QByteArrayView>
#include <QChar>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLatin1String>
#include <QList>
#include <QMap>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <Qt>
#include <QtEndian>
#include <QtGlobal>
#include <atomic>
#include <dsd-neo/runtime/log.h>
#include <initializer_list> // IWYU pragma: keep -- braced range-for lists require this header.
#include <qflags.h>
#include <utility>
#include "app_prefs.h"
#include "backup_archive_rules.h"
#include "csv_bundle_import.h"
#include "imported_files_model.h"

namespace dsd_qt {
namespace backup {

namespace {

using namespace detail;

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
        dropUncoveredFiles();
        QByteArray archive;
        if (!measureFiles() || !assemble(&archive) || !selfCheck(archive)) {
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
        m_claimedRoots.insert(path_key(relative), relative);
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
            // A bundled file belongs to its own row's bundle; one anywhere else under bundles/ keeps none.
            const QString own = QStringLiteral("imports/bundles/") + QFileInfo(root).fileName() + QLatin1Char('/');
            const bool fits = !path_key(file).startsWith(QStringLiteral("imports/bundles/")) || file.startsWith(own);
            carried = fits ? carryBundle(root, label) : QString();
            if (carried.isEmpty()) {
                notInclude(QObject::tr("%1: companion files").arg(label));
            }
        }
        row.insert(QLatin1String("bundleRoot"), carried.isEmpty() ? QString() : m_root + QLatin1Char('/') + carried);
        return true;
    }

    /** Whether @p row's file lies in a bundle another row claims; removing that row would delete it. */
    bool
    inForeignBundle(const QJsonObject& row) const {
        // Folded, like the claimed roots: imports/Bundles/<id>/ is the same directory on case-insensitive storage.
        const QString bundle = bundle_of(path_key(row.value(QLatin1String("path")).toString().mid(m_root.size() + 1)));
        if (bundle.isEmpty() || !m_claimedRoots.contains(bundle)) {
            return false;
        }
        const QString own = row.value(QLatin1String("bundleRoot")).toString().mid(m_root.size() + 1);
        return own.isEmpty() || path_key(own) != bundle;
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
        QJsonArray library;
        for (const auto& value : kept) {
            const QJsonObject row = value.toObject();
            if (inForeignBundle(row)) {
                notInclude(QObject::tr("Imported file %1").arg(row_label(row)));
                continue;
            }
            if (row.value(QLatin1String("type")).toString() == QLatin1String("chan")
                && row.value(QLatin1String("bundleRoot")).toString().isEmpty()) {
                m_channelMaps.insert(row.value(QLatin1String("path")).toString().mid(m_root.size() + 1),
                                     row_label(row));
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
            for (const auto& field : typed_fields()) {
                const QString key = QLatin1String(field.field);
                if (is_typed_field(store, field) && row.contains(key)) {
                    row.insert(key, sanitizedValue(field, variant_string(row.value(key)), row_label(row)));
                }
            }
            rows.replace(i, row);
        }
        m_stores.insert(store, rows);
    }

    /**
     * Keep only the files the sanitized stores still name or a kept row's bundle holds: the restore refuses any
     * other carried file, because no later restore would count it as configuration and replace it.
     */
    void
    dropUncoveredFiles() {
        QSet<QString> named, claimed;
        for (auto it = m_stores.cbegin(); it != m_stores.cend(); ++it) {
            for (const TypedPathValue& value : typed_path_values(it.key(), it.value())) {
                const QString relative = value.value.mid(m_root.size() + 1);
                if (value.field.kind == PathKind::File) {
                    named.insert(relative);
                } else if (value.field.kind == PathKind::BundleRoot) {
                    claimed.insert(relative);
                }
            }
        }
        CarriedSet kept;
        QMap<QString, QString> sources;
        for (const QString& path : m_carried.paths()) {
            QString why;
            if ((named.contains(path) || claimed.contains(bundle_of(path)))
                && kept.add(path, &why) == CarriedSet::Added) {
                sources.insert(path, m_sources.value(path));
            }
        }
        m_carried = std::move(kept);
        m_sources = std::move(sources);
    }

    void
    listAbsoluteReferences(const QString& label, const QByteArray& bytes) {
        for (const QString& reference : csv_companion_references(QString::fromUtf8(bytes))) {
            if (QDir::isAbsolutePath(reference)) {
                notInclude(QObject::tr("%1: %2 (named by full path)").arg(label, QFileInfo(reference).fileName()));
            }
        }
    }

    /** Pass one: size and hash a carried file without keeping it (a channel map is kept until it is scanned). */
    bool
    measureFile(const QString& relative) {
        QFile file(m_sources.value(relative));
        if (!file.open(QIODevice::ReadOnly)) {
            return fail(BackupError::Io, QStringLiteral("read: %1").arg(relative));
        }
        if (file.size() > kMaxFileBytes) {
            return fail(BackupError::TooLarge, QStringLiteral("files: %1 is larger than the file cap").arg(relative));
        }
        const auto map = m_channelMaps.constFind(relative);
        QCryptographicHash hash(QCryptographicHash::Sha256);
        QByteArray chunk(1 << 16, Qt::Uninitialized), text;
        qint64 size = 0, n = 0;
        while (size <= kMaxFileBytes && (n = file.read(chunk.data(), chunk.size())) > 0) {
            size += n;
            hash.addData(QByteArrayView(chunk.constData(), n));
            if (map != m_channelMaps.constEnd()) {
                text.append(chunk.constData(), n);
            }
        }
        wipe(chunk);
        if (map != m_channelMaps.constEnd() && n == 0 && size <= kMaxFileBytes) {
            listAbsoluteReferences(*map, text);
        }
        wipe(text);
        if (n < 0 || size > kMaxFileBytes) {
            return fail(n < 0 ? BackupError::Io : BackupError::TooLarge, QStringLiteral("read: %1").arg(relative));
        }
        m_fileBytes += size;
        m_fileTable.append(QJsonObject{{QStringLiteral("path"), relative},
                                       {QStringLiteral("size"), size},
                                       {QStringLiteral("sha256"), QString::fromLatin1(hash.result().toHex())}});
        return true;
    }

    bool
    measureFiles() {
        if (m_carried.paths().size() > kMaxFiles) {
            return fail(BackupError::TooLarge, QStringLiteral("files: more files than the file count cap"));
        }
        for (const QString& relative : m_carried.paths()) {
            if (cancelled() || !measureFile(relative)) {
                return false;
            }
            if (m_fileBytes > kMaxArchiveBytes) {
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

    /**
     * Pass two: read a carried file straight into its place in @p archive, checking it did not change since pass
     * one.
     */
    bool
    fillFile(const QJsonObject& entry, QByteArray* archive) {
        const QString relative = entry.value(QLatin1String("path")).toString();
        const qint64 size = entry.value(QLatin1String("size")).toInteger();
        QFile file(m_sources.value(relative));
        if (!file.open(QIODevice::ReadOnly)) {
            return fail(BackupError::Io, QStringLiteral("read: %1").arg(relative));
        }
        const qint64 at = archive->size();
        archive->resize(static_cast<qsizetype>(at + size)); // within the reserved capacity: no copy
        char* out = archive->data() + at;
        qint64 done = 0, n = 0;
        while (done < size && (n = file.read(out + done, size - done)) > 0) {
            done += n;
        }
        char extra = 0;
        const QByteArray hash =
            QCryptographicHash::hash(QByteArrayView(out, static_cast<qsizetype>(done)), QCryptographicHash::Sha256);
        if (done != size || file.read(&extra, 1) != 0
            || QString::fromLatin1(hash.toHex()) != entry.value(QLatin1String("sha256")).toString()) {
            return fail(BackupError::SourceChanged, QStringLiteral("read: %1 changed during the export").arg(relative));
        }
        return true;
    }

    bool
    assemble(QByteArray* archive) {
        QByteArray json = QJsonDocument(manifest()).toJson(QJsonDocument::Compact);
        const qint64 total = kHeaderBytes + json.size() + m_fileBytes;
        if (json.size() > kMaxManifestBytes || total > kMaxArchiveBytes) {
            wipe(json);
            return fail(BackupError::TooLarge, QStringLiteral("archive: larger than the archive cap"));
        }
        // One buffer, sized once: the files are read straight into it, never held alongside it.
        archive->reserve(static_cast<qsizetype>(total));
        archive->append(kMagic, 4);
        char length[4];
        qToBigEndian<quint32>(static_cast<quint32>(json.size()), length);
        archive->append(length, 4);
        archive->append(json);
        wipe(json);
        for (const auto& entry : m_fileTable) {
            if (cancelled() || !fillFile(entry.toObject(), archive)) {
                return false;
            }
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
    QMap<QString, QString> m_claimedRoots; // carried bundle root's folded key → its archive path
    QMap<QString, QString> m_channelMaps;  // archive path of a carried, unbundled channel map → its row's label
    QStringList m_notIncluded;
    QJsonArray m_fileTable;
    qint64 m_fileBytes = 0;
    BuildResult m_result;
};

} // namespace

BuildResult
backup_build_archive(const Snapshot& snapshot, const std::atomic<bool>* cancel) {
    Builder builder(snapshot, cancel);
    return builder.run();
}

} // namespace backup
} // namespace dsd_qt
