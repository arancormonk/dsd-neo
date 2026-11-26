// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The one-shot location broker (#800): the single location request in flight, and whose it is.
 *
 * The platform keeps one one-shot request and gives up the previous one on every new request (LocationSupport.kt), so
 * the requesters (the site chooser, Listen's Nearest, the RadioReference nearby search, the map's Locate me) share
 * this broker rather than calling DecoderHost directly:
 *
 *  - requestSite() / requestPrecise() / requestQuiet() / requestCurrent() start a request for a named requester and return its id. A request still
 *    pending (anyone's) is given up first: the host is told to cancel it and its requester gets ended().
 *  - Only the pending request's answer is passed on, as answered() to its requester with its id; an answer or a
 *    cancellation for any other id (a request given up, cancelled or already answered) is dropped.
 *  - The platform ending a request unanswered (a permission prompt closed, #782) reaches its requester as ended().
 *  - cancel() gives up a request its requester no longer wants; that requester gets no ended() for it.
 *  - `pendingRequester` tells each requester whether a request is pending and whose it is, so a requester can hold
 *    its own button while another's request runs.
 *
 * A host that answers inside the request call (one with no location support) is answered once the event loop turns,
 * after the requester has the id. Qt thread only.
 */

#ifndef DSD_NEO_SRC_UI_QT_LOCATION_REQUESTS_H_
#define DSD_NEO_SRC_UI_QT_LOCATION_REQUESTS_H_

#include <QObject>
#include <QPointer>
#include <QString>
#include <QVariantMap>
#include <QtGlobal>
#include <type_traits> // IWYU pragma: keep -- QPointer's conversion checks.

namespace dsd_qt {

class DecoderHost;

class LocationRequests : public QObject {
    Q_OBJECT
    /* The requester whose request is pending ("siteChooser", "savedSystems", "radioReference", "map"), or empty. */
    Q_PROPERTY(QString pendingRequester READ pendingRequester NOTIFY pendingChanged)
    /* Whether any request is pending. */
    Q_PROPERTY(bool busy READ busy NOTIFY pendingChanged)

  public:
    explicit LocationRequests(QObject* parent = nullptr);
    ~LocationRequests() override;
    LocationRequests(const LocationRequests&) = delete;
    LocationRequests& operator=(const LocationRequests&) = delete;

    /** @brief The host requests go to. Another host gives up the pending request (ended()). */
    void setHost(DecoderHost* host);

    /**
     * @brief Ask for fresh coordinates for distance ranking (DecoderHost::requestSiteLocation()) for @p requester.
     * @return The request's id; 0 with no host or an empty requester (nothing asked).
     */
    Q_INVOKABLE qint64 requestSite(const QString& requester);
    /**
     * @brief The map's Locate me: as requestSite(), but where only approximate location is allowed the platform first
     * offers precise location (DecoderHost::requestPreciseLocation()).
     */
    Q_INVOKABLE qint64 requestPrecise(const QString& requester);
    /** @brief The map's opening fix: as requestSite(), but never a permission prompt (DecoderHost::requestQuietLocation()). */
    Q_INVOKABLE qint64 requestQuiet(const QString& requester);
    /** @brief Ask for a fix with its postal code (DecoderHost::requestCurrentLocation()); as requestSite(). */
    Q_INVOKABLE qint64 requestCurrent(const QString& requester);
    /** @brief Give up request @p requestId when it is the pending one; any other id does nothing. */
    Q_INVOKABLE void cancel(qint64 requestId);
    /** @brief Whether @p requester has a request pending. */
    Q_INVOKABLE bool pendingFor(const QString& requester) const;

    QString pendingRequester() const;
    /** @brief The pending request's id; 0 with none. */
    qint64 pendingId() const;
    bool busy() const;

  Q_SIGNALS:
    /** @brief `pendingRequester`/`busy` changed: once per request, a request that replaces another included. */
    void pendingChanged();
    /**
     * @brief The answer to @p requester's request @p requestId: {fixOk, lat, lon, accuracyM, fixAtMs (epoch ms),
     * geocodeOk, postalCode, countryCode, error}, as DecoderHost::locationResult() carries them.
     */
    void answered(const QString& requester, qint64 requestId, const QVariantMap& result);
    /** @brief @p requester's request @p requestId ended unanswered: the platform cancelled it, or a newer one did. */
    void ended(const QString& requester, qint64 requestId);

  private:
    enum class Kind { Site, Precise, Quiet, Current };
    qint64 begin(const QString& requester, Kind kind);
    void giveUp(bool notify);
    void onResult(qint64 requestId, bool fixOk, double lat, double lon, double accuracyM, qint64 fixAtMs,
                  bool geocodeOk, const QString& postalCode, const QString& countryCode, const QString& error);
    void onCancelled(qint64 requestId);
    void settle(qint64 requestId, bool answer, const QVariantMap& result);

    QPointer<DecoderHost> m_host;
    QString m_requester;
    qint64 m_id = 0;
    /* Inside the host's request call: an answer now is delivered once the event loop turns. */
    bool m_requesting = false;
};

} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_LOCATION_REQUESTS_H_ */
