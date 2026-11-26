// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */
/*
 * LocationFeed (#800): idempotent named leases over one host feed, the feed started again once per foreground epoch
 * move with the notice first, the host's updates passed on in order, a host replacement that drops every lease, and a
 * record that never outlives the feed. The host is scripted; coordinates are made up.
 */
#include <QGuiApplication>
#include <QList>
#include <QMap>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantMap>
#include <Qt>
#include <QtGlobal>
#include <cstdio>
#include <utility>
#include "decoder_host.h"
#include "location_feed.h"

namespace {

using dsd_qt::LocationFeed;

int g_failures = 0;

void
check(bool ok, const char* label) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", label);
        ++g_failures;
    }
}

/* Counts the feed calls; the epoch is the case's. */
class FakeHost : public dsd_qt::DecoderHost {
  public:
    qint64 epochValue = 0;
    int starts = 0;
    int stops = 0;

    bool
    isRunning() const override {
        return false;
    }

    QString
    statusText() const override {
        return QString();
    }

    bool
    start(const QStringList& argv) override {
        (void)argv;
        return false;
    }

    void
    stop() override {}

    qint64
    foregroundEpoch() const override {
        return epochValue;
    }

    void
    startLocationStream() override {
        ++starts;
    }

    void
    stopLocationStream() override {
        ++stops;
    }

    void
    emitFix(qint64 ns, double lat) {
        Q_EMIT locationStreamUpdate({{"generation", 1},
                                     {"state", "ok"},
                                     {"hasFix", true},
                                     {"provider", "gps"},
                                     {"measurementNs", ns},
                                     {"lat", lat},
                                     {"lon", 20.0},
                                     {"accuracyM", 8.0},
                                     {"fixAtMs", 1000 + ns}});
    }

    void
    emitState(const char* state) {
        Q_EMIT locationStreamUpdate({{"generation", 1}, {"state", state}, {"hasFix", false}});
    }
};

/* What the feed told its consumers, in order: "update:<state>", "moved:<epoch>@<host starts then>", "dropped". */
struct Log {
    QStringList events;

    explicit Log(LocationFeed& feed, const FakeHost& host) {
        QObject::connect(&feed, &LocationFeed::updated, &feed, [this](const QVariantMap& record) {
            events.append(QStringLiteral("update:%1").arg(record.value("state").toString()));
        });
        QObject::connect(&feed, &LocationFeed::foregroundMoved, &feed, [this, &host](qint64 epoch) {
            events.append(QStringLiteral("moved:%1@%2").arg(epoch).arg(host.starts));
        });
        QObject::connect(&feed, &LocationFeed::leasesDropped, &feed,
                         [this] { events.append(QStringLiteral("dropped")); });
    }
};

QString
roaming() {
    return LocationFeed::roamingLease();
}

QString
map() {
    return LocationFeed::mapLease();
}

/* Two consumers share one feed: the first lease starts it, a second or a repeated lease changes nothing, and only the
 * last one leaving stops it. */
void
check_both_consumers() {
    FakeHost host;
    LocationFeed feed;
    feed.setHost(&host);
    Log log(feed, host);
    check(!feed.active() && host.starts == 0, "both: nothing runs before a lease");
    feed.acquire(roaming());
    check(feed.active() && feed.holds(roaming()) && host.starts == 1, "both: the first lease starts the feed");
    feed.acquire(map());
    feed.acquire(roaming());
    feed.acquire(map());
    check(host.starts == 1 && host.stops == 0 && feed.holds(map()), "both: a second or repeated lease is one start");
    host.emitFix(10, 10.0);
    check(log.events == QStringList{QStringLiteral("update:ok")}
              && feed.record().value("measurementNs").toLongLong() == 10,
          "both: an update reaches the consumers once and is the record");
    feed.release(map());
    feed.release(map());
    check(feed.active() && host.stops == 0, "both: one consumer leaving, twice, keeps the other's feed");
    feed.release(roaming());
    check(!feed.active() && host.stops == 1 && feed.record().isEmpty(), "both: the last lease stops it and the record");
    feed.release(roaming());
    check(host.stops == 1, "both: a lease not held is a no-op");
    feed.acquire(QString());
    check(!feed.active() && host.starts == 1, "both: an unnamed lease is none");
}

