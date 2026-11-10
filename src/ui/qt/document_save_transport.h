// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The export saves' transport (#826): a local file copied into the document a save picker chose.
 *
 * DecoderHost hands one out (saveTransport()); the export writer keeps it, so a save still running at teardown never
 * reaches back into a host that is gone. Thread-agnostic: any thread may call any member, and no member touches a
 * QObject. The base transport writes local documents; the Android host's takes content:// documents over JNI.
 */

#ifndef DSD_NEO_SRC_UI_QT_DOCUMENT_SAVE_TRANSPORT_H_
#define DSD_NEO_SRC_UI_QT_DOCUMENT_SAVE_TRANSPORT_H_

#include <QString>
#include <QtGlobal>
#include <atomic>

namespace dsd_qt {

class DocumentSaveTransport {
  public:
    DocumentSaveTransport() = default;
    virtual ~DocumentSaveTransport();
    DocumentSaveTransport(const DocumentSaveTransport&) = delete;
    DocumentSaveTransport& operator=(const DocumentSaveTransport&) = delete;

    /**
     * @brief Copy the file at @p sourcePath into the document @p reference names.
     *
     * Blocks until the copy ends; callers run it on a worker. The base transport takes a file:// URL or a plain
     * absolute path (anything else fails) and writes it owner-only through QSaveFile, so a failure leaves the document
     * as it was. Fails once abort() was called, before or during the copy.
     *
     * @return true when the document holds the source's bytes.
     */
    virtual bool save(const QString& sourcePath, const QString& reference);

    /**
     * @brief Let go of the Download-folder row (#829) a picked @p reference may hold, when nothing will be saved to it.
     *
     * A failed save() already let go of it. Never blocks. The base transport holds no rows and does nothing.
     */
    virtual void discardPendingDownload(const QString& reference);

    /**
     * @brief Stop the save in progress and fail every later one, for the rest of this transport.
     *
     * Teardown only: DecoderHost::abortDocumentTransfers() calls it on every transport it handed out, and the export
     * writer on its own. A platform transport stops its platform's copies too (abortPlatform()), which on Android stops
     * every document copy in the process.
     */
    void abort();
    bool aborted() const;

  protected:
    /** @brief Called by abort() once this transport is marked aborted; a platform stops its own copies. Base: none. */
    virtual void abortPlatform();

    /**
     * @brief Asked by the base save() before each chunk of its copy, with the bytes copied so far; false stops the
     *        copy, and the save fails with the document as it was.
     *
     * Base: true until abort(). Runs on the saving thread. A transport that overrides save() for its own documents
     * reaches it only through the base save(). Tests override it to hold a copy partway.
     */
    virtual bool proceedCopy(qint64 copied);

  private:
    std::atomic<bool> m_aborted{false};
};

} // namespace dsd_qt

#endif // DSD_NEO_SRC_UI_QT_DOCUMENT_SAVE_TRANSPORT_H_
