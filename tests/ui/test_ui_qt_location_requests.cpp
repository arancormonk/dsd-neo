// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */
/*
 * LocationRequests (#800): the one-shot location broker. Overlapping requesters (the newer request gives the older one
 * up, which hears it ended), a requester's own cancel, answers and cancellations for stale ids, a host that answers
 * inside the request call, and a host replacement. The host is scripted; coordinates are made up.
 */
#include <QCoreApplication>
#include <QLatin1String>
#include <QList>
#include <QMap>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantMap>
#include <QtGlobal>
#include <cmath>
#include <cstdio>
#include "decoder_host.h"
#include "location_requests.h"

namespace {

using dsd_qt::LocationRequests;

int g_failures = 0;

void
check(bool ok, const char* label) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", label);
        ++g_failures;
    }
}

bool
near(double a, double b) {
    return std::fabs(a - b) < 1e-9;
}

/* Records each call as "site:<id>", "current:<id>" or "cancel:<id>"; answers only when the case says. */
class FakeHost : public dsd_qt::DecoderHost {
  public:
    QStringList calls;

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

    bool
    locationSupported() const override {
        return true;
    }

    void
    requestCurrentLocation(qint64 requestId) override {
        calls.append(QStringLiteral("current:%1").arg(requestId));
    }

    void
    requestSiteLocation(qint64 requestId) override {
        calls.append(QStringLiteral("site:%1").arg(requestId));
    }

    void
    requestPreciseLocation(qint64 requestId) override {
        calls.append(QStringLiteral("precise:%1").arg(requestId));
    }

    void
    requestQuietLocation(qint64 requestId) override {
        calls.append(QStringLiteral("quiet:%1").arg(requestId));
    }

    void
    cancelLocationRequest(qint64 requestId) override {
        calls.append(QStringLiteral("cancel:%1").arg(requestId));
    }

    void
    answer(qint64 requestId, double lat) {
        Q_EMIT locationResult(requestId, true, lat, 20.0, 15.0, 5000, true, QStringLiteral("00000"),
                              QStringLiteral("US"), QString());
    }
};

/* No location support: the base host answers inside the request call, with an error. */
class UnsupportedHost : public dsd_qt::DecoderHost {
  public:
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
};

/* What the broker told the requesters, in order: "answered:<requester>:<id>" or "ended:<requester>:<id>". */
struct Log {
    QStringList events;
    QList<QVariantMap> results;

    explicit Log(LocationRequests& requests) {
        QObject::connect(&requests, &LocationRequests::answered, &requests,
                         [this](const QString& requester, qint64 id, const QVariantMap& result) {
                             events.append(QStringLiteral("answered:%1:%2").arg(requester).arg(id));
                             results.append(result);
                         });
        QObject::connect(&requests, &LocationRequests::ended, &requests, [this](const QString& requester, qint64 id) {
            events.append(QStringLiteral("ended:%1:%2").arg(requester).arg(id));
        });
    }
};

QString
call(const char* kind, qint64 id) {
    return QStringLiteral("%1:%2").arg(QLatin1String(kind)).arg(id);
}

/* The site chooser asks, then Listen's Nearest: the chooser's request is given up at the platform and the chooser
 * hears it ended; its late answer is dropped, and Nearest gets its own by id. Locate me sees another's request run. */
