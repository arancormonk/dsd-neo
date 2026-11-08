// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "replay_store.h"

#include <QByteArray>
#include <QChar>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QLatin1String>
#include <QList>
#include <QSet>
#include <QVariant>
#include <Qt>
#include <algorithm>
#include <limits>
#include <memory>
#include <utility>

#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/runtime/log.h>

#include "decoder_host.h"
#include "document_import_transport.h"
#include "import_names.h"
#include "import_service.h"
#include "json_store.h"
#include "saved_systems_model.h"

namespace dsd_qt {

namespace {

QString
manifest_name() {
    return QStringLiteral("replay_copies.json");
}

/* One copy the store made. */
struct ManifestEntry {
    QString path;
    bool kept = false;
};

/* The manifest's entries, or false when it cannot be read or holds anything but {absolute path, state} objects. */
bool
load_manifest(QList<ManifestEntry>* entries) {
    JsonStoreLoad status = JsonStoreLoad::Failed;
    const QJsonArray array = json_store_load_array(manifest_name(), &status);
    if (status == JsonStoreLoad::Failed) {
        return false;
    }
    for (const auto& value : array) {
        const QJsonObject object = value.toObject();
        const QString path = object.value(QStringLiteral("path")).toString();
        const QString state = object.value(QStringLiteral("state")).toString();
        if (!value.isObject() || !QDir::isAbsolutePath(path)
            || (state != QLatin1String("pending") && state != QLatin1String("kept"))) {
            return false;
        }
        entries->append({QDir::cleanPath(path), state == QLatin1String("kept")});
    }
    return true;
}

bool
save_manifest(const QList<ManifestEntry>& entries) {
    QJsonArray array;
    for (const ManifestEntry& entry : entries) {
        array.append(
            QJsonObject{{QStringLiteral("path"), entry.path},
                        {QStringLiteral("state"), entry.kept ? QStringLiteral("kept") : QStringLiteral("pending")}});
    }
    return json_store_save_array(manifest_name(), array);
}

/* Whether @p path lies in replaysRoot(): only such a path can be a copy this store made. */
bool
in_replays(const QString& path) {
    return !path.isEmpty()
           && QDir::cleanPath(path).startsWith(QDir::cleanPath(ReplayStore::replaysRoot()) + QLatin1Char('/'));
}

/* The index of the entry for @p path, or -1. */
qsizetype
entry_index(const QList<ManifestEntry>& entries, const QString& path) {
    for (qsizetype i = 0; i < entries.size(); i++) {
        if (ReplayStore::names({entries.at(i).path}, path)) {
            return i;
        }
    }
    return -1;
}

/* Set the copy at @p path kept or pending. True when it is so now or the manifest does not list it. */
bool
set_state(const QString& path, bool kept) {
    if (!in_replays(path)) {
        return true;
    }
    QList<ManifestEntry> entries;
    if (!load_manifest(&entries)) {
        return false;
    }
    const qsizetype found = entry_index(entries, path);
    if (found < 0 || entries.at(found).kept == kept) {
        return true;
    }
    entries[found].kept = kept;
    return save_manifest(entries);
}

/* Record the new copy at @p path pending. */
bool
record_pending(const QString& path) {
    QList<ManifestEntry> entries;
    if (!load_manifest(&entries)) {
        return false;
    }
    entries.append({QDir::cleanPath(path), false});
    return save_manifest(entries);
}

/*
 * Delete the pending copy at @p path for the reclaim. True when it is gone now (deleted, or already missing), so its
 * entry can go. Only a regular file directly in replaysRoot() is deleted, never through a link: anything else there is
 * not what this store made, so it and its entry stay.
 */
bool
delete_copy(const QString& path) {
    const QFileInfo info(path);
    if (!info.exists() && !info.isSymLink()) {
        return true;
    }
    if (!info.isFile() || info.isSymLink() || info.isJunction()
        || QDir::cleanPath(info.absolutePath()) != QDir::cleanPath(ReplayStore::replaysRoot())) {
        return false;
    }
    return QFile::remove(path);
}

QVariantMap
failure(const QString& error) {
    return {{QStringLiteral("ok"), false}, {QStringLiteral("error"), error}};
}

qint64
volume_free_bytes(const QString& folder) {
    return static_cast<qint64>(dsd_path_free_bytes(folder.toUtf8().constData()));
}

/*
 * A replay's stage, on the import worker: copy the document into the request's staging folder, at most what leaves
 * kReserveBytes free on the volume replaysRoot() is on (no cap when that cannot be read). On success the map carries
 * "staged", the copy's path, for the commit to publish.
 */
QVariantMap
stage_replay(const ImportJob& job, const QString& reference, const QString& hint,
             const ReplayStore::FreeSpace& freeSpace) {
    const QString root = ReplayStore::replaysRoot();
    if (!QDir().mkpath(root)) {
        auto result = failure(QStringLiteral("local"));
        result.insert(QStringLiteral("name"), sanitized_import_name(QString(), hint));
        return result;
    }
    const qint64 free = freeSpace(root);
    const qint64 budget =
        free < 0 ? std::numeric_limits<qint64>::max() : qMax<qint64>(0, free - ReplayStore::kReserveBytes);
    const QString staged = QDir(job.stagingDir).filePath(QStringLiteral("replay"));
    const int code = job.transport->copy(job.id, reference, staged, budget);
    const ImportCopyProgress copy = job.transport->progress(job.id);
    QVariantMap result;
    switch (code) {
        case DecoderHost::DocumentCopied:
            result = {{QStringLiteral("ok"), true}, {QStringLiteral("staged"), staged}};
            break;
        case DecoderHost::DocumentTooLarge:
            result = failure(QStringLiteral("too_large"));
            result.insert(QStringLiteral("neededBytes"),
                          copy.total >= 0 ? copy.total + ReplayStore::kReserveBytes : qint64{-1});
            result.insert(QStringLiteral("freeBytes"), free);
            break;
        case DecoderHost::DocumentLocalFailed:
            result = failure(QStringLiteral("local"));
            result.insert(QStringLiteral("noSpace"), copy.noSpace);
            break;
        case DecoderHost::DocumentCancelled: result = failure(QStringLiteral("cancelled")); break;
        default: result = failure(QStringLiteral("read")); break;
    }
    result.insert(QStringLiteral("name"), sanitized_import_name(copy.displayName, hint));
    return result;
}

} // namespace

ReplayStore::ReplayStore(DecoderHost* host, ImportService* imports, QObject* parent)
    : QObject(parent), m_host(host), m_imports(imports), m_freeSpace(&volume_free_bytes) {
    m_progressTimer.setInterval(kProgressIntervalMs);
    connect(&m_progressTimer, &QTimer::timeout, this, &ReplayStore::pollProgress);
}

// The service goes before this store at teardown; nothing here may reach it.
ReplayStore::~ReplayStore() = default;

QString
ReplayStore::replaysRoot() {
    return json_store_path(QStringLiteral("replays"));
}

QString
ReplayStore::manifestPath() {
    return json_store_path(manifest_name());
}

int
ReplayStore::beginReplayImport(const QString& reference, const QString& fileName) {
    cancelReplayImport();
    const QString local = local_document_path(reference);
    if (!local.isEmpty() || m_imports.isNull()) {
        const qint64 id = ImportService::newRequestId();
        QVariantMap result = failure(QStringLiteral("read"));
        if (!local.isEmpty()) {
            // A file the engine can open as it is: no copy, and nothing for the store to own (I5).
            result = {{QStringLiteral("ok"), true},
                      {QStringLiteral("path"), QDir::cleanPath(local)},
                      {QStringLiteral("name"), QFileInfo(local).fileName()}};
        }
        answerQueued(id, result);
        return static_cast<int>(id);
    }
    const FreeSpace freeSpace = m_freeSpace;
    QPointer<ReplayStore> self(this);
    const qint64 id =
        m_imports->submit([reference, fileName, freeSpace](
                              const ImportJob& job) { return stage_replay(job, reference, fileName, freeSpace); },
                          [self](qint64 request, const QVariantMap& staged, bool cancelled) {
                              if (!self.isNull()) {
                                  self->commitImport(request, staged, cancelled);
                              }
                          });
    m_current = id;
    m_copyName = sanitized_import_name(QString(), fileName);
    m_copyBytes = 0;
    m_copyTotal = -1;
    m_progressTimer.start();
    Q_EMIT progressChanged();
    return static_cast<int>(id);
}

void
ReplayStore::cancelReplayImport() {
    if (m_current != 0 && !m_imports.isNull()) {
        m_imports->cancel(m_current);
    }
}

bool
ReplayStore::copying() const {
    return m_current != 0;
}

QString
ReplayStore::copyName() const {
    return m_copyName;
}

qint64
ReplayStore::copyBytes() const {
    return m_copyBytes;
}

qint64
ReplayStore::copyTotal() const {
    return m_copyTotal;
}

bool
ReplayStore::adopt(const QString& path) {
    return set_state(path, true);
}

bool
ReplayStore::release(const QString& path) {
    return set_state(path, false);
}

void
ReplayStore::reclaim(const SavedSystemsModel& systems, bool systemsLoaded) {
    QList<ManifestEntry> entries;
    if (!load_manifest(&entries)) {
        // What it listed stays where it is (nothing proves the store made it); later copies are recorded again.
        const QString aside = manifestPath() + QStringLiteral(".bad");
        QFile::remove(aside);
        if (!QFile::rename(manifestPath(), aside)) {
            LOG_WARN("The replay copies list cannot be read or set aside; replay copies are kept.\n");
        }
        return;
    }
    if (!systemsLoaded || (m_host != nullptr && m_host->engineBusy())) {
        return;
    }
    const QStringList named = systems.filePaths();
    QList<ManifestEntry> left;
    for (const ManifestEntry& entry : entries) {
        if (entry.kept || names(named, entry.path) || !delete_copy(entry.path)) {
            left.append(entry);
        }
    }
    // A save that fails leaves entries for files that are gone; the next reclaim drops them.
    if (left.size() != entries.size() && !save_manifest(left)) {
        LOG_WARN("The replay copies list could not be saved after removing unused copies.\n");
    }
}

bool
ReplayStore::names(const QStringList& paths, const QString& path) {
    if (path.isEmpty()) {
        return false;
    }
    const QString clean = QDir::cleanPath(path);
    const QString canonical = QFileInfo(path).canonicalFilePath();
    return std::any_of(paths.cbegin(), paths.cend(), [&clean, &canonical](const QString& candidate) {
        return !candidate.isEmpty()
               && (QDir::cleanPath(candidate) == clean
                   || (!canonical.isEmpty() && QFileInfo(candidate).canonicalFilePath() == canonical));
    });
}

void
ReplayStore::setFreeSpace(FreeSpace freeSpace) {
    m_freeSpace = std::move(freeSpace);
}

void
ReplayStore::commitImport(qint64 id, const QVariantMap& staged, bool cancelled) {
    QVariantMap result = staged;
    const QString name = staged.value(QStringLiteral("name")).toString();
    if (cancelled) {
        result = failure(QStringLiteral("cancelled"));
        result.insert(QStringLiteral("name"), name);
    } else if (staged.value(QStringLiteral("ok")).toBool()) {
        // Recorded pending before QML sees it: a copy is never left unlisted, so a later start can reclaim it.
        QString path;
        const QDir root(replaysRoot());
        if (!ImportService::publish(staged.value(QStringLiteral("staged")).toString(), root, name, {}, &path)) {
            result = failure(QStringLiteral("local"));
        } else if (!record_pending(path)) {
            QFile::remove(path);
            result = failure(QStringLiteral("local"));
        } else {
            result = {{QStringLiteral("ok"), true}, {QStringLiteral("path"), path}};
        }
        result.insert(QStringLiteral("name"), name);
    }
    result.remove(QStringLiteral("staged"));
    endCopy(id);
    Q_EMIT replayImportFinished(static_cast<int>(id), result);
}

void
ReplayStore::answerQueued(qint64 id, const QVariantMap& result) {
    QMetaObject::invokeMethod(
        this, [this, id, result]() { Q_EMIT replayImportFinished(static_cast<int>(id), result); },
        Qt::QueuedConnection);
}

void
ReplayStore::pollProgress() {
    if (m_current == 0 || m_imports.isNull()) {
        return;
    }
    // Between copies, and once the stage ended, the service knows nothing: the last reading stands.
    const ImportCopyProgress progress = m_imports->progress(m_current);
    if (progress.bytes < 0) {
        return;
    }
    const QString name = progress.displayName.isEmpty() ? m_copyName : sanitized_import_name(progress.displayName, {});
    if (name == m_copyName && progress.bytes == m_copyBytes && progress.total == m_copyTotal) {
        return;
    }
    m_copyName = name;
    m_copyBytes = progress.bytes;
    m_copyTotal = progress.total;
    Q_EMIT progressChanged();
}

void
ReplayStore::endCopy(qint64 id) {
    if (id != m_current) {
        return;
    }
    m_current = 0;
    m_progressTimer.stop();
    Q_EMIT progressChanged();
}

} // namespace dsd_qt
