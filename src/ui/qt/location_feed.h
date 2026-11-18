// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The one owner of the host's continuous location feed (#800), shared by GPS roaming and the map.
 *
 * DecoderHost has a single feed: any stopLocationStream() withdraws it for every caller. LocationFeed holds the feed
 * for named consumers through idempotent leases, so one consumer leaving never ends another's feed:
 *
 *  - acquire(name) takes the lease @p name: the first lease starts the host's feed. Taking a lease already held
 *    changes nothing, except that it looks at the foreground epoch (below).
 *  - release(name) drops it; the last lease leaving stops the feed. Releasing a lease not held does nothing.
 *  - The platform stops the feed itself whenever the app leaves the foreground (DecoderHost::foregroundEpoch() moves).
 *    noteForeground(), which acquire() runs too, reads the epoch: on a move with any lease held it emits
 *    foregroundMoved() first and then starts the feed once again for every lease. A consumer that handles the move
 *    itself (SiteRoaming) therefore sees the notice before the first fix of the new epoch.
 *  - setHost() with another host drops every lease, stops the old host's feed and emits leasesDropped().
 *
 * Every locationStreamUpdate() of the host is passed on as updated() in the order it came, only while a lease is held.
 * The latest one is kept as `record` so a consumer joining a running feed reads where it stands; it is cleared
 * whenever the feed stops or starts again (recordChanged(), never updated()). Nothing here is ever saved: a stream fix is never a saved fix (the clocks
 * differ, and the privacy policy says stream readings are not stored). Qt thread only.
 */

#ifndef DSD_NEO_SRC_UI_QT_LOCATION_FEED_H_
#define DSD_NEO_SRC_UI_QT_LOCATION_FEED_H_

#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QVariantMap>
#include <QtGlobal>

namespace dsd_qt {

class DecoderHost;

class LocationFeed : public QObject {
    Q_OBJECT
    /* Whether any lease is held, so the host's feed runs (or waits for the app to be visible again). */
    Q_PROPERTY(bool active READ active NOTIFY activeChanged)
    /* The feed's latest record (DecoderHost::locationStreamUpdate()), or empty: none yet since the feed (re)started. */
    Q_PROPERTY(QVariantMap record READ record NOTIFY recordChanged)

  public:
    /** @brief GPS roaming's lease (SiteRoaming). */
    static QString
    roamingLease() {
        return QStringLiteral("roaming");
    }

    /** @brief The map's lease (live follow). */
    static QString
    mapLease() {
        return QStringLiteral("map");
    }

    explicit LocationFeed(QObject* parent = nullptr);
    ~LocationFeed() override;
    LocationFeed(const LocationFeed&) = delete;
    LocationFeed& operator=(const LocationFeed&) = delete;

    /** @brief The host whose feed this owns. Another host drops every lease (leasesDropped()) and stops the old feed. */
    void setHost(DecoderHost* next);
    DecoderHost* host() const;

    /** @brief Take the lease @p name (non-empty); the first one starts the feed. Idempotent; runs noteForeground(). */
    Q_INVOKABLE void acquire(const QString& name);
    /** @brief Drop the lease @p name; the last one stops the feed. A lease not held: nothing happens. */
    Q_INVOKABLE void release(const QString& name);
    /** @brief Whether the lease @p name is held. */
    Q_INVOKABLE bool holds(const QString& name) const;
    /**
     * @brief Read the host's foreground epoch: when it moved since the feed last started and a lease is held, emit
     * foregroundMoved() and start the feed again (once for all leases). An unreadable epoch changes nothing.
     */
    Q_INVOKABLE void noteForeground();

    bool active() const;
    QVariantMap record() const;

  Q_SIGNALS:
    /** @brief A host update passed on, in the order the host emitted it (only while a lease is held). */
    void updated(const QVariantMap& update);
    /** @brief `record` changed: an update came, or the feed stopped or started again and it was cleared. */
    void recordChanged();
    /** @brief The app left the foreground since the feed started (now @p epoch); the feed is about to start again. */
    void foregroundMoved(qint64 epoch);
    /** @brief The host changed: every lease is gone, and the old host's feed stopped. */
    void leasesDropped();
    void activeChanged();

  private:
    void onHostUpdate(const QVariantMap& update);
    void startFeed();
    void stopFeed();
    void clearRecord();

    QPointer<DecoderHost> m_host;
    QSet<QString> m_leases;
    QVariantMap m_record;
    /* The foreground epoch the feed last started in; -1 when it could not be read then. */
    qint64 m_epoch = -1;
};

} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_LOCATION_FEED_H_ */
