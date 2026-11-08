// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

/*
 * The backup's document copies (#580), less their JNI transport (platformHoldsResolver() and platformCopy*() in
 * decoder_host_android.cpp), so the desktop host test compiles this dispatch as it ships. They run on BackupModel's
 * worker thread.
 *
 * The document picker's requests and answers (#829), less their JNI transport (platform*Document*() there), on the Qt
 * thread.
 */

#include <QFile>
#include <QFileInfo>
#include <QLatin1String>
#include <QObject>
#include <QString>
#include <QUrl>
#include <Qt>
#include <QtGlobal>
#include "decoder_host.h"
#include "decoder_host_android.h"
#include "document_pick_record.h"

namespace dsd_android {

bool
DecoderHostAndroid::isContentUri(const QString& reference) {
    return QUrl(reference).scheme() == QLatin1String("content");
}

int
DecoderHostAndroid::copyDocumentToFile(const QString& reference, const QString& destPath, qint64 maxBytes) const {
    if (!isContentUri(reference)) {
        return dsd_qt::DecoderHost::copyDocumentToFile(reference, destPath, maxBytes);
    }
    if (documentTransfersAborted() || destPath.isEmpty() || maxBytes < 0) {
        return DocumentFailed;
    }
    if (!platformHoldsResolver()) {
        return DocumentLocalFailed; // the app's own state, not the document
    }
    /* Cleared first, so that a file found at destPath afterwards can only be this copy's (see below). A destination
     * that stays is this device's failure, not the document's. */
    QFile::remove(destPath);
    if (QFileInfo::exists(destPath) || QFileInfo(destPath).isSymLink()) {
        return DocumentLocalFailed;
    }
    const int copied = platformCopyDocumentToFile(reference, destPath, maxBytes);
    if (copied == DocumentTooLarge || copied == DocumentLocalFailed) {
        return copied;
    }
    /* A JNI call that failed (no such method, or an Error the Kotlin side does not catch) also reads as 0, so a copy
     * counts only with its file in place; without it the failure is the app's own, not the document's. */
    if (copied == DocumentCopied) {
        return QFileInfo(destPath).isFile() ? DocumentCopied : DocumentLocalFailed;
    }
    return DocumentFailed;
}

bool
DecoderHostAndroid::copyFileToDocument(const QString& sourcePath, const QString& reference) const {
    if (!isContentUri(reference)) {
        return dsd_qt::DecoderHost::copyFileToDocument(sourcePath, reference);
    }
    if (documentTransfersAborted() || !platformHoldsResolver()) {
        return false;
    }
    return platformCopyFileToDocument(sourcePath, reference);
}

qint64
DecoderHostAndroid::requestOpenDocument() {
    const qint64 id = nextDocumentRequestId();
    beginDocumentRequest(id);
    if (!platformOpenDocument(id)) {
        refuseDocumentRequestLater(id, QStringLiteral("failed"));
    }
    return id;
}

qint64
DecoderHostAndroid::requestSaveDocument(const QString& suggestedName, const QString& mimeType) {
    const qint64 id = nextDocumentRequestId();
    beginDocumentRequest(id);
    if (!platformSaveDocument(id, suggestedName, mimeType)) {
        refuseDocumentRequestLater(id, QStringLiteral("failed"));
    }
    return id;
}

void
DecoderHostAndroid::cancelDocumentRequest(qint64 requestId) {
    if (requestId == 0 || requestId != m_document_request) {
        return;
    }
    m_document_request = 0;
    platformCancelDocument(requestId);
}

void
DecoderHostAndroid::beginDocumentRequest(qint64 id) {
    const qint64 replaced = m_document_request;
    m_document_request = id;
    if (replaced != 0) {
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
            }
        },
        Qt::QueuedConnection);
}

void
DecoderHostAndroid::refreshDocumentPicks() {
    /* Drained even with no request live: a record for a request since given up may be a Download-folder row, which
     * deliver_document_pick_record() discards. */
    for (QString record = platformPollDocumentPick(); !record.isEmpty(); record = platformPollDocumentPick()) {
        deliver_document_pick_record(*this, m_document_request, record);
    }
}

} // namespace dsd_android
