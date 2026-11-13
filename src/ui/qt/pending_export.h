// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The pending CSV export (#826): one built file held under a token until a picker's answer, then saved.
 *
 * An owner builds its file, hold()s it, and asks QML to open a save picker with the token. The picker's answer is
 * save() (an accepted pick) or discard() (a cancelled one). save() hands the file to a DocumentExportWriter the helper
 * owns. Owner-thread only, like the writer; the owner destroys the helper at the point it would destroy a writer.
 */

#ifndef DSD_NEO_SRC_UI_QT_PENDING_EXPORT_H_
#define DSD_NEO_SRC_UI_QT_PENDING_EXPORT_H_

#include <QByteArray>
#include <QString>
#include <functional>
#include <memory>

#include "document_export_writer.h"

class QObject;

namespace dsd_qt {

class DocumentSaveTransport;

class PendingExport {
  public:
    using Reply = DocumentExportWriter::Reply;

    /**
     * @param transport The host's save transport: kept for a refused save's Download-folder row, and handed to the
     *        writer.
     * @param cacheRoot The writer's staging root (see DocumentExportWriter).
     * @param owner Where @p reply is delivered, on its thread. It must outlive the helper.
     * @param reply Told each accepted save's outcome, after busyChanged has run for its end.
     * @param busyChanged Runs after an accepted save() starts (busy() is true) and right before @p reply (busy() is
     *        false), so an owner can notify QML. Optional.
     */
    PendingExport(std::shared_ptr<DocumentSaveTransport> transport, const QString& cacheRoot, QObject* owner,
                  Reply reply, std::function<void()> busyChanged = {});
    ~PendingExport();

    PendingExport(const PendingExport&) = delete;
    PendingExport& operator=(const PendingExport&) = delete;

    /** The token after @p last: one more, wrapping from INT_MAX to 1 so it is never 0. */
    static int nextToken(int last);

    /** Drop any pending file. Every prepare calls this first, also when it then yields nothing. */
    void reset();

    /** Keep @p file as the one pending export, replacing any other, under a fresh token (above 0; INT_MAX wraps to 1). */
    int hold(const QByteArray& file);

    /**
     * @brief Save the pending export @p token into the document @p reference names, in the background.
     *
     * A token that is 0 or not the pending one lets go of the reference's Download-folder row (through the transport)
     * and returns false. Otherwise the file is consumed first, so a submit the writer refuses (a save running, or the
     * app closing) leaves nothing pending, and the writer's answer is returned.
     */
    bool save(int token, const QString& reference);

    /** Drop the pending export @p token; another token (or 0) is left alone. */
    void discard(int token);

    /** Whether an accepted save has not replied yet. */
    bool busy() const;

  private:
    std::shared_ptr<DocumentSaveTransport> m_transport;
    std::unique_ptr<DocumentExportWriter> m_writer;
    std::function<void()> m_busyChanged;
    Reply m_reply;
    QByteArray m_file;
    int m_token = 0, m_lastToken = 0;
    bool m_busy = false;
};

} // namespace dsd_qt

#endif // DSD_NEO_SRC_UI_QT_PENDING_EXPORT_H_
