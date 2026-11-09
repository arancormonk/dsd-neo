// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Unit tests: RadioReferenceModel, the QML-facing brain of the RadioReference
 * import, driven end to end with no network and no UI. The transport is faked
 * with the byte-exact fixtures under tests/fixtures/radioreference, keyed by the
 * method name and sid sniffed out of the request body, so the browse pipeline,
 * the preview and the import all run against real API responses.
 *
 * House style: no QSignalSpy and no QTRY_* — those live in Qt6::Test, which no
 * target here links and distributions package separately. Signals are counted
 * with a plain lambda, and the queued marshalling from the client's worker
 * thread is pumped with a deadline. */

#include <QByteArray>
#include <QChar>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QEvent>
#include <QEventLoop>
#include <QFile>
#include <QGuiApplication>
#include <QIODevice>
#include <QLatin1String>
#include <QList>
#include <QMap>
#include <QObject>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QSet>
#include <QSettings>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QUrl>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>
#include <QtGlobal>
#include <algorithm>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/runtime/radioreference.h>
#include <memory>
#include <stdio.h>
#include <stdlib.h>
#include <utility>
#include "../test_support/qt_test_paths.h"

#include "app_prefs.h"
#include "decoder_host.h"
#include "imported_files_model.h"
#include "qml_gc_policy.h"
#include "radio_reference_model.h"
#include "teardown_trace_capture.h"

#ifndef DSD_NEO_TEST_RR_FIXTURE_DIR
#error "DSD_NEO_TEST_RR_FIXTURE_DIR must be defined by the build"
#endif

namespace {

int g_failures = 0;

/* A value that could only have come from the password, so an assertion that it
 * is absent from a user-visible string is a real leak check. */
/* QLatin1String, not QString: its constructor is constexpr and cannot throw, so
 * these need no dynamic initialization. QString converts implicitly where a
 * setter wants one, and QString::contains() takes a QLatin1String directly. */
constexpr QLatin1String kPassword("SENTINEL_PW_9d3");
constexpr QLatin1String kUsername("SENTINEL_USER_4f1");
constexpr QLatin1String kAppKey("SENTINEL_KEY_7b2");

void
expect(const char* what, bool ok) {
    if (!ok) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", what);
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

void
expect_int(const char* what, int got, int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "FAIL: %s (got %d, want %d)\n", what, got, want);
        g_failures++;
    }
}

class TestHost : public dsd_qt::DecoderHost {
  public:
    qint64 requested = 0;
    qint64 cancelled = 0;

    bool
    locationSupported() const override {
        return true;
    }

    void
    requestCurrentLocation(qint64 id) override {
        requested = id;
    }

