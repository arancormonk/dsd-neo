// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Unit tests: RrPlaceService, the RadioReference place names and "Update from RadioReference" for saved systems,
 * driven with no network: each client gets a transport serving the captured fixtures under
 * tests/fixtures/radioreference, keyed by the SOAP method and the sid or stid read out of the request body. The
 * saved systems are the production model over a disposable store.
 *
 * House style, as test_ui_qt_radio_reference_model.cpp: no Qt6::Test; signals are counted with plain lambdas and the
 * queued marshalling from the clients' worker threads is pumped against a deadline. */

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QGuiApplication>
#include <QHash>
#include <QIODevice>
#include <QLatin1String>
#include <QList>
#include <QMetaObject>
#include <QMetaProperty>
#include <QMutex>
#include <QMutexLocker>
#include <QObject>
#include <QSemaphore>
#include <QSet>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>
#include <QtGlobal>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/runtime/radioreference.h>
#include <functional>
#include <stdio.h>
#include <stdlib.h>
#include "../test_support/qt_test_paths.h"

#include "app_prefs.h"
#include "decoder_host.h"
#include "imported_files_model.h"
#include "json_store.h"
#include "qml_gc_policy.h"
#include "radio_reference_model.h"
#include "rr_place_service.h"
#include "saved_systems_model.h"
#include "teardown_trace_capture.h"

#ifndef DSD_NEO_TEST_RR_FIXTURE_DIR
#error "DSD_NEO_TEST_RR_FIXTURE_DIR must be defined by the build"
#endif

namespace {

using dsd_qt::RadioReferenceModel;
using dsd_qt::RrPlaceService;
using dsd_qt::SavedSystemsModel;

int g_failures = 0;

constexpr QLatin1String kPassword("SENTINEL_PW_5c8");
constexpr QLatin1String kUsername("SENTINEL_USER_2e6");
constexpr QLatin1String kAppKey("SENTINEL_KEY_9a1");

void
expect(const char* what, bool ok) {
    if (!ok) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", what);
        g_failures++;
    }
}

void
expect_int(const char* what, int got, int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "FAIL: %s (got %d, want %d)\n", what, got, want);
        g_failures++;
    }
}

void
expect_str(const char* what, const QString& got, const QString& want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "FAIL: %s (got \"%s\", want \"%s\")\n", what, got.toUtf8().constData(),
                    want.toUtf8().constData());
        g_failures++;
    }
}

class TestHost : public dsd_qt::DecoderHost {
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

/* ------------------------------------------------------------------------- */
/* Fake transport                                                             */
/* ------------------------------------------------------------------------- */

/** Names a synthetic getStateInfo answer (the empty-county fixture re-labelled) for states with no capture. */
const QHash<int, QString>&
synthetic_states() {
    static const QHash<int, QString> names = {{17, QStringLiteral("Illinois")}, {46, QStringLiteral("South Dakota")}};
    return names;
}

/**
 * One client's transport. perform() runs on that client's worker thread, so everything a case sets or reads while
 * a request may be running goes through the mutex.
 */
struct Fake {
    QMutex mutex;
    QStringList log; /* "method:id" per request, in order */
    QSet<int> failStates;
    QSet<int> failSids;
    QList<QPair<QByteArray, QByteArray>> siteEdits; /* rewrites of getTrsSites payloads */
    /* A one-shot hold: the first request of this method waits for `proceed` (or its cancellation). */
    QString holdMethod;
    QSemaphore held;
    QSemaphore proceed;

    QStringList
    requests() {
        QMutexLocker lock(&mutex);
        return log;
    }

