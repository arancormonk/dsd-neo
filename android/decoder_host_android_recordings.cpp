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
#include <QMetaObject>
#include <QString>
#include <QStringList>
#include <QThreadPool>
#include <Qt>
#include <QtGlobal>
#include <atomic>
#include "decoder_host.h"
#include "decoder_host_android.h"

namespace dsd_android {
namespace {

/* RecordingExport tells transfers apart by id, process-wide, so a host made after another never reuses one. */
std::atomic<qint64> g_next_transfer_id{0};

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

bool
DecoderHostAndroid::beginRecordingTransfer(const QStringList& paths, const QStringList& names, const QString& reference,
                                           bool zip) {
    if (documentTransfersAborted()) {
        return false;
    }
    const qint64 id = ++g_next_transfer_id;
    m_transfer_id = id;
    m_transfer_total = -1;
    m_transfer_worker.setMaxThreadCount(1);
    m_transfer_worker.start(
        [this, id, paths, names, reference, zip]() { runRecordingTransfer(id, paths, names, reference, zip); });
    return true;
}

void
DecoderHostAndroid::runRecordingTransfer(qint64 id, const QStringList& paths, const QStringList& names,
                                         const QString& reference, bool zip) {
    // The worker thread: no QObject state is touched here, only queued back.
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
    QMetaObject::invokeMethod(
        this, [this, id, total]() { noteRecordingTransferTotal(id, total); }, Qt::QueuedConnection);

    int result = TransferMissing;
    if (!present.isEmpty()) {
        if (isContentUri(reference)) {
            const QStringList answer = platformTransferRecordings(id, present, presentNames, reference, zip);
            bool ok = false;
            const int code = answer.isEmpty() ? -1 : answer.first().toInt(&ok);
            result = ok && code >= TransferDone && code <= TransferMissing ? code : TransferFailed;
            missing += answer.mid(1);
        } else if (!zip) {
            result = copyFileToDocument(present.first(), reference) ? TransferDone : TransferFailed;
        } else {
            result = TransferFailed; // a ZIP goes only into a platform document
        }
    }
    QMetaObject::invokeMethod(
        this, [this, id, result, missing]() { endRecordingTransfer(id, result, missing); }, Qt::QueuedConnection);
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
    cancelRecordingTransfer();
    m_transfer_worker.waitForDone();
    if (m_playback_generation != 0) {
        platformStopPlayback();
        m_playback_generation = 0;
    }
}

bool
DecoderHostAndroid::shareRecordingFile(const QString& path, const QString& title) {
    return platformShareRecording(path, title);
}

} // namespace dsd_android
