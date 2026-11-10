// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "document_import_transport.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QSaveFile>
#include <QUrl>
#include <Qt>
#include <cerrno>
#include <cstdint>
#include <stddef.h>

#include <dsd-neo/crypto/backup_seal.h>

#include "decoder_host.h"

namespace dsd_qt {

namespace {

/**
 * @brief Whether @p out's last failure was this device running out of space.
 *
 * Qt reports ENOSPC on a write or flush as ResourceError (Unix). A failure Qt reports otherwise, such as a close that
 * found the disk full, still leaves ENOSPC in errno, which the caller cleared before the step.
 */
bool
out_of_space(const QSaveFile& out) {
    return out.error() == QFileDevice::ResourceError || errno == ENOSPC;
}

/** A copy_stream_capped() predicate that stops once @p aborted is raised (never with no flag). */
std::function<bool(qint64)>
until_aborted(const std::atomic<bool>* aborted) {
    return [aborted](qint64) { return aborted == nullptr || !aborted->load(); };
}

static_assert(kProbeHeaderBytes == DSD_BACKUP_HEADER_BYTES, "a probe reads a backup's whole header");

/* Read @p source into @p header until it holds kProbeHeaderBytes or the source ends, however short each read is. */
int
read_probe_header(QIODevice& source, QByteArray& header, const std::function<bool(qint64)>& proceed) {
    char buffer[kProbeHeaderBytes];
    while (header.size() < kProbeHeaderBytes) {
        if (!proceed(header.size())) {
            return DecoderHost::DocumentFailed;
        }
        const qint64 n = source.read(buffer, kProbeHeaderBytes - header.size());
        if (n < 0) {
            return DecoderHost::DocumentFailed;
        }
        if (n == 0) {
            break;
        }
        header.append(buffer, n);
    }
    return DecoderHost::DocumentCopied;
}

/*
 * Read the rest of @p source, discarding it, and set @p total to its whole length from the @p counted bytes already
 * read, stopping once it reaches @p maxCount + 1. As in copy_stream_capped(), the count is what was read, and the loop
 * ends on the read result.
 */
int
count_rest(QIODevice& source, qint64 counted, qint64 maxCount, const std::function<bool(qint64)>& proceed,
           qint64* total) {
    QByteArray chunk(1 << 16, Qt::Uninitialized);
    while (counted <= maxCount) {
        if (!proceed(counted)) {
            return DecoderHost::DocumentFailed;
        }
        const qint64 room = maxCount - counted;
        const qint64 n = source.read(chunk.data(), room >= chunk.size() ? chunk.size() : room + 1);
        if (n < 0) {
            return DecoderHost::DocumentFailed;
        }
        if (n == 0) {
            break;
        }
        counted += n;
    }
    *total = counted;
    return DecoderHost::DocumentCopied;
}

/* The probe of an open @p source: its header, then its length only when the header starts with the backup magic. */
int
probe_stream(QIODevice& source, ImportProbe* out, qint64 maxCount, const std::function<bool(qint64)>& proceed) {
    const int read = read_probe_header(source, out->header, proceed);
    const bool backup = read == DecoderHost::DocumentCopied
                        && dsd_backup_has_magic(reinterpret_cast<const uint8_t*>(out->header.constData()),
                                                static_cast<size_t>(out->header.size()))
                               != 0;
    if (!backup) {
        return read;
    }
    return count_rest(source, out->header.size(), maxCount, proceed, &out->total);
}

} // namespace

QString
local_document_path(const QString& reference) {
    const QUrl url(reference);
    if (url.isLocalFile()) {
        return url.toLocalFile();
    }
    return QDir::isAbsolutePath(reference) ? reference : QString();
}

bool
open_private(QSaveFile& out) {
    return out.open(QIODevice::WriteOnly) && out.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
}

/*
 * Loops on the read result rather than atEnd(): a read error returns an empty chunk without advancing pos(), so an
 * atEnd()-driven loop would spin forever on it, and a readable source QFile reports size 0 for (a FIFO, /proc) claims
 * to be at its end before a byte has been read. The cap is counted on what was read for the same reason, not taken
 * from the source's reported size. A failed write is not cancelled here: QSaveFile already refuses to commit after it
 * and discards the copy, and its error() keeps the write's cause for the caller.
 */
int
copy_stream_capped(QIODevice& source, QSaveFile& out, qint64 maxBytes, const std::function<bool(qint64)>& proceed) {
    QByteArray chunk(1 << 16, Qt::Uninitialized);
    qint64 total = 0;
    for (;;) {
        if (proceed && !proceed(total)) {
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
            return DecoderHost::DocumentLocalFailed;
        }
    }
}

bool
copy_file_to_local_document(const QString& sourcePath, const QString& reference, const std::atomic<bool>* aborted) {
    const QString path = local_document_path(reference);
    QFile source(sourcePath);
    if (path.isEmpty() || !QFileInfo(sourcePath).isFile() || !source.open(QIODevice::ReadOnly)) {
        return false;
    }
    QSaveFile out(path);
    return open_private(out)
           && copy_stream_capped(source, out, -1, until_aborted(aborted)) == DecoderHost::DocumentCopied
           && out.commit();
}

int
DocumentImportTransport::copy(qint64 request, const QString& reference, const QString& destPath, qint64 maxBytes) {
    int refusal = DecoderHost::DocumentFailed;
    if (!begin(request, refusal)) {
        return refusal;
    }
    const QString path = local_document_path(reference);
    const int copied = path.isEmpty() || destPath.isEmpty() || maxBytes < 0
                           ? static_cast<int>(DecoderHost::DocumentFailed)
                           : copyLocal(request, path, destPath, maxBytes);
    return finish(request, copied);
}

bool
DocumentImportTransport::begin(qint64 request, int& refusal) {
    const std::lock_guard<std::mutex> lock(m_lock);
    if (aborted()) {
        refusal = DecoderHost::DocumentFailed;
        return false;
    }
    Request& entry = m_requests[request];
    if (entry.cancelled) {
        refusal = DecoderHost::DocumentCancelled;
        return false;
    }
    if (entry.running) {
        refusal = DecoderHost::DocumentFailed; // A request's copies run one after another.
        return false;
    }
    entry.running = true;
    entry.progress = ImportCopyProgress();
    entry.progress.bytes = 0;
    return true;
}

int
DocumentImportTransport::finish(qint64 request, int copied) {
    const std::lock_guard<std::mutex> lock(m_lock);
    Request& entry = m_requests[request];
    entry.running = false;
    return entry.cancelled && copied != DecoderHost::DocumentCopied ? static_cast<int>(DecoderHost::DocumentCancelled)
                                                                    : copied;
}

/*
 * The source may be any readable file but a directory: a stream (a FIFO) copies under the cap like a provider's, with
 * no size to show. QSaveFile writes beside the destination and renames on commit, and an uncommitted one discards its
 * copy, so no failure, cap and cancel included, leaves a file there; every failure on that side is this device's, not
 * the document's.
 */
int
DocumentImportTransport::copyLocal(qint64 request, const QString& path, const QString& destPath, qint64 maxBytes) {
    const QFileInfo info(path);
    if (!info.exists() || info.isDir()) {
        return DecoderHost::DocumentFailed;
    }
    const qint64 total = info.isFile() ? info.size() : -1;
    update(request, [&info, total](ImportCopyProgress& progress) {
        progress.displayName = info.fileName();
        progress.total = total;
    });
    if (total > maxBytes) {
        return DecoderHost::DocumentTooLarge;
    }
    QFile source(path);
    if (!source.open(QIODevice::ReadOnly)) {
        return DecoderHost::DocumentFailed;
    }
    QSaveFile out(destPath);
    errno = 0;
    if (!open_private(out)) {
        return localFailure(request, out);
    }
    const int copied =
        copy_stream_capped(source, out, maxBytes, [this, request](qint64 bytes) { return proceed(request, bytes); });
    if (copied == DecoderHost::DocumentLocalFailed) {
        return localFailure(request, out);
    }
    if (copied != DecoderHost::DocumentCopied) {
        return copied;
    }
    if (stopped(request)) {
        return DecoderHost::DocumentFailed; // A cancel that came with the last chunk still wins (finish()).
    }
    errno = 0;
    return out.commit() ? static_cast<int>(DecoderHost::DocumentCopied) : localFailure(request, out);
}

int
DocumentImportTransport::probe(qint64 request, const QString& reference, ImportProbe* out, qint64 maxCount) {
    if (out == nullptr) {
        return DecoderHost::DocumentFailed;
    }
    *out = ImportProbe();
    int refusal = DecoderHost::DocumentFailed;
    if (!begin(request, refusal)) {
        return refusal;
    }
    const QString path = local_document_path(reference);
    const int probed = path.isEmpty() || maxCount < 0 ? static_cast<int>(DecoderHost::DocumentFailed)
                                                      : probeLocal(request, path, out, maxCount);
    return finish(request, probed);
}

/*
 * A stream (a FIFO) probes as a file does: the length is what was read, never a reported size. Unbuffered, so the
 * header read asks for the header alone: a buffered QFile fills its buffer first, and on a stream that read waits for
 * bytes a probe that stops after the header never needs.
 */
int
DocumentImportTransport::probeLocal(qint64 request, const QString& path, ImportProbe* out, qint64 maxCount) {
    const QFileInfo info(path);
    if (!info.exists() || info.isDir()) {
        return DecoderHost::DocumentFailed;
    }
    out->displayName = info.fileName();
    update(request, [&info](ImportCopyProgress& progress) { progress.displayName = info.fileName(); });
    QFile source(path);
    if (!source.open(QIODevice::ReadOnly | QIODevice::Unbuffered)) {
        return DecoderHost::DocumentFailed;
    }
    return probe_stream(source, out, maxCount, [this, request](qint64 bytes) { return proceed(request, bytes); });
}

int
DocumentImportTransport::localFailure(qint64 request, const QSaveFile& out) {
    const bool noSpace = out_of_space(out);
    update(request, [noSpace](ImportCopyProgress& progress) { progress.noSpace = noSpace; });
    return DecoderHost::DocumentLocalFailed;
}

bool
DocumentImportTransport::proceed(qint64 request, qint64 bytes) {
    update(request, [bytes](ImportCopyProgress& progress) { progress.bytes = bytes; });
    return !stopped(request);
}

bool
DocumentImportTransport::stopped(qint64 request) const {
    const std::lock_guard<std::mutex> lock(m_lock);
    const auto found = m_requests.constFind(request);
    return aborted() || (found != m_requests.constEnd() && found->cancelled);
}

void
DocumentImportTransport::update(qint64 request, const std::function<void(ImportCopyProgress&)>& change) {
    const std::lock_guard<std::mutex> lock(m_lock);
    change(m_requests[request].progress);
}

void
DocumentImportTransport::cancel(qint64 request) {
    const std::lock_guard<std::mutex> lock(m_lock);
    m_requests[request].cancelled = true;
}

ImportCopyProgress
DocumentImportTransport::progress(qint64 request) const {
    const std::lock_guard<std::mutex> lock(m_lock);
    const auto found = m_requests.constFind(request);
    return found == m_requests.constEnd() ? ImportCopyProgress() : found->progress;
}

void
DocumentImportTransport::forget(qint64 request) {
    const std::lock_guard<std::mutex> lock(m_lock);
    m_requests.remove(request);
}

bool
DocumentImportTransport::writeFromFile(const QString& path, const QString& reference) {
    return copy_file_to_local_document(path, reference, &m_aborted);
}

void
DocumentImportTransport::discardWrite(const QString& reference) {
    (void)reference;
}

void
DocumentImportTransport::abort() {
    m_aborted.store(true);
}

bool
DocumentImportTransport::aborted() const {
    return m_aborted.load();
}

} // namespace dsd_qt
