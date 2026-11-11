// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "pending_export.h"

#include <limits>
#include <utility>

#include "document_save_transport.h"

namespace dsd_qt {

PendingExport::PendingExport(std::shared_ptr<DocumentSaveTransport> transport, const QString& cacheRoot, QObject* owner,
                             Reply reply, std::function<void()> busyChanged)
    : m_transport(std::move(transport)), m_busyChanged(std::move(busyChanged)), m_reply(std::move(reply)) {
    /* The writer's reply never runs after the writer is gone, and the writer goes first in ~PendingExport(). */
    m_writer = std::make_unique<DocumentExportWriter>(m_transport, cacheRoot, owner, [this](int token, bool ok) {
        m_busy = false;
        if (m_busyChanged) {
            m_busyChanged();
        }
        if (m_reply) {
            m_reply(token, ok);
        }
    });
}

/* A save still running is aborted and waited for (DocumentExportWriter). */
PendingExport::~PendingExport() { m_writer.reset(); }

int
PendingExport::nextToken(int last) {
    return last >= std::numeric_limits<int>::max() ? 1 : last + 1;
}

void
PendingExport::reset() {
    m_token = 0;
    m_file.clear();
}

int
PendingExport::hold(const QByteArray& file) {
    /* Tokens are unique for the writer, which names the staged file after them, and never 0. */
    m_lastToken = nextToken(m_lastToken);
    m_token = m_lastToken;
    m_file = file;
    return m_token;
}

bool
PendingExport::save(int token, const QString& reference) {
    if (token == 0 || token != m_token) {
        m_transport->discardPendingDownload(reference);
        return false;
    }
    const QByteArray file = m_file;
    reset();
    /* A running save refuses it, and the writer lets go of the reference's Download-folder row. */
    if (!m_writer->submit(token, file, reference)) {
        return false;
    }
    m_busy = true;
    if (m_busyChanged) {
        m_busyChanged();
    }
    return true;
}

void
PendingExport::discard(int token) {
    if (token != 0 && token == m_token) {
        reset();
    }
}

bool
PendingExport::busy() const {
    return m_busy;
}

} // namespace dsd_qt