    void
    cancelLocationRequest(qint64 id) override {
        cancelled = id;
    }

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

struct Fake {
    int calls = 0;
    bool serveFault = false;
    /* Anything but DSD_RR_OK fails the transfer itself with this status, the
     * way a dropped connection does, before any payload is served. */
    dsd_rr_status failWith = DSD_RR_OK;
    QByteArray lastRequest;
    /* Rewrites the served payload, so a case can make the database change under
     * a refresh. Empty `from` leaves every fixture byte-exact. */
    QByteArray replaceFrom;
    QByteArray replaceTo;
    /* More rewrites of the same kind, each applied after the one above. */
    QList<QPair<QByteArray, QByteArray>> replacements;
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

/** @brief The SOAP method name, read out of the request envelope. */
QString
method_of(const QByteArray& body) {
    const int start = body.indexOf("<ns1:");
    if (start < 0) {
        return QString();
    }
    const int end = body.indexOf('>', start);
    if (end < 0) {
        return QString();
    }
    return QString::fromUtf8(body.mid(start + 5, end - start - 5));
}

/** @brief The `sid` part's value, or 0 when the message has none. */
int
sid_of(const QByteArray& body) {
    const int start = body.indexOf("<sid ");
    if (start < 0) {
        return 0;
    }
    const int open = body.indexOf('>', start);
    const int close = body.indexOf("</sid>", open);
    if (open < 0 || close < 0) {
        return 0;
    }
    return QString::fromUtf8(body.mid(open + 1, close - open - 1)).toInt();
}

/** @brief Which captured system a sid belongs to. */
QString
suffix_for_sid(int sid) {
    switch (sid) {
        case 6673: return QStringLiteral("p25");
        case 12574: return QStringLiteral("capplus");
        case 8697: return QStringLiteral("dmr_tier3");
        case 12918: return QStringLiteral("nxdn");
        case 220: return QStringLiteral("edacs");
        case 10880: return QStringLiteral("tetra");
        default: return QStringLiteral("dmr_conv");
    }
}

QString
fixture_for(const QString& method, int sid) {
    if (method == QLatin1String("getUserData")) {
        return QStringLiteral("user_data.xml");
    }
    if (method == QLatin1String("getZipcodeInfo")) {
        return QStringLiteral("zipcode_info.xml");
    }
    if (method == QLatin1String("getCountryList")) {
        return QStringLiteral("country_list.xml");
    }
    if (method == QLatin1String("getCountryInfo")) {
        return QStringLiteral("country_info.xml");
    }
    /* getStateInfo serves both the county list and the state-wide system list;
     * the response shape is what tells them apart, not the method. */
    if (method == QLatin1String("getStateInfo")) {
        return QStringLiteral("state_info.xml");
    }
    if (method == QLatin1String("getCountyInfo")) {
        return QStringLiteral("county_info.xml");
    }
    if (method == QLatin1String("getTrsType")) {
        return QStringLiteral("trs_types.xml");
    }
    if (method == QLatin1String("getTrsFlavor")) {
        return QStringLiteral("trs_flavors.xml");
    }
    if (method == QLatin1String("getTrsVoice")) {
        return QStringLiteral("trs_voices.xml");
    }
    if (method == QLatin1String("getTrsTalkgroupCats")) {
        return QStringLiteral("trs_talkgroup_cats_p25.xml");
    }

    const QString suffix = suffix_for_sid(sid);
    if (method == QLatin1String("getTrsDetails")) {
        return QStringLiteral("trs_details_") + suffix + QStringLiteral(".xml");
    }
    if (method == QLatin1String("getTrsSites")) {
        /* The two-repeater system is the ordinary conventional case; the 36-site
         * one is what forces truncation. */
        return sid == 12244 ? QStringLiteral("trs_sites_dmr_conv_small.xml")
                            : QStringLiteral("trs_sites_") + suffix + QStringLiteral(".xml");
    }
    if (method == QLatin1String("getTrsTalkgroups")) {
        return QStringLiteral("trs_talkgroups_") + suffix + QStringLiteral(".xml");
    }
    return QString();
}

int
fake_perform(void* ctx, const dsd_rr_request* req, dsd_rr_response* resp) {
    auto* fake = static_cast<Fake*>(ctx);
    DSD_MEMSET(resp, 0, sizeof(*resp));
    fake->calls++;
    fake->lastRequest = QByteArray(req->body, static_cast<int>(req->body_len));
    if (fake->failWith != DSD_RR_OK) {
        resp->status = fake->failWith;
        (void)DSD_SNPRINTF(resp->error, sizeof(resp->error), "%s", "connection refused");
        return -1;
    }

    const QString method = method_of(fake->lastRequest);
    QByteArray payload = read_fixture(fake->serveFault ? QStringLiteral("fault_auth.xml")
                                                       : fixture_for(method, sid_of(fake->lastRequest)));
    if (!fake->replaceFrom.isEmpty()) {
        payload.replace(fake->replaceFrom, fake->replaceTo);
    }
    for (const auto& edit : fake->replacements) {
        payload.replace(edit.first, edit.second);
    }
    if (payload.isEmpty()) {
        resp->status = DSD_RR_ERR_HTTP;
        (void)DSD_SNPRINTF(resp->error, sizeof(resp->error), "%s", "no fixture for this method");
        return -1;
    }

    /* A fault arrives as HTTP 500 with a text/xml body; classification comes from
     * the faultcode, never from the status. */
    resp->http_status = fake->serveFault ? 500 : 200;
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

/** @brief Run the event loop until the model goes idle or the deadline passes. */
void
pump(const dsd_qt::RadioReferenceModel& model, int timeoutMs = 15000) {
    QElapsedTimer timer;
    timer.start();
    while (model.busy() && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    if (model.busy()) {
        expect("model went idle before the deadline", false);
    }
}

/**
 * @brief A model wired to the fake transport with usable credentials.
 *
 * Declaration order is load-bearing: members are destroyed in reverse, and
 * ~RadioReferenceModel is what joins the client's worker thread. The transport
 * and its context therefore have to be declared BEFORE the model, or the worker
 * outlives the very thing it is calling.
 */
struct Harness {
    TestHost host;
    dsd_qt::AppPrefs prefs;
    dsd_qt::ImportedFilesModel library{&host};
    Fake fake;
    dsd_rr_transport transport{&fake_perform, &fake};
    dsd_qt::RadioReferenceModel model{&prefs, &library, &host};

    Harness() {
        prefs.setRrUsername(kUsername);
        prefs.setRrAppKey(kAppKey);
        model.setPassword(kPassword);
        model.setTransportForTests(&transport);
    }
};

/**
 * @brief Counts the completions the client's worker queues to the model.
 *
 * Every submitted request ends in exactly one queued call - a cancelled one
 * included - so this is how a case waits for a reply the model has already
 * stopped waiting for: busy() says nothing once a batch is retired, and the
 * point of those cases is what such a late reply does when it lands.
 */
class ReplyCounter : public QObject {
  public:
    explicit ReplyCounter(QObject* model) : m_model(model) { m_model->installEventFilter(this); }

    ~ReplyCounter() override { m_model->removeEventFilter(this); }

    ReplyCounter(const ReplyCounter&) = delete;
    ReplyCounter& operator=(const ReplyCounter&) = delete;

    int replies = 0;

    bool
    eventFilter(QObject* watched, QEvent* event) override {
        if (watched == m_model && event->type() == QEvent::MetaCall) {
            replies++;
        }
        return false;
    }

  private:
    QObject* m_model;
};

/** @brief Run the event loop until @p want replies have been delivered. */
void
await_replies(const ReplyCounter& counter, int want, int timeoutMs = 15000) {
    QElapsedTimer timer;
    timer.start();
    while (counter.replies < want && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }
    /* The filter sees the call before it runs; this lets it run. */
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    if (counter.replies < want) {
        expect("the late reply arrived before the deadline", false);
    }
}

/** @brief Whether any warning in a plan contains @p needle. */
bool
warned(const QVariantMap& plan, const QString& needle) {
    const QVariantList warnings = plan.value(QStringLiteral("warnings")).toList();
    return std::any_of(warnings.cbegin(), warnings.cend(),
                       [&needle](const QVariant& warning) { return warning.toString().contains(needle); });
}

/** @brief The bytes of a stored library file. */
QByteArray
read_stored(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        expect("stored file is readable", false);
        return QByteArray();
    }
    return file.readAll();
}

/** @brief The library row of @p type, or -1. */
int
row_of_type(const dsd_qt::ImportedFilesModel& library, const QString& type) {
    for (int row = 0; row < library.rowCount(); row++) {
        if (library.get(row).value(QStringLiteral("type")).toString() == type) {
            return row;
        }
    }
    return -1;
}

int
row_count(const QString& csv) {
    if (csv.isEmpty()) {
        return 0;
    }
    return static_cast<int>(csv.count(QLatin1Char('\n')));
}

/* ------------------------------------------------------------------------- */
/* Cases                                                                      */
/* ------------------------------------------------------------------------- */

void
test_credentials_gate(void) {
    TestHost host;
    dsd_qt::AppPrefs prefs;
    prefs.setRrUsername(QString());
    prefs.setRrAppKey(QString());
    dsd_qt::ImportedFilesModel library(&host);
    dsd_qt::RadioReferenceModel model(&prefs, &library, &host);

    /* Whether a fresh install already has a key is this build's answer, not a
     * constant: a keyed build ships one, a keyless build waits for the user to
     * supply it. Asserted against the build rather than assuming keyless, or
     * this case reads as a pass only in the configuration it happened to be
     * written in. */
    const char* builtin = dsd_rr_builtin_app_key();
    const bool baked = builtin != nullptr && builtin[0] != '\0';
    expect("a fresh install has a key only where one is baked in", model.hasAppKey() == baked);
    expect("credentials are never ready on a fresh install", !model.credentialsReady());

    prefs.setRrAppKey(kAppKey);
    expect("a key is in hand once one is stored", model.hasAppKey());
    expect("credentials still need a username", !model.credentialsReady());

    prefs.setRrUsername(kUsername);
    expect("credentials still need a password", !model.credentialsReady());

    model.setPassword(kPassword);
    expect("credentials complete", model.credentialsReady());

    model.clearPassword();
    expect("clearing the password re-gates", !model.credentialsReady());

    /* A request without credentials must fail loudly rather than reach the wire. */
    model.loadCountries();
    expect("no request runs without credentials", !model.busy());
    expect("missing credentials are reported", model.errorKind() != dsd_qt::RadioReferenceModel::NoError);
    expect("the error names no credential", !model.errorText().contains(kUsername));
}

void
test_browse_pipeline(void) {
    Harness h;

    int busyChanges = 0;
    QObject::connect(&h.model, &dsd_qt::RadioReferenceModel::busyChanged, &h.model,
                     [&busyChanges]() { busyChanges++; });

    h.model.checkAccount();
    expect("account check goes busy", h.model.busy());
    pump(h.model);
    expect("account check succeeds", h.model.errorKind() == dsd_qt::RadioReferenceModel::NoError);
    expect("busy toggled twice", busyChanges == 2);

    h.model.loadCountries();
    pump(h.model);
    expect_int("236 countries", h.model.countries().size(), 236);
    /* The list arrives alphabetically, so the US is found by coid, not position -
     * which is also how the screen defaults its country picker. */
    QString usName;
    for (const QVariant& row : h.model.countries()) {
        if (row.toMap().value(QStringLiteral("coid")).toInt() == 1) {
            usName = row.toMap().value(QStringLiteral("name")).toString();
        }
    }
    expect_str("coid 1 is the United States", usName, QStringLiteral("United States"));

    h.model.loadCountryStates(1);
    pump(h.model);
    expect_int("54 US states and territories", h.model.states().size(), 54);

    h.model.loadStateCounties(19);
    pump(h.model);
    expect_int("Iowa has 102 counties", h.model.counties().size(), 102);

    h.model.loadCountySystems(841);
    pump(h.model);
    expect_int("Linn County has 24 trunked systems", h.model.systems().size(), 24);

    /* A ZIP only resolves IDs, so the model chains straight into the county's
     * system list rather than making the user ask twice. */
    h.model.loadCountySystems(0);
    pump(h.model);
    const int before = h.fake.calls;
    h.model.lookupZip(QStringLiteral("52401"));
    pump(h.model);
    expect("a zip lookup costs two calls", h.fake.calls == before + 2);
    expect_int("a zip lookup ends on the county's systems", h.model.systems().size(), 24);

    /* Nothing user-visible may carry a credential. */
    expect("status text carries no password", !h.model.statusText().contains(kPassword));
    expect("status text carries no username", !h.model.statusText().contains(kUsername));
    expect("status text carries no app key", !h.model.statusText().contains(kAppKey));
}

void
test_trunked_system(void) {
    Harness h;
    h.model.loadSystem(6673);
    pump(h.model);

    expect("P25 system loaded", h.model.errorKind() == dsd_qt::RadioReferenceModel::NoError);
    expect("P25 is not conventional", !h.model.conventional());
    expect_int("35 sites", h.model.sites().size(), 35);

    const QVariantMap details = h.model.systemDetails();
    expect_str("system name", details.value(QStringLiteral("name")).toString(), QStringLiteral("SARA Network"));
    expect_str("resolved type", details.value(QStringLiteral("typeDescr")).toString(), QStringLiteral("Project 25"));
    expect_str("resolved flavor", details.value(QStringLiteral("flavorDescr")).toString(), QStringLiteral("Phase II"));
    expect("system is supported", details.value(QStringLiteral("supported")).toBool());
    expect_str("the protocol token names P25", details.value(QStringLiteral("protocolToken")).toString(),
               QStringLiteral("p25"));
    expect("no custom band plan in this capture", !details.value(QStringLiteral("hasCustomBandplan")).toBool());

    const QVariantMap firstSite = h.model.sites().at(0).toMap();
    /* siteNumber, never siteId: two database rows share RF site 1 here. */
    expect_int("first site number", firstSite.value(QStringLiteral("siteNumber")).toInt(), 1);
    expect("first site is simulcast", firstSite.value(QStringLiteral("simulcast")).toBool());
    expect_str("first site control frequency", firstSite.value(QStringLiteral("controlFreqMhz")).toString(),
               QStringLiteral("851.05"));

    const QVariantMap talkgroups = h.model.talkgroupSummary();
    expect_int("1793 talkgroups", talkgroups.value(QStringLiteral("count")).toInt(), 1793);
    expect_int("346 fully encrypted", talkgroups.value(QStringLiteral("encCount")).toInt(), 346);
    expect_int("16 partially encrypted", talkgroups.value(QStringLiteral("partialEncCount")).toInt(), 16);
    expect_int("64 categories", talkgroups.value(QStringLiteral("categories")).toList().size(), 64);
    expect("a talkgroup sample is offered", !talkgroups.value(QStringLiteral("sample")).toList().isEmpty());

    /* The simulcast default comes from the site record and stays overridable, and
     * -^ rides with every P25 map. */
    const QVariantMap plan = h.model.buildImportPlan(QVariantList{0}, QVariantMap());
    expect("plan is importable", plan.value(QStringLiteral("ok")).toBool());
    expect_str("simulcast is detected from the record", plan.value(QStringLiteral("decodeFlag")).toString(),
               QStringLiteral("-mq -^"));
    expect("plan is trunked", plan.value(QStringLiteral("trunking")).toBool());
    expect("plan is not a scan list", !plan.value(QStringLiteral("scanList")).toBool());
    expect_str("plan tunes the control channel", plan.value(QStringLiteral("freqMhz")).toString(),
               QStringLiteral("851.05"));
    expect_int("plan emits the 11-frequency hunt list", row_count(plan.value(QStringLiteral("chanCsvText")).toString()),
               12);
    const QString groupCsv = plan.value(QStringLiteral("groupCsvText")).toString();
    expect("plan opts into talkgroup categories",
           groupCsv.startsWith(QStringLiteral("DEC,Mode,Name,Category,(generated from RadioReference)\n")));
    expect("plan splices the fetched category onto its talkgroup",
           groupCsv.contains(QStringLiteral("\n52007,DE,RESERVOIR RANGRS,Coralville\n")));
    expect("plan warns the map column is a placeholder", warned(plan, QStringLiteral("placeholder")));
    expect("plan has no blocked reason", plan.value(QStringLiteral("blockedReason")).toString().isEmpty());

    QVariantMap options;
    options.insert(QStringLiteral("simulcast"), false);
    const QVariantMap plain = h.model.buildImportPlan(QVariantList{0}, options);
    expect_str("simulcast stays overridable", plain.value(QStringLiteral("decodeFlag")).toString(),
               QStringLiteral("-ft -^"));

    /* A trunked import takes one site and says so. */
    const QVariantMap many = h.model.buildImportPlan(QVariantList{0, 1, 2}, QVariantMap());
    expect_int("a trunked plan uses one site", many.value(QStringLiteral("siteCount")).toInt(), 1);
    expect("extra sites are reported", warned(many, QStringLiteral("only the first selected site")));

    /* Selecting nothing is a blocked plan, not a crash. */
    const QVariantMap empty = h.model.buildImportPlan(QVariantList(), QVariantMap());
    expect("an empty selection is blocked", !empty.value(QStringLiteral("ok")).toBool());
    expect("an empty selection explains itself", !empty.value(QStringLiteral("blockedReason")).toString().isEmpty());
}

void
test_conventional_system(void) {
    Harness h;
    /* 36 single-frequency repeaters, 33 of them distinct. */
    h.model.loadSystem(9340);
    pump(h.model);

    expect("conventional system loaded", h.model.errorKind() == dsd_qt::RadioReferenceModel::NoError);
    expect("the system is conventional", h.model.conventional());
    expect_int("36 repeaters", h.model.sites().size(), 36);

    const QVariantMap repeater = h.model.sites().at(0).toMap();
    /* A repeater list is unreadable without the frequency and colour code; the
     * colour code is display-only, because DMR decodes it off air. */
    expect_str("repeater frequency", repeater.value(QStringLiteral("freqMhz")).toString(), QStringLiteral("146.755"));
    expect_str("repeater description", repeater.value(QStringLiteral("descr")).toString(), QStringLiteral("Waukee"));
    expect("a repeater has no control frequency",
           repeater.value(QStringLiteral("controlFreqMhz")).toString().isEmpty());

    /* Two repeaters produce the scanner list; -Y comes with it. */
    const QVariantMap pair = h.model.buildImportPlan(QVariantList{0, 1}, QVariantMap());
    expect("a two-repeater plan is importable", pair.value(QStringLiteral("ok")).toBool());
    expect("a two-repeater plan is a scan list", pair.value(QStringLiteral("scanList")).toBool());
    expect("a conventional plan is not trunked", !pair.value(QStringLiteral("trunking")).toBool());
    expect_str("two repeaters take -fs -Y", pair.value(QStringLiteral("decodeFlag")).toString(),
               QStringLiteral("-fs -Y"));
    expect_int("two repeaters make a two-row list", row_count(pair.value(QStringLiteral("chanCsvText")).toString()), 3);
    expect_str("the session starts on the first repeater", pair.value(QStringLiteral("freqMhz")).toString(),
               QStringLiteral("146.755"));
    expect("the scan source requirement is surfaced", warned(pair, QStringLiteral("RTL-SDR")));

    /* One repeater is a feature, not a missing file: a one-entry scan list would
     * retune to the frequency it is already on at every hangtime. */
    const QVariantMap single = h.model.buildImportPlan(QVariantList{0}, QVariantMap());
    expect("a one-repeater plan is importable", single.value(QStringLiteral("ok")).toBool());
    expect("a one-repeater plan is not a scan list", !single.value(QStringLiteral("scanList")).toBool());
    expect("a one-repeater plan writes no channel map",
           single.value(QStringLiteral("chanCsvText")).toString().isEmpty());
    expect_str("one repeater takes plain -fs", single.value(QStringLiteral("decodeFlag")).toString(),
               QStringLiteral("-fs"));

    /* No ceiling: every distinct repeater frequency lands in the map however
     * long the list grows — pre-2026 builds truncated at 26 rows. These 30
     * repeaters carry 27 distinct frequencies. */
    QVariantList many;
    for (int i = 0; i < 30; i++) {
        many.append(i);
    }
    const QVariantMap uncapped = h.model.buildImportPlan(many, QVariantMap());
    expect_int("all 27 distinct frequencies are emitted",
               row_count(uncapped.value(QStringLiteral("chanCsvText")).toString()), 28);
    expect("no truncation warning", !warned(uncapped, QStringLiteral("scan limit")));
    expect("shared repeater frequencies are reported", warned(uncapped, QStringLiteral("share a frequency")));
}

void
test_per_site_import() {
    Harness h;
    h.model.loadSystem(6673);
    pump(h.model);
    const auto plan = h.model.buildImportPlan({0, 1}, {{"eachSite", true}});
    expect("multi site plan valid", plan.value("ok").toBool());
    const auto result = h.model.performImport(plan, "SARA", -1);
    const auto rows = result.value("rows").toList();
    expect("multi site import succeeds", result.value("ok").toBool());
    const int beforeRollback = h.library.rowCount();
    auto broken = plan;
    auto plans = broken.value("plans").toList();
    if (plans.size() > 1) {
        plans[1] = QVariantMap{{"ok", false}};
        broken.insert("plans", plans);
        expect("batch failure reported", !h.model.performImport(broken, "Failed batch", -1).value("ok").toBool());
        expect_int("batch failure retires adopted files", h.library.rowCount(), beforeRollback);
    }
    expect("invalid batch selection rejected",
           !h.model.buildImportPlan({-1, 0}, {{"eachSite", true}}).value("ok").toBool());
    expect_int("multi site import returns N rows", rows.size(), 2);
    if (rows.size() == 2) {
        expect_int("database id not RF number", rows[0].toMap().value("rrSiteId").toInt(), 16863);
        expect_int("second database id", rows[1].toMap().value("rrSiteId").toInt(), 23581);
        expect("first site coordinates", rows[0].toMap().value("hasSitePos").toBool());
        expect("exact RR coordinates retained",
               qAbs(rows[0].toMap().value("siteLat").toDouble() - 41.65503) < 1e-8
                   && qAbs(rows[0].toMap().value("siteLon").toDouble() + 91.60244) < 1e-8);
        expect("second site has position", rows[1].toMap().value("hasSitePos").toBool());
        const auto sentinel = h.model.buildImportPlan({h.model.sites().size() - 1}, {});
        expect("sentinel site lacks position", !sentinel.value("hasSitePos").toBool());
        expect("site-specific files", rows[0].toMap().value("chanCsvPath") != rows[1].toMap().value("chanCsvPath"));
    }
}

void
test_site_import_without_coverage() {
    for (const auto& replacement : {QByteArray(), QByteArray("<range xsi:nil=\"true\"/>")}) {
        Harness h;
        h.fake.replaceFrom = "<range xsi:type=\"xsd:decimal\">20</range>";
        h.fake.replaceTo = replacement;
        h.model.loadSystem(6673);
        pump(h.model);
        const auto plan = h.model.buildImportPlan({0}, {});
        const auto imported = h.model.performImport(plan, "Site without coverage", -1);
        expect("missing coverage still imports", imported.value("ok").toBool());
        expect("missing coverage preserves position", imported.value("hasSitePos").toBool());
        expect("missing coverage preserves exact coordinates",
               qAbs(imported.value("siteLat").toDouble() - 41.65503) < 1e-8
                   && qAbs(imported.value("siteLon").toDouble() + 91.60244) < 1e-8);
    }
}

void
test_mixed_simulcast_qml_plan() {
    Harness h;
    h.model.loadSystem(6673);
    pump(h.model);
    const auto sites = h.model.sites();
    int ordinary = -1, simulcast = -1;
    for (int i = 0; i < sites.size(); ++i) {
        if (sites[i].toMap().value("simulcast").toBool()) {
            simulcast = i;
        } else {
            ordinary = i;
        }
    }
    expect("fixture has ordinary and simulcast sites", ordinary >= 0 && simulcast >= 0);
    QQmlEngine engine;
    engine.rootContext()->setContextProperty("radioReference", &h.model);
    engine.rootContext()->setContextProperty("prefs", &h.prefs);
    engine.rootContext()->setContextProperty("decoderHost", &h.host);
    QQmlComponent component(&engine, QUrl::fromLocalFile(QStringLiteral(DSD_QML_UI_DIR "/RadioReferenceScreen.qml")));
    std::unique_ptr<QObject> screen(component.create());
    expect("production RadioReference screen loads", screen != nullptr);
    if (!screen) {
        return;
    }
    screen->setProperty("eachSite", true);
    for (const QVariantList& selection : {QVariantList{ordinary, simulcast}, QVariantList{simulcast, ordinary}}) {
        screen->setProperty("selectedSites", selection);
        QMetaObject::invokeMethod(screen.get(), "refreshPlan");
        const auto plan = screen->property("plan").toMap();
        const auto plans = plan.value("plans").toList();
        expect("mixed per-site QML plan valid", plan.value("ok").toBool() && plans.size() == 2);
        for (int i = 0; i < plans.size(); ++i) {
            const auto flag = plans[i].toMap().value("decodeFlag").toString();
            const bool expected = sites[selection[i].toInt()].toMap().value("simulcast").toBool();
            expect("QML batch preserves each site's recorded modulation",
                   flag.contains("-mq") == expected && flag.contains("-ft") != expected);
        }
    }
}

void
test_import_lands_in_the_library(void) {
    QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).removeRecursively();

    Harness h;
    h.model.loadSystem(6673);
    pump(h.model);

    const QVariantMap plan = h.model.buildImportPlan(QVariantList{0}, QVariantMap());
    expect("plan built", plan.value(QStringLiteral("ok")).toBool());

    const QVariantMap result = h.model.performImport(plan, QStringLiteral("SARA Network"), -1);
    expect("import ok", result.value(QStringLiteral("ok")).toBool());
    expect_int("single import site identity", result.value("rrSiteId").toInt(), 16863);
    expect("import returns a channel map path", !result.value(QStringLiteral("chanCsvPath")).toString().isEmpty());
    expect("import returns a talkgroup path", !result.value(QStringLiteral("groupCsvPath")).toString().isEmpty());
    expect_str("import carries the decode flag", result.value(QStringLiteral("decodeFlag")).toString(),
               QStringLiteral("-mq -^"));
    expect_str("import carries the start frequency", result.value(QStringLiteral("freqMhz")).toString(),
               QStringLiteral("851.05"));
    expect("import carries the trunking flag", result.value(QStringLiteral("trunking")).toBool());

    expect_int("two library rows", h.library.rowCount(), 2);
    bool sawChan = false;
    bool sawGroup = false;
    for (int row = 0; row < h.library.rowCount(); row++) {
        const QVariantMap entry = h.library.get(row);
        expect_str("provenance origin", entry.value(QStringLiteral("origin")).toString(),
                   QStringLiteral("radioreference"));
        expect_int("provenance sid", entry.value(QStringLiteral("rrSid")).toInt(), 6673);
        expect_str("provenance site ids", entry.value(QStringLiteral("rrSiteIds")).toString(), QStringLiteral("16863"));
        expect("every imported row loaded rows", entry.value(QStringLiteral("accepted")).toInt() > 0);
        expect_str("provenance kind matches the type", entry.value(QStringLiteral("rrKind")).toString(),
                   entry.value(QStringLiteral("type")).toString());
        sawChan = sawChan || entry.value(QStringLiteral("type")).toString() == QLatin1String("chan");
        sawGroup = sawGroup || entry.value(QStringLiteral("type")).toString() == QLatin1String("group");
    }
    expect("a channel map was imported", sawChan);
    expect("a talkgroup list was imported", sawGroup);

    /* Nothing may be left in staging: on Android that root is persistent
     * storage, not cache. */
    const QDir staging(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                       + QStringLiteral("/rr_staging"));
    expect("staging is empty after an import",
           !staging.exists() || staging.entryList(QDir::Files | QDir::NoDotAndDotDot).isEmpty());

    /* A blocked plan must not write anything. */
    QVariantMap blocked;
    blocked.insert(QStringLiteral("ok"), false);
    expect("a blocked plan imports nothing",
           !h.model.performImport(blocked, QStringLiteral("Nope"), -1).value(QStringLiteral("ok")).toBool());
    expect_int("a blocked plan adds no row", h.library.rowCount(), 2);
}

void
test_refresh_replaces_a_row_in_place(void) {
    QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).removeRecursively();