/* Either consumer may leave first; an update after the last one left is not kept or passed on. */
void
check_either_leaving() {
    FakeHost host;
    LocationFeed feed;
    feed.setHost(&host);
    Log log(feed, host);
    feed.acquire(map());
    feed.acquire(roaming());
    feed.release(roaming());
    host.emitFix(10, 10.0);
    check(host.stops == 0 && log.events.size() == 1, "either: roaming leaving first keeps the map's feed");
    feed.acquire(roaming());
    feed.release(map());
    host.emitFix(20, 10.1);
    check(host.starts == 1 && host.stops == 0 && log.events.size() == 2,
          "either: the map leaving first keeps roaming's feed");
    feed.release(roaming());
    host.emitFix(30, 10.2);
    check(host.stops == 1 && log.events.size() == 2 && feed.record().isEmpty(),
          "either: nothing is kept or passed on once no one holds it");
}

/* Each foreground epoch move starts the feed again once for every lease, with the notice before the start; Qt never
 * stops it in between, and an unreadable epoch decides nothing. */
void
check_foreground_cycles() {
    FakeHost host;
    LocationFeed feed;
    feed.setHost(&host);
    Log log(feed, host);
    feed.acquire(roaming());
    feed.acquire(map());
    host.emitFix(10, 10.0);
    log.events.clear();
    host.epochValue = 1;
    feed.noteForeground();
    check(log.events == QStringList{QStringLiteral("moved:1@1")} && host.starts == 2 && host.stops == 0,
          "cycles: the notice, then one start for both leases");
    check(feed.record().isEmpty(), "cycles: the record from before the move is gone");
    feed.noteForeground();
    feed.acquire(map());
    feed.acquire(roaming());
    check(host.starts == 2, "cycles: once per epoch, not on every look");
    host.epochValue = 2;
    feed.acquire(map()); // a consumer's own pass sees the next move
    check(host.starts == 3 && log.events.last() == QStringLiteral("moved:2@2"), "cycles: a lease taken again looks");
    host.epochValue = -1;
    feed.noteForeground();
    host.epochValue = 2;
    feed.noteForeground();
    check(host.starts == 3 && host.stops == 0, "cycles: an unreadable epoch moves nothing");
    host.emitFix(20, 10.1);
    check(log.events.last() == QStringLiteral("update:ok"), "cycles: the new epoch's fixes pass on");

    // A consumer that leaves on the notice, the last one: the feed stops and is not started again.
    FakeHost lone;
    LocationFeed single;
    single.setHost(&lone);
    single.acquire(map());
    QObject::connect(&single, &LocationFeed::foregroundMoved, &single, [&single] { single.release(map()); });
    lone.epochValue = 1;
    single.noteForeground();
    check(lone.starts == 1 && lone.stops == 1 && !single.active(), "cycles: the last lease leaving on the notice");

    // An epoch unreadable when the feed started: the first readable one starts it again, once.
    FakeHost blind;
    blind.epochValue = -1;
    LocationFeed late;
    late.setHost(&blind);
    late.acquire(roaming());
    blind.epochValue = 4;
    late.noteForeground();
    late.noteForeground();
    check(blind.starts == 2, "cycles: unknown at the start, started again once it reads");
}

/* Another host drops every lease and stops the old host's feed; the old host's updates no longer count. */
void
check_host_replacement() {
    FakeHost first;
    FakeHost second;
    LocationFeed feed;
    feed.setHost(&first);
    Log log(feed, first);
    feed.acquire(roaming());
    feed.acquire(map());
    first.emitFix(10, 10.0);
    feed.setHost(&second);
    check(first.stops == 1 && !feed.active() && !feed.holds(map()) && !feed.holds(roaming()) && feed.record().isEmpty(),
          "host: replacement drops the leases and stops the old feed");
    check(log.events.last() == QStringLiteral("dropped") && second.starts == 0, "host: consumers hear, nothing starts");
    first.emitFix(20, 10.1);
    check(log.events.last() == QStringLiteral("dropped"), "host: the old host's updates are not passed on");
    feed.acquire(map());
    second.emitFix(30, 10.2);
    check(second.starts == 1 && log.events.last() == QStringLiteral("update:ok"), "host: a lease on the new host");
    feed.setHost(nullptr);
    check(second.stops == 1 && !feed.active(), "host: no host stops it too");
    feed.acquire(map());
    check(!feed.active(), "host: no lease without a host");
    feed.setHost(&second);
    const int dropped = static_cast<int>(log.events.count(QStringLiteral("dropped")));
    feed.setHost(&second);
    check(log.events.count(QStringLiteral("dropped")) == dropped, "host: the same host again changes nothing");
}

/* A permission answer changes the feed's state in place (the platform registers again by itself): every state passes
 * on in order, with no start or stop from here, and a consumer joining later reads where it stands. */
