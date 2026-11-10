// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

/*
 * The import copies' transport (#832), less its JNI calls (platform*Import*() in decoder_host_android.cpp), so the
 * desktop host test compiles this dispatch as it ships. Its copies run on the import worker thread, and the backup's
 * save (writeFromFile(), #831) on BackupModel's worker; it never holds the host, only calls the host's static JNI
 * functions.
 */

#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QString>
#include <QtGlobal>
#include <memory>
#include <mutex>
#include "decoder_host_android.h"
#include "document_import_transport.h"

namespace dsd_android {

class DecoderHostAndroid::ImportTransport final : public dsd_qt::DocumentImportTransport {
  public:
    int copy(qint64 request, const QString& reference, const QString& destPath, qint64 maxBytes) override;
    void cancel(qint64 request) override;
    dsd_qt::ImportCopyProgress progress(qint64 request) const override;
    void forget(qint64 request) override;
    bool writeFromFile(const QString& path, const QString& reference) override;
    void discardWrite(const QString& reference) override;

  private:
    int copyContent(qint64 request, const QString& reference, const QString& destPath, qint64 maxBytes) const;
    bool onPlatform(qint64 request) const;

    mutable std::mutex m_lock;
    /* Whether each request's latest copy went to the platform; guarded by m_lock. */
    QHash<qint64, bool> m_platform;
};

int
DecoderHostAndroid::ImportTransport::copy(qint64 request, const QString& reference, const QString& destPath,
                                          qint64 maxBytes) {
    const bool content = isContentUri(reference);
    {
        const std::lock_guard<std::mutex> lock(m_lock);
        m_platform.insert(request, content);
    }
    if (!content) {
        return dsd_qt::DocumentImportTransport::copy(request, reference, destPath, maxBytes);
    }
    return copyContent(request, reference, destPath, maxBytes);
}

int
DecoderHostAndroid::ImportTransport::copyContent(qint64 request, const QString& reference, const QString& destPath,
                                                 qint64 maxBytes) const {
    if (aborted() || destPath.isEmpty() || maxBytes < 0) {
        return DocumentFailed;
    }
    if (!platformHoldsResolver()) {
        return DocumentLocalFailed; // the app's own state, not the document
    }
    /* Cleared first, so that a file found at destPath afterwards can only be this copy's. A destination that stays is
     * this device's failure, not the document's. */
    QFile::remove(destPath);
    if (QFileInfo::exists(destPath) || QFileInfo(destPath).isSymLink()) {
        return DocumentLocalFailed;
    }
    const int copied = platformCopyImportDocument(request, reference, destPath, maxBytes);
    switch (copied) {
        /* A JNI call that failed (no such method, or an Error the Kotlin side does not catch) also reads as 0, so a
         * copy counts only with its file in place; without it the failure is the app's own, not the document's. */
        case DocumentCopied: return QFileInfo(destPath).isFile() ? DocumentCopied : DocumentLocalFailed;
        case DocumentTooLarge:
        case DocumentFailed:
        case DocumentLocalFailed:
        case DocumentCancelled: return copied;
        default: return DocumentFailed; // an answer this side does not know
    }
}

void
DecoderHostAndroid::ImportTransport::cancel(qint64 request) {
    dsd_qt::DocumentImportTransport::cancel(request);
    platformCancelImport(request);
}

dsd_qt::ImportCopyProgress
DecoderHostAndroid::ImportTransport::progress(qint64 request) const {
    if (!onPlatform(request)) {
        return dsd_qt::DocumentImportTransport::progress(request);
    }
    dsd_qt::ImportCopyProgress progress;
    platformImportProgress(request, progress.bytes, progress.total);
    progress.displayName = platformImportName(request);
    progress.noSpace = platformImportNoSpace(request);
    return progress;
}

void
DecoderHostAndroid::ImportTransport::forget(qint64 request) {
    {
        const std::lock_guard<std::mutex> lock(m_lock);
        m_platform.remove(request);
    }
    dsd_qt::DocumentImportTransport::forget(request);
    platformForgetImport(request);
}

/*
 * The host's copyFileToDocument() without the host. A save that never reached DocumentCopies leaves a Download-folder
 * row (#829) to discard; one that did settled it there, so the discard after a failed call is a no-op unless the JNI
 * call itself failed.
 */
bool
DecoderHostAndroid::ImportTransport::writeFromFile(const QString& path, const QString& reference) {
    if (!isContentUri(reference)) {
        return dsd_qt::DocumentImportTransport::writeFromFile(path, reference);
    }
    if (aborted() || !platformHoldsResolver() || !platformCopyFileToDocument(path, reference)) {
        platformDiscardPendingDownload(reference);
        return false;
    }
    return true;
}

void
DecoderHostAndroid::ImportTransport::discardWrite(const QString& reference) {
    platformDiscardPendingDownload(reference);
}

bool
DecoderHostAndroid::ImportTransport::onPlatform(qint64 request) const {
    const std::lock_guard<std::mutex> lock(m_lock);
    return m_platform.value(request, false);
}

std::shared_ptr<dsd_qt::DocumentImportTransport>
DecoderHostAndroid::importTransport() const {
    return keepImportTransport(std::make_shared<ImportTransport>());
}

} // namespace dsd_android