    int
    count(const QString& entry) {
        return static_cast<int>(requests().count(entry));
    }
};

QByteArray
read_fixture(const QString& leaf) {
    QFile file(QString::fromUtf8(DSD_NEO_TEST_RR_FIXTURE_DIR) + QLatin1Char('/') + leaf);
    if (!file.open(QIODevice::ReadOnly)) {
        DSD_FPRINTF(stderr, "FAIL: cannot open fixture %s\n", leaf.toUtf8().constData());
        g_failures++;
        return QByteArray();
    }
    return file.readAll();
}

QString
method_of(const QByteArray& body) {
    const int start = body.indexOf("<ns1:");
    const int end = start < 0 ? -1 : body.indexOf('>', start);
    return end < 0 ? QString() : QString::fromUtf8(body.mid(start + 5, end - start - 5));
}

/** @brief The value of the `<name ...>` part, or 0. */
int
part_of(const QByteArray& body, const QByteArray& name) {
    const int start = body.indexOf("<" + name + " ");
    const int open = start < 0 ? -1 : body.indexOf('>', start);
    const int close = open < 0 ? -1 : body.indexOf("</" + name + ">", open);
    return close < 0 ? 0 : QString::fromUtf8(body.mid(open + 1, close - open - 1)).toInt();
}

QString
suffix_for_sid(int sid) {
    switch (sid) {
        case 6673: return QStringLiteral("p25");
        case 12574: return QStringLiteral("capplus");
        case 12918: return QStringLiteral("nxdn");
        case 220: return QStringLiteral("edacs");
        default: return QStringLiteral("dmr_conv");
    }
}

/** @brief The payload for one request, or empty to fail it. */
QByteArray
payload_for(Fake* fake, const QString& method, int sid, int stid) {
    if (method == QLatin1String("getStateInfo")) {
        if (fake->failStates.contains(stid)) {
            return QByteArray();
        }
        if (stid == 19) {
            return read_fixture(QStringLiteral("state_info_19.xml"));
        }
        QByteArray body = read_fixture(QStringLiteral("state_info_empty_counties.xml"));
        body.replace(">11</stid>", ">" + QByteArray::number(stid) + "</stid>");
        body.replace(">District of Columbia<",
                     ">" + synthetic_states().value(stid, QStringLiteral("Elsewhere")).toUtf8() + "<");
        return body;
    }
    if (fake->failSids.contains(sid)) {
        return QByteArray();
    }
    if (method == QLatin1String("getTrsDetails")) {
        return read_fixture(QStringLiteral("trs_details_") + suffix_for_sid(sid) + QStringLiteral(".xml"));
    }
    if (method == QLatin1String("getTrsSites")) {
        QByteArray body =
            read_fixture(sid == 12244 ? QStringLiteral("trs_sites_dmr_conv_small.xml")
                                      : QStringLiteral("trs_sites_") + suffix_for_sid(sid) + QStringLiteral(".xml"));
        for (const auto& edit : fake->siteEdits) {
            body.replace(edit.first, edit.second);
        }
        return body;
    }
    if (method == QLatin1String("getTrsTalkgroups")) {
        return read_fixture(QStringLiteral("trs_talkgroups_") + suffix_for_sid(sid) + QStringLiteral(".xml"));
    }
    if (method == QLatin1String("getTrsTalkgroupCats")) {
        return read_fixture(QStringLiteral("trs_talkgroup_cats_p25.xml"));
    }
    static const QHash<QString, QString> tables = {
        {QStringLiteral("getTrsType"), QStringLiteral("trs_types.xml")},
        {QStringLiteral("getTrsFlavor"), QStringLiteral("trs_flavors.xml")},
        {QStringLiteral("getTrsVoice"), QStringLiteral("trs_voices.xml")},
        {QStringLiteral("getCountryList"), QStringLiteral("country_list.xml")}};
    return tables.contains(method) ? read_fixture(tables.value(method)) : QByteArray();
}

/** @brief Wait out a hold: true to answer, false when the request was cancelled meanwhile. */
bool
wait_for_release(Fake* fake, const dsd_rr_request* req) {
    fake->held.release();
    while (!fake->proceed.tryAcquire(1, 10)) {
        if (dsd_rr_cancel_requested(req->cancel) != 0) {
            return false;
        }
    }
    return true;
}

int
fake_perform(void* ctx, const dsd_rr_request* req, dsd_rr_response* resp) {
    auto* fake = static_cast<Fake*>(ctx);
    DSD_MEMSET(resp, 0, sizeof(*resp));
    const QByteArray body(req->body, static_cast<int>(req->body_len));
    const QString method = method_of(body);
    const int sid = part_of(body, "sid");
    const int stid = part_of(body, "stid");
    bool hold = false;
    QByteArray payload;
    {
        QMutexLocker lock(&fake->mutex);
        fake->log.append(method + QLatin1Char(':') + QString::number(stid > 0 ? stid : sid));
        hold = !fake->holdMethod.isEmpty() && fake->holdMethod == method;
        if (hold) {
            fake->holdMethod.clear();
        }
    }
    if (hold && !wait_for_release(fake, req)) {
        resp->status = DSD_RR_ERR_CANCELLED;
        return -1;
    }
    {
        QMutexLocker lock(&fake->mutex);
        payload = payload_for(fake, method, sid, stid);
    }
    if (payload.isEmpty()) {
        /* Not DSD_RR_ERR_NETWORK, which the client retries once: each request here is one the service sent. */
        resp->status = DSD_RR_ERR_HTTP;
        (void)DSD_SNPRINTF(resp->error, sizeof(resp->error), "%s", "service unavailable");
        return -1;
    }
    resp->http_status = 200;
    resp->body = static_cast<char*>(malloc(static_cast<size_t>(payload.size()) + 1U));
    if (resp->body == nullptr) {
        resp->status = DSD_RR_ERR_NOMEM;
        return -1;
    }
    DSD_MEMCPY(resp->body, payload.constData(), static_cast<size_t>(payload.size()));
    resp->body[payload.size()] = '\0';
    resp->body_len = static_cast<size_t>(payload.size());
    resp->status = DSD_RR_OK;
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Harness                                                                    */
/* ------------------------------------------------------------------------- */

QString
store_path() {
    return dsd_qt::json_store_path(QStringLiteral("saved_systems.json"));
}

/** @brief Start a case with no saved systems on disk. */
void
clear_store() {
    QFile::remove(store_path());
}

/** @brief Make the next save of the systems store fail (a directory where the file goes). */
bool
block_store() {
    const QString path = store_path();
    return QFile::rename(path, path + QStringLiteral(".saved")) && QDir().mkpath(path);
}

bool
unblock_store() {
    const QString path = store_path();
    return QDir().rmdir(path) && QFile::rename(path + QStringLiteral(".saved"), path);
}

/**
 * Declaration order is load-bearing, as in test_ui_qt_radio_reference_model.cpp: the transports and their fakes
 * outlive the model and the service, whose destructors join their workers.
 */
struct Harness {
    TestHost host;
    dsd_qt::AppPrefs prefs;
    dsd_qt::ImportedFilesModel library{&host};
    Fake modelFake;
    Fake fake;
    dsd_rr_transport modelTransport{&fake_perform, &modelFake};
    dsd_rr_transport transport{&fake_perform, &fake};
    SavedSystemsModel systems;
    RadioReferenceModel model{&prefs, &library, &host};
    RrPlaceService service{&model, &systems};

    Harness() {
        prefs.setRrUsername(kUsername);
        prefs.setRrAppKey(kAppKey);
        model.setPassword(kPassword);
        model.setTransportForTests(&modelTransport);
        service.setTransportForTests(&transport);
    }
};

/** @brief Run the event loop until the model and the service have been idle for a few rounds. */
void
settle(Harness& h, int timeoutMs = 15000) {
    QElapsedTimer timer;
    timer.start();
    int quiet = 0;
    while (quiet < 3 && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        const bool busy = h.model.busy() || h.service.busy() || h.service.updating();
        quiet = busy ? 0 : quiet + 1;
    }
    expect("settled before the deadline", quiet >= 3);
}

/** @brief Pump until the fake has a request held. */
bool
await_hold(Fake& fake, int timeoutMs = 15000) {
    QElapsedTimer timer;
    timer.start();
    while (!fake.held.tryAcquire() && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }
    return timer.elapsed() < timeoutMs;
}

QVariantMap
site_row(const QString& name, int sid, int siteId, const QString& siteName, const QVariantMap& place = QVariantMap()) {
    QVariantMap row{{"name", name},     {"sourceType", "usb"}, {"freqMhz", "851.05"}, {"decodeFlag", "-mq"},
                    {"trunking", true}, {"rrSid", sid},        {"rrSiteId", siteId},  {"siteName", siteName},
                    {"siteLat", 41.0},  {"siteLon", -91.0},    {"hasSitePos", true}};
    if (!place.isEmpty()) {
        row.insert("place", place);
    }
    return row;
}

QVariantMap
county_place(int ctid, const QVariantList& candidates) {
    return {{"states", QVariantList()},
            {"counties", QVariantList{QVariantMap{{"ctid", ctid}}}},
            {"candidateStates", candidates}};
}

QVariantMap
place_of(const SavedSystemsModel& systems, int row) {
    return systems.get(row).value("place").toMap();
}

QVariantMap
county_of(const QVariantMap& place) {
    return place.value("counties").toList().value(0).toMap();
}

/** @brief Whether @p place's states are exactly @p stid named @p name. */
bool
only_state(const QVariantMap& place, int stid, const QString& name) {
    const QVariantList states = place.value("states").toList();
    return states.size() == 1 && states[0].toMap().value("stid").toInt() == stid
           && states[0].toMap().value("name").toString() == name;
}

/** @brief Whether @p place reads Johnson County, Iowa. */
bool
johnson_iowa(const QVariantMap& place) {
    const QVariantMap county = county_of(place);
    return county.value("ctid").toInt() == 836 && county.value("stid").toInt() == 19
           && county.value("name").toString() == QLatin1String("Johnson") && only_state(place, 19, "Iowa");
}

/** @brief The saved-system map an import result makes, as the wizard saves it. */
QVariantMap
saved_from(const QVariantMap& imported) {
    QVariantMap row = imported;
    row.insert("sourceType", "usb");
    row.remove("ok");
    row.remove("error");
    row.remove("savedRow");
    return row;
}

struct Finished {
    int count = 0;
    int updated = -1;
    int unreachable = -1;
};

void
record_finished(RrPlaceService& service, Finished* finished) {
    QObject::connect(&service, &RrPlaceService::updateFinished, &service, [finished](int updated, int unreachable) {
        finished->count++;
        finished->updated = updated;
        finished->unreachable = unreachable;
    });
}

/* ------------------------------------------------------------------------- */
/* Names                                                                      */
/* ------------------------------------------------------------------------- */

/* getStateInfo(19) names every saved row that names Iowa's counties: Johnson gets its name and its state, and the row
 * gains Iowa as its state. */
void
test_resolve_names_saved_rows() {
    clear_store();
    Harness h;
    h.systems.add(
        site_row("SARA — Johnson Co Simulcast", 6673, 16863, "Johnson Co Simulcast", county_place(836, {19, 17, 46})));
    h.systems.add(site_row("Elsewhere", 9999, 1, "Somewhere", county_place(5000, {55})));
    expect_int("names: both rows unplaced", h.service.unplacedCount(), 2);
    int changes = 0;
    QObject::connect(&h.service, &RrPlaceService::unplacedCountChanged, [&changes]() { changes++; });
    h.service.resolveStates({19});
    settle(h);
    expect("names: Johnson, Iowa", johnson_iowa(place_of(h.systems, 0)));
    expect("names: a row naming other ids is left alone", place_of(h.systems, 1) == county_place(5000, {55}));
    expect_int("names: one getStateInfo", h.fake.count("getStateInfo:19"), 1);
    expect_int("names: one row left unplaced", h.service.unplacedCount(), 1);
    expect_int("names: the count told once", changes, 1);
    expect("names: the names are saved", johnson_iowa(SavedSystemsModel().get(0).value("place").toMap()));
}

/* The automatic trigger: a completed loadSystem() asks for at most eight of its states, the first eight in order. */
void
test_load_resolves_at_most_eight_states() {
    clear_store();
    Harness h;
    h.service.resolveStates({0, -3, 19, 19, 17, 46, 1, 2, 3, 4, 5, 6, 7});
    settle(h);
    const QStringList asked = h.fake.requests();
    expect_int("eight: eight lookups", asked.size(), 8);
    expect("eight: the first eight valid ids, in order",
           asked
               == (QStringList{"getStateInfo:19", "getStateInfo:17", "getStateInfo:46", "getStateInfo:1",
                               "getStateInfo:2", "getStateInfo:3", "getStateInfo:4", "getStateInfo:5"}));

    clear_store();
    Harness loaded;
    loaded.model.loadSystem(6673);
    settle(loaded);
    expect("load: SARA's states looked up after its load",
           loaded.fake.requests() == (QStringList{"getStateInfo:19", "getStateInfo:17", "getStateInfo:46"}));
}

/* Names that arrive before the plan, between the plan and the save, or after the save all end on the saved row: the
 * model fills a new row from the cache, and a lookup names rows already saved. */
void
test_names_whatever_the_order() {
    for (int when = 0; when < 3; when++) {
        clear_store();
        Harness h;
        if (when == 0) {
            h.service.resolveStates({19});
            settle(h);
        } else {
            /* Hold the lookup the load triggers until the plan is built (and, for the last case, saved). */
            QMutexLocker lock(&h.fake.mutex);
            h.fake.holdMethod = QStringLiteral("getStateInfo");
        }
        h.model.loadSystem(6673);
        if (when != 0) {
            expect("order: the lookup is held", await_hold(h.fake));
        } else {
            settle(h);
        }
        const QVariantMap plan = h.model.buildImportPlan({0}, {});
        const QVariantMap imported = h.model.performImport(plan, "SARA — Johnson Co Simulcast", -1);
        expect("order: plan unnamed while the lookup waits",
               when == 0 || county_of(plan.value("place").toMap()).value("name").isNull());
        if (when == 1) {
            h.fake.proceed.release();
            settle(h);
        }
        expect("order: saved", h.systems.add(saved_from(imported)));
        if (when == 2) {
            expect("order: unnamed until the lookup lands", !johnson_iowa(place_of(h.systems, 0)));
            h.fake.proceed.release();
        }
        settle(h);
        expect("order: Johnson, Iowa in the end", johnson_iowa(place_of(h.systems, 0)));
        expect("order: Johnson, Iowa on disk", johnson_iowa(SavedSystemsModel().get(0).value("place").toMap()));
    }
}

/* A failed lookup is recorded for its state alone and asked again only when a later call asks for it. */
void
test_failure_recorded_per_state() {
    clear_store();
    Harness h;
    h.systems.add(site_row("SARA", 6673, 16863, "Johnson Co Simulcast", county_place(836, {19, 17})));
    {
        QMutexLocker lock(&h.fake.mutex);
        h.fake.failStates = {17};
    }
    h.service.resolveStates({19, 17});
    settle(h);
    expect("failure: 17 failed", h.service.stateFailed(17));
    expect("failure: 19 did not", !h.service.stateFailed(19));
    expect("failure: the other state still named the row", johnson_iowa(place_of(h.systems, 0)));
    expect_int("failure: asked once", h.fake.count("getStateInfo:17"), 1);
    for (int i = 0; i < 20; i++) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }
    expect_int("failure: no retry by itself", h.fake.count("getStateInfo:17"), 1);

    {
        QMutexLocker lock(&h.fake.mutex);
        h.fake.failStates.clear();
    }
    h.service.resolveStates({19, 17});
    settle(h);
    expect_int("failure: a new call asks again", h.fake.count("getStateInfo:17"), 2);
    expect_int("failure: a known state is not asked again", h.fake.count("getStateInfo:19"), 1);
    expect("failure: answered now", !h.service.stateFailed(17));
}

/* A save that fails while names are applied keeps the rows as they were; the next resolve, of any state, applies the
 * names it already has without asking again. */
void
test_failed_name_save_is_retried() {
    clear_store();
    Harness h;
    h.systems.add(site_row("SARA", 6673, 16863, "Johnson Co Simulcast", county_place(836, {19, 17, 46})));
    expect("retry: store blocked", block_store());
    h.service.resolveStates({19});
    settle(h);
    expect("retry: the failed save left the row unnamed", county_of(place_of(h.systems, 0)).value("name").isNull());
    expect("retry: store unblocked", unblock_store());
    h.service.resolveStates({46});
    settle(h);
    expect("retry: named at the next resolve", johnson_iowa(place_of(h.systems, 0)));
    expect_int("retry: Iowa asked once", h.fake.count("getStateInfo:19"), 1);
    expect("retry: on disk", johnson_iowa(SavedSystemsModel().get(0).value("place").toMap()));
}

/* Signing out (any credential change) drops what was learned and stops asking. */
void
test_sign_out_clears_the_cache() {
    clear_store();
    Harness h;
    h.service.resolveStates({19});
    settle(h);
    h.model.clearPassword();
    expect("sign-out: update refused (account screen)", !h.service.updateFromRadioReference({}));
    h.service.resolveStates({17});
    settle(h);
    expect_int("sign-out: nothing asked while signed out", h.fake.count("getStateInfo:17"), 0);
    h.systems.add(site_row("SARA", 6673, 16863, "Johnson Co Simulcast", county_place(836, {19})));
    expect("sign-out: a new row is not named from a dropped cache",
           county_of(place_of(h.systems, 0)).value("name").isNull());

    h.model.setPassword(kPassword);
    {
        QMutexLocker lock(&h.fake.mutex);
        h.fake.holdMethod = QStringLiteral("getStateInfo");
    }
    h.service.resolveStates({19});
    expect("sign-out: a lookup in flight", await_hold(h.fake));
    h.model.clearPassword();
    h.fake.proceed.release();
    settle(h);
    expect("sign-out: its late answer is dropped", county_of(place_of(h.systems, 0)).value("name").isNull());
}

/* ------------------------------------------------------------------------- */
/* Update from RadioReference                                                 */
/* ------------------------------------------------------------------------- */

/* An update refreshes each saved site still on the site it was read for: name (a generated system name follows it),
 * position and place ids, keeping the names it already had; then names whatever is still unnamed. */
void
test_update_refreshes_sites() {
    clear_store();
    Harness h;
    {
        QMutexLocker lock(&h.fake.mutex);
        h.fake.siteEdits = {{">Johnson Co Simulcast<", ">Johnson County Simulcast<"}, {">41.65503<", ">41.70000<"}};
    }
    h.systems.add(
        site_row("SARA — Johnson Co Simulcast", 6673, 16863, "Johnson Co Simulcast", county_place(999, {19})));
    h.systems.add(site_row("My Linn", 6673, 23581, "Old Linn"));
    Finished finished;
    record_finished(h.service, &finished);
    QStringList progress;
    QObject::connect(&h.service, &RrPlaceService::progressTextChanged,
                     [&h, &progress]() { progress.append(h.service.progressText()); });
    expect("update: started", h.service.updateFromRadioReference({h.systems.get(0).value("uid").toString(),
                                                                  h.systems.get(1).value("uid").toString()}));
    expect("update: updating", h.service.updating());
    expect_str("update: progress", h.service.progressText(), "Updating 1 of 2");
    settle(h);
    const QVariantMap johnson = h.systems.get(0);
    expect_str("update: site renamed", johnson.value("siteName").toString(), "Johnson County Simulcast");
    expect_str("update: generated name follows", johnson.value("name").toString(), "SARA — Johnson County Simulcast");
    expect("update: new position", qAbs(johnson.value("siteLat").toDouble() - 41.7) < 1e-9
                                       && qAbs(johnson.value("siteLon").toDouble() + 91.60244) < 1e-9);
    expect("update: place ids refreshed and named", johnson_iowa(johnson.value("place").toMap()));
    expect("update: SARA's states as candidates",
           johnson.value("place").toMap().value("candidateStates").toList() == (QVariantList{19, 17, 46}));
    const QVariantMap linn = h.systems.get(1);
    expect_str("update: a custom name stays", linn.value("name").toString(), "My Linn");
    expect_str("update: its site name is refreshed", linn.value("siteName").toString(), "Linn Co Simulcast");
    expect_int("update: Linn county", county_of(linn.value("place").toMap()).value("ctid").toInt(), 841);
    expect_int("update: finished once", finished.count, 1);
    expect_int("update: both updated", finished.updated, 2);
    expect_int("update: none unreachable", finished.unreachable, 0);
    expect_str("update: summary", h.service.summaryText(), "Updated 2 systems.");
    expect("update: progress cleared", h.service.progressText().isEmpty() && !h.service.updating());
    expect("update: progress was shown", progress.contains("Updating 1 of 2"));
    expect_int("update: one system read once", h.fake.count("getTrsSites:6673"), 1);
    expect_int("update: Iowa named", h.fake.count("getStateInfo:19"), 1);
    expect_str("update: saved", SavedSystemsModel().get(0).value("siteName").toString(), "Johnson County Simulcast");

    /* Again, with the cache dropped (a credential change) and every lookup failing: the names the row had stay. */
    h.model.clearPassword();
    h.model.setPassword(kPassword);
    {
        QMutexLocker lock(&h.fake.mutex);
        h.fake.failStates = {19, 17, 46};
    }
    expect("update: second run", h.service.updateFromRadioReference({johnson.value("uid").toString()}));
    settle(h);
    expect("update: names kept", johnson_iowa(place_of(h.systems, 0)));
}

/* A row edited onto another system while its system is read is left alone and not counted; a site RadioReference no
 * longer lists is left alone and counted. */
void
test_update_skips_moved_and_counts_unlisted() {
    clear_store();
    Harness h;
    h.systems.add(site_row("SARA — Johnson Co Simulcast", 6673, 16863, "Johnson Co Simulcast"));
    h.systems.add(site_row("SARA — Linn Co Simulcast", 6673, 23581, "Linn Co Simulcast"));
    h.systems.add(site_row("SARA — Gone", 6673, 99999, "Gone"));
    {
        QMutexLocker lock(&h.fake.mutex);
        h.fake.holdMethod = QStringLiteral("getTrsSites");
        h.fake.siteEdits = {{">Linn Co Simulcast<", ">Linn County<"}};
    }
    Finished finished;
    record_finished(h.service, &finished);
    QStringList uids;
    for (int row = 0; row < 3; row++) {
        uids.append(h.systems.get(row).value("uid").toString());
    }
    expect("moved: started", h.service.updateFromRadioReference(uids));
    expect("moved: sites held", await_hold(h.fake));
    expect("moved: edited onto another system", h.systems.update(1, {{"rrSid", 7777}}));
    h.fake.proceed.release();
    settle(h);
    expect_str("moved: the moved row keeps its site name", h.systems.get(1).value("siteName").toString(),
               "Linn Co Simulcast");
    expect_int("moved: and its system", h.systems.get(1).value("rrSid").toInt(), 7777);
    expect_str("moved: the unlisted site is left", h.systems.get(2).value("name").toString(), "SARA — Gone");
    expect_int("moved: finished once", finished.count, 1);
    expect_int("moved: one updated", finished.updated, 1);
    expect_int("moved: the unlisted site counted", finished.unreachable, 1);
    expect_str("moved: summary", h.service.summaryText(), "Updated 1 systems. 1 could not be reached.");
}

/* Partial success: a system RadioReference does not answer is counted per saved system; the others are saved. */
void
test_update_partial_success() {
    clear_store();
    Harness h;
    const QList<QPair<int, QString>> sara = {{16863, "Johnson Co Simulcast"},
                                             {23581, "Linn Co Simulcast"},
                                             {31563, "Black Hawk Co Simulcast"},
                                             {31568, "Polk Co Simulcast"},
                                             {36465, "Dubuque Co Simulcast"}};
    for (const auto& site : sara) {
        h.systems.add(site_row("SARA — " + site.second, 6673, site.first, site.second));
    }
    h.systems.add(site_row("Cap+", 12574, 43552, "Cedar Rapids"));
    h.systems.add(site_row("EDACS", 220, 1, "Somewhere"));
    {
        QMutexLocker lock(&h.fake.mutex);
        h.fake.failSids = {220};
    }
    QStringList uids;
    for (int row = 0; row < h.systems.count(); row++) {
        uids.append(h.systems.get(row).value("uid").toString());
    }
    Finished finished;
    record_finished(h.service, &finished);
    expect("partial: started", h.service.updateFromRadioReference(uids));
    settle(h);
    expect_int("partial: finished once", finished.count, 1);
    expect_int("partial: six updated", finished.updated, 6);
    expect_int("partial: one unreachable", finished.unreachable, 1);
    expect_str("partial: summary", h.service.summaryText(), "Updated 6 systems. 1 could not be reached.");
    expect_int("partial: Cap+ placed in Linn County", county_of(place_of(h.systems, 5)).value("ctid").toInt(), 841);
    expect("partial: the unreachable row is unchanged", place_of(h.systems, 6).isEmpty());
    expect_int("partial: one read per system", h.fake.count("getTrsSites:6673"), 1);
}

/* A save that fails while an update writes a system's sites leaves those rows as they were and counts them. */
void
test_update_failed_save() {
    clear_store();
    Harness h;
    h.systems.add(site_row("SARA — Old", 6673, 16863, "Old"));
    Finished finished;
    record_finished(h.service, &finished);
    expect("failed save: store blocked", block_store());
    expect("failed save: started", h.service.updateFromRadioReference({h.systems.get(0).value("uid").toString()}));
    settle(h);
    expect("failed save: store unblocked", unblock_store());
    expect_str("failed save: the row as it was", h.systems.get(0).value("siteName").toString(), "Old");
    expect_int("failed save: finished once", finished.count, 1);
    expect_int("failed save: none updated", finished.updated, 0);
    expect_int("failed save: counted", finished.unreachable, 1);
}

/* Cancel stops the update between systems: what was saved stays, the rest is untouched, and it ends once. */
void
test_cancel_keeps_what_was_saved() {
    clear_store();
    Harness h;
    h.systems.add(site_row("SARA — Old", 6673, 16863, "Old"));
    h.systems.add(site_row("Cap+ — Old", 12574, 43552, "Old"));
    Finished finished;
    record_finished(h.service, &finished);
    const QStringList uids{h.systems.get(0).value("uid").toString(), h.systems.get(1).value("uid").toString()};
    /* Armed once SARA is saved, so the hold lands on Cap+'s details: the user cancels between the two systems. */
    bool armed = false;
    QObject::connect(&h.systems, &SavedSystemsModel::dataChanged, &h.service, [&h, &armed]() {
        if (!armed) {
            armed = true;
            QMutexLocker lock(&h.fake.mutex);
            h.fake.holdMethod = QStringLiteral("getTrsDetails");
        }
    });
    expect("cancel: started", h.service.updateFromRadioReference(uids));
    expect("cancel: the second system is held", await_hold(h.fake));
    h.service.cancelUpdate();
    settle(h);
    expect_str("cancel: the first system was saved", SavedSystemsModel().get(0).value("siteName").toString(),
               "Johnson Co Simulcast");
    expect_str("cancel: the second is untouched", h.systems.get(1).value("siteName").toString(), "Old");
    expect_int("cancel: finished once", finished.count, 1);
    expect_int("cancel: one updated", finished.updated, 1);
    expect_int("cancel: none unreachable", finished.unreachable, 0);
    expect("cancel: idle", !h.service.updating() && !h.service.busy());
}

/* A cancel from a handler of the saved rows' own signals, while a step is saving: the update ends there, once, and
 * asks for nothing more. */
void
test_cancel_from_a_row_signal() {
    clear_store();
    Harness h;
    h.systems.add(site_row("SARA — Old", 6673, 16863, "Old"));
    h.systems.add(site_row("Cap+ — Old", 12574, 43552, "Old"));
    Finished finished;
    record_finished(h.service, &finished);
    QMetaObject::Connection cancel =
        QObject::connect(&h.systems, &SavedSystemsModel::dataChanged, &h.service, [&h]() { h.service.cancelUpdate(); });
    expect("row signal: started", h.service.updateFromRadioReference({h.systems.get(0).value("uid").toString(),
                                                                      h.systems.get(1).value("uid").toString()}));
    settle(h);
    expect_int("row signal: finished once", finished.count, 1);
    expect_int("row signal: the second system never asked", h.fake.count("getTrsDetails:12574"), 0);
    expect("row signal: idle", !h.service.updating() && !h.service.busy());

    /* The names step: Iowa is known but its names were never saved (the save failed), so the step saves them itself,
     * before it would ask for Wisconsin. */
    QObject::disconnect(cancel);
    h.systems.add({{"name", "Repeaters"}, {"sourceType", "usb"}, {"place", county_place(836, {19})}});
    h.systems.add({{"name", "More"}, {"sourceType", "usb"}, {"place", county_place(5000, {55})}});
    expect("row signal: store blocked", block_store());
    h.service.resolveStates({19});
    settle(h);
    expect("row signal: store unblocked", unblock_store());
    expect("row signal: unnamed after the failed save", !johnson_iowa(place_of(h.systems, 2)));
    cancel =
        QObject::connect(&h.systems, &SavedSystemsModel::dataChanged, &h.service, [&h]() { h.service.cancelUpdate(); });
    finished = Finished();
    expect("row signal: second start", h.service.updateFromRadioReference({h.systems.get(2).value("uid").toString(),
                                                                           h.systems.get(3).value("uid").toString()}));
    settle(h);
    QObject::disconnect(cancel);
    expect("row signal: named by the step", johnson_iowa(place_of(h.systems, 2)));
    expect_int("row signal: the names step ended once", finished.count, 1);
    expect_int("row signal: Wisconsin never asked", h.fake.count("getStateInfo:55"), 0);
}

/* A row with a place and no site (a conventional repeater list) is updated by naming its place; nothing to update is
 * answered at once; while signed out the account screen is asked for. */
void
test_update_place_rows_and_edges() {
    clear_store();
    Harness h;
    h.systems.add({{"name", "Repeaters"},
                   {"sourceType", "usb"},
                   {"freqMhz", "146.755"},
                   {"decodeFlag", "-fs -Y"},
                   {"place", county_place(836, {19})}});
    h.systems.add({{"name", "Manual"}, {"sourceType", "usb"}, {"freqMhz", "460.0"}});
    Finished finished;
    record_finished(h.service, &finished);
    expect("place rows: started", h.service.updateFromRadioReference({h.systems.get(0).value("uid").toString(),
                                                                      h.systems.get(1).value("uid").toString()}));
    settle(h);
    expect("place rows: named", johnson_iowa(place_of(h.systems, 0)));
    expect_int("place rows: one updated", finished.updated, 1);
    expect("place rows: no site read", h.fake.requests().filter("getTrsSites").isEmpty());

    expect("nothing: started", h.service.updateFromRadioReference({h.systems.get(1).value("uid").toString()}));
    expect_int("nothing: finished at once", finished.count, 2);
    expect_int("nothing: nothing updated", finished.updated, 0);
    expect("nothing: idle", !h.service.updating());
}

/* unplacedCount counts what an update could place and shows no place, and follows the saved systems. */
void
test_unplaced_count() {
    clear_store();
    Harness h;
    h.systems.add(site_row("Site", 6673, 16863, "Johnson Co Simulcast"));
    h.systems.add({{"name", "Repeaters"}, {"sourceType", "usb"}, {"place", county_place(836, {19})}});
    h.systems.add({{"name", "Manual"}, {"sourceType", "usb"}});
    QVariantMap named = county_place(836, {19});
    named.insert("states", QVariantList{QVariantMap{{"stid", 19}, {"name", "Iowa"}}});
    h.systems.add(site_row("Named", 6673, 23581, "Linn Co Simulcast", named));
    expect_int("unplaced: the site and the repeater list", h.service.unplacedCount(), 2);
    h.systems.remove(0);
    expect_int("unplaced: follows a removal", h.service.unplacedCount(), 1);
}

/* ------------------------------------------------------------------------- */
/* Isolation from the import screen                                           */
/* ------------------------------------------------------------------------- */

struct ModelSignals {
    int system = 0;
    int lists = 0;
    int busy = 0;
    int status = 0;
    int refresh = 0;
};

void
record_model(RadioReferenceModel& model, ModelSignals* log) {
    QObject::connect(&model, &RadioReferenceModel::systemChanged, [log]() { log->system++; });
    QObject::connect(&model, &RadioReferenceModel::listsChanged, [log]() { log->lists++; });
    QObject::connect(&model, &RadioReferenceModel::busyChanged, [log]() { log->busy++; });
    QObject::connect(&model, &RadioReferenceModel::statusChanged, [log]() { log->status++; });
    QObject::connect(&model, &RadioReferenceModel::refreshFinished, [log](int, const QVariantMap&) { log->refresh++; });
}

/* Failing lookups after a load change nothing the import screen shows: its lists, busy, the loaded system and its
 * selection, its status, a library refresh riding on the load; and they never send systemChanged. */
void
test_failed_lookups_leave_the_import_screen_alone() {
    clear_store();
    Harness h;
    {
        QMutexLocker lock(&h.fake.mutex);
        h.fake.failStates = {19, 17, 46};
        h.fake.holdMethod = QStringLiteral("getStateInfo");
    }
    h.model.loadCountries();
    settle(h);
    h.model.loadSystem(6673);
    QElapsedTimer timer;
    timer.start();
    while (h.model.busy() && timer.elapsed() < 15000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }
    expect("isolation: the first lookup is held", await_hold(h.fake));
    const QVariantList countries = h.model.countries();
    const QVariantList sites = h.model.sites();
    const QVariantMap details = h.model.systemDetails();
    const QVariantMap plan = h.model.buildImportPlan({0}, {});
    ModelSignals log;
    record_model(h.model, &log);
    h.fake.proceed.release();
    settle(h);
    expect("isolation: all three failed",
           h.service.stateFailed(19) && h.service.stateFailed(17) && h.service.stateFailed(46));
    expect("isolation: not busy", !h.model.busy());
    expect_int("isolation: no systemChanged", log.system, 0);
    expect_int("isolation: no listsChanged", log.lists, 0);
    expect_int("isolation: no busyChanged", log.busy, 0);
    expect_int("isolation: no statusChanged", log.status, 0);
    expect("isolation: browse lists unchanged", h.model.countries() == countries && !countries.isEmpty());
    expect("isolation: the loaded system unchanged", h.model.sites() == sites && h.model.systemDetails() == details);
    expect("isolation: the same plan for the same selection", h.model.buildImportPlan({0}, {}) == plan);
    expect_int("isolation: no error", h.model.errorKind(), RadioReferenceModel::NoError);
    expect_int("isolation: the model sent no getStateInfo",
               static_cast<int>(h.modelFake.requests().filter("getStateInfo").size()), 0);

    /* A library refresh: the load it rides on triggers lookups that fail; it still completes, once. */
    const QVariantMap imported = h.model.performImport(plan, "SARA", -1);
    expect("isolation: imported", imported.value("ok").toBool());
    int row = -1;
    for (int i = 0; i < h.library.rowCount(); i++) {
        if (h.library.get(i).value("type").toString() == QLatin1String("chan")) {
            row = i;
        }
    }
    {
        QMutexLocker lock(&h.fake.mutex);
        h.fake.holdMethod = QStringLiteral("getStateInfo");
    }
    QVariantMap refreshed;
    QObject::connect(&h.model, &RadioReferenceModel::refreshFinished,
                     [&refreshed](int, const QVariantMap& result) { refreshed = result; });
    const int before = log.refresh;
    expect("isolation: refresh started", h.model.refreshRow(row));
    expect("isolation: a refresh's lookup is held", await_hold(h.fake));
    h.fake.proceed.release();
    settle(h);
    expect_int("isolation: the refresh finished once", log.refresh - before, 1);
    expect("isolation: the refresh succeeded", refreshed.value("ok").toBool());
    expect("isolation: the refresh's lookups failed too", h.service.stateFailed(19));
}

/* The QML surface. RrPlacesFake in tests/ui/qml_test_context.h publishes exactly these properties, so the QML
 * fixture completeness check holds screens to what this service really has; change both together. */
void
test_qml_properties() {
    QStringList names;
    const QMetaObject& meta = RrPlaceService::staticMetaObject;
    for (int i = meta.propertyOffset(); i < meta.propertyCount(); ++i) {
        names.append(QString::fromLatin1(meta.property(i).name()));
    }
    expect("qml: the published properties",
           names == (QStringList{"updating", "progressText", "summaryText", "unplacedCount"}));
}

/* ------------------------------------------------------------------------- */
/* Teardown                                                                   */
/* ------------------------------------------------------------------------- */

void
test_destroy_with_a_request_in_flight() {
    clear_store();
    Harness h;
    {
        QMutexLocker lock(&h.fake.mutex);
        h.fake.holdMethod = QStringLiteral("getStateInfo");
    }
    h.service.resolveStates({19});
    expect("teardown: a lookup in flight", await_hold(h.fake));
    /* ~Harness destroys the service first; its client cancels the held request and joins. */
}

void
test_destructor_marks_its_teardown_stage() {
    TeardownCapture::lines().clear();
    delete new RrPlaceService(nullptr, nullptr);
    expect("teardown: no marker while markers are off", TeardownCapture::lines().isEmpty());
    {
        const TeardownCapture capture;
        delete new RrPlaceService(nullptr, nullptr);
        expect("teardown: ~RrPlaceService marks its stage",
               TeardownCapture::lines()
                   == QStringList({QStringLiteral("teardown: ~RrPlaceService started"),
                                   QStringLiteral("teardown: ~RrPlaceService took 7 ms")}));
    }
}

} // namespace

