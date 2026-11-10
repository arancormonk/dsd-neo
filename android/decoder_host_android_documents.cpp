// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

/*
 * The export saves' transport (#826) and the teardown abort, less their JNI transport (platformHoldsResolver(),
 * platformCopyFileToDocument() and platformAbortDocumentCopies() in decoder_host_android.cpp), so the desktop host test
 * compiles this dispatch as it ships. The saves run on the export writer's thread.
 *
 * The document picker's requests and answers (#829), less their JNI transport (platform*Document*() there), on the Qt
 * thread. The Download-folder rows its saves write into (DownloadsSave.kt) are named and discarded through
 * platformSavedDocumentName() and platformDiscardPendingDownload() there, from any thread. The documents other apps
 * open with this one (#831) reach the host's slot here too, through platformTakeIncomingDocument(): each refresh()
 * offers one, and a take asks for one newer first.
 */

#include <QLatin1String>
#include <QObject>
#include <QString>
#include <QUrl>
#include <Qt>
#include <QtGlobal>
#include <memory>
#include "decoder_host.h"
#include "decoder_host_android.h"
#include "document_pick_record.h"
#include "document_save_transport.h"

namespace dsd_android {

bool
DecoderHostAndroid::isContentUri(const QString& reference) {
    return QUrl(reference).scheme() == QLatin1String("content");
}

bool
DecoderHostAndroid::saveContentDocument(const QString& sourcePath, const QString& reference, bool aborted) {
    /* A save that never reached DocumentCopies leaves a Download-folder row (#829) to discard. One that did settled it
     * there, so the discard after a failed call is a no-op unless the JNI call itself failed. */
    if (aborted || !platformHoldsResolver() || !platformCopyFileToDocument(sourcePath, reference)) {
        platformDiscardPendingDownload(reference);
        return false;
    }
    return true;
}

void
DecoderHostAndroid::abortDocumentTransfers() {
    /* Cancels the provider calls in flight, stops the copies at their next chunk and refuses new ones. */
    platformAbortDocumentCopies();
    dsd_qt::DecoderHost::abortDocumentTransfers();
}

/* The export saves' transport (#826). It runs on the export writer's thread and never holds the host, only calls the
 * host's static functions, so a save may finish after the host is gone. */
class DecoderHostAndroid::SaveTransport final : public dsd_qt::DocumentSaveTransport {
  public:
    bool
    save(const QString& sourcePath, const QString& reference) override {
        if (!isContentUri(reference)) {
            return dsd_qt::DocumentSaveTransport::save(sourcePath, reference);
        }
        return saveContentDocument(sourcePath, reference, aborted());
    }

    void
    discardPendingDownload(const QString& reference) override {
        platformDiscardPendingDownload(reference);
    }

  protected:
    /* DocumentCopies' copy keeps its own cancellation, which only the process-wide abort reaches. */
    void
    abortPlatform() override {
        platformAbortDocumentCopies();
    }
};

std::shared_ptr<dsd_qt::DocumentSaveTransport>
DecoderHostAndroid::saveTransport() const {
    return keepSaveTransport(std::make_shared<SaveTransport>());
}

qint64
DecoderHostAndroid::requestOpenDocument() {
    const qint64 id = nextDocumentRequestId();
    const bool wasPending = m_document_request != 0;
    beginDocumentRequest(id);
    if (!platformOpenDocument(id)) {
        refuseDocumentRequestLater(id, QStringLiteral("failed"));
    }
    publishDocumentRequest(wasPending);
    return id;
}

qint64
DecoderHostAndroid::requestSaveDocument(const QString& suggestedName, const QString& mimeType) {
    const qint64 id = nextDocumentRequestId();
    const bool wasPending = m_document_request != 0;
    beginDocumentRequest(id);
    if (!platformSaveDocument(id, suggestedName, mimeType)) {
        refuseDocumentRequestLater(id, QStringLiteral("failed"));
    }
    publishDocumentRequest(wasPending);
    return id;
}

void
DecoderHostAndroid::cancelDocumentRequest(qint64 requestId) {
    if (requestId == 0 || requestId != m_document_request) {
        return;
    }
    m_document_request = 0;
    platformCancelDocument(requestId);
    publishDocumentRequest(true);
}

QString
DecoderHostAndroid::savedDocumentName(const QString& reference) const {
    return platformSavedDocumentName(reference);
}

void
DecoderHostAndroid::discardPendingDownload(const QString& reference) {
    platformDiscardPendingDownload(reference);
}

void
DecoderHostAndroid::beginDocumentRequest(qint64 id) {
    const qint64 replaced = m_document_request;
    m_document_request = id;
    if (replaced != 0) {
        /* The platform drops it too, so its record (and a Download-folder row it holds) is never left waiting when the
         * new request does not reach the platform. A request that does would replace it there anyway. */
        platformCancelDocument(replaced);
        QMetaObject::invokeMethod(
            this, [this, replaced] { Q_EMIT documentPickCancelled(replaced); }, Qt::QueuedConnection);
    }
}

void
DecoderHostAndroid::refuseDocumentRequestLater(qint64 id, const QString& reason) {
    QMetaObject::invokeMethod(
        this,
        [this, id, reason] {
            if (m_document_request == id) {
                m_document_request = 0;
                Q_EMIT documentPickUnavailable(id, reason);
                publishDocumentRequest(true);
            }
        },
        Qt::QueuedConnection);
}

void
DecoderHostAndroid::refreshDocumentPicks() {
    /* Drained even with no request live: a record for a request since given up may be a Download-folder row, which
     * deliver_document_pick_record() discards. */
    for (QString record = platformPollDocumentPick(); !record.isEmpty(); record = platformPollDocumentPick()) {
        const bool wasPending = m_document_request != 0;
        deliver_document_pick_record(*this, m_document_request, record);
        publishDocumentRequest(wasPending);
    }
}

void
DecoderHostAndroid::publishDocumentRequest(bool wasPending) {
    if (wasPending != (m_document_request != 0)) {
        Q_EMIT documentRequestChanged();
    }
}

void
DecoderHostAndroid::refreshIncomingDocuments() {
    offerIncomingDocument(platformTakeIncomingDocument());
}

QString
DecoderHostAndroid::newerIncomingDocument() {
    return platformTakeIncomingDocument();
}

} // namespace dsd_android