    Harness h;

    /* Two repeaters, because a conventional selection is exactly what a
     * single-site provenance key could not carry: a refresh driven by one would
     * shrink the scan list to one row. */
    h.model.loadSystem(12244);
    pump(h.model);
    const QVariantMap plan = h.model.buildImportPlan(QVariantList{0, 1}, QVariantMap());
    expect("a two-repeater plan built", plan.value(QStringLiteral("ok")).toBool());
    expect_str("the plan records the whole selection", plan.value(QStringLiteral("siteIds")).toString(),
               QStringLiteral("42099,42100"));

    expect("import ok",
           h.model.performImport(plan, QStringLiteral("Linn County REC"), -1).value(QStringLiteral("ok")).toBool());

    const int chanRow = row_of_type(h.library, QStringLiteral("chan"));
    expect("a channel map row exists", chanRow >= 0);
    if (chanRow < 0) {
        return;
    }
    const QVariantMap before = h.library.get(chanRow);
    const QString path = before.value(QStringLiteral("path")).toString();
    /* siteId, not siteNumber: the numbers are what a user recognises but they
     * repeat within a system, so only these identify a site — and the whole
     * selection is recorded, not just the first. */
    expect_str("provenance records the site ids", before.value(QStringLiteral("rrSiteIds")).toString(),
               QStringLiteral("42099,42100"));
    const QByteArray original = read_stored(path);
    expect("the stored map carries the second repeater", original.contains("464525000"));

