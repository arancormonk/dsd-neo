// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "document_save_transport.h"

#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QSaveFile>
#include <QtGlobal>

#include "decoder_host.h"
#include "document_import_transport.h"

namespace dsd_qt {

DocumentSaveTransport::~DocumentSaveTransport() = default;

bool
DocumentSaveTransport::save(const QString& sourcePath, const QString& reference) {
    return !aborted() && copy_file_to_local_document(sourcePath, reference, &m_aborted);
}

void
DocumentSaveTransport::discardPendingDownload(const QString& reference) {
    (void)reference;
}

void
DocumentSaveTransport::abort() {
    m_aborted.store(true);
    abortPlatform();
}

bool
DocumentSaveTransport::aborted() const {
    return m_aborted.load();
}

void
DocumentSaveTransport::abortPlatform() {}

bool
copy_file_to_local_document(const QString& sourcePath, const QString& reference, const std::atomic<bool>* aborted) {
    const QString path = local_document_path(reference);
    QFile source(sourcePath);
    if (path.isEmpty() || !QFileInfo(sourcePath).isFile() || !source.open(QIODevice::ReadOnly)) {
        return false;
    }
    QSaveFile out(path);
    const auto proceed = [aborted](qint64) { return aborted == nullptr || !aborted->load(); };
    return open_private(out) && copy_stream_capped(source, out, -1, proceed) == DecoderHost::DocumentCopied
           && out.commit();
}

} // namespace dsd_qt