void
check_permission_change() {
    FakeHost host;
    LocationFeed feed;
    feed.setHost(&host);
    Log log(feed, host);
    feed.acquire(roaming());
    host.emitState("noPermission");
    host.emitState("approximateOnly");
    host.emitFix(10, 10.0);
    check(log.events
              == QStringList{QStringLiteral("update:noPermission"), QStringLiteral("update:approximateOnly"),
                             QStringLiteral("update:ok")},
          "permission: each state in order");
    check(host.starts == 1 && host.stops == 0, "permission: no restart");
    feed.acquire(map());
    check(feed.record().value("state").toString() == QStringLiteral("ok") && feed.record().value("hasFix").toBool(),
          "permission: a consumer joining reads the standing record");
    host.emitState("approximateOnly");
    check(feed.record().value("state").toString() == QStringLiteral("approximateOnly")
              && !feed.record().contains("lat"),
          "permission: precise taken away leaves no coordinates in the record");
}

/* `record` notifies on every change, its clearing included, so a binding never shows a fix the feed no longer has. */
void
check_record_notifies() {
    FakeHost host;
    LocationFeed feed;
    feed.setHost(&host);
    int changes = 0;
    int actives = 0;
    QObject::connect(&feed, &LocationFeed::recordChanged, &feed, [&changes] { ++changes; });
    QObject::connect(&feed, &LocationFeed::activeChanged, &feed, [&actives] { ++actives; });
    feed.acquire(map());
    host.emitFix(10, 10.0);
    feed.release(map());
    check(changes == 2 && actives == 2, "record: set and cleared, active on and off");
    feed.acquire(map());
    feed.release(map());
    check(changes == 2, "record: no change when there was none to clear");
}

void
app_state(Qt::ApplicationState state) {
    Q_EMIT qGuiApp->applicationStateChanged(state);
}

/* The map alone holds the feed across a trip to the background: leaving clears the record at once, and coming back
 * starts the feed again by itself (no consumer call), notice first. */
void
check_background_cycle_map_only() {
    FakeHost host;
    LocationFeed feed;
    feed.setHost(&host);
    Log log(feed, host);
    feed.acquire(map());
    host.emitFix(10, 10.0);
    app_state(Qt::ApplicationInactive);
    check(!feed.record().isEmpty(), "background: a dialog on top keeps the record");
    app_state(Qt::ApplicationSuspended);
    check(feed.record().isEmpty() && feed.active(), "background: leaving clears the record, the lease stays");
    host.epochValue = 1; // the platform withdrew the feed and moved the epoch
    app_state(Qt::ApplicationActive);
    check(host.starts == 2 && host.stops == 0 && log.events.last() == QStringLiteral("moved:1@1"),
          "background: back on screen, the feed starts again by itself, notice first");
    host.emitFix(20, 10.1);
    check(feed.record().value("measurementNs").toLongLong() == 20, "background: the new epoch's fix is the record");
    app_state(Qt::ApplicationActive);
    check(host.starts == 2, "background: Active again while on screen does nothing");

    // Hidden with no epoch move (the feed was not withdrawn): the host is asked once to say where it stands again.
    app_state(Qt::ApplicationHidden);
    app_state(Qt::ApplicationActive);
    check(host.starts == 3 && feed.record().isEmpty(), "background: no move, the host is asked to repeat");
    // Nothing held: nothing starts on return.
    feed.release(map());
    app_state(Qt::ApplicationSuspended);
    host.epochValue = 2;
    app_state(Qt::ApplicationActive);
    check(host.starts == 3 && host.stops == 1, "background: no lease, nothing starts");
}

/* A consumer that already looked after the return (roaming's own pass) started the feed in the new epoch: Active adds
 * no second start. */
void
check_background_return_seen_first() {
    FakeHost host;
    LocationFeed feed;
    feed.setHost(&host);
    feed.acquire(roaming());
    host.emitFix(10, 10.0);
    app_state(Qt::ApplicationSuspended);
    host.epochValue = 1;
    feed.acquire(roaming()); // roaming's tick ran first
    app_state(Qt::ApplicationActive);
    check(host.starts == 2, "background: once per epoch whoever looks first");
}

} // namespace

int
main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    check_both_consumers();
    check_either_leaving();
    check_foreground_cycles();
    check_host_replacement();
    check_permission_change();
    check_record_notifies();
    check_background_cycle_map_only();
    check_background_return_seen_first();
    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    return 0;
}