    int finishedRow = -2;
    QVariantMap finished;
    QObject::connect(&h.model, &dsd_qt::RadioReferenceModel::refreshFinished, &h.model,
                     [&finishedRow, &finished](int row, const QVariantMap& result) {
                         finishedRow = row;
                         finished = result;
                     });

    /* RadioReference moved one of the repeaters. */
    h.fake.replaceFrom = QByteArrayLiteral("464.525");
    h.fake.replaceTo = QByteArrayLiteral("464.550");
    expect("the refresh starts", h.model.refreshRow(chanRow));
    pump(h.model);

    expect_int("the refresh reported the row it was given", finishedRow, chanRow);
    expect("the refresh succeeded", finished.value(QStringLiteral("ok")).toBool());
    /* The path is the row's identity: every saved system referencing it would
     * break if a refresh moved the file. */
    expect_str("the stored path is unchanged", h.library.get(chanRow).value(QStringLiteral("path")).toString(), path);

    const QByteArray refreshed = read_stored(path);
    expect("the refreshed map follows the database", refreshed.contains("464550000"));
    expect("the moved repeater is gone from the file", !refreshed.contains("464525000"));
    /* Both repeaters survive: matching by site id is what reproduces the
     * original selection, and a lost one would silently shorten the scan list. */
    expect("the refreshed map still carries the first repeater", refreshed.contains("451275000"));
    expect_int("the refreshed map has the same row count", row_count(QString::fromUtf8(refreshed)),
               row_count(QString::fromUtf8(original)));

    /* The destructive-refresh guard: a fetch that fails must leave the stored
     * copy byte-identical, or a fault page would destroy working local data. */
    h.fake.replaceFrom.clear();
    h.fake.serveFault = true;
    finishedRow = -2;
    finished.clear();
    expect("a refresh against a failing server still starts", h.model.refreshRow(chanRow));
    pump(h.model);
    expect_int("the failed refresh reported the row", finishedRow, chanRow);
    expect("the failed refresh reports failure", !finished.value(QStringLiteral("ok")).toBool());
    expect("a failed refresh leaves the stored file byte-identical", read_stored(path) == refreshed);
    expect_int("a failed refresh adds no row", h.library.rowCount(), 2);
    h.fake.serveFault = false;

    /* A picked file has no provenance, so there is nothing to re-fetch. */
    const int groupRow = row_of_type(h.library, QStringLiteral("group"));
    expect("a talkgroup row exists", groupRow >= 0);
    expect("a row outside the library cannot be refreshed", !h.model.refreshRow(99));
}

/**
 * @brief The library row is identified by its path, not by the index it sat at.
 *
 * A refresh waits on four network calls and the imports library stays
 * interactive throughout. Removing any earlier row shifts every later index
 * down, so completing on the index the tap recorded would rewrite a different
 * row's stored file in place - and refreshGeneratedFile() preserves the path, so
 * every saved system pointing at it would silently load the wrong system.
 */
void
test_refresh_survives_a_row_removed_mid_flight(void) {
    QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).removeRecursively();

    Harness h;
    h.model.loadSystem(12244);
    pump(h.model);
    const QVariantMap plan = h.model.buildImportPlan(QVariantList{0, 1}, QVariantMap());
    expect("a plan built", plan.value(QStringLiteral("ok")).toBool());
    expect("import ok",
           h.model.performImport(plan, QStringLiteral("Linn County REC"), -1).value(QStringLiteral("ok")).toBool());

    const int chanRow = row_of_type(h.library, QStringLiteral("chan"));
    const int groupRow = row_of_type(h.library, QStringLiteral("group"));
    expect("both rows exist", chanRow >= 0 && groupRow >= 0 && chanRow != groupRow);
    if (chanRow < 0 || groupRow < 0) {
        return;
    }
    const QString chanPath = h.library.get(chanRow).value(QStringLiteral("path")).toString();

    QVariantMap finished;
    QObject::connect(&h.model, &dsd_qt::RadioReferenceModel::refreshFinished, &h.model,
                     [&finished](int, const QVariantMap& result) { finished = result; });

    h.fake.replaceFrom = QByteArrayLiteral("464.525");
    h.fake.replaceTo = QByteArrayLiteral("464.550");
    expect("the refresh starts", h.model.refreshRow(chanRow));
    /* The user deletes another row while the fetch is still on the wire. */
    h.library.remove(groupRow);
    pump(h.model);

    expect("the refresh still succeeded", finished.value(QStringLiteral("ok")).toBool());
    const int movedRow = row_of_type(h.library, QStringLiteral("chan"));
    expect("the channel map is still in the library", movedRow >= 0);
    if (movedRow < 0) {
        return;
    }
    expect_str("the channel map kept its path", h.library.get(movedRow).value(QStringLiteral("path")).toString(),
               chanPath);
    expect("the refreshed content landed in the right file", read_stored(chanPath).contains("464550000"));

    /* Removing the row being refreshed leaves nothing to write to, and that has
     * to be reported rather than written over whatever now sits at that index. */
    finished.clear();
    h.fake.replaceFrom.clear();
    expect("a second refresh starts", h.model.refreshRow(movedRow));
    h.library.remove(movedRow);
    pump(h.model);
    expect("a refresh of a removed row reports failure", !finished.value(QStringLiteral("ok")).toBool());
    expect_int("nothing was written back", h.library.rowCount(), 0);
}

/**
 * @brief siteNumber is not unique within a system, so a refresh cannot use it.
 *
 * The captured SARA network numbers its 35 sites 1, 1, 10, 10, 10, 10, 10, 20,
 * ... - picking "the site numbered 1" would be a coin flip between two towers on
 * different frequencies. This is what the live emulator pass surfaced.
 */
void
test_site_numbers_are_ambiguous(void) {
    Harness h;
    h.model.loadSystem(6673);
    pump(h.model);

    const QVariantList sites = h.model.sites();
    expect("the P25 capture has many sites", sites.size() > 2);

    QSet<int> numbers;
    int duplicate_numbers = 0;
    for (const QVariant& row : sites) {
        const int number = row.toMap().value(QStringLiteral("siteNumber")).toInt();
        if (numbers.contains(number)) {
            duplicate_numbers++;
        }
        numbers.insert(number);
    }
    expect("site numbers repeat within this system", duplicate_numbers > 0);

    /* Two sites that share a number must still be told apart by what the
     * provenance records, or a refresh regenerates from the wrong tower. The
     * number itself survives only as a display field on sites(); the plan
     * deliberately no longer carries it. */
    expect("the first two sites share a number",
           sites.at(0).toMap().value(QStringLiteral("siteNumber")).toInt()
               == sites.at(1).toMap().value(QStringLiteral("siteNumber")).toInt());
    const QVariantMap first = h.model.buildImportPlan(QVariantList{0}, QVariantMap());
    const QVariantMap second = h.model.buildImportPlan(QVariantList{1}, QVariantMap());
    expect("but their recorded ids differ",
           first.value(QStringLiteral("siteIds")).toString() != second.value(QStringLiteral("siteIds")).toString());
    expect("and so do the channel maps they generate", first.value(QStringLiteral("chanCsvText")).toString()
                                                           != second.value(QStringLiteral("chanCsvText")).toString());
}

/**
 * @brief A refresh regenerates from the tower the import used, not the first one
 *        sharing its RF site number.
 *
 * Sites 0 and 1 of the captured SARA network are both numbered 1 but are
 * different towers on different frequencies. Importing site 1 and refreshing it
 * against an unchanged database must reproduce the file byte for byte; matching
 * on the number would find site 0 first and silently rewrite it with that
 * tower's map.
 */
void
test_refresh_uses_the_recorded_tower(void) {
    QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).removeRecursively();

    Harness h;
    h.model.loadSystem(6673);
    pump(h.model);

    const QVariantList sites = h.model.sites();
    expect("the capture has two sites sharing a number",
           sites.size() > 1
               && sites.at(0).toMap().value(QStringLiteral("siteNumber")).toInt()
                      == sites.at(1).toMap().value(QStringLiteral("siteNumber")).toInt());

    const QVariantMap plan = h.model.buildImportPlan(QVariantList{1}, QVariantMap());
    expect("a plan for the second tower built", plan.value(QStringLiteral("ok")).toBool());
    expect("import ok",
           h.model.performImport(plan, QStringLiteral("SARA Network"), -1).value(QStringLiteral("ok")).toBool());

    const int chanRow = row_of_type(h.library, QStringLiteral("chan"));
    expect("a channel map row exists", chanRow >= 0);
    if (chanRow < 0) {
        return;
    }
    const QString path = h.library.get(chanRow).value(QStringLiteral("path")).toString();
    const QByteArray before = read_stored(path);

    /* The two towers must actually differ, or the assertion below proves nothing. */
    expect(
        "the other tower's map is different",
        plan.value(QStringLiteral("chanCsvText")).toString()
            != h.model.buildImportPlan(QVariantList{0}, QVariantMap()).value(QStringLiteral("chanCsvText")).toString());

    QVariantMap finished;
    QObject::connect(&h.model, &dsd_qt::RadioReferenceModel::refreshFinished, &h.model,
                     [&finished](int, const QVariantMap& result) { finished = result; });
    expect("the refresh starts", h.model.refreshRow(chanRow));
    pump(h.model);

    expect("the refresh succeeded", finished.value(QStringLiteral("ok")).toBool());
    /* Unchanged database, unchanged tower: byte-identical. */
    expect("the refresh regenerated from the same tower", read_stored(path) == before);
}

/**
 * @brief Regression: a refresh whose talkgroups all lack a usable ID keeps the file.
 *
 * Every tgDec is pushed past UINT32_MAX. The old parser wrapped each one into a
 * different 32-bit ID and the refresh replaced the stored list with them; now
 * the writer skips them all, and the refresh keeps the stored copy and says why.
 */
