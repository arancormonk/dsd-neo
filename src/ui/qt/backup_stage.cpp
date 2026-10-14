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
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantMap>
#include <QtGlobal>
#include <dsd-neo/runtime/log.h>
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
    Stager(const DataRoot& root, const ParseResult& parsed, const QByteArray& archive, const QString& dir)
        : m_root(root), m_parsed(parsed), m_archive(archive), m_dir(dir) {}

    BackupError
    run() {
        BackupError error = checkTarget();
        if (error == BackupError::None) {
            error = writeFiles();
        }
        if (error == BackupError::None) {
            error = validateLibrary();
        }
        if (error == BackupError::None && !(writeStores() && writePrefs() && writeAvoids() && writeManifest())) {
            error = fail(BackupError::Io, QStringLiteral("a staged file could not be written"));
        }
        return error;
    }

    const QString&
    detail() const {
        return m_detail;
    }

  private:
    BackupError
    fail(BackupError error, const QString& detail) {
        m_detail = detail;
        return error;
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
                return fail(BackupError::Io, QStringLiteral("a staged file could not be written"));
            }
        }
        return BackupError::None;
    }

    /** The library's own dry run on each staged library file: a restored library never holds one it cannot use. */
    BackupError
    validateLibrary() {
        for (const auto& value : m_parsed.stores.value(QLatin1String(kImportedFilesStore))) {
            const QJsonObject row = value.toObject();
            const QString path = row.value(QLatin1String("path")).toString();
            const QString staged = m_dir + QStringLiteral("/files/") + path.mid(m_root.path().size() + 1);
            int accepted = 0, skipped = 0;
            if (!ImportedFilesModel::validate(staged, row.value(QLatin1String("type")).toString(), &accepted,
                                              &skipped)) {
                return fail(BackupError::Damaged, QStringLiteral("a library file does not parse as its type"));
            }
        }
        return BackupError::None;
    }

    bool
    writeStores() {
        for (const QString& store : backup_store_names()) {
            const auto it = m_parsed.stores.constFind(store);
            if (it == m_parsed.stores.constEnd()) {
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
        const QJsonObject manifest{
            {QStringLiteral("stagingVersion"), kStagingVersion},
            {QStringLiteral("files"), m_files},
            {QStringLiteral("summary"), QJsonObject::fromVariantMap(summary_to_map(m_parsed.summary))}};
        return m_root.writeFile(m_dir + QLatin1Char('/') + QLatin1String(kManifestName),
                                QJsonDocument(manifest).toJson(QJsonDocument::Compact));
    }

    const DataRoot& m_root;
    const ParseResult& m_parsed;
    const QByteArray& m_archive;
    QString m_dir;
    QJsonArray m_files;
    QString m_detail;
};

} // namespace

QString
staging_dir_for_job(const QString& jobId) {
    const DataRoot root;
    return root.valid() && owned_name(jobId) ? root.at(QLatin1String(kStagingDirPrefix) + jobId) : QString();
}

bool
stage_restore(const ParseResult& parsed, const QByteArray& archive, const QString& stagingDir, QString* error) {
    const DataRoot root;
    BackupError result = parsed.error;
    QString detail = QStringLiteral("the backup did not parse");
    if (result == BackupError::None) {
        if (!staging_dir_ok(root, stagingDir) || !root.removeTree(stagingDir) || !root.makeDirs(stagingDir)) {
            result = BackupError::Io;
            detail = QStringLiteral("no staging directory");
        } else {
            Stager stager(root, parsed, archive, stagingDir);
            result = stager.run();
            detail = stager.detail();
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
        const bool committed =
            is_real_dir(pending) && entry_exists(pending + QLatin1Char('/') + QLatin1String(kCommittedName));
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
    if (entry_exists(marker) && entry_exists(pending + QStringLiteral("/journal/COMPLETE"))) {
        return false;
    }
    // The marker goes first, like every ending of the apply: once it is gone nothing applies this stage, even if some
    // of its files stay (the next start removes them). A marker that cannot be removed leaves the stage whole.
    return root.removeFile(marker) && root.removeTree(pending);
}

bool
pending_exists() {
    const DataRoot root;
    return root.valid() && is_real_dir(pending_dir(root));
}

bool
pending_committed() {
    const DataRoot root;
    return pending_exists() && is_real_file(pending_dir(root) + QLatin1Char('/') + QLatin1String(kCommittedName));
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
