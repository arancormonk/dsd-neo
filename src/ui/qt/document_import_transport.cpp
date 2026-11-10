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
#include <memory>

#ifndef _WIN32
#include <cstdio>

#include <dsd-neo/platform/file_compat.h>
#endif

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

#ifndef _WIN32
/** @brief A QFile reading a stdio stream it owns: closed with the device. */
class StreamSource : public QFile {
  public:
    explicit StreamSource(FILE* stream) : m_stream(stream) {}

    StreamSource(const StreamSource&) = delete;
    StreamSource& operator=(const StreamSource&) = delete;

    ~StreamSource() override {
        close();
        (void)std::fclose(m_stream);
    }

  private:
    FILE* m_stream;
};
#endif

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
 * The source must be a regular file, after symbolic links (QFileInfo follows them). Anything else is no document: a
 * FIFO with no writer would hold open() until one came, and with it the import worker and every import queued behind
 * it (#830 B1); a socket or a device has nothing to import either. QSaveFile writes beside the destination and renames
 * on commit, and an uncommitted one discards its copy, so no failure, cap and cancel included, leaves a file there;
 * every failure on that side is this device's, not the document's.
 */
int
DocumentImportTransport::copyLocal(qint64 request, const QString& path, const QString& destPath, qint64 maxBytes) {
    const QFileInfo info(path);
    if (!info.isFile()) {
        return DecoderHost::DocumentFailed;
    }
    const qint64 total = info.size();
    update(request, [&info, total](ImportCopyProgress& progress) {
        progress.displayName = info.fileName();
        progress.total = total;
    });
    if (total > maxBytes) {
        return DecoderHost::DocumentTooLarge;
    }
    const std::unique_ptr<QIODevice> source = openSource(path);
    if (!source) {
        return DecoderHost::DocumentFailed;
    }
    QSaveFile out(destPath);
    errno = 0;
    if (!open_private(out)) {
        return localFailure(request, out);
    }
    const int copied =
        copy_stream_capped(*source, out, maxBytes, [this, request](qint64 bytes) { return proceed(request, bytes); });
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

/*
 * copyLocal() checked the type a moment ago, but the name may lead to a FIFO by now. On POSIX the open never waits on
 * one: the path is resolved first (a link to a regular file still opens it), then the resolved name is opened
 * non-blocking without following it and refused unless the descriptor is a regular file
 * (dsd_fopen_existing_regular_file()). Windows has no FIFO at a file path, so a plain QFile opens it there.
 */
std::unique_ptr<QIODevice>
DocumentImportTransport::openSource(const QString& path) {
#ifdef _WIN32
    auto source = std::make_unique<QFile>(path);
    if (!source->open(QIODevice::ReadOnly)) {
        return nullptr;
    }
    return source;
#else
    char resolved[4096];
    if (dsd_path_canonical(QFile::encodeName(path).constData(), resolved, sizeof(resolved)) != 0) {
        return nullptr;
    }
    FILE* stream = dsd_fopen_existing_regular_file(resolved, "rb");
    if (stream == nullptr) {
        return nullptr;
    }
    auto source = std::make_unique<StreamSource>(stream);
    if (!source->open(stream, QIODevice::ReadOnly, QFileDevice::DontCloseHandle)) {
        return nullptr;
    }
    return source;
#endif
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

void
DocumentImportTransport::abort() {
    m_aborted.store(true);
}

bool
DocumentImportTransport::aborted() const {
    return m_aborted.load();
}

} // namespace dsd_qt