void
check_overlapping_requesters() {
    FakeHost host;
    LocationRequests requests;
    requests.setHost(&host);
    Log log(requests);
    int changes = 0;
    QObject::connect(&requests, &LocationRequests::pendingChanged, &requests, [&changes] { ++changes; });
    const qint64 chooser = requests.requestSite(QStringLiteral("siteChooser"));
    check(chooser > 0 && host.calls == QStringList{call("site", chooser)} && requests.busy()
              && requests.pendingRequester() == QStringLiteral("siteChooser") && requests.pendingId() == chooser,
          "overlap: the chooser's request is pending");
    check(requests.pendingFor(QStringLiteral("siteChooser")) && !requests.pendingFor(QStringLiteral("map")),
          "overlap: the map sees another requester's request run");
    const qint64 nearest = requests.requestSite(QStringLiteral("savedSystems"));
    check(nearest > chooser && host.calls.mid(1) == QStringList{call("cancel", chooser), call("site", nearest)},
          "overlap: the newer request gives the older one up at the platform first");
    check(log.events == QStringList{QStringLiteral("ended:siteChooser:%1").arg(chooser)},
          "overlap: the chooser hears its request ended");
    check(requests.pendingRequester() == QStringLiteral("savedSystems") && changes == 2,
          "overlap: Nearest's is pending, one change per request (a replacement too)");
    host.answer(chooser, 10.0);
    check(log.events.size() == 1, "overlap: the given-up request's answer is dropped");
    host.answer(nearest, 10.5);
    check(log.events.last() == QStringLiteral("answered:savedSystems:%1").arg(nearest) && !requests.busy()
              && requests.pendingRequester().isEmpty() && changes == 3,
          "overlap: Nearest gets its answer by its id, and nothing is pending");
    const QVariantMap result = log.results.last();
    check(result.value("fixOk").toBool() && near(result.value("lat").toDouble(), 10.5)
              && near(result.value("lon").toDouble(), 20.0) && near(result.value("accuracyM").toDouble(), 15.0)
              && result.value("fixAtMs").toLongLong() == 5000 && result.value("geocodeOk").toBool()
              && result.value("postalCode").toString() == QStringLiteral("00000")
              && result.value("countryCode").toString() == QStringLiteral("US")
              && result.value("error").toString().isEmpty(),
          "overlap: the answer carries every field");
    const qint64 lookup = requests.requestCurrent(QStringLiteral("radioReference"));
    check(host.calls.last() == call("current", lookup), "overlap: a lookup asks for the postal code too");
    const qint64 locate = requests.requestPrecise(QStringLiteral("map"));
    check(host.calls.mid(host.calls.size() - 2) == QStringList{call("cancel", lookup), call("precise", locate)}
              && requests.pendingFor(QStringLiteral("map")),
          "overlap: the map's Locate me asks for a precise fix, after giving the lookup up");
    host.answer(locate, 11.0);
    check(log.events.last() == QStringLiteral("answered:map:%1").arg(locate), "overlap: the map gets its answer");
    const qint64 opening = requests.requestQuiet(QStringLiteral("map"));
    check(host.calls.last() == call("quiet", opening) && requests.pendingFor(QStringLiteral("map")),
          "overlap: the map's opening fix asks without a prompt");
}

/* A host with no precise prompt of its own answers the map's Locate me as a site request. */
void
check_precise_defaults_to_site() {
    UnsupportedHost host;
    LocationRequests requests;
    requests.setHost(&host);
    Log log(requests);
    const qint64 id = requests.requestPrecise(QStringLiteral("map"));
    QCoreApplication::processEvents();
    check(id > 0 && log.events == QStringList{QStringLiteral("answered:map:%1").arg(id)}
              && !log.results.last().value("fixOk").toBool(),
          "precise: the base host answers it as it answers any request");
    const qint64 quiet = requests.requestQuiet(QStringLiteral("map"));
    QCoreApplication::processEvents();
    check(quiet > id && log.events.last() == QStringLiteral("answered:map:%1").arg(quiet),
          "quiet: so does the opening fix");
}

/* A requester's own cancel gives the platform request up, tells no one it ended, and drops its late answer; a cancel
 * of any other id does nothing at all. */
void
check_cancel() {
    FakeHost host;
    LocationRequests requests;
    requests.setHost(&host);
    Log log(requests);
    const qint64 id = requests.requestSite(QStringLiteral("map"));
    requests.cancel(id + 1);
    requests.cancel(0);
    check(requests.busy() && host.calls.size() == 1, "cancel: another id, or none, does nothing");
    requests.cancel(id);
    check(!requests.busy() && host.calls.last() == call("cancel", id) && log.events.isEmpty(),
          "cancel: given up at the platform, with no ended for its own requester");
    requests.cancel(id);
    check(host.calls.size() == 2, "cancel: twice is once");
    host.answer(id, 10.0);
    check(log.events.isEmpty(), "cancel: the late answer is dropped");
}

