// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The import copies' transport (#832): a picked document copied into a local file, cancellable by request id.
 *
 * DecoderHost hands one out (importTransport()); the import worker keeps it, so a copy still running at teardown never
 * reaches back into a host that is gone. Thread-agnostic: any thread may call any member, and no member touches a
 * QObject. The base transport copies a local file; the Android host's takes content:// documents over JNI.
 */

#ifndef DSD_NEO_SRC_UI_QT_DOCUMENT_IMPORT_TRANSPORT_H_
#define DSD_NEO_SRC_UI_QT_DOCUMENT_IMPORT_TRANSPORT_H_

#include <QHash>
#include <QString>
#include <QtGlobal>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>

class QIODevice;
class QSaveFile;

namespace dsd_qt {

/** @brief What progress() knows of a request's latest copy. */
struct ImportCopyProgress {
    /** The document's name: the source file's, or the one its provider gave; empty when unknown. */
    QString displayName;
    /** Bytes copied so far; -1 when the request has no copy. */
    qint64 bytes = -1;
    /** The document's size; -1 when unknown. */
    qint64 total = -1;
    /** The copy failed (DocumentLocalFailed) because this device ran out of space. */
    bool noSpace = false;
};

class DocumentImportTransport {
  public:
    DocumentImportTransport() = default;
    virtual ~DocumentImportTransport() = default;
    DocumentImportTransport(const DocumentImportTransport&) = delete;
    DocumentImportTransport& operator=(const DocumentImportTransport&) = delete;

    /**
     * @brief Copy the document @p reference names to @p destPath for import @p request, at most @p maxBytes of it.
     *
     * Blocks until the copy ends; callers run it on the import worker. The base transport takes a file:// URL or a
     * plain absolute path to a regular file (anything else is DocumentFailed: a FIFO, socket, device or directory,
     * also through a symbolic link) and writes through QSaveFile in the destination's directory, which must exist.
     * A request's copies run one after another: a second copy while one runs is DocumentFailed. Callers name a fresh
     * @p destPath; only DocumentCopied leaves a file there.
     *
     * @return A DecoderHost::DocumentCopy value: DocumentCancelled once cancel(@p request) was called (before or
     *         during the copy), DocumentFailed after abort(), DocumentLocalFailed when this device could not write the
     *         copy (progress() then says whether it ran out of space).
     */
    virtual int copy(qint64 request, const QString& reference, const QString& destPath, qint64 maxBytes);

    /**
     * @brief Cancel import @p request: the copy it runs stops at its next chunk, and every later copy for it answers
     * DocumentCancelled until forget(@p request). A request not seen yet is cancelled before its first copy.
     */
    virtual void cancel(qint64 request);

    /**
     * @brief The latest copy of @p request: its name, bytes and total, and whether it failed for want of space.
     *
     * bytes and total are what a running copy has reached; once copy() returns a transport may keep them or answer -1
     * (the base transport keeps them, Android's answers -1). The name and noSpace stay until the next copy.
     */
    virtual ImportCopyProgress progress(qint64 request) const;

    /**
     * @brief Drop what this transport keeps of @p request, its cancel included.
     *
     * Call it only once the request has ended and its last copy() has returned. Forgotten while a copy still ran, a
     * later cancel(@p request) would no longer reach that copy, and a new copy for the id could run beside it.
     */
    virtual void forget(qint64 request);

    /**
     * @brief Stop every copy in progress and fail every later one (DocumentFailed), for the rest of this transport.
     *
     * DecoderHost::abortDocumentTransfers() calls it on every transport it handed out, at teardown.
     */
    void abort();
    bool aborted() const;

  protected:
    /**
     * @brief Open @p path, a regular file, for reading; nullptr when it cannot be opened.
     *
     * The only source a copy opens. The base transport never waits on what it opens: on POSIX a name that leads to
     * anything but a regular file by now (a FIFO swapped in after the check) is refused at once. A test overrides it
     * to hand the copy a device it paces.
     */
    virtual std::unique_ptr<QIODevice> openSource(const QString& path);

  private:
    struct Request {
        bool cancelled = false;
        bool running = false;
        ImportCopyProgress progress;
    };

    /** Start a copy of @p request; false with @p refusal set when it may not run. */
    bool begin(qint64 request, int& refusal);
    /** End the copy begin() started: DocumentCancelled for a failure after a cancel, else @p copied. */
    int finish(qint64 request, int copied);
    int copyLocal(qint64 request, const QString& path, const QString& destPath, qint64 maxBytes);
    /** DocumentLocalFailed, noting whether @p out failed for want of space. */
    int localFailure(qint64 request, const QSaveFile& out);
    /** Record @p bytes copied; false once the copy must stop. */
    bool proceed(qint64 request, qint64 bytes);
    /** Whether @p request was cancelled or this transport aborted. */
    bool stopped(qint64 request) const;
    void update(qint64 request, const std::function<void(ImportCopyProgress&)>& change);

    std::atomic<bool> m_aborted{false};
    mutable std::mutex m_lock;
    QHash<qint64, Request> m_requests; // guarded by m_lock
};

/*
 * The local document copies DecoderHost and the base transport share.
 */

/** @brief The local path a file:// URL or a plain absolute path names; empty for anything else (a content URI). */
QString local_document_path(const QString& reference);

/** @brief Open @p out for writing, readable and writable by the owner only. */
bool open_private(QSaveFile& out);

/**
 * @brief Copy @p source to @p out in chunks, at most @p maxBytes of it (no limit when negative).
 *
 * @param proceed Optional; asked before every chunk with the bytes copied so far. Once it answers false the copy
 *        fails (DocumentFailed).
 * @return A DecoderHost::DocumentCopy value (a failed write to @p out is DocumentLocalFailed). After any failure @p out
 *         never commits; a failed write keeps its cause in out.error().
 */
int copy_stream_capped(QIODevice& source, QSaveFile& out, qint64 maxBytes,
                       const std::function<bool(qint64)>& proceed = std::function<bool(qint64)>());

} // namespace dsd_qt

#endif // DSD_NEO_SRC_UI_QT_DOCUMENT_IMPORT_TRANSPORT_H_
