// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

/*
 * Call recordings on Android (#708): roots, playback, export, save and share, less their JNI transport (the
 * platform*() functions in decoder_host_android.cpp), so the desktop host test compiles this dispatch as it ships.
 */

#include <QCoreApplication>
#include <QDebug>
#include <QFileInfo>
#include <QGuiApplication>
#include <QLatin1String>
#include <QList>
#include <QMap>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QThreadPool>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>
#include <Qt>
#include <QtGlobal>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <stddef.h>
#include <utility>
#include <vector>
#include "abandoned_workers.h"
#include "decoder_host.h"
#include "decoder_host_android.h"
#include "dsd-neo/platform/file_compat.h"
#include "playback_status_record.h"
#include "recording_selection_token.h"

namespace dsd_android {
namespace {

/* RecordingExport tells transfers apart by id, process-wide, so a host made after another never reuses one. */
std::atomic<qint64> g_next_transfer_id{0};

/* How long a host going away waits for its transfer to stop. */
constexpr int kTransferStopWaitMs = 2000;

/* The token of each transfer RecordingExport runs now, by id (recordingSelectionCurrent()). */
std::mutex g_transfer_tokens_lock;
std::map<qint64, DecoderHostAndroid::RecordingSelectionToken> g_transfer_tokens;

/* Answers for transfer @p id's token while it lives. */
class TransferTokenEntry {
  public:
    TransferTokenEntry(qint64 id, const DecoderHostAndroid::RecordingSelectionToken& token) : m_id(id) {
        const std::lock_guard<std::mutex> lock(g_transfer_tokens_lock);
        g_transfer_tokens[id] = token;
    }

    ~TransferTokenEntry() {
        const std::lock_guard<std::mutex> lock(g_transfer_tokens_lock);
        g_transfer_tokens.erase(m_id);
    }

    TransferTokenEntry(const TransferTokenEntry&) = delete;
    TransferTokenEntry& operator=(const TransferTokenEntry&) = delete;

