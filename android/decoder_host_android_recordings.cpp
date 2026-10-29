// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

/*
 * Call recordings on Android (#708): roots, playback, export, save and share, less their JNI transport (the
 * platform*() functions in decoder_host_android.cpp), so the desktop host test compiles this dispatch as it ships.
 */

#include <QChar>
#include <QFileInfo>
#include <QLatin1String>
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QThreadPool>
#include <Qt>
#include <QtGlobal>
#include <atomic>
#include <memory>
#include <mutex>
#include <utility>
#include "decoder_host.h"
#include "decoder_host_android.h"

namespace dsd_android {
namespace {

/* RecordingExport tells transfers apart by id, process-wide, so a host made after another never reuses one. */
std::atomic<qint64> g_next_transfer_id{0};

/* How long a host going away waits for its transfer to stop. */
constexpr int kTransferStopWaitMs = 2000;

/* Workers a host left running a blocked job: reachable for the rest of the process, never deleted (that would wait). */
QList<QThreadPool*>&
abandoned_transfer_workers() {
    static auto* workers = new QList<QThreadPool*>();
    return *workers;
}

} // namespace

QStringList
DecoderHostAndroid::recordingRoots() const {
    const QStringList roots = platformRecordingRoots();
    return roots.isEmpty() ? dsd_qt::DecoderHost::recordingRoots() : roots;
}

void
DecoderHostAndroid::beginPlayback(const QString& path) {
    const int generation = platformPlayRecording(path);
    m_playback_generation = generation > 0 ? generation : 0;
    if (m_playback_generation == 0) {
        // As the base host's failures do: the play replaces what plays, so the player stops before the store is told
        // nothing plays it.
        platformStopPlayback();
        publishPlayback(path, PlaybackFailed, 0, 0, QStringLiteral("failed"));
    }
}

void
DecoderHostAndroid::pausePlayback() {
    if (m_playback_generation != 0) {
        platformPausePlayback();
    }
}

void
DecoderHostAndroid::resumePlayback() {
    if (m_playback_generation != 0) {
        platformResumePlayback();
    }
}

void
DecoderHostAndroid::seekPlayback(qint64 positionMs) {
    if (m_playback_generation == 0) {
        return;
    }
    platformSeekPlayback(positionMs);
    // Shown at once: the next poll would otherwise show the old position for a moment.
    const qint64 length = playbackDurationMs();
    const qint64 target = length > 0 ? qBound<qint64>(0, positionMs, length) : qMax<qint64>(0, positionMs);
    publishPlayback(playbackPath(), playbackState(), target, length, playbackError());
}

void
DecoderHostAndroid::stopPlayback() {
    // Always asked: the player may still be preparing a play no poll has seen yet. Before the first play it is a no-op.
    platformStopPlayback();
    m_playback_generation = 0;
    dsd_qt::DecoderHost::stopPlayback();
}

void
DecoderHostAndroid::refreshPlayback() {
    if (m_playback_generation == 0) {
        return;
    }
    const QStringList fields = platformPlaybackStatus().split(QLatin1Char('\t'));
    if (fields.size() != 5) {
        return;
    }
    bool ok[4] = {};
    const int generation = fields.at(0).toInt(&ok[0]);
    const int state = fields.at(1).toInt(&ok[1]);
    const qint64 position = fields.at(2).toLongLong(&ok[2]);
    const qint64 length = fields.at(3).toLongLong(&ok[3]);
    // A record of an earlier play (the player has not reached this one yet) or one that cannot be read says nothing.
    if (!ok[0] || !ok[1] || !ok[2] || !ok[3] || generation != m_playback_generation || state < PlaybackStopped
        || state > PlaybackFailed) {
        return;
    }
    const QString path = playbackPath();
    if (state == PlaybackFailed) {
        m_playback_generation = 0;
        const QString error = fields.at(4).isEmpty() ? QStringLiteral("failed") : fields.at(4);
        publishPlayback(path, PlaybackFailed, 0, 0, error);
        if (error == QLatin1String("missing")) {
            reportRecordingsMissing({path});
        }
        return;
    }
    if (state == PlaybackStopped) {
        m_playback_generation = 0; // played to its end: nothing more to poll
    }
    publishPlayback(path, state, position, length, QString());
}

/*
 * A transfer job holds this, never the host itself: the destructor clears host after its bounded wait, so a job it
 * stopped waiting for (blocked in a provider's write, or in a file URL's write) posts nothing more and never touches
 * the host. The file copy stops before its next chunk once aborted is set.
 */
struct DecoderHostAndroid::TransferLink {
    std::mutex mutex;
    DecoderHostAndroid* host = nullptr;
    std::atomic<bool> aborted{false};
};

std::shared_ptr<DecoderHostAndroid::TransferLink>
DecoderHostAndroid::newTransferLink(DecoderHostAndroid* host) {
    auto link = std::make_shared<TransferLink>();
    link->host = host;
    return link;
}

std::unique_ptr<QThreadPool>
DecoderHostAndroid::newTransferWorker() {
    auto worker = std::make_unique<QThreadPool>();
    worker->setMaxThreadCount(1);
    return worker;
}

bool
DecoderHostAndroid::beginRecordingTransfer(const QStringList& paths, const QStringList& names, const QString& reference,
                                           bool zip) {
    if (documentTransfersAborted()) {
        return false;
    }
    const qint64 id = ++g_next_transfer_id;
    m_transfer_id = id;
    m_transfer_total = -1;
    m_transfer_worker->start([link = m_transfer_link, id, paths, names, reference, zip]() {
        runRecordingTransfer(link, id, paths, names, reference, zip);
    });
    return true;
}

void
DecoderHostAndroid::runRecordingTransfer(const std::shared_ptr<TransferLink>& link, qint64 id, const QStringList& paths,
                                         const QStringList& names, const QString& reference, bool zip) {
    // The worker thread: no QObject state is touched here, only queued back through the link.
    const auto post = [&link](const auto& call) {
        const std::lock_guard<std::mutex> lock(link->mutex);
        DecoderHostAndroid* host = link->host;
        if (host != nullptr) {
            QMetaObject::invokeMethod(host, [host, call]() { call(host); }, Qt::QueuedConnection);
        }
    };
    QStringList present;
    QStringList presentNames;
    QStringList missing;
    qint64 total = 0;
    for (qsizetype i = 0; i < paths.size(); ++i) {
        const QFileInfo file(paths.at(i));
        if (!file.isFile()) {
            missing.append(paths.at(i));
            continue;
        }
        present.append(paths.at(i));
        if (zip && i < names.size()) {
            presentNames.append(names.at(i));
        }
        total += file.size();
    }
    post([id, total](DecoderHostAndroid* host) { host->noteRecordingTransferTotal(id, total); });

    int result = TransferMissing;
    if (!present.isEmpty()) {
        if (isContentUri(reference)) {
            const QStringList answer = platformTransferRecordings(id, present, presentNames, reference, zip);
            bool ok = false;
            const int code = answer.isEmpty() ? -1 : answer.first().toInt(&ok);
            result = ok && code >= TransferDone && code <= TransferMissing ? code : TransferFailed;
            missing += answer.mid(1);
        } else if (!zip) {
            // The base host's copy, on the link's own abort flag: the host may be gone before it ends.
            result =
                copyFileToLocalDocument(present.first(), reference, &link->aborted) ? TransferDone : TransferFailed;
        } else {
            result = TransferFailed; // a ZIP goes only into a platform document
        }
    }
    post([id, result, missing](DecoderHostAndroid* host) { host->endRecordingTransfer(id, result, missing); });
}

void
DecoderHostAndroid::noteRecordingTransferTotal(qint64 id, qint64 total) {
    if (id != m_transfer_id) {
        return;
    }
    m_transfer_total = total;
    publishRecordingTransferProgress(0, total);
}

void
DecoderHostAndroid::refreshRecordingTransfer() {
    if (m_transfer_id == 0 || m_transfer_total < 0) {
        return;
    }
    const qint64 bytes = platformRecordingTransferProgress(m_transfer_id);
    if (bytes >= 0) {
        publishRecordingTransferProgress(qMin(bytes, m_transfer_total), m_transfer_total);
    }
}

void
DecoderHostAndroid::endRecordingTransfer(qint64 id, int result, const QStringList& missing) {
    if (id != m_transfer_id) {
        return;
    }
    if (result == TransferDone && m_transfer_total >= 0) {
        publishRecordingTransferProgress(m_transfer_total, m_transfer_total);
    }
    m_transfer_id = 0;
    m_transfer_total = -1;
    finishRecordingTransfer(result, missing);
}

void
DecoderHostAndroid::cancelRecordingTransfer() {
    if (m_transfer_id != 0) {
        platformCancelRecordingTransfer(m_transfer_id);
    }
}

void
DecoderHostAndroid::closeRecordingWork() {
    if (m_transfer_id != 0) {
        // As ~BackupModel: the provider calls in flight are cancelled and no document transfer starts again, then this
        // transfer is cancelled, and a file URL's copy is stopped; each stops at its next chunk.
        abortDocumentTransfers();
        cancelRecordingTransfer();
        m_transfer_link->aborted.store(true);
    }
    if (!m_transfer_worker->waitForDone(kTransferStopWaitMs)) {
        // A write blocked on a provider's pipe, or on a file URL's storage, ignores all of these. Deleting the pool
        // would wait for it, so the pool is left to finish that job (reachable, never deleted), and the cleared link
        // keeps the job away from this host: the job never calls into it.
        hostDiagnostic(QStringLiteral("A call recording export did not stop within %1 ms; the app closes without it")
                           .arg(kTransferStopWaitMs));
        abandoned_transfer_workers().append(m_transfer_worker.release());
    }
    {
        const std::lock_guard<std::mutex> lock(m_transfer_link->mutex);
        m_transfer_link->host = nullptr;
    }
    // What a share job reports from here on never arrives: its hold goes now.
    for (const int hold : std::as_const(m_share_holds)) {
        releaseRecordingHold(hold);
    }
    m_share_holds.clear();
    if (m_playback_generation != 0) {
        platformStopPlayback();
        m_playback_generation = 0;
    }
}

bool
DecoderHostAndroid::shareRecordingFile(const QString& path, const QString& title) {
    if (documentTransfersAborted() || !platformCanShareRecording(path)) {
        return false;
    }
    // Held in the store until its copy is made: behind a running export, the limit or a delete would otherwise take
    // the recording before the copy, and the share the user was told began would fail.
    const int hold = holdRecording(path);
    if (hold > 0) {
        m_share_holds.insert(hold);
    }
    // The copy is a file's worth of I/O, so it runs on the transfer worker, never the GUI or Android main thread.
    m_transfer_worker->start([link = m_transfer_link, path, title, hold]() {
        const bool staged = platformShareRecording(path, title);
        const std::lock_guard<std::mutex> lock(link->mutex);
        DecoderHostAndroid* host = link->host;
        if (host != nullptr) {
            QMetaObject::invokeMethod(
                host, [host, path, hold, staged]() { host->endRecordingShare(path, hold, staged); },
                Qt::QueuedConnection);
        }
    });
    return true;
}

void
DecoderHostAndroid::endRecordingShare(const QString& path, int hold, bool staged) {
    if (m_share_holds.remove(hold)) {
        releaseRecordingHold(hold);
    }
    if (!staged) {
        noteRecordingShareFailed(path);
    }
}

void
DecoderHostAndroid::refreshRecordingShares() {
    // Bounded: a few per tick at most, so a platform that keeps answering cannot hold the tick.
    for (int i = 0; i < 8; ++i) {
        const QString path = platformTakeRecordingShareFailure();
        if (path.isEmpty()) {
            return;
        }
        noteRecordingShareFailed(path);
    }
}

void
DecoderHostAndroid::noteRecordingShareFailed(const QString& path) {
    Q_EMIT recordingShareFailed(path);
    // Gone by the time the share copied it (deleted from a computer, say): the store looks again, as a play or an
    // export that finds a file missing has it do.
    if (!QFileInfo(path).isFile()) {
        reportRecordingsMissing({path});
    }
}

void
DecoderHostAndroid::startRecordingShareClean() {
    m_transfer_worker->start([]() { platformCleanRecordingShares(); });
}

} // namespace dsd_android
