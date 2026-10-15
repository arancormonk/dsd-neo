// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Staging a validated backup for the next start (issue #580): the staging directory, its promotion to
 * restore-pending/, the commit marker, discarding, the stale-staging sweep and the apply's result file. The apply
 * itself lives in backup_apply.cpp. */

#include "backup_apply.h"

#include <QByteArray>
#include <QChar>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QLatin1String>
#include <QList>
#include <QMap>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantMap>
#include <QtGlobal>
#include <algorithm>
#include <dsd-neo/runtime/log.h>
#include <functional>
#include <qflags.h>
#include <utility>
#include "backup_apply_fs.h"
#include "backup_archive_rules.h"
#include "imported_files_model.h"

namespace dsd_qt {
namespace backup {

namespace {

using namespace detail;

constexpr qint64 kMaxResultBytes = 1024LL * 1024;
/** Most dry-run passes over a restored library; see Stager::dropUnreadableRows(). */
constexpr int kMaxLibraryPasses = 3;

/** Whether @p dir is `<root>/restore-staging-<job id>`. */
bool
staging_dir_ok(const DataRoot& root, const QString& dir) {
    const QString prefix = root.at(QLatin1String(kStagingDirPrefix));
    return root.valid() && dir.startsWith(prefix) && owned_name(dir.mid(prefix.size()));
}

QString
pending_dir(const DataRoot& root) {
    return root.at(QLatin1String(kPendingDirName));
}

/** Writes one parse result into a staging directory and lists every file it wrote. */
class Stager {
  public:
    Stager(const DataRoot& root, const ParseResult& parsed, const QByteArray& archive, const QString& dir,
           const std::function<bool()>& proceed)
        : m_root(root), m_parsed(parsed), m_archive(archive), m_dir(dir), m_proceed(proceed), m_stores(parsed.stores),
          m_summary(parsed.summary) {}

    /** None with stopped() set when the caller asked to stop; nothing more is written then. */
    BackupError
    run() {
        BackupError error = checkTarget();
        if (error == BackupError::None && proceed()) {
            error = writeFiles();
        }
        if (error == BackupError::None && !stopped()) {
            error = dropUnreadableRows();
        }
        if (error == BackupError::None && !stopped()
            && !(writeStores() && writePrefs() && writeAvoids() && writeManifest())) {
            error = fail(BackupError::StagingFailed, QStringLiteral("a staged file could not be written"));
        }
        return error;
    }

    /** Whether the caller asked to stop. */
    bool
    stopped() const {
        return m_stopped;
    }

    const QString&
    detail() const {
        return m_detail;
    }

    /** The staged backup's summary: the parse's, less the library rows left out. */
    const Summary&
    summary() const {
        return m_summary;
    }

  private:
    BackupError
    fail(BackupError error, const QString& detail) {
        m_detail = detail;
        return error;
    }

    /** Whether the caller still wants the staging; asked before each dry run, the costly step. */
    bool
    proceed() {
        if (!m_stopped && m_proceed && !m_proceed()) {
            m_stopped = true;
        }
        return !m_stopped;
    }

    /** Every path the stores name must belong to this data root's imports/, as a parse for it rebased them. */
    BackupError
    checkTarget() {
        const QString imports = m_root.at(QStringLiteral("imports/"));
        for (auto it = m_parsed.stores.cbegin(); it != m_parsed.stores.cend(); ++it) {
            for (const TypedPathValue& value : typed_path_values(it.key(), it.value())) {
                if (value.field.kind == PathKind::Recording || !value.value.startsWith(imports)) {
                    return fail(BackupError::Damaged, QStringLiteral("a path does not belong to this data root"));
                }
            }
        }
        return BackupError::None;
    }

    bool
    writeEntry(const QString& name, const QByteArray& bytes) {
        if (!m_root.writeFile(m_dir + QLatin1Char('/') + name, bytes)) {
            return false;
        }
        m_files.append(QJsonObject{{QStringLiteral("path"), name},
                                   {QStringLiteral("size"), static_cast<qint64>(bytes.size())},
                                   {QStringLiteral("sha256"), sha256_hex(bytes)}});
        return true;
    }