void
test_refresh_keeps_a_group_list_when_no_id_is_usable(void) {
    QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).removeRecursively();

    Harness h;
    h.model.loadSystem(6673);
    pump(h.model);
    const QVariantMap plan = h.model.buildImportPlan(QVariantList{0}, QVariantMap());
    expect("no-id: a plan built", plan.value(QStringLiteral("ok")).toBool());
    expect("no-id: import ok",
           h.model.performImport(plan, QStringLiteral("SARA Network"), -1).value(QStringLiteral("ok")).toBool());
    const int groupRow = row_of_type(h.library, QStringLiteral("group"));
    expect("no-id: a talkgroup row exists", groupRow >= 0);
    if (groupRow < 0) {
        return;
    }
    const QString path = h.library.get(groupRow).value(QStringLiteral("path")).toString();
    const QByteArray before = read_stored(path);

    QVariantMap finished;
    QObject::connect(&h.model, &dsd_qt::RadioReferenceModel::refreshFinished, &h.model,
                     [&finished](int, const QVariantMap& result) { finished = result; });
    h.fake.replaceFrom = QByteArrayLiteral("<tgDec xsi:type=\"xsd:int\">");
    h.fake.replaceTo = QByteArrayLiteral("<tgDec xsi:type=\"xsd:int\">99999999999");
    expect("no-id: the refresh starts", h.model.refreshRow(groupRow));
    pump(h.model);

    expect("no-id: the refresh reports failure", !finished.value(QStringLiteral("ok")).toBool());
    expect_str("no-id: the reason is given", h.model.errorText(),
               QStringLiteral("None of the talkgroups RadioReference lists has an ID dsd-neo can use, so the stored "
                              "copy was kept."));
    expect("no-id: the stored list is byte-identical", read_stored(path) == before);
}

/**
 * @brief A TETRA system's group-only refresh succeeds (new feature).
 *
 * TETRA has no channel map, so its only file is the talkgroup list, and the
 * refresh regenerates it through the TETRA-aware writer.
 */
void
test_refresh_of_a_tetra_group_list(void) {
    QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).removeRecursively();

    Harness h;
    h.model.loadSystem(10880);
    pump(h.model);
    const QVariantMap plan = h.model.buildImportPlan(QVariantList{0}, QVariantMap());
    expect("tetra: a plan built", plan.value(QStringLiteral("ok")).toBool());
    expect("tetra: no channel map", plan.value(QStringLiteral("chanCsvText")).toString().isEmpty());
    expect("tetra: a group list",
           plan.value(QStringLiteral("groupCsvText")).toString().contains(QStringLiteral("\n1017000,A,Tow 1017000")));
    expect("tetra: import ok",
           h.model.performImport(plan, QStringLiteral("Kordia KorKor"), -1).value(QStringLiteral("ok")).toBool());
    expect_int("tetra: one row, the group list", h.library.rowCount(), 1);
    const int groupRow = row_of_type(h.library, QStringLiteral("group"));
    expect("tetra: the row is a talkgroup list", groupRow >= 0);
    if (groupRow < 0) {
        return;
    }
    const QString path = h.library.get(groupRow).value(QStringLiteral("path")).toString();

    QVariantMap finished;
    QObject::connect(&h.model, &dsd_qt::RadioReferenceModel::refreshFinished, &h.model,
                     [&finished](int, const QVariantMap& result) { finished = result; });
    h.fake.replaceFrom = QByteArrayLiteral("Tow 1017000");
    h.fake.replaceTo = QByteArrayLiteral("Tow Renamed");
    expect("tetra: the refresh starts", h.model.refreshRow(groupRow));
    pump(h.model);

    expect("tetra: the refresh succeeded", finished.value(QStringLiteral("ok")).toBool());
    const QByteArray refreshed = read_stored(path);
    expect("tetra: the refreshed list follows the database", refreshed.contains("\n1017000,A,Tow Renamed"));
    expect("tetra: the old name is gone", !refreshed.contains("Tow 1017000"));
}

/** @brief The talkgroup sample row whose ID is @p dec, or an empty map. */
QVariantMap
sample_row(const QVariantMap& summary, qulonglong dec) {
    for (const QVariant& value : summary.value(QStringLiteral("sample")).toList()) {
        const QVariantMap row = value.toMap();
        if (row.value(QStringLiteral("dec")).toULongLong() == dec) {
            return row;
        }
    }
    return QVariantMap();
}

/** @brief Whether the talkgroup sample shows a row named @p name. */
bool
sample_names(const QVariantMap& summary, const QString& name) {
    const QVariantList sample = summary.value(QStringLiteral("sample")).toList();
    return std::any_of(sample.cbegin(), sample.cend(), [&name](const QVariant& value) {
        return value.toMap().value(QStringLiteral("name")).toString() == name;
    });
}

/**
 * @brief A TETRA system names itself to QML by its protocol token (new feature).
 */
void
test_tetra_system_token(void) {
    Harness h;
    h.model.loadSystem(10880);
    pump(h.model);
    expect("tetra token: loaded", h.model.errorKind() == dsd_qt::RadioReferenceModel::NoError);
    expect_str("tetra token: the protocol token names TETRA",
               h.model.systemDetails().value(QStringLiteral("protocolToken")).toString(), QStringLiteral("tetra"));
}

/**
 * @brief The talkgroup preview counts and shows only the talkgroups the group file holds.
 *
 * Regression: the preview's count, sample and encryption counts read every row
 * RadioReference lists, while the writer skips a row with no usable ID and, for
 * TETRA, an ID above the 24-bit ceiling (0xFFFFFF). Such rows showed in the
 * preview but never reached the file, and so did a repeated ID, which the
 * writer drops after its first row. Four TETRA rows are rewritten: one to
 * 0x1000000 and fully encrypted (skipped), one to 0xFFFFFF and partially
 * encrypted (kept: the ceiling is inclusive), one past any 32-bit ID and fully
 * encrypted (no usable ID, skipped), and a later one to 0xFFFFFF again, fully
 * encrypted (a duplicate, dropped).
 */
void
test_talkgroup_preview_matches_the_file(void) {
    Harness h;
    const QByteArray encTail =
        QByteArrayLiteral("</tgAlpha><tgMode xsi:type=\"xsd:string\">T</tgMode><enc xsi:type=\"xsd:int\">");
    h.fake.replacements = {
        {QByteArrayLiteral(">1017000</tgDec>"), QByteArrayLiteral(">16777216</tgDec>")},
        {"Tow 1017000" + encTail + '0', "Tow 1017000" + encTail + '2'},
        {QByteArrayLiteral(">1025000</tgDec>"), QByteArrayLiteral(">16777215</tgDec>")},
        {"Bin 1025000" + encTail + '0', "Bin 1025000" + encTail + '1'},
        {QByteArrayLiteral(">1025001</tgDec>"), QByteArrayLiteral(">99999999999</tgDec>")},
        {"Trucking 1025001" + encTail + '0', "Trucking 1025001" + encTail + '2'},
        {QByteArrayLiteral(">1033000</tgDec>"), QByteArrayLiteral(">16777215</tgDec>")},
        {"Mobility Taxi" + encTail + '0', "Mobility Taxi" + encTail + '2'},
    };
    h.model.loadSystem(10880);
    pump(h.model);

    const QVariantMap summary = h.model.talkgroupSummary();
    expect_int("preview: the two skipped rows and the duplicate are not counted",
               summary.value(QStringLiteral("count")).toInt(), 13);
    expect_int("preview: no skipped or duplicate encrypted row is counted",
               summary.value(QStringLiteral("encCount")).toInt(), 0);
    expect_int("preview: the kept partially encrypted row is counted",
               summary.value(QStringLiteral("partialEncCount")).toInt(), 1);
    expect("preview: an ID above 0xFFFFFF is not shown",
           sample_row(summary, 16777216ULL).isEmpty() && !sample_names(summary, QStringLiteral("Tow 1017000")));
    expect("preview: 0xFFFFFF is shown",
           sample_row(summary, 16777215ULL).value(QStringLiteral("name")).toString() == QStringLiteral("Bin 1025000"));
    expect("preview: a row with no usable ID is not shown", !sample_names(summary, QStringLiteral("Trucking 1025001")));
    expect("preview: a repeated ID is not shown again", !sample_names(summary, QStringLiteral("Mobility Taxi")));

    /* The file agrees with the preview. */
    const QString groupCsv =
        h.model.buildImportPlan(QVariantList{0}, QVariantMap()).value(QStringLiteral("groupCsvText")).toString();
    expect("preview: the file holds 0xFFFFFF", groupCsv.contains(QStringLiteral("\n16777215,")));
    expect("preview: the file skips the same rows", !groupCsv.contains(QStringLiteral("16777216"))
                                                        && !groupCsv.contains(QStringLiteral("Trucking 1025001"))
                                                        && !groupCsv.contains(QStringLiteral("Mobility Taxi")));
    /* One header line, then one line per talkgroup the preview counts. */
    expect_int("preview: the file holds as many rows as the preview counts", row_count(groupCsv),
               summary.value(QStringLiteral("count")).toInt() + 1);
}

/**
 * @brief A trunked site with no marked control channel is still importable.
 *
 * Capacity Plus has no fixed control channel and RadioReference leaves `use` nil
 * on every one of its frequencies, so the control-channel lookup returns 0.
 * Blocking on that made the import dead for the whole Capacity Plus family,
 * which the live emulator pass caught.
 */
void
test_site_without_a_marked_control_channel(void) {
    Harness h;
    h.model.loadSystem(12574);
    pump(h.model);

    const QVariantList sites = h.model.sites();
    expect("the Capacity Plus capture has a site", !sites.isEmpty());
    expect_str("the site marks no control channel",
               sites.first().toMap().value(QStringLiteral("controlFreqMhz")).toString(), QString());

    const QVariantMap plan = h.model.buildImportPlan(QVariantList{0}, QVariantMap());
    expect("the plan is importable anyway", plan.value(QStringLiteral("ok")).toBool());
    expect("it starts on the site's first frequency", !plan.value(QStringLiteral("freqMhz")).toString().isEmpty());
    expect("and says the frequency is a fallback", warned(plan, QStringLiteral("no control channel")));
    /* Capacity Plus expands each RadioReference LCN into the LSN pair 2n-1, 2n. */
    expect_int("four frequencies become eight LSN rows",
               row_count(plan.value(QStringLiteral("chanCsvText")).toString()), 9 /* header + 8 */);
}