  private:
    qint64 m_id;
};

} // namespace

bool
DecoderHostAndroid::recordingSelectionCurrent(qint64 id) {
    const std::lock_guard<std::mutex> lock(g_transfer_tokens_lock);
    const auto it = g_transfer_tokens.find(id);
    return it != g_transfer_tokens.end() && it->second.current();
}

QStringList
DecoderHostAndroid::recordingRoots() const {
    const QStringList roots = platformRecordingRoots();
    return roots.isEmpty() ? dsd_qt::DecoderHost::recordingRoots() : roots;
}

namespace {

/* @p key of @p item, @p absent when the key is missing or holds no value: as playbackItemsValid() read it (M4), so the
 * check and the request agree on every item. */
QVariant
playback_value(const QVariantMap& item, const QString& key, const QVariant& absent) {
    const QVariant value = item.value(key);
    return value.isValid() ? value : absent;
}

/* @p items (already checked: DecoderHost::playbackItemsValid()) as RecordingPlayback.play() takes them. */
DecoderHostAndroid::PlaybackRequest
playback_request(const QVariantList& items, int mode) {
    DecoderHostAndroid::PlaybackRequest request;
    request.mode = mode;
    for (const QVariant& value : items) {
        const QVariantMap item = value.toMap();
        request.paths.append(item.value(QStringLiteral("path")).toString());
        // A queued item's identity was checked present; a single play may name its file by path alone (-1).
        request.sizes.append(playback_value(item, QStringLiteral("size"), qint64{-1}).toLongLong());
        request.mtimeSecs.append(playback_value(item, QStringLiteral("mtimeSec"), qint64{0}).toLongLong());
        request.mtimeNanos.append(playback_value(item, QStringLiteral("mtimeNanos"), 0).toInt());
        request.calls.append(playback_value(item, QStringLiteral("call"), 0).toInt());
        // An item with no numbering (a single play named by path alone) is part 1 of 1, as RecordingPlayer.Item(path)
        // is (R63); the validity check took the same defaults, for a missing key and one holding no value alike.
        request.partOrdinals.append(playback_value(item, QStringLiteral("partOrdinal"), 1).toInt());
        request.partCounts.append(playback_value(item, QStringLiteral("partCount"), 1).toInt());
        request.titles.append(item.value(QStringLiteral("title")).toString());
        request.lines.append(item.value(QStringLiteral("line")).toString());
        request.keys.append(item.value(QStringLiteral("key")).toString());
    }
    return request;
}

} // namespace

void
DecoderHostAndroid::beginPlayback(const QString& path) {
    const int generation = platformPlayRecording(path);
    drainRecordingRecheck();
    if (generation <= 0) {
        // As the base host's failures do: the play replaces what plays, so the player stops before the store is told
        // nothing plays it. The play it replaced lets its items go with it.
        closePlayback();
        platformStopPlayback();
        publishPlayback(path, PlaybackFailed, 0, 0, QStringLiteral("failed"));
        return;
    }
    PlaybackRequest single;
    single.paths = QStringList{path};
    single.calls = QList<int>{0};
    // Part 1 of 1, as the player's RecordingPlayer.Item(path) reads it (M5): its first record changes nothing here.
    single.partOrdinals = QList<int>{1};
    single.partCounts = QList<int>{1};
    single.titles = QStringList{QString()};
    single.keys = QStringList{QString()};
    openPlayback(generation, QString(), single);
}

int
DecoderHostAndroid::playRecordings(const QVariantList& items, int mode, const QString& origin) {
    if (!playbackItemsValid(items, mode)) {
        return 0;
    }
    const PlaybackRequest request = playback_request(items, mode);
    const int generation = platformPlayRecordings(request);
    drainRecordingRecheck();
    if (generation <= 0) {
        // Refused before it was accepted (or the call failed): what plays plays on, its readings untouched.
        return 0;
    }
    // Accepted: the platform has replaced what played. Every later reading, a failure to start included, is this
    // play's, under its generation.
    openPlayback(generation, origin, request);
    return generation;
}

void
DecoderHostAndroid::showPlaybackItem(const PlaybackItem& item, PlaybackReading* reading) {
    reading->path = item.path;
    reading->title = item.title;
    reading->key = item.key;
    reading->size = item.size;
    reading->mtimeSec = item.mtimeSec;
    reading->mtimeNanos = item.mtimeNanos;
}

void
DecoderHostAndroid::openPlayback(int generation, const QString& origin, const PlaybackRequest& request) {
    m_playback_generation = generation;
    m_playback_mode = request.mode;
    m_playback_origin = origin;
    m_playback_items.clear();
    m_playback_items.reserve(static_cast<size_t>(request.paths.size()));
    for (qsizetype i = 0; i < request.paths.size(); ++i) {
        m_playback_items.push_back({request.paths.at(i), request.titles.value(i), request.keys.value(i),
                                    request.sizes.value(i, -1), request.mtimeSecs.value(i),
                                    request.mtimeNanos.value(i)});
    }
    const qsizetype first = qBound(qsizetype{0}, static_cast<qsizetype>(request.start), request.paths.size() - 1);
    PlaybackReading reading;
    reading.generation = generation;
    reading.state = PlaybackPreparing;
    reading.mode = request.mode;
    reading.origin = origin;
    showPlaybackItem(m_playback_items.at(static_cast<size_t>(first)), &reading);
    reading.callIndex = request.calls.value(first);
    reading.callCount = request.calls.isEmpty() ? 0 : request.calls.last() + 1;
    reading.partOrdinal = request.partOrdinals.value(first);
    reading.partCount = request.partCounts.value(first);
    publishPlayback(reading);
}

bool
DecoderHostAndroid::playbackTokenCurrent(qint64 token) const {
    return m_playback_generation != 0 && token > 0 && token == playbackToken();
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
DecoderHostAndroid::nextPlayback() {
    if (m_playback_generation != 0 && m_playback_mode != PlaybackSingle) {
        platformNextPlayback();
    }
}

void
DecoderHostAndroid::previousPlayback() {
    if (m_playback_generation != 0 && m_playback_mode != PlaybackSingle) {
        platformPreviousPlayback();
    }
}

void
DecoderHostAndroid::seekPlayback(qint64 positionMs) {
    seekPlaybackFor(playbackToken(), positionMs);
}

void
DecoderHostAndroid::seekPlaybackFor(qint64 token, qint64 positionMs) {
    // A gesture begun on another recording than the one open now says nothing of this one: nothing is sent.
    if (!playbackTokenCurrent(token)) {
        return;
    }
    // Nothing is shown here: the token is the one the last poll read, and the player may have moved on since, so
    // only its next record says where it is (a seek it dropped included).
    platformSeekPlaybackFor(token, positionMs);
}

void
DecoderHostAndroid::pausePlaybackFor(qint64 token) {
    if (playbackTokenCurrent(token)) {
        platformPausePlaybackFor(token);
    }
}

void
DecoderHostAndroid::resumePlaybackFor(qint64 token) {
    if (playbackTokenCurrent(token)) {
        platformResumePlaybackFor(token);
    }
}

void
DecoderHostAndroid::stopPlaybackGeneration(int generation) {
    // One that ended (polled no more) or was replaced is left alone: the stop is for that play only.
    if (generation <= 0 || generation != m_playback_generation) {
        return;
    }
    platformStopPlaybackGeneration(generation);
    closePlayback();
    dsd_qt::DecoderHost::stopPlayback();
}

void
DecoderHostAndroid::stopPlayback() {
    // Always asked: the player may still be preparing a play no poll has seen yet. Before the first play it is a no-op.
    platformStopPlayback();
    closePlayback();
    dsd_qt::DecoderHost::stopPlayback();
}

void
DecoderHostAndroid::closePlayback() {
    m_playback_generation = 0;
    // A long playlist's paths, titles and identities are let go with it; nothing reads them once it is polled no more.
    std::vector<PlaybackItem>().swap(m_playback_items);
}

void
DecoderHostAndroid::drainRecordingRecheck() {
    reportRecordingsMissing(platformTakeRecordingRecheck());
}

bool
DecoderHostAndroid::refreshPlayback() {
    if (m_playback_generation == 0) {
        return false;
    }
    PlaybackStatusRecord record;
    if (!parse_playback_status(platformPlaybackStatus(), &record)) {
        return false; // a record that cannot be read says nothing
    }
    bool drained = false;
    if (record.recheckSerial != m_recheck_serial) {
        m_recheck_serial = record.recheckSerial;
        drainRecordingRecheck();
        drained = true;
    }
    // A record of an earlier play (the player has not reached this one yet) says nothing of this one.
    if (record.generation == m_playback_generation) {
        applyPlaybackRecord(record);
    }
    return drained;
}

void
DecoderHostAndroid::applyPlaybackRecord(const PlaybackStatusRecord& record) {
    if (record.itemIndex >= static_cast<int>(m_playback_items.size())) {
        return; // names no item of this play: not a record of it after all
    }
    PlaybackReading reading = playback();
    if (record.itemIndex >= 0) {
        showPlaybackItem(m_playback_items.at(static_cast<size_t>(record.itemIndex)), &reading);
    }
    reading.generation = record.generation;
    reading.state = record.state;
    reading.positionMs = record.positionMs;
    reading.durationMs = record.durationMs;
    reading.error = record.error;
    reading.mode = m_playback_mode;
    reading.origin = m_playback_origin;
    reading.token = record.token;
    reading.callIndex = record.call;
    reading.callCount = record.calls;
    reading.partOrdinal = record.partOrdinal;
    reading.partCount = record.partCount;
    reading.gap = record.gap;
    reading.interrupted = record.interrupted;
    reading.canSeek = record.canSeek;
    reading.pausedReason = record.pausedReason;
    reading.endedReason = record.endedReason;
    reading.skipped = record.skipped;
    reading.refusals = record.refusals;
    reading.skipNoticeCall = record.skipNoticeCall;
    reading.skipNoticeCount = record.skipNoticeCount;
    reading.seeksLanded = record.seeksLanded;
    const bool failed = record.state == PlaybackFailed;
    if (failed) {
        reading.positionMs = 0;
        reading.durationMs = 0;
        reading.canSeek = false;
        reading.error = record.error.isEmpty() ? QStringLiteral("failed") : record.error;
    }
    // Ended (a stop from the notification, the end, a failure): its last reading stays, and it is polled no more.
    // Zeroed before the publish, so a handler that starts the next play from inside the signal is safe.
    if (failed || record.state == PlaybackStopped) {
        closePlayback();
    }
    publishPlayback(reading);
    // A single recording gone is told here; a part or a playlist item gone comes through the recheck list.
    if (failed && reading.error == QLatin1String("missing") && m_playback_mode == PlaybackSingle) {
        reportRecordingsMissing({reading.path});
    }
}

void
DecoderHostAndroid::followApplicationState() {
    auto* app = qobject_cast<QGuiApplication*>(QCoreApplication::instance());
    if (app == nullptr) {
        return;
    }
    const Qt::ApplicationState state = app->applicationState();
    m_app_away =
        state == Qt::ApplicationSuspended || state == Qt::ApplicationHidden || state == Qt::ApplicationInactive;
    QObject::connect(app, &QGuiApplication::applicationStateChanged, this, &DecoderHostAndroid::noteApplicationState);
}

void
DecoderHostAndroid::noteApplicationState(Qt::ApplicationState state) {
    // Inactive too (#909, R51): an app still visible but not active (split screen) holds the bar's events, so the
    // first active reading must come before the bar says anything.
    if (state == Qt::ApplicationSuspended || state == Qt::ApplicationHidden || state == Qt::ApplicationInactive) {
        m_app_away = true;
        return;
    }
    if (state != Qt::ApplicationActive || !m_app_away) {
        return;
    }
    m_app_away = false;
    // Back on screen: what the player did while Qt was not running (an end, a notification Stop), first, then the
    // files it found gone meanwhile, even when no play is open any more.
    if (!refreshPlayback()) {
        drainRecordingRecheck();
    }
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
DecoderHostAndroid::beginRecordingTransfer(const QStringList& paths, const QStringList& names,
                                           const QList<dsd_file_identity_t>& expected, const QString& reference,
                                           bool zip, const RecordingSelectionToken& token) {
    if (documentTransfersAborted()) {
        return false;
    }
    const qint64 id = ++g_next_transfer_id;
    m_transfer_id = id;
    m_transfer_total = -1;
    m_transfer_worker->start([link = m_transfer_link, id, paths, names, expected, reference, zip, token]() {
        runRecordingTransfer(link, id, paths, names, expected, reference, zip, token);
    });
    return true;
}

void
DecoderHostAndroid::runRecordingTransfer(const std::shared_ptr<TransferLink>& link, qint64 id, const QStringList& paths,
                                         const QStringList& names, const QList<dsd_file_identity_t>& expected,
                                         const QString& reference, bool zip, const RecordingSelectionToken& token) {
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
    QList<dsd_file_identity_t> presentFiles;
    QStringList missing;
    qint64 total = 0;
    // The store read storage again since the selection (a transfer that waited behind another, a share's copy): no
    // file is looked at, and nothing is written.
    if (!token.current()) {
        platformDiscardPendingDownload(reference);
        post([id](DecoderHostAndroid* host) { host->endRecordingTransfer(id, TransferChanged, {}); });
        return;
    }
    bool changed = expected.size() != paths.size();
    for (qsizetype i = 0; i < paths.size() && !changed; ++i) {
        // A file that is not the one selected (another size or time, a link, no file) abandons the whole transfer.
        const RecordingInput input = recordingInputCheck(paths.at(i), expected.at(i));
        if (input == RecordingInput::Missing) {
            missing.append(paths.at(i));
            continue;
        }
        changed = input == RecordingInput::Changed;
        present.append(paths.at(i));
        presentFiles.append(expected.at(i));
        if (zip && i < names.size()) {
            presentNames.append(names.at(i));
        }
        total += expected.at(i).size;
    }
    post([id, total](DecoderHostAndroid* host) { host->noteRecordingTransferTotal(id, total); });

    int result = TransferMissing;
    if (changed) {
        platformDiscardPendingDownload(reference); // nothing written: a Download-folder row (#829) goes
        missing.append(present.last());            // looked at again by the store, with the ones gone
        result = TransferChanged;
    } else if (present.isEmpty()) {
        platformDiscardPendingDownload(reference); // nothing to write: a Download-folder row (#829) goes
    } else if (isContentUri(reference)) {
        result = transferToDocument(id, present, presentNames, presentFiles, reference, zip, token, missing);
    } else if (!zip) {
        // The base host's save, on the link's own abort flag: the host may be gone before it ends.
        result = saveRecordingToLocalDocument(present.first(), presentFiles.first(), reference, token, &link->aborted,
                                              missing);
    } else {
        platformDiscardPendingDownload(reference);
        result = TransferFailed; // a ZIP goes only into a platform document
    }
    post([id, result, missing](DecoderHostAndroid* host) { host->endRecordingTransfer(id, result, missing); });
}

int
DecoderHostAndroid::transferToDocument(qint64 id, const QStringList& paths, const QStringList& names,
                                       const QList<dsd_file_identity_t>& expected, const QString& reference, bool zip,
                                       const RecordingSelectionToken& token, QStringList& missing) {
    QStringList answer;
    {
        const TransferTokenEntry entry(id, token);
        answer = platformTransferRecordings(id, paths, names, expected, reference, zip);
    }
    bool ok = false;
    const int code = answer.isEmpty() ? -1 : answer.first().toInt(&ok);
    missing += answer.mid(1);
    if (ok && ((code >= TransferDone && code <= TransferMissing) || code == TransferChanged)) {
        return code;
    }
    /* No answer: the JNI call failed, maybe before RecordingExport settled the document. A row it settled is no longer
     * pending, so the discard then does nothing. */
    platformDiscardPendingDownload(reference);
    return TransferFailed;
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
        dsd_qt::abandon_worker(m_transfer_worker.release());
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
    }
    closePlayback();
}

bool
DecoderHostAndroid::shareRecordingFile(const QString& path, const QString& title) {
    if (documentTransfersAborted() || !platformCanShareRecording(path)) {
        return false;
    }
    // Held in the store until its copy is made: behind a running export, the limit or a delete would otherwise take
    // the recording before the copy, and the share the user was told began would fail. No hold, no share (r12): the
    // store refuses one for a recording not available now (its folder read again, or its storage seen away), whose
    // path may name another call's file.
    const int hold = holdRecording(path);
    if (hold <= 0) {
        return false;
    }
    m_share_holds.insert(hold);
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
    m_transfer_worker->start([]() {
        platformCleanRecordingShares();
        platformCleanDiagnosticsShares();
    });
}

} // namespace dsd_android