    BackupError
    writeFiles() {
        for (const ArchiveFile& file : m_parsed.files) {
            if (file.offset < 0 || file.size < 0 || file.offset + file.size > m_archive.size()) {
                return fail(BackupError::Damaged, QStringLiteral("a file lies outside the archive"));
            }
            // A view, not a copy: library files can be key lists.
            const QByteArray bytes =
                QByteArray::fromRawData(m_archive.constData() + file.offset, static_cast<qsizetype>(file.size));
            if (sha256_hex(bytes) != file.sha256) {
                return fail(BackupError::Damaged, QStringLiteral("a file does not match its sha256"));
            }
            if (!writeEntry(QStringLiteral("files/") + file.path, bytes)) {
                return fail(BackupError::StagingFailed, QStringLiteral("a staged file could not be written"));
            }
        }
        return BackupError::None;
    }

    /** A store path (root + "/" + archive path) as its staged copy. */
    QString
    stagedPath(const QString& path) const {
        return m_dir + QStringLiteral("/files/") + path.mid(m_root.path().size() + 1);
    }

    /** Whether the row's staged file passes the library's own dry run (ImportedFilesModel::validate()). */
    bool
    readable(const QJsonObject& row) const {
        int accepted = 0, skipped = 0;
        return ImportedFilesModel::validate(stagedPath(row.value(QLatin1String("path")).toString()),
                                            row.value(QLatin1String("type")).toString(), &accepted, &skipped);
    }

    /**
     * The library's own dry run on each staged library file, so a restored library never holds one it cannot use. A
     * row whose file does not load here (a channel map whose companions this phone refuses, say) is left out, the way
     * an export leaves out what it cannot carry: its staged files go, every typed reference to them is cleared, and
     * the summary lists it. Dropping files can fail other rows, so the rest are checked again until none fails.
     *
     * A row the app writes names only files of its own (its file, its bundle), so leaving rows out fails no other one
     * and a second pass finds nothing. Only a backup made to chain rows into each other keeps losing rows, and each
     * pass costs a dry run per row: a library still losing rows on its kMaxLibraryPasses-th pass is refused as
     * damaged, which bounds the work at kMaxLibraryPasses dry runs a row.
     */
    BackupError
    dropUnreadableRows() {
        const QString library = QLatin1String(kImportedFilesStore);
        for (int pass = 1;; ++pass) {
            QJsonArray kept;
            QList<QJsonObject> failed;
            for (const auto& value : m_stores.value(library)) {
                if (!proceed()) {
                    return BackupError::None;
                }
                const QJsonObject row = value.toObject();
                if (readable(row)) {
                    kept.append(value);
                } else {
                    failed.append(row);
                }
            }
            if (failed.isEmpty()) {
                break;
            }
            if (pass == kMaxLibraryPasses) {
                return fail(BackupError::Damaged, QStringLiteral("library files that fail each other in a chain"));
            }
            m_stores.insert(library, kept);
            if (!leaveOut(failed, kept)) {
                return fail(BackupError::StagingFailed, QStringLiteral("a staged file could not be removed"));
            }
        }
        m_summary.libraryFiles = static_cast<int>(m_stores.value(library).size());
        return BackupError::None;
    }

    /** Leave out the @p failed rows' files (those no @p kept row still needs) and clear what names them. */
    bool
    leaveOut(const QList<QJsonObject>& failed, const QJsonArray& kept) {
        const auto rootOf = [](const QJsonObject& row) { return row.value(QLatin1String("bundleRoot")).toString(); };
        QSet<QString> keptFiles;
        QStringList keptRoots;
        for (const auto& value : kept) {
            const QJsonObject row = value.toObject();
            keptFiles.insert(row.value(QLatin1String("path")).toString());
            if (!rootOf(row).isEmpty()) {
                keptRoots.append(rootOf(row) + QLatin1Char('/'));
            }
        }
        const auto needed = [&](const QString& path) {
            return keptFiles.contains(path) || std::any_of(keptRoots.cbegin(), keptRoots.cend(), [&](const QString& r) {
                       return path.startsWith(r);
                   });
        };
        QSet<QString> goneFiles, goneRoots;
        for (const QJsonObject& row : failed) {
            notInclude(QObject::tr("%1: couldn't be read on this phone").arg(row_label(row)));
            QStringList candidates{row.value(QLatin1String("path")).toString()};
            const QString root = rootOf(row);
            if (!root.isEmpty()) {
                goneRoots.insert(root);
                for (const ArchiveFile& file : m_parsed.files) {
                    if (m_root.at(file.path).startsWith(root + QLatin1Char('/'))) {
                        candidates.append(m_root.at(file.path));
                    }
                }
            }
            for (const QString& path : candidates) {
                if (!needed(path)) {
                    goneFiles.insert(path);
                }
            }
        }
        QSet<QString> goneEntries;
        for (const QString& path : goneFiles) {
            if (!m_root.removeFile(stagedPath(path))) {
                return false;
            }
            goneEntries.insert(QStringLiteral("files/") + path.mid(m_root.path().size() + 1));
        }
        QJsonArray listed;
        for (const auto& entry : m_files) {
            if (!goneEntries.contains(entry.toObject().value(QLatin1String("path")).toString())) {
                listed.append(entry);
            }
        }
        m_files = listed;
        for (auto it = m_stores.begin(); it != m_stores.end(); ++it) {
            *it = map_typed_path_values(it.key(), *it, [&](const TypedPathValue& value) {
                const bool gone = value.field.kind == PathKind::BundleRoot ? goneRoots.contains(value.value)
                                                                           : goneFiles.contains(value.value);
                return gone ? QString() : value.value;
            });
        }
        return true;
    }

