// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <QLatin1String>
#include <QList>
#include <QMap>
#include <QVariant>
#include <QVariantList>
#include <tuple>
#include <utility>
#include "decoder_host.h"

#include <QByteArray>
#include <QClipboard>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QIODevice>
#include <QLatin1Char>
#include <QSaveFile>
#include <QString>
#include <QStringList>

#include "document_import_transport.h"
#include "document_save_transport.h"
#include "json_store.h"
#include "platform_path.h"
#include "qstring_scrub.h"

#include <Qt>
#include <QtGlobal>
#include <algorithm>
#include <cerrno>
#include <dsd-neo/platform/file_compat.h>
#include <stdint.h>
#ifdef DSD_NEO_TEST_HOOKS
#include <functional>
#include "decoder_host_test_hooks.h"
#endif

namespace dsd_qt {

namespace {

#ifdef DSD_NEO_TEST_HOOKS
std::function<void(const QString&)>&
after_recording_check() {
    static std::function<void(const QString&)> hook;
    return hook;
}
#endif

/* Whether the recording at @p path is a file there now: a play's one admission check. */
bool
recording_file_present(const QString& path) {
    const bool present = QFileInfo(path).isFile();
#ifdef DSD_NEO_TEST_HOOKS
    if (present && after_recording_check()) {
        after_recording_check()(path);
    }
#endif
    return present;
}

/* @p selection's files as the store selected them (sizes, mtimeSecs, mtimeNanos), one for each of @p count paths, into
 * @p out; false when they are not all there. */
bool
selected_files(const QVariantMap& selection, qsizetype count, QList<dsd_file_identity_t>* out) {
    const QVariantList sizes = selection.value(QStringLiteral("sizes")).toList();
    const QVariantList secs = selection.value(QStringLiteral("mtimeSecs")).toList();
    const QVariantList nanos = selection.value(QStringLiteral("mtimeNanos")).toList();
    if (sizes.size() != count || secs.size() != count || nanos.size() != count) {
        return false;
    }
    out->reserve(count);
    for (qsizetype i = 0; i < count; i++) {
        dsd_file_identity_t file{};
        file.kind = 1;
        file.size = sizes.at(i).toLongLong();
        file.mtime_s = secs.at(i).toLongLong();
        file.mtime_ns = static_cast<int32_t>(nanos.at(i).toInt());
        out->append(file);
    }
    return true;
}

/** @brief Keep a weak reference to @p transport, dropping the expired ones; abort it when @p aborted is already set. */
template <typename Transport>
void
keep_handed_out(std::vector<std::weak_ptr<Transport>>& handedOut, const std::shared_ptr<Transport>& transport,
                const std::atomic<bool>& aborted) {
    handedOut.erase(std::remove_if(handedOut.begin(), handedOut.end(),
                                   [](const std::weak_ptr<Transport>& kept) { return kept.expired(); }),
                    handedOut.end());
    handedOut.push_back(transport);
    /* abortDocumentTransfers() raises the flag before it takes the lock, so a transport kept after its sweep sees the
     * flag here. */
    if (aborted.load()) {
        transport->abort();
    }
}

/** @brief Abort every transport in @p handedOut still alive. */
template <typename Transport>
void
abort_handed_out(const std::vector<std::weak_ptr<Transport>>& handedOut) {
    for (const std::weak_ptr<Transport>& kept : handedOut) {
        if (const std::shared_ptr<Transport> transport = kept.lock()) {
            transport->abort();
        }
    }
}

/** @brief Where the source code of the Qt version this app is built with is, named from Qt's own version macros, and
 * the packaged file with the directions written from the build's Qt kit (the Public Suffix List's pinned source). */
QString
qt_source_notice() {
    const QString series = QStringLiteral("%1.%2").arg(QT_VERSION_MAJOR).arg(QT_VERSION_MINOR);
    const QString version = QStringLiteral("%1.%2").arg(series).arg(QT_VERSION_PATCH);
    QStringList lines{
        QStringLiteral("Qt source code"),
        QString(),
        QStringLiteral(
            "This app includes Qt %1 (The Qt Company Ltd. and other contributors) under the GNU Lesser "
            "General Public License version 3, whose text is licenses/LGPL-3.0-only.txt in the list below. That "
            "license adds permissions to the GNU General Public License version 3, whose text is LICENSE. dsd-neo "
            "applies no patches to Qt: the app carries Qt's libraries, plugins and Android code as Qt %1 "
            "publishes them.")
            .arg(version),
        QString(),
        QStringLiteral("The complete source code of Qt %1 is in Qt's official archive:").arg(version),
    };
    for (const auto* module : {"qtbase", "qtdeclarative", "qtsvg"}) {
        lines.append(QStringLiteral("https://download.qt.io/archive/qt/%1/%2/submodules/%3-everywhere-src-%2.tar.xz")
                         .arg(series, version, QLatin1String(module)));
    }
    lines.append(QString());
    lines.append(QStringLiteral("While Qt %1 is a current release, the same files are also under "
                                "https://download.qt.io/official_releases/qt/%2/%1/submodules/.")
                     .arg(version, series));
    lines.append(QString());
    lines.append(QStringLiteral("licenses/Qt-SOURCE.txt in the list below gives these directions as the release "
                                "does, written from the Qt kit the app was built with. It also names the source form "
                                "of the Public Suffix List that Qt Network compiles in, at the commit this app "
                                "carries."));
    return lines.join(QLatin1Char('\n'));
}

/** @brief Whether @p name, a line of the notice index, is a plain path inside the notice folder. */
bool
notice_path_ok(const QString& name) {
    if (name.isEmpty() || name.startsWith(QLatin1Char('/')) || name.contains(QLatin1Char('\\'))
        || name.contains(QLatin1Char(':'))) {
        return false;
    }
    const QStringList parts = name.split(QLatin1Char('/'));
    return std::none_of(parts.cbegin(), parts.cend(), [](const QString& part) {
        return part.isEmpty() || part == QLatin1String(".") || part == QLatin1String("..");
    });
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

bool
DecoderHost::copyFileToDocument(const QString& sourcePath, const QString& reference) const {
    return copyFileToLocalDocument(sourcePath, reference, &m_documentTransfersAborted);
}

bool
DecoderHost::copyFileToLocalDocument(const QString& sourcePath, const QString& reference,
                                     const std::atomic<bool>* aborted) {
    return copy_file_to_local_document(sourcePath, reference, aborted);
}

void
DecoderHost::abortDocumentTransfers() {
    m_documentTransfersAborted.store(true);
    const std::lock_guard<std::mutex> lock(m_transportsLock);
    abort_handed_out(m_importTransports);
    abort_handed_out(m_saveTransports);
}

std::shared_ptr<DocumentImportTransport>
DecoderHost::importTransport() const {
    return keepImportTransport(std::make_shared<DocumentImportTransport>());
}

std::shared_ptr<DocumentImportTransport>
DecoderHost::keepImportTransport(std::shared_ptr<DocumentImportTransport> transport) const {
    const std::lock_guard<std::mutex> lock(m_transportsLock);
    keep_handed_out(m_importTransports, transport, m_documentTransfersAborted);
    return transport;
}

std::shared_ptr<DocumentSaveTransport>
DecoderHost::saveTransport() const {
    return keepSaveTransport(std::make_shared<DocumentSaveTransport>());
}

std::shared_ptr<DocumentSaveTransport>
DecoderHost::keepSaveTransport(std::shared_ptr<DocumentSaveTransport> transport) const {
    const std::lock_guard<std::mutex> lock(m_transportsLock);
    keep_handed_out(m_saveTransports, transport, m_documentTransfersAborted);
    return transport;
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
DecoderHost::qtSourceNotice() {
    return qt_source_notice();
}

QStringList
DecoderHost::licenseFiles() const {
    const QString directory = noticeDirectory();
    QFile index(directory + QStringLiteral("/notice-index.txt"));
    if (directory.isEmpty() || !index.open(QIODevice::ReadOnly)) {
        return {};
    }
    QStringList names;
    for (const QString& line : QString::fromUtf8(index.readAll()).split(QLatin1Char('\n'))) {
        const QString name = line.trimmed();
        if (!name.isEmpty()) {
            names.append(name);
        }
    }
    return names;
}

QString
DecoderHost::licenseFileText(const QString& name) const {
    QFile file(noticeDirectory() + QLatin1Char('/') + name);
    if (notice_path_ok(name) && licenseFiles().contains(name) && file.open(QIODevice::ReadOnly)) {
        return QString::fromUtf8(file.readAll());
    }
    return QStringLiteral("This file is missing from the package.");
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
    if (!recording_file_present(path)) {
        stopPlayback();
        publishPlayback(path, PlaybackFailed, 0, 0, QStringLiteral("missing"));
        reportRecordingsMissing({path});
        return;
    }
    startRecordingPlayback(path);
}

int
DecoderHost::playRecordingOwned(const QString& path) {
    // A file that is gone is refused before the play is taken, as a refused playlist is: what plays plays on and
    // nothing is published; the store is asked to look again, and the sheet says the recording is gone. That one
    // check is the only one: the start after it never reaches playRecording()'s, which stops what plays.
    if (path.isEmpty()) {
        return 0;
    }
    if (!recording_file_present(path)) {
        reportRecordingsMissing({path});
        return 0;
    }
    startRecordingPlayback(path);
    // A play the platform took is published Preparing under its generation; one that failed at once is not.
    const bool taken = m_playback.state == PlaybackPreparing && m_playback.path == path;
    return taken ? m_playback.generation : 0;
}

void
DecoderHost::startRecordingPlayback(const QString& path) {
    if (!playbackSupported()) {
        stopPlayback();
        publishPlayback(path, PlaybackFailed, 0, 0, QStringLiteral("unsupported"));
        return;
    }
    // Published before the platform is asked, so the store holds the file from the first moment it may be open.
    publishPlayback(path, PlaybackPreparing, 0, 0, QString());
    beginPlayback(path);
}

namespace {

/* The call of playlist item @p item (0 when it names none), into @p call; false when it names one that is no number. */
bool
playback_item_call(const QVariantMap& item, int* call) {
    const QVariant value = item.value(QStringLiteral("call"));
    if (!value.isValid()) {
        *call = 0;
        return true;
    }
    bool ok = false;
    *call = value.toInt(&ok);
    return ok;
}

/* Whether queued item @p item names the file the store listed (issue #909): a size from 0, and the modification time's
 * seconds and nanoseconds, each a number. The store lists them for every recording, so an item without them is
 * malformed; the queue would otherwise have no identity to check the file found at its turn against. */
bool
playback_item_identified(const QVariantMap& item) {
    bool sizeOk = false;
    bool secOk = false;
    bool nanosOk = false;
    const qint64 size = item.value(QStringLiteral("size")).toLongLong(&sizeOk);
    (void)item.value(QStringLiteral("mtimeSec")).toLongLong(&secOk);
    (void)item.value(QStringLiteral("mtimeNanos")).toInt(&nanosOk);
    return sizeOk && size >= 0 && secOk && nanosOk;
}

/* Whether item @p index of a queued request is well formed (issue #909): it names a path, and its call is 0 for a part
 * of one call, else the call before's (@p previous) or the next, from 0 with no hole. A part or playlist item (@p queued)
 * also names its identity. Writes the item's call to @p call. */
bool
playback_item_valid(const QVariantMap& item, qsizetype index, int previous, bool parts, bool queued, int* call) {
    if (item.value(QStringLiteral("path")).toString().isEmpty() || !playback_item_call(item, call)) {
        return false;
    }
    const bool inOrder = *call == previous || (index > 0 && *call == previous + 1);
    return inOrder && (!parts || *call == 0) && (!queued || playback_item_identified(item));
}

/* The readings a change of which is news on playbackChanged: all but the position. */
auto
playback_shown(const DecoderHost::PlaybackReading& r) {
    return std::tie(r.generation, r.state, r.durationMs, r.error, r.mode, r.origin, r.path, r.title, r.key, r.size,
                    r.mtimeSec, r.mtimeNanos, r.token, r.callIndex, r.callCount, r.partOrdinal, r.partCount, r.gap,
                    r.interrupted, r.canSeek, r.pausedReason, r.endedReason, r.skipped, r.refusals, r.skipNoticeCall,
                    r.skipNoticeCount, r.seeksLanded);
}

} // namespace

bool
DecoderHost::playbackItemsValid(const QVariantList& items, int mode) {
    if (items.isEmpty() || mode < PlaybackSingle || mode > PlaybackPlaylist
        || (mode == PlaybackSingle && items.size() != 1)) {
        return false;
    }
    // Calls from 0 up, with no hole: each item's call is the one before's, or the next. Parts are all of one call.
    // Parts and a playlist check each file at its turn, so every item names its identity; a single play names a path.
    int previous = 0;
    for (qsizetype i = 0; i < items.size(); ++i) {
        int call = 0;
        if (!playback_item_valid(items.at(i).toMap(), i, previous, mode == PlaybackParts, mode != PlaybackSingle,
                                 &call)) {
            return false;
        }
        previous = call;
    }
    return true;
}

int
DecoderHost::playRecordings(const QVariantList& items, int mode, const QString& origin) {
    (void)origin;
    if (!playbackItemsValid(items, mode)) {
        return 0;
    }
    publishPlayback(items.first().toMap().value(QStringLiteral("path")).toString(), PlaybackFailed, 0, 0,
                    QStringLiteral("unsupported"));
    return 0;
}

void
DecoderHost::beginPlayback(const QString& path) {
    publishPlayback(path, PlaybackFailed, 0, 0, QStringLiteral("unsupported"));
}

namespace {

/* A play still open: opening, starting, playing or paused (issue #909), not stopped or failed. */
bool
playback_open(int state) {
    return state == DecoderHost::PlaybackPreparing || state == DecoderHost::PlaybackStarting
           || state == DecoderHost::PlaybackPlaying || state == DecoderHost::PlaybackPaused;
}

} // namespace

void
DecoderHost::stopPlaybackGeneration(int generation) {
    if (generation > 0 && generation == m_playback.generation && playback_open(m_playback.state)) {
        stopPlayback();
    }
}

void
DecoderHost::stopPlayback() {
    publishPlayback(PlaybackReading());
}

QString
DecoderHost::playbackHoldPath() const {
    return playback_open(m_playback.state) && m_playback.mode == PlaybackSingle ? m_playback.path : QString();
}

void
DecoderHost::publishPlayback(const PlaybackReading& reading) {
    const bool changed = playback_shown(reading) != playback_shown(m_playback);
    const bool moved = reading.positionMs != m_playback.positionMs;
    m_playback = reading;
    if (changed) {
        Q_EMIT playbackChanged();
    }
    if (moved) {
        Q_EMIT playbackPositionChanged();
    }
}

void
DecoderHost::publishPlayback(const QString& path, int state, qint64 positionMs, qint64 durationMs,
                             const QString& error) {
    PlaybackReading reading;
    reading.path = path;
    reading.state = state;
    reading.positionMs = positionMs;
    reading.durationMs = durationMs;
    reading.error = error;
    reading.canSeek =
        (state == PlaybackPlaying || state == PlaybackPaused || state == PlaybackStarting) && durationMs > 0;
    publishPlayback(reading);
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
    const auto refuse = [this, id, &reference](int result) {
        // Nothing will write the document: a Download-folder save the picker made for it goes (#829).
        discardPendingDownload(reference);
        announceTransferEnd(id, result, 0);
        return false;
    };
    if (recordingTransferActive()) {
        return refuse(TransferBusy);
    }
    if (zip && !recordingExportSupported()) {
        return refuse(TransferUnsupported);
    }
    // A save copies one recording; a zip names each entry, so the two lists go together, and every file comes with
    // what it was when selected.
    QList<dsd_file_identity_t> expected;
    if (id <= 0 || reference.isEmpty() || paths.isEmpty() || (zip ? names.size() != paths.size() : paths.size() != 1)
        || !selected_files(selection, paths.size(), &expected)) {
        return refuse(TransferFailed);
    }
    // The store read storage again since the selection: a path it names may now be another recording's. The token
    // goes with the transfer, which checks it again before it reads a file and before it keeps the document.
    RecordingSelectionToken token;
    if (m_recordingSelectionCurrent && !m_recordingSelectionCurrent(id, &token)) {
        return refuse(TransferChanged);
    }
    // No file is looked at here, on the GUI thread (an export may name tens of thousands): the transfer finds the ones
    // gone on its own thread, reports them at its end and publishes the total it really has.
    m_transferSelection = id;
    m_transferBytes = 0;
    m_transferTotalBytes = selection.value(QStringLiteral("bytes")).toLongLong();
    Q_EMIT recordingTransferChanged();
    if (!beginRecordingTransfer(paths, zip ? names : QStringList(), expected, reference, zip, token)) {
        discardPendingDownload(reference);
        finishRecordingTransfer(TransferFailed, {});
        return false;
    }
    return true;
}

bool
DecoderHost::beginRecordingTransfer(const QStringList& paths, const QStringList& names,
                                    const QList<dsd_file_identity_t>& expected, const QString& reference, bool zip,
                                    const RecordingSelectionToken& token) {
    (void)names;
    if (zip || paths.size() != 1 || expected.size() != 1) {
        return false;
    }
    const qint64 size = expected.first().size;
    publishRecordingTransferProgress(0, size);
    QStringList missing;
    const int result = saveRecordingToLocalDocument(paths.first(), expected.first(), reference, token,
                                                    &m_documentTransfersAborted, missing);
    if (result == TransferDone) {
        publishRecordingTransferProgress(size, size);
    }
    // Gone, or another file the next selection must not name as it: either is looked at again by the store.
    finishRecordingTransfer(result, missing);
    return true;
}

int
DecoderHost::saveRecordingToLocalDocument(const QString& path, const dsd_file_identity_t& expected,
                                          const QString& reference, const RecordingSelectionToken& token,
                                          const std::atomic<bool>* aborted, QStringList& missing) {
    if (!token.current()) {
        return TransferChanged; // the store read storage again: nothing read, nothing written
    }
    FILE* fp = nullptr;
    const RecordingInput input = openRecordingInput(path, expected, &fp);
    if (input != RecordingInput::Selected) {
        missing.append(path);
        return input == RecordingInput::Missing ? TransferMissing : TransferChanged;
    }
    QFile source;
    const QString target = local_document_path(reference);
    QSaveFile out(target);
    int result = TransferFailed;
    if (!target.isEmpty() && source.open(fp, QIODevice::ReadOnly) && open_private(out)
        && copy_stream_capped(source, out, -1, [aborted](qint64) { return aborted == nullptr || !aborted->load(); })
               == DocumentCopied) {
        if (!token.current()) {
            out.cancelWriting(); // read again while it was copied: the document is not kept
            result = TransferChanged;
        } else if (out.commit()) {
            result = TransferDone;
        }
    }
    source.close();
    (void)fclose(fp);
    return result;
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

qint64
DecoderHost::requestOpenDocument() {
    return refuseDocumentRequest();
}

qint64
DecoderHost::requestSaveDocument(const QString& suggestedName, const QString& mimeType) {
    (void)suggestedName;
    (void)mimeType;
    return refuseDocumentRequest();
}

qint64
DecoderHost::refuseDocumentRequest() {
    const qint64 id = nextDocumentRequestId();
    m_refusedDocumentRequests.insert(id);
    QMetaObject::invokeMethod(
        this,
        [this, id] {
            if (m_refusedDocumentRequests.remove(id)) {
                Q_EMIT documentPickUnavailable(id, QStringLiteral("unsupported"));
            }
        },
        Qt::QueuedConnection);
    return id;
}

void
DecoderHost::cancelDocumentRequest(qint64 requestId) {
    m_refusedDocumentRequests.remove(requestId);
}

QString
DecoderHost::takeIncomingDocument() {
    /* Into the slot without a signal: the take's own signal tells both, and no slot can take it mid-take. */
    QString newer = newerIncomingDocument();
    if (!newer.isEmpty()) {
        m_incomingDocument.swap(newer);
    }
    if (m_incomingDocument.isEmpty()) {
        return QString();
    }
    QString reference;
    reference.swap(m_incomingDocument);
    Q_EMIT incomingDocumentChanged();
    return reference;
}

void
DecoderHost::offerIncomingDocument(const QString& reference) {
    if (reference.isEmpty()) {
        return;
    }
    m_incomingDocument = reference;
    Q_EMIT incomingDocumentChanged();
}

QVariantMap
DecoderHost::fileFailure(FileRequestKind kind, const QString& code) {
    switch (kind) {
        case FileRequestKind::Handoff: return {{QStringLiteral("ok"), false}, {QStringLiteral("reason"), code}};
        case FileRequestKind::Access: return {{QStringLiteral("outcome"), code}};
        case FileRequestKind::Roots:
            return {{QStringLiteral("status"), code},
                    {QStringLiteral("generation"), qint64(0)},
                    {QStringLiteral("roots"), QVariantList()}};
        case FileRequestKind::Listing:
        case FileRequestKind::Page: break;
    }
    return {{QStringLiteral("status"), code}};
}

QVariantMap
DecoderHost::fileAccess() const {
    return {{QStringLiteral("declared"), false},
            {QStringLiteral("granted"), false},
            {QStringLiteral("route"), QStringLiteral("none")},
            {QStringLiteral("apiLevel"), 0},
            {QStringLiteral("rootsGeneration"), qint64(0)}};
}

qint64
DecoderHost::openFileBrowserSession() {
    const qint64 id = nextDocumentRequestId();
    m_fileSessions.insert(id);
    return id;
}

void
DecoderHost::closeFileBrowserSession(qint64 sessionId) {
    m_fileSessions.remove(sessionId);
    QList<qint64> ids;
    for (auto it = m_fileRequests.cbegin(); it != m_fileRequests.cend(); ++it) {
        if (it->session == sessionId) {
            ids.append(it.key());
        }
    }
    std::sort(ids.begin(), ids.end());
    for (const qint64 id : ids) {
        cancelFileAnswer(id);
    }
}

qint64
DecoderHost::requestFileRoots(qint64 sessionId) {
    return refuseFileRequest(sessionId, FileRequestKind::Roots);
}

qint64
DecoderHost::requestFolderListing(qint64 sessionId, const QString& rootKey, qint64 generation, const QString& path,
                                  const QVariantMap& filter) {
    (void)rootKey;
    (void)generation;
    (void)path;
    (void)filter;
    return refuseFileRequest(sessionId, FileRequestKind::Listing);
}

qint64
DecoderHost::requestMorePage(qint64 sessionId, qint64 snapshot, int offset) {
    (void)snapshot;
    (void)offset;
    return refuseFileRequest(sessionId, FileRequestKind::Page);
}

qint64
DecoderHost::requestFileHandoff(qint64 sessionId, const QString& rootKey, qint64 generation, const QString& reference) {
    (void)rootKey;
    (void)generation;
    (void)reference;
    return refuseFileRequest(sessionId, FileRequestKind::Handoff);
}

qint64
DecoderHost::requestFileAccess(qint64 sessionId) {
    return refuseFileRequest(sessionId, FileRequestKind::Access);
}

void
DecoderHost::cancelFileRequest(qint64 requestId) {
    cancelFileAnswer(requestId);
}

qint64
DecoderHost::refuseFileRequest(qint64 sessionId, FileRequestKind kind) {
    const qint64 joined = kind == FileRequestKind::Access ? m_fileAccessRequest : 0;
    const qint64 id = beginFileRequest(sessionId, kind);
    if (id == joined) {
        return id; // The pending access request, answered already.
    }
    settleFileRequestLater(id, fileFailure(kind, QStringLiteral("unsupported")));
    return id;
}

qint64
DecoderHost::beginFileRequest(qint64 sessionId, FileRequestKind kind) {
    const bool open = m_fileSessions.contains(sessionId);
    if (open && kind == FileRequestKind::Access && m_fileAccessRequest != 0) {
        return m_fileAccessRequest; // One access request app-wide: a second one joins it.
    }
    const qint64 id = nextDocumentRequestId();
    if (open && (kind == FileRequestKind::Listing || kind == FileRequestKind::Page)) {
        supersedeFolderRequests(sessionId);
    }
    FileRequestState state;
    state.kind = kind;
    state.session = sessionId;
    m_fileRequests.insert(id, state);
    if (!open) {
        settleFileRequestLater(id, fileFailure(kind, QStringLiteral("cancelled")));
    } else if (kind == FileRequestKind::Access) {
        m_fileAccessRequest = id;
    }
    return id;
}

bool
DecoderHost::fileRequestLive(qint64 id) const {
    const auto found = m_fileRequests.constFind(id);
    return found != m_fileRequests.constEnd() && !found->answered;
}

bool
DecoderHost::settleFileRequest(qint64 id, const QVariantMap& result) {
    if (!fileRequestLive(id)) {
        return false;
    }
    const FileRequestKind kind = m_fileRequests.take(id).kind;
    if (m_fileAccessRequest == id) {
        m_fileAccessRequest = 0;
    }
    emitFileAnswer(id, kind, result);
    return true;
}

void
DecoderHost::settleFileRequestLater(qint64 id, const QVariantMap& result) {
    if (!fileRequestLive(id)) {
        return;
    }
    FileRequestState& state = m_fileRequests[id];
    state.answered = true;
    state.answer = result;
    QMetaObject::invokeMethod(this, [this, id] { deliverFileAnswer(id); }, Qt::QueuedConnection);
}

void
DecoderHost::cancelFileAnswer(qint64 id) {
    const auto found = m_fileRequests.find(id);
    if (found == m_fileRequests.end()) {
        return;
    }
    if (m_fileAccessRequest == id) {
        m_fileAccessRequest = 0;
    }
    const QVariantMap cancelled = fileFailure(found->kind, QStringLiteral("cancelled"));
    if (found->answered) {
        found->answer = cancelled; // Its delivery is queued already: it delivers this instead.
        return;
    }
    settleFileRequestLater(id, cancelled);
    fileRequestRetired(id);
}

void
DecoderHost::supersedeFolderRequests(qint64 sessionId) {
    QList<qint64> ids;
    for (auto it = m_fileRequests.cbegin(); it != m_fileRequests.cend(); ++it) {
        if (it->session == sessionId && (it->kind == FileRequestKind::Listing || it->kind == FileRequestKind::Page)) {
            ids.append(it.key());
        }
    }
    for (const qint64 id : ids) {
        cancelFileAnswer(id);
    }
}

void
DecoderHost::deliverFileAnswer(qint64 id) {
    const auto found = m_fileRequests.constFind(id);
    if (found == m_fileRequests.constEnd() || !found->answered) {
        return;
    }
    const FileRequestState state = m_fileRequests.take(id);
    if (m_fileAccessRequest == id) {
        m_fileAccessRequest = 0;
    }
    emitFileAnswer(id, state.kind, state.answer);
}

void
DecoderHost::emitFileAnswer(qint64 id, FileRequestKind kind, const QVariantMap& result) {
    switch (kind) {
        case FileRequestKind::Roots: Q_EMIT fileRootsListed(id, result); break;
        case FileRequestKind::Listing:
        case FileRequestKind::Page: Q_EMIT folderListed(id, result); break;
        case FileRequestKind::Handoff: Q_EMIT fileHandoffChecked(id, result); break;
        case FileRequestKind::Access:
            Q_EMIT fileAccessRequestDone(id, result.value(QStringLiteral("outcome")).toString());
            break;
    }
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
DecoderHost::setRecordingSelectionCheck(std::function<bool(int, RecordingSelectionToken*)> current) {
    m_recordingSelectionCurrent = std::move(current);
}

DecoderHost::RecordingInput
DecoderHost::recordingInputCheck(const QString& path, const dsd_file_identity_t& expected) {
    dsd_file_identity_t now{};
    if (dsd_path_file_identity(platform_path(path).constData(), &now) != 0) {
        const int err = errno;
        return err == ENOENT ? RecordingInput::Missing : RecordingInput::Changed; // only ENOENT is a name gone
    }
    return dsd_file_identity_equal(&expected, &now) != 0 ? RecordingInput::Selected : RecordingInput::Changed;
}

DecoderHost::RecordingInput
DecoderHost::openRecordingInput(const QString& path, const dsd_file_identity_t& expected, FILE** out) {
    *out = nullptr;
    FILE* fp = dsd_fopen_existing_regular_file(platform_path(path).constData(), "rb");
    if (fp == nullptr) {
        // Gone only when the name itself is not in a folder that is there (dsd_path_file_identity()'s ENOENT, which
        // checks the parent): a parent that is a dangling link or missing never looked the name up.
        dsd_file_identity_t none{};
        const bool gone =
            errno == ENOENT && dsd_path_file_identity(platform_path(path).constData(), &none) != 0 && errno == ENOENT;
        return gone ? RecordingInput::Missing : RecordingInput::Changed;
    }
    dsd_file_identity_t opened{};
    if (dsd_file_stream_identity(fp, &opened) != 0 || dsd_file_identity_equal(&expected, &opened) == 0) {
        (void)fclose(fp);
        return RecordingInput::Changed;
    }
    *out = fp;
    return RecordingInput::Selected;
}

void
DecoderHost::setRecordingHold(std::function<int(const QString&)> hold, std::function<void(int)> release) {
    m_recordingHold = std::move(hold);
    m_recordingRelease = std::move(release);
}

void
DecoderHost::setSignInFillSink(SignInFillSink sink) {
    m_signInFill = std::move(sink);
}

void
DecoderHost::deliverSignInFill(int token, bool hasUsername, QString& username, bool hasPassword,
                               QString& password) const {
    /* A copy: a sink that replaces or clears itself mid-call must not destroy the function running. */
    const SignInFillSink sink = m_signInFill;
    if (sink) {
        sink(token, hasUsername, username, hasPassword, password);
    }
    scrub_qstring(username);
    scrub_qstring(password);
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

#ifdef DSD_NEO_TEST_HOOKS
void
decoder_host_test_after_recording_check(std::function<void(const QString& path)> hook) {
    after_recording_check() = std::move(hook);
}
#endif

} // namespace dsd_qt