void
test_error_and_cancel(void) {
    Harness h;
    h.fake.serveFault = true;

    h.model.checkAccount();
    pump(h.model);
    /* The fault arrives as HTTP 500; the classification is the faultcode's. */
    expect_int("an auth fault maps to the auth kind", h.model.errorKind(), dsd_qt::RadioReferenceModel::AuthError);
    expect("errorIsAuth is set", h.model.errorIsAuth());
    expect("errorIsSubscription is not", !h.model.errorIsSubscription());
    expect("the server's text is shown", !h.model.errorText().isEmpty());
    expect("the error text carries no password", !h.model.errorText().contains(kPassword));
    expect("the error text carries no app key", !h.model.errorText().contains(kAppKey));
    expect("the status text carries no password", !h.model.statusText().contains(kPassword));

    /* A later success must clear the error rather than leave a stale banner. */
    h.fake.serveFault = false;
    h.model.loadCountries();
    pump(h.model);
    expect_int("a success clears the error", h.model.errorKind(), dsd_qt::RadioReferenceModel::NoError);

    /* Cancel drops the batch and goes idle; a reply that lands afterwards is
     * retired by the generation and must not overwrite fresh state. */
    const int countriesBefore = h.model.countries().size();
    h.model.loadSystem(6673);
    h.model.cancel();
    expect("cancel goes idle at once", !h.model.busy());
    pump(h.model);
    expect_int("a cancelled load leaves the lists alone", h.model.countries().size(), countriesBefore);
    expect("a cancelled load leaves no sites", h.model.sites().isEmpty());
}

void
test_results_and_loaded_system_stay_exclusive(void) {
    Harness h;

    /* buildHasAppKey reports the baked key and only the baked key: the harness
     * stores a prefs override, which must not count. */
    const char* builtin = dsd_rr_builtin_app_key();
    const bool baked = builtin != nullptr && builtin[0] != '\0';
    expect("buildHasAppKey answers for the baked key alone", h.model.buildHasAppKey() == baked);

    /* Which key a request carries. Asserted through chooseAppKey() rather than
     * only through the model, because the baked value is a compile-time
     * constant: a test that could reach this only via hasAppKey()/fillAuth()
     * would exercise whichever single branch this build was configured for, and
     * the branch that matters most - a shipped key refusing to be displaced by
     * a stale override - is the one a keyless developer build can never take. */
    const QByteArray bakedKey("BAKED_KEY_9f4");
    const QString stored = QStringLiteral("OVERRIDE_KEY_1a2");
    expect("a baked key is authoritative",
           dsd_qt::RadioReferenceModel::chooseAppKey(bakedKey.constData(), stored) == bakedKey);
    expect("a baked key stands alone when nothing is stored",
           dsd_qt::RadioReferenceModel::chooseAppKey(bakedKey.constData(), QString()) == bakedKey);
    expect("without a baked key the stored key is the key",
           dsd_qt::RadioReferenceModel::chooseAppKey("", stored) == stored.toUtf8());
    expect("a NULL builtin counts as no baked key",
           dsd_qt::RadioReferenceModel::chooseAppKey(nullptr, stored) == stored.toUtf8());
    expect("neither candidate leaves no key to send",
           dsd_qt::RadioReferenceModel::chooseAppKey("", QString()).isEmpty());

    /* And that the model asks the same question the same way. */
    h.prefs.setRrAppKey(QString());
    expect("a keyed build needs no stored key; a keyless one has none yet", h.model.hasAppKey() == baked);
    h.prefs.setRrAppKey(stored);
    expect("a stored key completes a keyless build", h.model.hasAppKey());

    h.model.loadSystem(6673);
    pump(h.model);
    expect("a system is loaded", !h.model.systemDetails().isEmpty());

    /* The choice is not merely reported but sent. This assertion is meaningful
     * in both configurations, which is what makes the CI step that reconfigures
     * this build with a baked key worth its seconds: there it flips to proving
     * the shipped key goes on the wire and the stale override does not. */
    expect("the envelope carries the chosen application key",
           h.fake.lastRequest.contains(dsd_qt::RadioReferenceModel::chooseAppKey(builtin, stored)));
    if (baked) {
        expect("the envelope does not carry the ignored override", !h.fake.lastRequest.contains(stored.toUtf8()));
    }

    /* A fresh search must land on a visible results stage: the screen shows the
     * list only while no system is loaded, so the reply retires the system. */
    h.model.loadCountySystems(841);
    pump(h.model);
    expect_int("the county's systems arrived", h.model.systems().size(), 24);
    expect("the results retired the loaded system", h.model.systemDetails().isEmpty());
    expect("the results retired the site list", h.model.sites().isEmpty());

    /* closeSystem() is the screen's back affordance: the system goes, the
     * results stay, and a stale error goes with the system it described. */
    h.model.loadSystem(6673);
    pump(h.model);
    int systemChanges = 0;
    QObject::connect(&h.model, &dsd_qt::RadioReferenceModel::systemChanged, &h.model,
                     [&systemChanges]() { systemChanges++; });
    h.model.closeSystem();
    expect("closing the system clears its details", h.model.systemDetails().isEmpty());
    expect("closing the system clears its sites", h.model.sites().isEmpty());
    expect("closing the system clears its talkgroups", h.model.talkgroupSummary().isEmpty());
    expect("closing the system keeps the results list", h.model.systems().size() == 24);
    expect("closing the system announced itself", systemChanges > 0);

    /* And with an error up: back means the failure is acknowledged. */
    h.fake.serveFault = true;
    h.model.loadSystem(6673);
    pump(h.model);
    expect("the fault landed as an error", h.model.errorKind() != dsd_qt::RadioReferenceModel::NoError);
    h.fake.serveFault = false;
    h.model.closeSystem();
    expect_int("closing the system retires the error", h.model.errorKind(), dsd_qt::RadioReferenceModel::NoError);
    expect("closing the system clears the error text", h.model.errorText().isEmpty());

    /* And mid-load, which is what reset() does when the screen is reopened over
     * a fetch that is still running. The replies carry the live generation, so
     * without retiring the batch they land afterwards and finishSystemLoad()
     * rebuilds the system around the sid clearSystem() just zeroed - details and
     * sites fully populated with sid 0, which the screen's stage predicate
     * (sid > 0) can never show. */
    h.model.closeSystem();
    h.model.loadSystem(6673);
    h.model.closeSystem();
    pump(h.model);
    expect("a system closed mid-load stays closed", h.model.systemDetails().isEmpty());
    expect("a system closed mid-load leaves no sites", h.model.sites().isEmpty());
    expect("a system closed mid-load leaves no talkgroups", h.model.talkgroupSummary().isEmpty());
    expect("a system closed mid-load leaves the model idle", !h.model.busy());
    /* And the screen can still open it again afterwards. */
    h.model.loadSystem(6673);
    pump(h.model);
    expect_int("reopening after a mid-load close works", h.model.systemDetails().value(QStringLiteral("sid")).toInt(),
               6673);
}

void
test_nearby() {
    const struct {
        const char* postal;
        const char* country;
        const char* zip;
    } cases[] = {{"52401", "US", "52401"}, {"00501", "US", "00501"}, {"52401-1234", "US", ""}, {"1234", "US", ""},
                 {"123456", "US", ""},     {"ABCDE", "US", ""},      {"12345", "CA", ""},      {"", "US", ""}};

    for (const auto& c : cases) {
        expect("postal accepts only US five digits", dsd_qt::rr_zip_from_postal(c.postal, c.country) == c.zip);
    }
    Harness h;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    h.model.lookupNearby();
    expect("location is busy", h.model.busy());
    Q_EMIT h.host.locationResult(h.host.requested, true, 41, -91, 150, now, true, "52401", "US", "");
    pump(h.model);
    expect("nearby resolves county systems", h.model.systems().size() == 24);
    expect("fix persisted with accuracy", h.prefs.lastFixAt() == now && h.prefs.lastAccuracyM() == 150);
    h.model.lookupNearby();
    Q_EMIT h.host.locationResult(h.host.requested, true, 42, -92, 200, now, false, "", "", "Geocoding unavailable");
    expect("geocode failure retains fix", h.prefs.lastLat() == 42 && !h.model.busy());
    expect("geocode error visible", !h.model.errorText().isEmpty());
    h.model.lookupNearby();
    Q_EMIT h.host.locationResult(h.host.requested, false, 0, 0, 0, 0, false, "", "", "Location permission denied");
    expect("denial settles without replacing fix", !h.model.busy() && h.prefs.lastLat() == 42);
    // #782: a permission prompt Android closed unanswered is a cancel, not a failure: it settles with nothing shown.
    h.model.lookupNearby();
    const qint64 interrupted = h.host.requested;
    Q_EMIT h.host.locationCancelled(interrupted + 1);
    expect("another request's cancel is ignored", h.model.busy());
    Q_EMIT h.host.locationCancelled(interrupted);
    expect("platform cancel settles quietly", !h.model.busy() && h.model.errorText().isEmpty()
                                                  && h.model.statusText().isEmpty() && h.prefs.lastLat() == 42);
    Q_EMIT h.host.locationResult(interrupted, true, 46, -96, 100, now, true, "52401", "US", "");
    expect("a result after the cancel is ignored", h.prefs.lastLat() == 42 && !h.model.busy());
    h.model.lookupNearby();
    const qint64 old = h.host.requested;
    h.model.loadCountries();
    pump(h.model);
    const int calls = h.fake.calls;
    Q_EMIT h.host.locationResult(old, true, 43, -93, 100, now, true, "52401", "US", "");
    pump(h.model);
    expect("new browse cancels location", h.host.cancelled == old);
    expect("late fix cannot replace browse or prefs", h.fake.calls == calls && h.prefs.lastLat() == 42);
    h.model.lookupNearby();
    const qint64 beforeSearch = h.host.requested;
    h.model.lookupZip("00501");
    pump(h.model);
    const int afterSearch = h.fake.calls;
    Q_EMIT h.host.locationResult(beforeSearch, true, 43, -93, 100, now, true, "52401", "US", "");
    pump(h.model);
    expect("late location cannot replace newer ZIP search", h.fake.calls == afterSearch && h.prefs.lastLat() == 42);
    h.model.lookupNearby();
    Q_EMIT h.host.locationResult(h.host.requested, true, 44, -94, 100, now, true, "K1A0B1", "CA", "");
    expect("non-US result retains fix and directs Browse",
           h.prefs.lastLat() == 44
               && h.model.errorText() == "RadioReference looks up US ZIP codes only; use Browse for CA.");
    h.model.lookupNearby();
    const qint64 first = h.host.requested;
    h.model.lookupNearby();
    expect("replacement has distinct id and cancels old", first != h.host.requested && h.host.cancelled == first);
    h.model.cancel();
    expect("explicit cancel settles", !h.model.busy() && h.host.cancelled == h.host.requested);
    h.model.lookupNearby();
    const qint64 dismissed = h.host.requested;
    const qint64 chooser = h.model.nextLocationRequestId();
    expect("chooser ownership retires nearby caller", !h.model.busy() && h.host.cancelled == dismissed);
    expect("shared allocator advances for chooser", chooser > dismissed);
    Q_EMIT h.host.locationResult(dismissed, true, 45, -95, 100, now, true, "52401", "US", "");
    expect("retired nearby result ignored", h.prefs.lastLat() == 44 && !h.model.busy());
    h.model.lookupNearby();
    expect("reopened nearby obtains a newer id", h.host.requested > chooser && h.model.busy());
    h.model.cancel();
    Harness recreated;
    recreated.model.lookupNearby();
    expect("requester recreation preserves allocation", recreated.host.requested > h.host.requested);
}