    void
    notInclude(const QString& line) {
        // Bounded like the manifest's list.
        if (m_summary.notIncluded.size() < kMaxRowsPerStore && !m_summary.notIncluded.contains(line)) {
            m_summary.notIncluded.append(line);
        }
    }

    bool
    writeStores() {
        for (const QString& store : backup_store_names()) {
            const auto it = m_stores.constFind(store);
            if (it == m_stores.constEnd()) {
                continue;
            }
            QByteArray bytes = QJsonDocument(*it).toJson(QJsonDocument::Compact);
            const bool ok = writeEntry(QStringLiteral("stores/") + store, bytes);
            wipe(bytes); // profiles hold keys
            if (!ok) {
                return false;
            }
        }
        return true;
    }

    bool
    writePrefs() {
        return writeEntry(QStringLiteral("prefs.json"),
                          QJsonDocument(QJsonObject::fromVariantMap(m_parsed.prefs)).toJson(QJsonDocument::Compact));
    }

    bool
    writeAvoids() {
        QJsonArray avoids;
        for (const quint32 frequency : m_parsed.avoids) {
            avoids.append(static_cast<qint64>(frequency));
        }
        return writeEntry(QStringLiteral("avoids.json"), QJsonDocument(avoids).toJson(QJsonDocument::Compact));
    }

    bool
    writeManifest() {
        const QJsonObject manifest{{QStringLiteral("stagingVersion"), kStagingVersion},
                                   {QStringLiteral("files"), m_files},
                                   {QStringLiteral("summary"), QJsonObject::fromVariantMap(summary_to_map(m_summary))}};
        return m_root.writeFile(m_dir + QLatin1Char('/') + QLatin1String(kManifestName),
                                QJsonDocument(manifest).toJson(QJsonDocument::Compact));
    }

