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
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QIODevice>
#include <QLatin1String>
#include <QList>
#include <QMap>
#include <QObject>
#include <QSet>
#include <QSettings>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/protocol/nxdn/nxdn_lfsr.h>
#include <dsd-neo/runtime/radioreference.h>
#include <stdio.h>
#include <stdlib.h>

#include "app_prefs.h"
#include "decoder_host.h"
#include "imported_files_model.h"
#include "radio_reference_model.h"

#ifndef DSD_NEO_TEST_RR_FIXTURE_DIR
#error "DSD_NEO_TEST_RR_FIXTURE_DIR must be defined by the build"
#endif

void
LFSRN(const char* BufferIn, char* BufferOut, dsd_state* state) {
    (void)BufferIn;
    (void)BufferOut;
    (void)state;
}

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
    QByteArray lastRequest;
    /* Rewrites the served payload, so a case can make the database change under
     * a refresh. Empty `from` leaves every fixture byte-exact. */
    QByteArray replaceFrom;
    QByteArray replaceTo;
};

QByteArray
read_fixture(const QString& leaf) {
    QFile file(QStringLiteral(DSD_NEO_TEST_RR_FIXTURE_DIR "/") + leaf);
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

    const QString method = method_of(fake->lastRequest);
    QByteArray payload = read_fixture(fake->serveFault ? QStringLiteral("fault_auth.xml")
                                                       : fixture_for(method, sid_of(fake->lastRequest)));
    if (!fake->replaceFrom.isEmpty()) {
        payload.replace(fake->replaceFrom, fake->replaceTo);
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
pump(dsd_qt::RadioReferenceModel& model, int timeoutMs = 15000) {
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

/** @brief Whether any warning in a plan contains @p needle. */
bool
warned(const QVariantMap& plan, const QString& needle) {
    const QVariantList warnings = plan.value(QStringLiteral("warnings")).toList();
    for (const QVariant& warning : warnings) {
        if (warning.toString().contains(needle)) {
            return true;
        }
    }
    return false;
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
    expect("plan emits a talkgroup file", !plan.value(QStringLiteral("groupCsvText")).toString().isEmpty());
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

    /* Past 26 the generator truncates, and the user has to be told. */
    QVariantList many;
    for (int i = 0; i < 30; i++) {
        many.append(i);
    }
    const QVariantMap capped = h.model.buildImportPlan(many, QVariantMap());
    expect_int("the scan list caps at 26 rows", row_count(capped.value(QStringLiteral("chanCsvText")).toString()), 27);
    expect("truncation is reported", warned(capped, QStringLiteral("scan limit")));
    expect("shared repeater frequencies are reported", warned(capped, QStringLiteral("share a frequency")));
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

} // namespace

int
main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("dsd-neo-test"));
    QCoreApplication::setApplicationName(
        QStringLiteral("dsd-neo-radio-reference-%1").arg(QCoreApplication::applicationPid()));
    QStandardPaths::setTestModeEnabled(true);

    /* AppPrefs hardcodes its own QSettings scope, so redirecting the standard
     * paths alone would still write a real profile. */
    QTemporaryDir settingsDir;
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());

    const QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir(dataDir).removeRecursively();

    test_credentials_gate();
    test_browse_pipeline();
    test_trunked_system();
    test_conventional_system();
    test_import_lands_in_the_library();
    test_refresh_replaces_a_row_in_place();
    test_refresh_survives_a_row_removed_mid_flight();
    test_site_numbers_are_ambiguous();
    test_refresh_uses_the_recorded_tower();
    test_site_without_a_marked_control_channel();
    test_error_and_cancel();
    test_results_and_loaded_system_stay_exclusive();
    test_destroy_with_requests_in_flight();

    QDir(dataDir).removeRecursively();
    if (g_failures != 0) {
        DSD_FPRINTF(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    DSD_FPRINTF(stderr, "OK\n");
    return 0;
}
