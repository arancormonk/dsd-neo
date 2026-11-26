// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "location_feed.h"

#include <QCoreApplication>
#include <QDebug>
#include <QGuiApplication>
#include <QVariant>
#include <dsd-neo/runtime/decode_clock.h>
#include "decoder_host.h"

namespace dsd_qt {

LocationFeed::LocationFeed(QObject* parent) : QObject(parent) {
    // Inactive is still on screen (a dialog, such as the permission prompt, on top); only Hidden and Suspended are not.
    if (QGuiApplication* app = qobject_cast<QGuiApplication*>(QCoreApplication::instance())) {
        connect(app, &QGuiApplication::applicationStateChanged, this, &LocationFeed::onApplicationState);
    }
}

LocationFeed::~LocationFeed() = default;

void
LocationFeed::setHost(DecoderHost* next) {
    if (m_host == next) {
        return;
    }
    const bool hadLeases = !m_leases.isEmpty();
    if (m_host != nullptr) {
        disconnect(m_host, nullptr, this, nullptr);
    }
    if (hadLeases) {
        stopFeed(); // on the old host, which is still m_host here
    }
    m_leases.clear();
    clearRecord();
    m_epoch = -1;
    m_host = next;
    if (next != nullptr) {
        connect(next, &DecoderHost::locationStreamUpdate, this, &LocationFeed::onHostUpdate);
    }
    if (hadLeases) {
        Q_EMIT leasesDropped();
        Q_EMIT activeChanged();
    }
}

DecoderHost*
LocationFeed::host() const {
    return m_host;
}

bool
LocationFeed::acquire(const QString& name) {
    if (name.isEmpty() || m_host == nullptr) {
        return false;
    }
    if (m_leases.contains(name)) {
        noteForeground();
        return m_leases.contains(name); // a consumer leaving on the notice may be this one
    }
    const bool first = m_leases.isEmpty();
    m_leases.insert(name);
    if (first) {
        startFeed();
        Q_EMIT activeChanged();
    } else {
        noteForeground();
    }
    return m_leases.contains(name);
}

void
LocationFeed::release(const QString& name) {
    if (!m_leases.remove(name)) {
        return;
    }
    if (m_leases.isEmpty()) {
        stopFeed();
        Q_EMIT activeChanged();
    }
}

bool
LocationFeed::holds(const QString& name) const {
    return m_leases.contains(name);
}

qint64
LocationFeed::nowMs() const {
    return static_cast<qint64>(dsd_realtime_mono_ms());
}

void
LocationFeed::noteForeground() {
    if (m_host == nullptr || m_leases.isEmpty()) {
        return;
    }
    const qint64 epoch = m_host->foregroundEpoch();
    if (epoch < 0 || epoch == m_epoch) {
        return; // unreadable (the next read decides), or the feed already runs in this epoch
    }
    // The platform withdrew the feed when the app left the foreground: what it last said is gone with it. The notice
    // goes first, so a consumer that handles the move itself has done so before any fix of the new epoch reaches it.
    m_epoch = epoch;
    clearRecord();
    const QPointer<DecoderHost> notified = m_host;
    Q_EMIT foregroundMoved(epoch);
    // A consumer may have left (the last lease stops the feed) or the host changed under the notice.
    if (notified != nullptr && notified == m_host && !m_leases.isEmpty() && m_epoch == epoch) {
        notified->startLocationStream();
    }
}

bool
LocationFeed::active() const {
    return !m_leases.isEmpty();
}

QVariantMap
LocationFeed::record() const {
    return m_record;
}

void
LocationFeed::onHostUpdate(const QVariantMap& update) {
    if (m_leases.isEmpty()) {
        return; // a feed no one holds: nothing of it is kept
    }
    m_record = update;
    Q_EMIT updated(update);
    Q_EMIT recordChanged();
}

/* The epoch is read before the start: a start the platform then refuses as from an older epoch shows up as a move at
 * the next noteForeground(), which asks again. */
void
LocationFeed::startFeed() {
    m_epoch = m_host->foregroundEpoch();
    clearRecord();
    m_host->startLocationStream();
}

void
LocationFeed::stopFeed() {
    m_epoch = -1;
    clearRecord();
    if (m_host != nullptr) {
        m_host->stopLocationStream();
    }
}

/* Leaving the foreground: what the feed last said goes at once (the platform withdraws the feed as well). Coming back:
 * the epoch decides; a return with no move seen, whose record leaving cleared, asks the host to say it again. */
void
LocationFeed::onApplicationState(Qt::ApplicationState state) {
    if (state == Qt::ApplicationHidden || state == Qt::ApplicationSuspended) {
        if (!m_away) {
            m_away = true;
            m_epochAtLeave = m_epoch;
            m_recordDroppedAtLeave = !m_record.isEmpty();
        }
        clearRecord();
        return;
    }
    if (state != Qt::ApplicationActive || !m_away) {
        return;
    }
    m_away = false;
    if (m_host == nullptr || m_leases.isEmpty()) {
        return;
    }
    const qint64 before = m_epoch;
    noteForeground();
    if (m_host != nullptr && !m_leases.isEmpty() && m_epoch == before && before == m_epochAtLeave
        && m_recordDroppedAtLeave) {
        m_host->startLocationStream();
    }
}

void
LocationFeed::clearRecord() {
    if (!m_record.isEmpty()) {
        m_record.clear();
        Q_EMIT recordChanged();
    }
}

} // namespace dsd_qt