    const DataRoot& m_root;
    const ParseResult& m_parsed;
    const QByteArray& m_archive;
    QString m_dir;
    const std::function<bool()>& m_proceed;
    bool m_stopped = false;
    QMap<QString, QJsonArray> m_stores; // the parse's stores, less the rows left out
    Summary m_summary;
    QJsonArray m_files;
    QString m_detail;
};

/** Whether @p pending holds the commit marker: a regular file, as the apply reads it (anything else there is not). */
bool
marked(const QString& pending) {
    return is_real_file(pending + QLatin1Char('/') + QLatin1String(kCommittedName));
}

/** A committed stage whose journal is complete: its apply has started. */
bool
apply_started(const QString& pending) {
    return marked(pending) && entry_exists(pending + QStringLiteral("/journal/COMPLETE"));
}

} // namespace

QString
staging_dir_for_job(const QString& jobId) {
    const DataRoot root;
    return root.valid() && owned_name(jobId) ? root.at(QLatin1String(kStagingDirPrefix) + jobId) : QString();
}

bool
stage_restore(const QString& dataRoot, const ParseResult& parsed, const QByteArray& archive, const QString& stagingDir,
              QString* error, Summary* summary, const std::function<bool()>& proceed) {
    const DataRoot root(dataRoot);
    BackupError result = parsed.error;
    QString detail = QStringLiteral("the backup did not parse");
    if (result == BackupError::None) {
        if (!staging_dir_ok(root, stagingDir) || !root.removeTree(stagingDir) || !root.makeDirs(stagingDir)) {
            result = BackupError::StagingFailed;
            detail = QStringLiteral("no staging directory");
        } else {
            Stager stager(root, parsed, archive, stagingDir, proceed);
            result = stager.run();
            detail = stager.detail();
            if (stager.stopped()) {
                // Asked to stop: no refusal to log or report, and nothing left behind.
                root.removeTree(stagingDir);
                if (error != nullptr) {
                    error->clear();
                }
                return false;
            }
            if (result == BackupError::None && summary != nullptr) {
                *summary = stager.summary();
            }
            if (result != BackupError::None) {
                root.removeTree(stagingDir);
            }
        }
    }
    if (result != BackupError::None) {
        LOG_WARN("Backup restore could not be staged: %s\n", qUtf8Printable(detail));
    }
    if (error != nullptr) {
        *error = result == BackupError::None ? QString() : backup_error_name(result);
    }
    return result == BackupError::None;
}

bool
promote_staging(const QString& stagingDir) {
    const DataRoot root;
    if (!staging_dir_ok(root, stagingDir) || !is_real_dir(stagingDir)) {
        return false;
    }
    const QString pending = pending_dir(root);
    if (entry_exists(pending)) {
        // A committed restore belongs to the user's confirmation; only an unconfirmed one (or anything else of that
        // name, which the apply ignores) is replaced.
        const bool committed = is_real_dir(pending) && marked(pending);
        if (committed || !root.removeEntry(pending)) {
            return false;
        }
    }
    return root.rename(stagingDir, pending);
}

bool
commit_pending() {
    const DataRoot root;
    const QString pending = pending_dir(root);
    if (!root.valid() || !is_real_dir(pending) || entry_exists(pending + QStringLiteral("/journal"))) {
        return false; // nothing staged, or an apply has started on it
    }
    const QString marker = pending + QLatin1Char('/') + QLatin1String(kCommittedName);
    if (is_real_file(marker)) {
        return true;
    }
    QByteArray manifest;
    if (!read_file(pending + QLatin1Char('/') + QLatin1String(kManifestName), kMaxStagingManifestBytes, &manifest)) {
        return false;
    }
    return root.writeFile(marker, sha256_hex(manifest).toLatin1());
}

bool
discard_pending() {
    const DataRoot root;
    const QString pending = pending_dir(root);
    if (!root.valid() || !entry_exists(pending)) {
        return true;
    }
    if (!is_real_dir(pending)) {
        return root.removeEntry(pending); // not a stage: the apply ignores it
    }
    // A committed restore with a complete journal is an apply in progress: only it may finish or undo it. Without the
    // marker the apply is past its commit point and the journal is only left over.
    const QString marker = pending + QLatin1Char('/') + QLatin1String(kCommittedName);
    if (apply_started(pending)) {
        return false;
    }
    // The marker goes first, like every ending of the apply: once it is gone nothing applies this stage, even if some
    // of its files stay (the next start removes them). A marker that cannot be removed leaves the stage whole. Whatever
    // else has the marker's name never marked it, and goes the same way.
    return root.removeEntry(marker) && root.removeTree(pending);
}

bool
pending_exists() {
    const DataRoot root;
    return root.valid() && is_real_dir(pending_dir(root));
}

bool
pending_committed() {
    const DataRoot root;
    return pending_exists() && marked(pending_dir(root));
}

bool
pending_apply_started() {
    const DataRoot root;
    return root.valid() && is_real_dir(pending_dir(root)) && apply_started(pending_dir(root));
}

void
sweep_stale_staging() {
    const DataRoot root;
    if (!root.valid()) {
        return;
    }
    const QStringList names = QDir(root.path())
                                  .entryList({QLatin1String(kStagingDirPrefix) + QLatin1Char('*')},
                                             QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
    for (const QString& name : names) {
        const QString path = root.at(name);
        if (is_real_dir(path)) {
            root.removeTree(path);
        } else if (is_real_file(path)) {
            root.removeFile(path);
        }
    }
}

bool
discard_staging(const QString& stagingDir) {
    const DataRoot root;
    if (!staging_dir_ok(root, stagingDir)) {
        return false;
    }
    return !entry_exists(stagingDir) || root.removeEntry(stagingDir);
}

QVariantMap
read_result_file() {
    const DataRoot root;
    QByteArray bytes;
    if (!root.valid() || !read_file(root.at(QLatin1String(kResultName)), kMaxResultBytes, &bytes)) {
        return {};
    }
    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &error);
    return error.error == QJsonParseError::NoError && document.isObject() ? document.object().toVariantMap()
                                                                          : QVariantMap();
}

bool
clear_result_file() {
    const DataRoot root;
    return root.valid() && root.removeFile(root.at(QLatin1String(kResultName)));
}

} // namespace backup
} // namespace dsd_qt