/* ------------------------------------------------------------------------- */
/* Account check                                                              */
/* ------------------------------------------------------------------------- */

/** @brief What a verified sink saw. */
struct SinkLog {
    int calls = 0;
    int stateAtCall = -1;
    QString username;
    QString password;
};

void
install_sink(Harness& h, SinkLog* log) {
    dsd_qt::RadioReferenceModel* model = &h.model;
    h.model.setVerifiedSink([log, model](const QString& username, const QString& password) {
        log->calls++;
        log->stateAtCall = model->accountState();
        /* Deep copies: the model scrubs its own once this returns. */
        log->username = QString(username.constData(), username.size());
        log->password = QString(password.constData(), password.size());
    });
}

/** @brief One way to change a credential while a check is in flight. */
struct CredentialEdit {
    const char* name;
    void (*apply)(Harness& h);
};

/** @brief "<edit>: <what>", for an assertion repeated once per edit. */
QByteArray
edit_label(const CredentialEdit& edit, const char* what) {
    return QByteArray(edit.name) + ": " + what;
}

/** @brief Every kind of credential edit, built per call: no static initializer that could throw. */
QList<CredentialEdit>
credential_edits() {
    return {
        {"username", [](Harness& h) { h.prefs.setRrUsername(QStringLiteral("someone_else")); }},
        {"password", [](Harness& h) { h.model.setPassword(QStringLiteral("another_password")); }},
        {"password cleared", [](Harness& h) { h.model.clearPassword(); }},
        {"application key", [](Harness& h) { h.prefs.setRrAppKey(QStringLiteral("another_key")); }},
    };
}

/* B6: a check answers for the credentials it was sent with. Once they change, its
 * verdict describes nothing on screen, and "Account verified." over credentials
 * nobody checked is exactly what a password manager would then offer to save. */
void
test_edit_retires_the_check_in_flight(void) {
    for (const CredentialEdit& edit : credential_edits()) {
        Harness h;
        const ReplyCounter counter(&h.model);
        int credentialSignals = 0;
        QObject::connect(&h.model, &dsd_qt::RadioReferenceModel::credentialsChanged, &h.model,
                         [&credentialSignals]() { credentialSignals++; });

        h.model.checkAccount();
        edit.apply(h);
        expect(edit_label(edit, "the edit retires the check at once").constData(), !h.model.busy());
        expect_int(edit_label(edit, "credentialsChanged fires once").constData(), credentialSignals, 1);
        await_replies(counter, 1);
        expect_str(edit_label(edit, "the late success is not shown").constData(), h.model.statusText(), QString());
        expect_int(edit_label(edit, "and leaves no error").constData(), h.model.errorKind(),
                   dsd_qt::RadioReferenceModel::NoError);
        expect_int(edit_label(edit, "or any verdict").constData(), h.model.accountState(),
                   dsd_qt::RadioReferenceModel::AccountNone);
    }
}

/* B6: a refusal of the old credentials must not mark the new ones as refused -
 * errorIsAuth is what reopens the sign-in card. */
void
test_edit_drops_a_late_auth_failure(void) {
    for (const CredentialEdit& edit : credential_edits()) {
        Harness h;
        const ReplyCounter counter(&h.model);
        h.fake.serveFault = true;

        h.model.checkAccount();
        edit.apply(h);
        await_replies(counter, 1);
        expect(edit_label(edit, "a late refusal never sets errorIsAuth").constData(), !h.model.errorIsAuth());
        expect_int(edit_label(edit, "or any error").constData(), h.model.errorKind(),
                   dsd_qt::RadioReferenceModel::NoError);
    }
}

/* Plan 2.1: the same for any other fetch. An edit leaves a list fetch running, and
 * one that leaves the credentials incomplete starts no check to retire it, so its
 * refusal of the old credentials still lands - and must not blame the new ones. */
void
test_edit_drops_a_late_auth_failure_of_any_fetch(void) {
    for (const CredentialEdit& edit : credential_edits()) {
        Harness h;
        const ReplyCounter counter(&h.model);
        h.fake.serveFault = true;

        h.model.loadCountries();
        edit.apply(h);
        await_replies(counter, 1);
        expect(edit_label(edit, "a late list refusal never sets errorIsAuth").constData(), !h.model.errorIsAuth());
        expect_int(edit_label(edit, "or any error").constData(), h.model.errorKind(),
                   dsd_qt::RadioReferenceModel::NoError);
        expect(edit_label(edit, "and the batch settles").constData(), !h.model.busy());
        expect_int(edit_label(edit, "or gives a verdict").constData(), h.model.accountState(),
                   dsd_qt::RadioReferenceModel::AccountNone);
    }
}

/* A refusal by any fetch is a verdict on the current credentials: after "Account
 * verified.", a search RadioReference refuses (the password changed on the
 * website) must not leave the form saying verified next to a notice saying the
 * password was not accepted. Other failures say nothing about the account. */
void
test_any_refusal_updates_the_verdict(void) {
    using M = dsd_qt::RadioReferenceModel;
    {
        Harness h;
        SinkLog log;
        install_sink(h, &log);
        h.model.checkAccount();
        pump(h.model);
        expect_int("the check verifies", h.model.accountState(), M::AccountVerified);
        h.fake.serveFault = true;
        h.model.loadCountries();
        pump(h.model);
        expect_int("a refused browse makes it Rejected", h.model.accountState(), M::AccountRejected);
        expect("with errorIsAuth as before", h.model.errorIsAuth());
        expect_int("and calls no sink", log.calls, 1);
    }
    {
        Harness h;
        h.model.checkAccount();
        pump(h.model);
        h.fake.failWith = DSD_RR_ERR_NETWORK;
        h.model.loadCountries();
        pump(h.model);
        expect_int("a network failure keeps Verified", h.model.accountState(), M::AccountVerified);
        h.fake.failWith = DSD_RR_ERR_HTTP;
        h.model.loadCountries();
        pump(h.model);
        expect_int("and so does a server failure", h.model.accountState(), M::AccountVerified);
    }
    {
        Harness h;
        h.fake.serveFault = true;
        h.model.loadCountries();
        pump(h.model);
        expect_int("a refused browse with no check yet is Rejected too", h.model.accountState(), M::AccountRejected);
        h.fake.serveFault = false;
        h.model.checkAccount();
        pump(h.model);
        expect_int("and a check then verifies", h.model.accountState(), M::AccountVerified);
    }
}

/* B7: every field's editingFinished and the Check button all ask, so the same
 * check is requested several times in a row. The second must not cancel the
 * first and start the wait over. */
void
test_check_is_deduplicated_while_in_flight(void) {
    Harness h;
    const ReplyCounter counter(&h.model);

    h.model.checkAccount();
    h.model.checkAccount();
    pump(h.model);
    expect_int("a repeated check sends one request", h.fake.calls, 1);
    expect_int("a repeated check gets one reply", counter.replies, 1);
    expect_str("the one check verifies", h.model.statusText(), QStringLiteral("Account verified."));

    /* Finished, so asking again is a real second check. */
    h.model.checkAccount();
    pump(h.model);
    expect_int("a check after a finished one is sent", h.fake.calls, 2);
    expect_str("and verifies again", h.model.statusText(), QStringLiteral("Account verified."));
}

void
test_account_state_follows_the_check(void) {
    using M = dsd_qt::RadioReferenceModel;
    Harness h;
    int stateSignals = 0;
    QObject::connect(&h.model, &M::accountStateChanged, &h.model, [&stateSignals]() { stateSignals++; });

    expect_int("no verdict before a check", h.model.accountState(), M::AccountNone);
    expect("not checking before a check", !h.model.checkingAccount());

    h.model.checkAccount();
    expect_int("a started check is Checking", h.model.accountState(), M::AccountChecking);
    expect("checkingAccount follows", h.model.checkingAccount());
    pump(h.model);
    expect_int("a success is Verified", h.model.accountState(), M::AccountVerified);
    expect("checkingAccount clears", !h.model.checkingAccount());
    expect_str("Verified carries no message", h.model.accountMessage(), QString());
    expect_int("one signal per transition", stateSignals, 2);

    /* A verdict is about the credentials it checked; other actions keep it. */
    h.model.loadCountries();
    pump(h.model);
    expect_int("a browse keeps the verdict", h.model.accountState(), M::AccountVerified);

    h.prefs.setRrUsername(QStringLiteral("someone_else"));
    expect_int("an edit retires the verdict", h.model.accountState(), M::AccountNone);
}

void
test_account_state_for_each_failure(void) {
    using M = dsd_qt::RadioReferenceModel;
    {
        Harness h;
        h.fake.serveFault = true;
        h.model.checkAccount();
        pump(h.model);
        expect_int("an auth fault is Rejected", h.model.accountState(), M::AccountRejected);
        expect_str("Rejected carries no message", h.model.accountMessage(), QString());
        expect("errorIsAuth as before", h.model.errorIsAuth());
    }
    {
        Harness h;
        /* RadioReference answers a lapsed subscription with a past date, not a fault. */
        h.fake.replaceFrom = QByteArrayLiteral("11-24-2026");
        h.fake.replaceTo = QByteArrayLiteral("01-01-2000");
        SinkLog log;
        install_sink(h, &log);
        h.model.checkAccount();
        pump(h.model);
        expect_int("a lapsed subscription is Expired", h.model.accountState(), M::AccountExpired);
        expect_str("Expired carries no message", h.model.accountMessage(), QString());
        expect("errorIsSubscription as before", h.model.errorIsSubscription());
        expect_int("an Expired check reaches no sink", log.calls, 0);
    }
    {
        Harness h;
        h.fake.failWith = DSD_RR_ERR_NETWORK;
        SinkLog log;
        install_sink(h, &log);
        h.model.checkAccount();
        pump(h.model);
        expect_int("a network failure is Failed", h.model.accountState(), M::AccountFailed);
        expect_int("a failed check reaches no sink", log.calls, 0);
        expect_int("errorKind as before", h.model.errorKind(), M::NetworkError);
        expect("Failed says why", !h.model.accountMessage().isEmpty());
        expect_str("in the error's own words", h.model.accountMessage(), h.model.errorText());
    }
    {
        Harness h;
        h.model.clearPassword();
        h.model.checkAccount();
        expect_int("a check that cannot start is Failed", h.model.accountState(), M::AccountFailed);
        expect_str("with the reason", h.model.accountMessage(), h.model.errorText());
        expect("and nothing in flight", !h.model.busy());
    }
}

