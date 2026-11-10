// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <QLatin1String>
#include <QList>
#include <QMap>
#include <QVariant>
#include <QVariantList>
#include <utility>
#include "decoder_host.h"

#include <QByteArray>
#include <QClipboard>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QIODevice>

#include "document_import_transport.h"
#include "document_save_transport.h"
#include "json_store.h"
#include "qstring_scrub.h"

#include <Qt>
#include <QtGlobal>
#include <algorithm>

namespace dsd_qt {

namespace {

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
    // A save copies one recording; a zip names each entry, so the two lists go together.
    if (id <= 0 || reference.isEmpty() || paths.isEmpty() || (zip ? names.size() != paths.size() : paths.size() != 1)) {
        return refuse(TransferFailed);
    }
    // No file is looked at here, on the GUI thread (an export may name tens of thousands): the transfer finds the ones
    // gone on its own thread, reports them at its end and publishes the total it really has.
    m_transferSelection = id;
    m_transferBytes = 0;
    m_transferTotalBytes = selection.value(QStringLiteral("bytes")).toLongLong();
    Q_EMIT recordingTransferChanged();
    if (!beginRecordingTransfer(paths, zip ? names : QStringList(), reference, zip)) {
        discardPendingDownload(reference);
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

} // namespace dsd_qt