/* Only the pending id is answered or ended: one answered already, or never asked for, is stale. */
void
check_stale_ids() {
    FakeHost host;
    LocationRequests requests;
    requests.setHost(&host);
    Log log(requests);
    const qint64 first = requests.requestSite(QStringLiteral("siteChooser"));
    host.answer(first, 10.0);
    host.answer(first, 10.1);
    Q_EMIT host.locationCancelled(first);
    check(log.events == QStringList{QStringLiteral("answered:siteChooser:%1").arg(first)},
          "stale: an id answered already is answered once");
    const qint64 second = requests.requestSite(QStringLiteral("siteChooser"));
    Q_EMIT host.locationCancelled(second + 1);
    host.answer(second + 1, 10.2);
    check(requests.pendingId() == second && log.events.size() == 1, "stale: an id never asked for is dropped");
    Q_EMIT host.locationCancelled(second);
    check(log.events.last() == QStringLiteral("ended:siteChooser:%1").arg(second) && !requests.busy(),
          "stale: the platform ending the pending one reaches its requester");
    host.answer(second, 10.3);
    check(log.events.size() == 2, "stale: an answer after the platform ended it is dropped");
}

/* A host with no location answers inside the request call: the requester has its id before the answer comes. */
void
check_answer_inside_the_call() {
    UnsupportedHost host;
    LocationRequests requests;
    requests.setHost(&host);
    Log log(requests);
    const qint64 id = requests.requestSite(QStringLiteral("siteChooser"));
    check(id > 0 && log.events.isEmpty() && requests.pendingId() == id, "inside: nothing is answered in the call");
    QCoreApplication::processEvents();
    check(log.events == QStringList{QStringLiteral("answered:siteChooser:%1").arg(id)}
              && !log.results.last().value("fixOk").toBool() && !log.results.last().value("error").toString().isEmpty(),
          "inside: the failure is answered once the loop turns");
    const qint64 cancelled = requests.requestSite(QStringLiteral("siteChooser"));
    requests.cancel(cancelled);
    QCoreApplication::processEvents();
    check(log.events.size() == 1, "inside: a request cancelled before the loop turned is not answered");
}

/* Another host gives the pending request up at the old one; no host asks nothing; an unnamed requester asks nothing. */
void
check_host_replacement() {
    FakeHost first;
    FakeHost second;
    LocationRequests requests;
    Log log(requests);
    check(requests.requestSite(QStringLiteral("map")) == 0 && !requests.busy(), "host: none asks nothing");
    requests.setHost(&first);
    check(requests.requestSite(QString()) == 0 && first.calls.isEmpty(), "host: an unnamed requester asks nothing");
    const qint64 id = requests.requestSite(QStringLiteral("map"));
    requests.setHost(&second);
    check(first.calls.last() == call("cancel", id) && log.events == QStringList{QStringLiteral("ended:map:%1").arg(id)}
              && !requests.busy(),
          "host: the pending request is given up and ended");
    first.answer(id, 10.0);
    check(log.events.size() == 1, "host: the old host's answer is dropped");
    const qint64 next = requests.requestSite(QStringLiteral("map"));
    second.answer(next, 10.1);
    check(next > id && log.events.last() == QStringLiteral("answered:map:%1").arg(next), "host: the new host answers");
}

/* Ids stay unique across brokers, as they do across a recreated requester while a platform worker retires one. */
void
check_ids_unique_across_brokers() {
    FakeHost host;
    qint64 before = 0;
    {
        LocationRequests requests;
        requests.setHost(&host);
        before = requests.requestSite(QStringLiteral("map"));
    }
    LocationRequests recreated;
    recreated.setHost(&host);
    check(recreated.requestSite(QStringLiteral("map")) > before, "ids: a new broker continues the ids");
}

} // namespace

int
main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    check_overlapping_requesters();
    check_precise_defaults_to_site();
    check_cancel();
    check_stale_ids();
    check_answer_inside_the_call();
    check_host_replacement();
    check_ids_unique_across_brokers();
    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    return 0;
}