void
test_account_state_when_the_check_is_retired(void) {
    using M = dsd_qt::RadioReferenceModel;
    {
        Harness h;
        const ReplyCounter counter(&h.model);
        h.model.checkAccount();
        h.model.cancel();
        expect_int("a cancelled check has no verdict", h.model.accountState(), M::AccountNone);
        expect_str("cancel still says so", h.model.statusText(), QStringLiteral("Cancelled."));
        await_replies(counter, 1);
        expect_int("and its late reply gives none", h.model.accountState(), M::AccountNone);
    }
    {
        Harness h;
        h.model.checkAccount();
        h.model.loadCountries();
        expect_int("another action retires the check", h.model.accountState(), M::AccountNone);
        pump(h.model);
        expect_int("its reply gives no verdict", h.model.accountState(), M::AccountNone);
        expect_int("the other action completes", h.model.countries().size(), 236);
    }
    {
        Harness h;
        const ReplyCounter counter(&h.model);
        h.model.checkAccount();
        h.model.setPassword(QStringLiteral("another_password"));
        expect_int("an edit retires the check", h.model.accountState(), M::AccountNone);
        await_replies(counter, 1);
        expect_int("its late reply gives no verdict", h.model.accountState(), M::AccountNone);
    }
}

/* A finished verdict is about the credentials, and only an edit changes them:
 * cancelling some other work, or leaving a system mid-load, must not blank the
 * account line Settings shows. */
void
test_finished_verdict_survives_other_work(void) {
    using M = dsd_qt::RadioReferenceModel;
    {
        Harness h;
        h.model.checkAccount();
        pump(h.model);
        h.model.loadCountries();
        h.model.cancel();
        expect_int("Verified survives cancel()", h.model.accountState(), M::AccountVerified);
        h.model.loadSystem(6673);
        h.model.closeSystem();
        expect_int("Verified survives closeSystem() mid-load", h.model.accountState(), M::AccountVerified);
        h.model.cancel();
        expect_int("and a cancel with nothing in flight", h.model.accountState(), M::AccountVerified);
    }
    {
        Harness h;
        h.fake.serveFault = true;
        h.model.checkAccount();
        pump(h.model);
        h.fake.serveFault = false;
        h.model.loadCountries();
        pump(h.model);
        expect_int("Rejected survives a later browse", h.model.accountState(), M::AccountRejected);
        h.model.loadCountries();
        h.model.cancel();
        expect_int("and a cancel", h.model.accountState(), M::AccountRejected);
    }
    {
        Harness h;
        h.fake.failWith = DSD_RR_ERR_HTTP;
        h.model.checkAccount();
        pump(h.model);
        const QString message = h.model.accountMessage();
        h.fake.failWith = DSD_RR_OK;
        h.model.loadSystem(6673);
        h.model.closeSystem();
        expect_int("Failed survives closeSystem() mid-load", h.model.accountState(), M::AccountFailed);
        expect_str("with its message", h.model.accountMessage(), message);
    }
}

/* The verdict lands before busy settles, so a handler reacting to busy going
 * false never finds the check still Checking. */
void
test_verdict_is_set_before_busy_settles(void) {
    using M = dsd_qt::RadioReferenceModel;
    for (const bool fault : {false, true}) {
        Harness h;
        h.fake.serveFault = fault;
        QList<int> statesAtIdle;
        QObject::connect(&h.model, &M::busyChanged, &h.model, [&h, &statesAtIdle]() {
            if (!h.model.busy()) {
                statesAtIdle.append(h.model.accountState());
            }
        });
        h.model.checkAccount();
        pump(h.model);
        expect_int(fault ? "a refusal settles busy once" : "a success settles busy once", statesAtIdle.size(), 1);
        expect_int(fault ? "Rejected already when busy goes false" : "Verified already when busy goes false",
                   statesAtIdle.value(0, -1), fault ? M::AccountRejected : M::AccountVerified);
    }
}

/* A credential edit retires only the account check: a list fetch says nothing
 * about the account and must still land. */
void
test_edit_leaves_other_fetches_alone(void) {
    for (const CredentialEdit& edit : credential_edits()) {
        Harness h;
        h.model.loadCountries();
        edit.apply(h);
        expect(edit_label(edit, "a list fetch survives the edit").constData(), h.model.busy());
        pump(h.model);
        expect_int(edit_label(edit, "and lands").constData(), h.model.countries().size(), 236);
        expect(edit_label(edit, "uncancelled").constData(), h.model.statusText() != QStringLiteral("Cancelled."));
    }
}

void
test_has_password(void) {
    Harness h;
    int credentialSignals = 0;
    QObject::connect(&h.model, &dsd_qt::RadioReferenceModel::credentialsChanged, &h.model,
                     [&credentialSignals]() { credentialSignals++; });

    expect("a held password is reported", h.model.hasPassword());
    h.model.setPassword(kPassword);
    expect_int("setting the same password is no change", credentialSignals, 0);
    h.model.clearPassword();
    expect("a cleared one is not", !h.model.hasPassword());
    expect_int("clearing signals once", credentialSignals, 1);
    h.model.clearPassword();
    expect_int("clearing nothing is no change", credentialSignals, 1);
    h.model.setPassword(QStringLiteral("another_password"));
    expect("a new one is", h.model.hasPassword());
    expect_int("setting signals once", credentialSignals, 2);
}

void
test_verified_sink_gets_the_checked_credentials(void) {
    Harness h;
    SinkLog log;
    install_sink(h, &log);

    h.model.checkAccount();
    /* Behind AppPrefs' back, so nothing retires the check: the sink must still
     * report what was sent, not what the settings hold when the reply lands. */
    QSettings(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("dsd-neo"), QStringLiteral("dsd-neo-app"))
        .setValue(QStringLiteral("rr/username"), QStringLiteral("changed_behind_the_check"));
    expect_str("the settings really changed", h.prefs.rrUsername(), QStringLiteral("changed_behind_the_check"));
    pump(h.model);
    expect_int("the sink is called once", log.calls, 1);
    expect_int("after the state is Verified", log.stateAtCall, dsd_qt::RadioReferenceModel::AccountVerified);
    expect_str("with the username that was checked", log.username, kUsername);
    expect_str("and the password that was checked", log.password, kPassword);

    h.model.setVerifiedSink(nullptr);
    h.model.checkAccount();
    pump(h.model);
    expect_int("a cleared sink is not called", log.calls, 1);
    expect_str("and a check still verifies without one", h.model.statusText(), QStringLiteral("Account verified."));
}

void
test_verified_sink_skips_failures_and_stale_checks(void) {
    {
        Harness h;
        SinkLog log;
        install_sink(h, &log);
        h.fake.serveFault = true;
        h.model.checkAccount();
        pump(h.model);
        expect_int("a refused check reaches no sink", log.calls, 0);
    }
    for (const CredentialEdit& edit : credential_edits()) {
        Harness h;
        SinkLog log;
        install_sink(h, &log);
        const ReplyCounter counter(&h.model);
        h.model.checkAccount();
        edit.apply(h);
        await_replies(counter, 1);
        expect_int(edit_label(edit, "a retired check reaches no sink").constData(), log.calls, 0);
    }
}

void
test_destroy_with_requests_in_flight(void) {
    /* The client is destroyed first in ~RadioReferenceModel, which joins the
     * worker before any member it might read goes away. Under ASan this case is
     * what catches a use-after-free or a leaked result. */
    auto* h = new Harness;
    h->model.loadSystem(6673);
    delete h;
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    expect("destroying mid-flight is survivable", true);
}

/* #782: the Android app's teardown times this destructor, which joins the client's worker. */
void
test_destructor_marks_its_teardown_stage(void) {
    TeardownCapture::lines().clear();
    delete new Harness;
    expect("no teardown marker while markers are off", TeardownCapture::lines().isEmpty());
    {
        const TeardownCapture capture;
        auto* h = new Harness;
        h->model.loadSystem(6673);
        delete h;
        // The harness's library brings its own import worker (#832), which marks its own teardown stage too.
        expect("~RadioReferenceModel marks its teardown stage",
               TeardownCapture::lines().filter(QStringLiteral("~RadioReferenceModel"))
                   == QStringList({QStringLiteral("teardown: ~RadioReferenceModel started"),
                                   QStringLiteral("teardown: ~RadioReferenceModel took 7 ms")}));
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
}

} // namespace

int
main(int argc, char** argv) {
    dsd_qt::applyQmlGcPolicy();
    QGuiApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("dsd-neo-test"));
    QCoreApplication::setApplicationName(
        QStringLiteral("dsd-neo-radio-reference-%1").arg(QCoreApplication::applicationPid()));
    dsd_test_qt_isolate_paths();

    /* AppPrefs hardcodes its own QSettings scope, so redirecting the standard
     * paths alone would still write a real profile. */
    QTemporaryDir settingsDir;
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());

    const QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir(dataDir).removeRecursively();

    test_nearby();
    test_credentials_gate();
    test_browse_pipeline();
    test_trunked_system();
    test_conventional_system();
    test_import_lands_in_the_library();
    test_per_site_import();
    test_site_import_without_coverage();
    test_mixed_simulcast_qml_plan();
    test_refresh_replaces_a_row_in_place();
    test_refresh_survives_a_row_removed_mid_flight();
    test_site_numbers_are_ambiguous();
    test_refresh_uses_the_recorded_tower();
    test_refresh_keeps_a_group_list_when_no_id_is_usable();
    test_refresh_of_a_tetra_group_list();
    test_tetra_system_token();
    test_talkgroup_preview_matches_the_file();
    test_site_without_a_marked_control_channel();
    test_error_and_cancel();
    test_results_and_loaded_system_stay_exclusive();
    test_edit_retires_the_check_in_flight();
    test_edit_drops_a_late_auth_failure();
    test_edit_drops_a_late_auth_failure_of_any_fetch();
    test_any_refusal_updates_the_verdict();
    test_check_is_deduplicated_while_in_flight();
    test_account_state_follows_the_check();
    test_account_state_for_each_failure();
    test_account_state_when_the_check_is_retired();
    test_finished_verdict_survives_other_work();
    test_verdict_is_set_before_busy_settles();
    test_edit_leaves_other_fetches_alone();
    test_has_password();
    test_verified_sink_gets_the_checked_credentials();
    test_verified_sink_skips_failures_and_stale_checks();
    test_destroy_with_requests_in_flight();
    test_destructor_marks_its_teardown_stage();

    QDir(dataDir).removeRecursively();
    if (g_failures != 0) {
        DSD_FPRINTF(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    DSD_FPRINTF(stderr, "OK\n");
    return 0;
}
