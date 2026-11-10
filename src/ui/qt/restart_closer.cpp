// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "restart_closer.h"

#include <QObject>
#include <QString>
#include <QTimer>
#include <cstdint>
#include <functional>
#include <utility>

#include "decoder_host.h"

namespace dsd_qt {

RestartCloser::RestartCloser(DecoderHost* host, std::function<bool()> importsBusy, std::function<bool()> backupBusy,
                             QObject* parent, std::function<uint64_t()> nowMs)
    : QObject(parent), m_host(host), m_importsBusy(std::move(importsBusy)), m_backupBusy(std::move(backupBusy)),
      m_nowMs(std::move(nowMs)) {
    m_poll.setInterval(kPollMs);
    connect(&m_poll, &QTimer::timeout, this, &RestartCloser::poll);
}

bool
RestartCloser::active() const {
    return m_active;
}

bool
RestartCloser::refused() const {
    return (m_importsBusy && m_importsBusy()) || (m_backupBusy && m_backupBusy());
}

bool
RestartCloser::begin() {
    if (m_active || m_host == nullptr || refused()) {
        return false;
    }
    m_active = true;
    Q_EMIT activeChanged();
    m_deadlineMs = m_nowMs() + kStopWaitMs;
    if (m_host->sessionActive()) {
        m_host->stop();
    }
    // The quit waits for the first poll even when nothing runs, so it never happens inside the caller's call.
    m_poll.start();
    return true;
}

void
RestartCloser::poll() {
    if (m_host->sessionActive() || m_host->engineBusy()) {
        if (m_nowMs() >= m_deadlineMs) {
            fail(QStringLiteral("stopFailed"));
        }
        return;
    }
    m_poll.stop();
    if (!m_host->quitApp()) {
        fail(QStringLiteral("quitFailed"));
        return;
    }
    // The process is ending: the closer stays active so nothing starts in the time that takes.
    Q_EMIT finished(QStringLiteral("closing"));
}

void
RestartCloser::fail(const QString& outcome) {
    m_poll.stop();
    m_active = false;
    Q_EMIT activeChanged();
    Q_EMIT finished(outcome);
}

} // namespace dsd_qt
