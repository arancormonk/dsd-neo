// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <QMap>
#include <QVariant>
#include <Qt>
#include <utility>
#include "location_requests.h"

#include "decoder_host.h"

namespace dsd_qt {

namespace {

/* One Qt-thread allocator for every requester, across recreation: an id stays unique while a platform worker is still
 * retiring a request given up earlier. */
qint64
next_request_id() {
    static qint64 next = 0;
    return ++next;
}

} // namespace

LocationRequests::LocationRequests(QObject* parent) : QObject(parent) {}

LocationRequests::~LocationRequests() = default;

void
LocationRequests::setHost(DecoderHost* host) {
    if (m_host == host) {
        return;
    }
    giveUp(true); // on the old host, which is still m_host here
    if (m_host != nullptr) {
        disconnect(m_host, nullptr, this, nullptr);
    }
    m_host = host;
    if (host != nullptr) {
        connect(host, &DecoderHost::locationResult, this, &LocationRequests::onResult);
        connect(host, &DecoderHost::locationCancelled, this, &LocationRequests::onCancelled);
    }
}

qint64
LocationRequests::requestSite(const QString& requester) {
    return begin(requester, Kind::Site);
}

qint64
LocationRequests::requestPrecise(const QString& requester) {
    return begin(requester, Kind::Precise);
}

qint64
LocationRequests::requestQuiet(const QString& requester) {
    return begin(requester, Kind::Quiet);
}

qint64
LocationRequests::requestCurrent(const QString& requester) {
    return begin(requester, Kind::Current);
}

void
LocationRequests::cancel(qint64 requestId) {
    if (requestId == 0 || requestId != m_id) {
        return;
    }
    m_id = 0;
    m_requester.clear();
    if (m_host != nullptr) {
        m_host->cancelLocationRequest(requestId);
    }
    Q_EMIT pendingChanged();
}

bool
LocationRequests::pendingFor(const QString& requester) const {
    return m_id != 0 && !requester.isEmpty() && requester == m_requester;
}

QString
LocationRequests::pendingRequester() const {
    return m_requester;
}

qint64
LocationRequests::pendingId() const {
    return m_id;
}

bool
LocationRequests::busy() const {
    return m_id != 0;
}

qint64
LocationRequests::begin(const QString& requester, Kind kind) {
    if (requester.isEmpty() || m_host == nullptr) {
        return 0;
    }
    giveUp(false); // one pendingChanged() for the replacement, below
    const qint64 id = next_request_id();
    m_id = id;
    m_requester = requester;
    Q_EMIT pendingChanged();
    m_requesting = true;
    switch (kind) {
        case Kind::Site: m_host->requestSiteLocation(id); break;
        case Kind::Precise: m_host->requestPreciseLocation(id); break;
        case Kind::Quiet: m_host->requestQuietLocation(id); break;
        case Kind::Current: m_host->requestCurrentLocation(id); break;
    }
    m_requesting = false;
    return id;
}

/* The pending request, if any, is given up: the platform cancels it and its requester hears it ended. @p notify is
 * false when a new request replaces it at once, which emits the one pendingChanged() for both. */
void
LocationRequests::giveUp(bool notify) {
    if (m_id == 0) {
        return;
    }
    const qint64 id = m_id;
    const QString requester = m_requester;
    m_id = 0;
    m_requester.clear();
    if (m_host != nullptr) {
        m_host->cancelLocationRequest(id);
    }
    if (notify) {
        Q_EMIT pendingChanged();
    }
    Q_EMIT ended(requester, id);
}

void
LocationRequests::onResult(qint64 requestId, bool fixOk, double lat, double lon, double accuracyM, qint64 fixAtMs,
                           bool geocodeOk, const QString& postalCode, const QString& countryCode,
                           const QString& error) {
    if (requestId == 0 || requestId != m_id) {
        return; // given up, cancelled or answered already
    }
    const QVariantMap result{{QStringLiteral("fixOk"), fixOk},
                             {QStringLiteral("lat"), lat},
                             {QStringLiteral("lon"), lon},
                             {QStringLiteral("accuracyM"), accuracyM},
                             {QStringLiteral("fixAtMs"), fixAtMs},
                             {QStringLiteral("geocodeOk"), geocodeOk},
                             {QStringLiteral("postalCode"), postalCode},
                             {QStringLiteral("countryCode"), countryCode},
                             {QStringLiteral("error"), error}};
    if (m_requesting) {
        QMetaObject::invokeMethod(
            this, [this, requestId, result] { settle(requestId, true, result); }, Qt::QueuedConnection);
        return;
    }
    settle(requestId, true, result);
}

void
LocationRequests::onCancelled(qint64 requestId) {
    if (requestId == 0 || requestId != m_id) {
        return;
    }
    if (m_requesting) {
        QMetaObject::invokeMethod(
            this, [this, requestId] { settle(requestId, false, QVariantMap()); }, Qt::QueuedConnection);
        return;
    }
    settle(requestId, false, QVariantMap());
}

/* The pending request @p requestId ends with @p result (an answer) or without one; any other id is stale by now. */
void
LocationRequests::settle(qint64 requestId, bool answer, const QVariantMap& result) {
    if (requestId == 0 || requestId != m_id) {
        return;
    }
    const QString requester = m_requester;
    m_id = 0;
    m_requester.clear();
    Q_EMIT pendingChanged();
    if (answer) {
        Q_EMIT answered(requester, requestId, result);
    } else {
        Q_EMIT ended(requester, requestId);
    }
}

} // namespace dsd_qt