int
main(int argc, char** argv) {
    dsd_qt::applyQmlGcPolicy();
    QGuiApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("dsd-neo-test"));
    QCoreApplication::setApplicationName(
        QStringLiteral("dsd-neo-rr-place-service-%1").arg(QCoreApplication::applicationPid()));
    dsd_test_qt_isolate_paths();
    const QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir(dataDir).removeRecursively();

    test_resolve_names_saved_rows();
    test_load_resolves_at_most_eight_states();
    test_names_whatever_the_order();
    test_failure_recorded_per_state();
    test_failed_name_save_is_retried();
    test_sign_out_clears_the_cache();
    test_update_refreshes_sites();
    test_update_skips_moved_and_counts_unlisted();
    test_update_partial_success();
    test_update_failed_save();
    test_cancel_keeps_what_was_saved();
    test_cancel_from_a_row_signal();
    test_update_place_rows_and_edges();
    test_unplaced_count();
    test_failed_lookups_leave_the_import_screen_alone();
    test_qml_properties();
    test_destroy_with_a_request_in_flight();
    test_destructor_marks_its_teardown_stage();

    QDir(dataDir).removeRecursively();
    if (g_failures != 0) {
        DSD_FPRINTF(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    DSD_FPRINTF(stderr, "OK\n");
    return 0;
}
