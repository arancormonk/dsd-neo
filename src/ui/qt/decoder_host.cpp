// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <QLatin1String>
#include <QList>
#include <QMap>
#include <QVariant>
#include <utility>
#include "decoder_host.h"

#include <QByteArray>
#include <QClipboard>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QIODevice>
#include <QSaveFile>
#include <QUrl>

#include "json_store.h"

#include <Qt>
#include <QtGlobal>

namespace dsd_qt {

namespace {

QString
imports_dir_path() {
    // Same root json_store uses, so the library index and the files it points at
    // can never end up under different app-data locations.
    return json_store_path(QStringLiteral("imports"));
}

/** @brief chan.csv, chan (2).csv, chan (3).csv … first name not already taken. */
QString
unique_destination(const QDir& dir, const QString& fileName) {
    if (!dir.exists(fileName)) {
        return dir.filePath(fileName);
    }
    const QFileInfo info(fileName);
    const QString base = info.completeBaseName();
    const QString suffix = info.suffix();
    for (int i = 2; i < 1000; i++) {
        const QString candidate = suffix.isEmpty() ? QStringLiteral("%1 (%2)").arg(base).arg(i)
                                                   : QStringLiteral("%1 (%2).%3").arg(base).arg(i).arg(suffix);
        if (!dir.exists(candidate)) {
            return dir.filePath(candidate);
        }
    }
    return QString();
}

/**
 * @brief @p replacePath when it names a file directly inside @p dir, else empty.
 *
 * Canonicalizes the *directory*, not the file: QFileInfo::canonicalPath() gives
 * up on a missing leaf and returns ".", which would silently turn an update of a
 * copy that vanished behind the app's back into a fresh unique copy the caller
 * then rejects and never cleans up. The Android host resolves the parent too, so
 * this keeps the two in step.
 */
QString
replace_destination(const QDir& dir, const QString& replacePath) {
    if (replacePath.isEmpty()) {
        return QString();
    }
    const QString canonicalDir = QFileInfo(dir.absolutePath()).canonicalFilePath();
    const QString canonicalTarget = QFileInfo(QFileInfo(replacePath).absolutePath()).canonicalFilePath();
    if (canonicalDir.isEmpty() || canonicalTarget != canonicalDir) {
        return QString();
    }
    return replacePath;
}

/**
 * @brief Copy @p source to @p out in chunks, at most @p maxBytes of it (no limit when negative).
 *
 * Loops on the read result rather than atEnd(): a read error returns an empty
 * chunk without advancing pos(), so an atEnd()-driven loop would spin forever on
 * it, and a readable source QFile reports size 0 for (a FIFO, /proc) claims to be
 * at its end before a byte has been read. The cap is counted on what was read for
 * the same reason, not taken from the source's reported size.
 *
 * @param abort Optional; checked before every chunk. Once raised the copy fails.
 * @return A DecoderHost::DocumentCopy value (a failed write to @p out is DocumentLocalFailed); any failure cancels
 *         @p out.
 */
int
copy_stream_capped(QIODevice& source, QSaveFile& out, qint64 maxBytes, const std::atomic<bool>* abort = nullptr) {
    QByteArray chunk(1 << 16, Qt::Uninitialized);
    qint64 total = 0;
    for (;;) {
        if (abort != nullptr && abort->load()) {
            out.cancelWriting();
            return DecoderHost::DocumentFailed;
        }
        const qint64 n = source.read(chunk.data(), chunk.size());
        if (n < 0) {
            out.cancelWriting();
            return DecoderHost::DocumentFailed;
        }
        if (n == 0) {
            return DecoderHost::DocumentCopied;
        }
        total += n;
        if (maxBytes >= 0 && total > maxBytes) {
            out.cancelWriting();
            return DecoderHost::DocumentTooLarge;
        }
        if (out.write(chunk.constData(), n) != n) {
            out.cancelWriting();
            return DecoderHost::DocumentLocalFailed;
        }
    }
}

/** @brief Copy @p source to @p out in chunks; false on any read or write error. */
bool
copy_stream(QIODevice& source, QSaveFile& out) {
    return copy_stream_capped(source, out, -1) == DecoderHost::DocumentCopied;
}

/** @brief The local path a file:// URL or a plain absolute path names; empty for anything else (a content URI). */
QString
local_document_path(const QString& reference) {
    const QUrl url(reference);
    if (url.isLocalFile()) {
        return url.toLocalFile();
    }
    return QDir::isAbsolutePath(reference) ? reference : QString();
}

/** @brief Open @p out for writing, readable and writable by the owner only. */
bool
open_private(QSaveFile& out) {
    return out.open(QIODevice::WriteOnly) && out.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
}

} // namespace

DecoderHost::DecoderHost(QObject* parent) : QObject(parent) {
    /* sessionState(), and the three properties derived from it, default to a view of
     * isRunning() — whose change signal is runningChanged. A host that leaves that
     * default in place therefore never emits sessionStateChanged, so the monitoring
     * view would never appear and UiController would never clear the models between
     * runs, even though the engine was decoding the whole time. Forward it here so the
     * documented minimal host works. A host that owns a real state machine emits
     * sessionStateChanged itself; the extra emission is absorbed by the value
     * comparison in UiController::onSessionStateChanged and by QML binding equality. */
    connect(this, &DecoderHost::runningChanged, this, &DecoderHost::sessionStateChanged);
}

DecoderHost::~DecoderHost() = default;

int
DecoderHost::copyDocumentToFile(const QString& reference, const QString& destPath, qint64 maxBytes) const {
    const QString path = local_document_path(reference);
    if (path.isEmpty() || destPath.isEmpty() || maxBytes < 0 || !QFileInfo(path).isFile()) {
        return DocumentFailed;
    }
    QFile source(path);
    if (!source.open(QIODevice::ReadOnly)) {
        return DocumentFailed;
    }
    if (source.size() > maxBytes) {
        return DocumentTooLarge;
    }
    // QSaveFile writes beside the destination and renames on commit, so no
    // failure, cap included, can leave a partial file there. Every failure on
    // that side is this device's, not the document's.
    QSaveFile out(destPath);
    if (!open_private(out)) {
        return DocumentLocalFailed;
    }
    const int copied = copy_stream_capped(source, out, maxBytes, &m_documentTransfersAborted);
    if (copied != DocumentCopied) {
        return copied;
    }
    return out.commit() ? DocumentCopied : DocumentLocalFailed;
}

bool
DecoderHost::copyFileToDocument(const QString& sourcePath, const QString& reference) const {
    return copyFileToLocalDocument(sourcePath, reference, &m_documentTransfersAborted);
}

bool
DecoderHost::copyFileToLocalDocument(const QString& sourcePath, const QString& reference,
                                     const std::atomic<bool>* aborted) {
    const QString path = local_document_path(reference);
    QFile source(sourcePath);
    if (path.isEmpty() || !QFileInfo(sourcePath).isFile() || !source.open(QIODevice::ReadOnly)) {
        return false;
    }
    QSaveFile out(path);
    return open_private(out) && copy_stream_capped(source, out, -1, aborted) == DocumentCopied && out.commit();
}

void
DecoderHost::abortDocumentTransfers() {
    m_documentTransfersAborted.store(true);
}

QVariantMap
DecoderHost::documentInfo(const QString& path) const {
    const QFileInfo info(path);
    return {{"name", info.fileName()},
            {"exists", info.isFile()},
            {"sizeBytes", info.size()},
            {"modified", info.lastModified().toString(Qt::ISODate)}};
}

bool
DecoderHost::copyText(const QString& text) const {
    if (auto* clipboard = QGuiApplication::clipboard()) {
        clipboard->setText(text);
        return true;
    }
    return false;
}

QString
DecoderHost::licenseNotices() const {
    QStringList notices;
    for (const auto* name : {"LICENSE",
                             "COPYRIGHT",
                             "THIRD_PARTY.md",
                             "src/ui/qt/fonts/IBMPlex-LICENSE.txt",
                             "src/third_party/ezpwd/lesser.txt",
                             "pffft-LICENSE.txt",
                             "pffft-NOTICES.txt",
                             "tetra_crypto-LICENSE.txt",
                             "legal/status.md",
                             "legal/source-delivery.md",
                             "LICENSES/ISC.txt",
                             "LICENSES/Unlicense.txt",
                             "LICENSES/gqrx-scanner-MIT.txt",
                             "LICENSES/LicenseRef-BCH3-Noncommercial.txt",
                             "LICENSES/LicenseRef-Simon-Rockliff.txt",
                             "mbelib-neo/LICENSE",
                             "mbelib-neo/GPL-2.0-or-later.txt",
                             "mbelib-neo/ISC.txt",
                             "mbelib-neo/LicenseRef-PFFFT.txt",
                             "mbelib-neo/LicenseRef-ETSI-EN-300-395-2-data.txt",
                             "mbelib-neo/upstream-mbelib-attribution.md",
                             "mbelib-neo/legal/status.md",
                             "mbelib-neo/legal/patents.md",
                             "mbelib-neo/pffft/LICENSE.txt",
                             "mbelib-neo/pffft/PFFFT-NOTICES.txt"}) {
        QFile file(QStringLiteral(":/dsdneo/notices/") + QLatin1String(name));
        if (file.open(QIODevice::ReadOnly)) {
            notices.append(QLatin1String(name) + QStringLiteral("\n\n") + QString::fromUtf8(file.readAll()));
        }
    }
    for (const auto* name :
         {"libusb-LGPL-2.1-or-later.txt", "librtlsdr-GPL-2.0-or-later.txt", "libairspy-LICENSE.txt"}) {
        QFile file(QStringLiteral("assets:/doc/dsd-neo/licenses/") + QLatin1String(name));
        if (file.open(QIODevice::ReadOnly)) {
            notices.append(QString::fromUtf8(file.readAll()));
        }
    }
    return notices.join(QStringLiteral("\n\n"));
}

QStringList
DecoderHost::recordingRoots() const {
    // json_store's root, like the imports: one app data location for everything the app keeps.
    return {json_store_path(QStringLiteral("Call recordings"))};
}

void
DecoderHost::playRecording(const QString& path) {
    if (path.isEmpty()) {
        return;
    }
    // A play that fails still replaces the recording playing: the player stops (stopPlayback(), which a platform
    // overrides) before the failure is published, so the store lets go of a file only once nothing plays it.
    if (!QFileInfo(path).isFile()) {
        stopPlayback();
        publishPlayback(path, PlaybackFailed, 0, 0, QStringLiteral("missing"));
        reportRecordingsMissing({path});
        return;
    }
    if (!playbackSupported()) {
        stopPlayback();
        publishPlayback(path, PlaybackFailed, 0, 0, QStringLiteral("unsupported"));
        return;
    }
    // Published before the platform is asked, so the store holds the file from the first moment it may be open.
    publishPlayback(path, PlaybackPreparing, 0, 0, QString());
    beginPlayback(path);
}

void
DecoderHost::beginPlayback(const QString& path) {
    publishPlayback(path, PlaybackFailed, 0, 0, QStringLiteral("unsupported"));
}

void
DecoderHost::stopPlayback() {
    publishPlayback(QString(), PlaybackStopped, 0, 0, QString());
}

void
DecoderHost::publishPlayback(const QString& path, int state, qint64 positionMs, qint64 durationMs,
                             const QString& error) {
    const bool changed = path != m_playbackPath || state != m_playbackState || durationMs != m_playbackDurationMs
                         || error != m_playbackError;
    const bool moved = positionMs != m_playbackPositionMs;
    m_playbackPath = path;
    m_playbackState = state;
    m_playbackPositionMs = positionMs;
    m_playbackDurationMs = durationMs;
    m_playbackError = error;
    if (changed) {
        Q_EMIT playbackChanged();
    }
    if (moved) {
        Q_EMIT playbackPositionChanged();
    }
}

bool
DecoderHost::exportRecordings(const QVariantMap& selection, const QString& reference) {
    return beginTransfer(selection, reference, true);
}

bool
DecoderHost::saveRecording(const QVariantMap& selection, const QString& reference) {
    return beginTransfer(selection, reference, false);
}

bool
DecoderHost::beginTransfer(const QVariantMap& selection, const QString& reference, bool zip) {
    const int id = selection.value(QStringLiteral("selection")).toInt();
    const QStringList paths = selection.value(QStringLiteral("paths")).toStringList();
    const QStringList names = selection.value(QStringLiteral("names")).toStringList();
    if (recordingTransferActive()) {
        announceTransferEnd(id, TransferBusy, 0);
        return false;
    }
    if (zip && !recordingExportSupported()) {
        announceTransferEnd(id, TransferUnsupported, 0);
        return false;
    }
    // A save copies one recording; a zip names each entry, so the two lists go together.
    if (id <= 0 || reference.isEmpty() || paths.isEmpty() || (zip ? names.size() != paths.size() : paths.size() != 1)) {
        announceTransferEnd(id, TransferFailed, 0);
        return false;
    }
    // No file is looked at here, on the GUI thread (an export may name tens of thousands): the transfer finds the ones
    // gone on its own thread, reports them at its end and publishes the total it really has.
    m_transferSelection = id;
    m_transferBytes = 0;
    m_transferTotalBytes = selection.value(QStringLiteral("bytes")).toLongLong();
    Q_EMIT recordingTransferChanged();
    if (!beginRecordingTransfer(paths, zip ? names : QStringList(), reference, zip)) {
        finishRecordingTransfer(TransferFailed, {});
        return false;
    }
    return true;
}

bool
DecoderHost::beginRecordingTransfer(const QStringList& paths, const QStringList& names, const QString& reference,
                                    bool zip) {
    (void)names;
    if (zip || paths.size() != 1) {
        return false;
    }
    const QFileInfo file(paths.first());
    if (!file.isFile()) {
        publishRecordingTransferProgress(0, 0);
        finishRecordingTransfer(TransferMissing, paths);
        return true;
    }
    publishRecordingTransferProgress(0, file.size());
    const bool saved = copyFileToDocument(paths.first(), reference);
    if (saved) {
        publishRecordingTransferProgress(file.size(), file.size());
    }
    finishRecordingTransfer(saved ? TransferDone : TransferFailed, {});
    return true;
}

void
DecoderHost::publishRecordingTransferProgress(qint64 bytes, qint64 totalBytes) {
    if (!recordingTransferActive() || (bytes == m_transferBytes && totalBytes == m_transferTotalBytes)) {
        return;
    }
    m_transferBytes = bytes;
    m_transferTotalBytes = totalBytes;
    Q_EMIT recordingTransferChanged();
}

void
DecoderHost::finishRecordingTransfer(int result, const QStringList& missing) {
    if (!recordingTransferActive()) {
        return;
    }
    reportRecordingsMissing(missing);
    const int selection = m_transferSelection;
    const int gone = static_cast<int>(missing.size());
    m_transferSelection = -1;
    m_transferBytes = 0;
    m_transferTotalBytes = 0;
    Q_EMIT recordingTransferChanged();
    announceTransferEnd(selection, result, gone);
}

void
DecoderHost::announceTransferEnd(int selection, int result, int missing) {
    QMetaObject::invokeMethod(
        this, [this, selection, result, missing]() { Q_EMIT recordingTransferFinished(selection, result, missing); },
        Qt::QueuedConnection);
}

bool
DecoderHost::shareRecording(const QString& path, const QString& title) {
    if (path.isEmpty() || !shareSupported() || !m_recordingIndexed || !m_recordingIndexed(path)) {
        return false;
    }
    if (!QFileInfo(path).isFile()) {
        reportRecordingsMissing({path});
        return false;
    }
    return shareRecordingFile(path, title);
}

void
DecoderHost::setRecordingIndex(std::function<bool(const QString&)> indexed) {
    m_recordingIndexed = std::move(indexed);
}

void
DecoderHost::setRecordingHold(std::function<int(const QString&)> hold, std::function<void(int)> release) {
    m_recordingHold = std::move(hold);
    m_recordingRelease = std::move(release);
}

int
DecoderHost::holdRecording(const QString& path) const {
    return m_recordingHold ? m_recordingHold(path) : 0;
}

void
DecoderHost::releaseRecordingHold(int hold) const {
    if (hold > 0 && m_recordingRelease) {
        m_recordingRelease(hold);
    }
}

void
DecoderHost::reportRecordingsMissing(const QStringList& paths) {
    if (!paths.isEmpty()) {
        Q_EMIT recordingFilesMissing(paths);
    }
}

QString
DecoderHost::importDocument(const QString& reference, const QString& fileName, const QString& replacePath) {
    const QUrl url(reference);
    return importLocalFile(url.isLocalFile() ? url.toLocalFile() : reference, fileName, replacePath);
}

QString
DecoderHost::importLocalFile(const QString& sourcePath, const QString& fileName, const QString& replacePath) {
    QFile source(sourcePath);
    if (!source.open(QIODevice::ReadOnly)) {
        return QString();
    }

    QDir importsDir(imports_dir_path());
    if (!importsDir.mkpath(QStringLiteral("."))) {
        return QString();
    }

    // The display name comes from the picker; keep only its last component so
    // it cannot navigate out of the imports directory.
    QString name = QFileInfo(fileName).fileName();
    if (name.isEmpty()) {
        name = QFileInfo(sourcePath).fileName();
    }
    if (name.isEmpty()) {
        return QString();
    }

    QString destination = replace_destination(importsDir, replacePath);
    if (destination.isEmpty()) {
        destination = unique_destination(importsDir, name);
    }
    if (destination.isEmpty()) {
        return QString();
    }

    // QSaveFile stages beside the target and renames over it on commit, so an
    // update never leaves a half-written CSV where a saved system points.
    QSaveFile out(destination);
    if (!open_private(out)) {
        return QString();
    }
    if (!copy_stream(source, out) || !out.commit()) {
        return QString();
    }
    return destination;
}

} // namespace dsd_qt
