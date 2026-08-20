// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "radio_reference_model.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QIODevice>
#include <QLatin1Char>
#include <QLatin1String>
#include <QPointer>
#include <QSaveFile>
#include <QStringList>
#include <QVariant>
#include <Qt>

#include <memory>
#include <stdint.h>
#include <stdlib.h>
#include <type_traits>
#include <vector>

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/runtime/radioreference.h>
#include <dsd-neo/runtime/radioreference_generate.h>
#include <dsd-neo/runtime/radioreference_import.h>

#include "app_prefs.h"
#include "imported_files_model.h"
#include "json_store.h"

namespace dsd_qt {

namespace {

/* Literal names only: json_store_path() does not sanitize, so a system name or a
 * talkgroup label must never be interpolated into a staging path. */
constexpr const char kStagingDir[] = "rr_staging";
constexpr const char kStagingGroup[] = "group.csv";
constexpr const char kStagingChan[] = "chan.csv";

/** @brief How many talkgroups the preview shows. */
constexpr int kTalkgroupSampleMax = 8;

QString
staging_dir_path() {
    return json_store_path(QLatin1String(kStagingDir));
}

/**
 * @brief Overwrite a QString's storage before dropping it.
 *
 * QString::clear() only drops a reference; fill() writes over the characters
 * first. The runtime's own rr_secure_zero is static to the client, so this is
 * the Qt-side equivalent.
 *
 * Best effort by construction: fill() resizes, which detaches first, so a string
 * that is still implicitly shared - which a value handed over from QML is -
 * leaves the original buffer alone and zeroes the fresh copy instead. There is
 * no way to reach the shared buffer that does not also corrupt the other holder,
 * so anything that must actually be erased has to be a buffer this side owns
 * outright; see scrub_bytes() and fillAuth().
 */
void
scrub(QString& text) {
    text.fill(QChar(u'\0'));
    text.clear();
}

/**
 * @brief Overwrite a byte buffer this scope owns outright, then drop it.
 *
 * Unlike scrub(), this is not best effort: data() detaches if it has to, and the
 * write then lands in the buffer that is about to be freed.
 */
void
scrub_bytes(QByteArray& bytes) {
    volatile char* p = bytes.data();
    for (qsizetype i = 0; i < bytes.size(); i++) {
        p[i] = '\0';
    }
    bytes.clear();
}

/**
 * @brief Overwrite a credential buffer the optimizer cannot elide.
 *
 * DSD_MEMSET is __builtin_memset and may be dropped as a dead store on a buffer
 * that is never read again, which is exactly this case.
 */
void
scrub_auth(dsd_rr_auth* auth) {
    volatile unsigned char* p = reinterpret_cast<volatile unsigned char*>(auth);
    for (size_t i = 0; i < sizeof(*auth); i++) {
        p[i] = 0U;
    }
}

/**
 * @brief Format integer Hz as MHz text, exactly and without floating point.
 *
 * @param hz Frequency in Hz.
 * @return e.g. "851.0125", or empty when there is no frequency.
 */
QString
hz_to_mhz_text(long long hz) {
    char buf[32];
    if (dsd_rr_hz_to_mhz_text(hz, buf, sizeof buf) != 0) {
        return QString();
    }
    return QString::fromUtf8(buf);
}

/** @brief Copy a fixed-size C field into a QString. */
QString
field(const char* text) {
    return QString::fromUtf8(text);
}

/** @brief Free a completion result with the free function its shape needs. */
void
free_country_list(dsd_rr_country_list* p) {
    dsd_rr_country_list_free(p);
    free(p);
}

QVariantList
to_warning_list(const dsd_rr_warning_list& warnings) {
    QVariantList out;
    for (size_t i = 0; i < warnings.count; i++) {
        out.append(field(warnings.items[i].text));
    }
    return out;
}

/** @brief Map a runtime status onto the UI's error kinds. */
int
error_kind_for(dsd_rr_status status) {
    switch (status) {
        case DSD_RR_OK: return RadioReferenceModel::NoError;
        case DSD_RR_ERR_AUTH: return RadioReferenceModel::AuthError;
        case DSD_RR_ERR_SUBSCRIPTION: return RadioReferenceModel::SubscriptionError;
        case DSD_RR_ERR_NETWORK: return RadioReferenceModel::NetworkError;
        case DSD_RR_ERR_HTTP:
        case DSD_RR_ERR_SOAP_FAULT: return RadioReferenceModel::ServerError;
        case DSD_RR_ERR_PARSE: return RadioReferenceModel::ParseError;
        case DSD_RR_ERR_CANCELLED: return RadioReferenceModel::CancelledError;
        case DSD_RR_ERR_UNSUPPORTED: return RadioReferenceModel::UnsupportedError;
        default: return RadioReferenceModel::ConfigError;
    }
}

} // namespace

/* ------------------------------------------------------------------------- */
/* Request and reply plumbing                                                 */
/* ------------------------------------------------------------------------- */

/**
 * One completion, already converted off the worker thread.
 *
 * Site and talkgroup payloads keep their C form because the generators read the
 * structs rather than the QVariant views. They travel as shared_ptr so the data
 * is released even when the queued call is dropped - which happens when the
 * model is destroyed between the callback and the event loop.
 */
struct RadioReferenceModel::Reply {
    Fetch kind = FetchUserData;
    quint64 generation = 0;
    int errorKind = NoError;
    QString errorText;
    QVariantList list;
    QVariantMap map;
    std::shared_ptr<dsd_rr_site_list> sites;
    std::shared_ptr<dsd_rr_talkgroup_list> talkgroups;
};

/** Per-request context handed to the C callback as its user pointer. */
struct RadioReferenceModel::Request {
    QPointer<RadioReferenceModel> model;
    dsd_rr_client* client = nullptr;
    Fetch kind = FetchUserData;
    quint64 generation = 0;
    dsd_rr_auth auth{};

    ~Request() { scrub_auth(&auth); }
};

namespace {

/** @brief Convert a country list, then release it. */
QVariantList
take_countries(void* result) {
    QVariantList out;
    auto* list = static_cast<dsd_rr_country_list*>(result);
    for (size_t i = 0; i < list->count; i++) {
        QVariantMap row;
        row.insert(QStringLiteral("coid"), list->items[i].coid);
        row.insert(QStringLiteral("name"), field(list->items[i].name));
        row.insert(QStringLiteral("code"), field(list->items[i].code));
        out.append(row);
    }
    free_country_list(list);
    return out;
}

QVariantList
take_states(void* result) {
    QVariantList out;
    auto* list = static_cast<dsd_rr_state_list*>(result);
    for (size_t i = 0; i < list->count; i++) {
        QVariantMap row;
        row.insert(QStringLiteral("stid"), list->items[i].stid);
        row.insert(QStringLiteral("name"), field(list->items[i].name));
        row.insert(QStringLiteral("code"), field(list->items[i].code));
        out.append(row);
    }
    dsd_rr_state_list_free(list);
    free(list);
    return out;
}

QVariantList
take_counties(void* result) {
    QVariantList out;
    auto* list = static_cast<dsd_rr_county_list*>(result);
    for (size_t i = 0; i < list->count; i++) {
        QVariantMap row;
        row.insert(QStringLiteral("ctid"), list->items[i].ctid);
        row.insert(QStringLiteral("name"), field(list->items[i].county_name));
        row.insert(QStringLiteral("stateName"), field(list->items[i].state_name));
        out.append(row);
    }
    dsd_rr_county_list_free(list);
    free(list);
    return out;
}

QVariantList
take_systems(void* result) {
    QVariantList out;
    auto* list = static_cast<dsd_rr_trs_list*>(result);
    for (size_t i = 0; i < list->count; i++) {
        QVariantMap row;
        row.insert(QStringLiteral("sid"), list->items[i].sid);
        row.insert(QStringLiteral("name"), field(list->items[i].name));
        row.insert(QStringLiteral("city"), field(list->items[i].city));
        /* Numeric IDs on the wire; the descriptions are resolved once a system
         * is opened, because that is the only place they are needed. */
        row.insert(QStringLiteral("typeId"), list->items[i].type_id);
        row.insert(QStringLiteral("flavorId"), list->items[i].flavor_id);
        out.append(row);
    }
    dsd_rr_trs_list_free(list);
    free(list);
    return out;
}

QVariantList
take_categories(void* result) {
    QVariantList out;
    auto* list = static_cast<dsd_rr_talkgroup_cat_list*>(result);
    for (size_t i = 0; i < list->count; i++) {
        QVariantMap row;
        row.insert(QStringLiteral("tgCid"), list->items[i].tg_cid);
        row.insert(QStringLiteral("name"), field(list->items[i].name));
        out.append(row);
    }
    dsd_rr_talkgroup_cat_list_free(list);
    free(list);
    return out;
}

QVariantMap
take_zip(void* result) {
    auto* info = static_cast<dsd_rr_zip_info*>(result);
    QVariantMap map;
    map.insert(QStringLiteral("zipCode"), info->zip_code);
    map.insert(QStringLiteral("stid"), info->stid);
    map.insert(QStringLiteral("ctid"), info->ctid);
    map.insert(QStringLiteral("city"), field(info->city));
    free(info);
    return map;
}

QVariantMap
take_user_info(void* result) {
    auto* info = static_cast<dsd_rr_user_info*>(result);
    QVariantMap map;
    /* subExpireDate only. The username is never echoed back: it is a credential,
     * and the UI already holds the copy the user typed. */
    map.insert(QStringLiteral("subExpire"), field(info->sub_expire));
    free(info);
    return map;
}

/**
 * @brief Resolve a system's numeric IDs and classify it. WORKER THREAD ONLY.
 *
 * dsd_rr_get_support_maps() blocks for up to three round trips and mutates the
 * client's unsynchronized cache, so it belongs on the worker thread and nowhere
 * else. Calling it from the GUI thread would both stall the UI and race the
 * cache against the very worker that fills it.
 */
QVariantMap
take_details(void* result, dsd_rr_client* client, const dsd_rr_auth* auth) {
    auto* details = static_cast<dsd_rr_trs_details*>(result);
    QVariantMap map;
    /* Straight field copies, read from the record rather than from the resolved
     * struct: they carry no policy, and this keeps them right whatever the
     * resolver leaves behind when the support-map fetch fails. */
    map.insert(QStringLiteral("name"), field(details->name));
    map.insert(QStringLiteral("city"), field(details->city));
    map.insert(QStringLiteral("hasCustomBandplan"), details->bandplan_count > 0);

    if (details->sysid_count > 0) {
        map.insert(QStringLiteral("sysid"), field(details->sysids[0].sysid));
        map.insert(QStringLiteral("wacn"), field(details->sysids[0].wacn));
    }

    dsd_rr_system_info info;
    DSD_MEMSET(&info, 0, sizeof(info));
    /* DSD_MEMSET leaves DSD_RR_PROTO_P25 here: UNSUPPORTED is the LAST
     * enumerator of dsd_rr_protocol, not the first. */
    info.protocol = DSD_RR_PROTO_UNSUPPORTED;
    dsd_rr_error err;
    DSD_MEMSET(&err, 0, sizeof(err));
    if (dsd_rr_system_info_resolve(client, auth, details, &info, &err) == 0) {
        map.insert(QStringLiteral("typeDescr"), field(info.type_descr));
        map.insert(QStringLiteral("flavorDescr"), field(info.flavor_descr));
        map.insert(QStringLiteral("voiceDescr"), field(info.voice_descr));
        map.insert(QStringLiteral("esk"), info.record_says_esk != 0);
    }
    map.insert(QStringLiteral("protocol"), static_cast<int>(info.protocol));
    map.insert(QStringLiteral("supported"), info.protocol != DSD_RR_PROTO_UNSUPPORTED);

    dsd_rr_trs_details_free(details);
    free(details);
    return map;
}

} // namespace

void
RadioReferenceModel::convertResult(const Request& request, void* result, Reply* reply) {
    /* WORKER THREAD. The C result becomes the reply's, one way or another: the
     * small shapes are converted and released here, while sites and talkgroups
     * keep their C form under a shared_ptr whose deleter runs even if the queued
     * call is never delivered. */
    switch (request.kind) {
        case FetchUserData: reply->map = take_user_info(result); break;
        case FetchZip: reply->map = take_zip(result); break;
        case FetchCountries: reply->list = take_countries(result); break;
        case FetchStates: reply->list = take_states(result); break;
        case FetchCounties: reply->list = take_counties(result); break;
        case FetchSystems: reply->list = take_systems(result); break;
        case FetchDetails: reply->map = take_details(result, request.client, &request.auth); break;
        case FetchTalkgroupCats: reply->list = take_categories(result); break;
        case FetchSites:
            reply->sites =
                std::shared_ptr<dsd_rr_site_list>(static_cast<dsd_rr_site_list*>(result), [](dsd_rr_site_list* p) {
                    dsd_rr_site_list_free(p);
                    free(p);
                });
            break;
        case FetchTalkgroups:
            reply->talkgroups = std::shared_ptr<dsd_rr_talkgroup_list>(static_cast<dsd_rr_talkgroup_list*>(result),
                                                                       [](dsd_rr_talkgroup_list* p) {
                                                                           dsd_rr_talkgroup_list_free(p);
                                                                           free(p);
                                                                       });
            break;
        default: break;
    }
}

void
RadioReferenceModel::onFetchDone(void* user, dsd_rr_status status, const dsd_rr_error* err, void* result) {
    /* WORKER THREAD. Nothing here may touch a member: the conversion is pure,
     * and everything else is marshalled to the GUI thread. */
    std::unique_ptr<Request> request(static_cast<Request*>(user));
    if (!request) {
        return;
    }

    Reply reply;
    reply.kind = request->kind;
    reply.generation = request->generation;
    reply.errorKind = error_kind_for(status);
    if (status != DSD_RR_OK) {
        /* Server text only. dsd_rr_error::detail is sanitized by the client and
         * never echoes the request body, which carries the password. */
        reply.errorText = (err != nullptr) ? field(err->detail) : QString();
    } else if (result != nullptr) {
        convertResult(*request, result, &reply);
    }

    QPointer<RadioReferenceModel> model = request->model;
    QMetaObject::invokeMethod(
        model,
        [model, reply]() {
            if (model.isNull()) {
                return;
            }
            model->applyReply(reply);
        },
        Qt::QueuedConnection);
}

/* ------------------------------------------------------------------------- */
/* Construction                                                               */
/* ------------------------------------------------------------------------- */

RadioReferenceModel::RadioReferenceModel(AppPrefs* prefs, ImportedFilesModel* importedFiles, DecoderHost* host,
                                         QObject* parent)
    : QObject(parent), m_prefs(prefs), m_importedFiles(importedFiles), m_host(host) {
    DSD_MEMSET(&m_siteData, 0, sizeof(m_siteData));
    DSD_MEMSET(&m_talkgroupData, 0, sizeof(m_talkgroupData));
    m_client = dsd_rr_client_create(nullptr);

    /* AppDataLocation is persistent storage on Android, not cache, so a process
     * kill between the library commit and the staging remove leaves a file
     * behind. Sweep it here rather than trusting a queued cleanup: nothing
     * installs an aboutToQuit handler, and Android tears the engine down with a
     * queued exit that drops pending invocations. */
    QDir staging(staging_dir_path());
    if (staging.exists()) {
        staging.removeRecursively();
    }

    if (m_prefs != nullptr) {
        connect(m_prefs, &AppPrefs::rrUsernameChanged, this, &RadioReferenceModel::credentialsChanged);
        connect(m_prefs, &AppPrefs::rrAppKeyChanged, this, &RadioReferenceModel::credentialsChanged);
    }
}

RadioReferenceModel::~RadioReferenceModel() {
    /* FIRST statement: this cancels everything in flight and joins the worker,
     * so no callback can still be running when the members below are destroyed.
     * The same rule ~CallHistoryModel() follows for its save pool. */
    dsd_rr_client_destroy(m_client);
    m_client = nullptr;

    dsd_rr_site_list_free(&m_siteData);
    dsd_rr_talkgroup_list_free(&m_talkgroupData);
    scrub(m_password);
}

/* ------------------------------------------------------------------------- */
/* Credentials                                                                */
/* ------------------------------------------------------------------------- */

bool
// cppcheck-suppress functionStatic -- a Q_PROPERTY READ accessor cannot be static
RadioReferenceModel::available() const {
    return dsd_rr_available() != 0;
}

bool
// cppcheck-suppress functionStatic -- a Q_PROPERTY READ accessor cannot be static
RadioReferenceModel::buildHasAppKey() const {
    const char* builtin = dsd_rr_builtin_app_key();
    return builtin != nullptr && builtin[0] != '\0';
}

QByteArray
RadioReferenceModel::chooseAppKey(const char* builtin, const QString& override) {
    /* Named local, not a temporary: dsd_rr_choose_app_key() returns one of its
     * two arguments, so the chosen key can point into `stored`. */
    const QByteArray stored = override.toUtf8();
    return QByteArray(dsd_rr_choose_app_key(builtin, stored.constData()));
}

bool
RadioReferenceModel::hasAppKey() const {
    /* Asked through the same choice fillAuth() sends, so "is there a key" and
     * "which key" can never answer from different rules. */
    const QString override = (m_prefs != nullptr) ? m_prefs->rrAppKey() : QString();
    return !chooseAppKey(dsd_rr_builtin_app_key(), override).isEmpty();
}

bool
RadioReferenceModel::credentialsReady() const {
    const bool haveUser = (m_prefs != nullptr) && !m_prefs->rrUsername().isEmpty();
    return haveUser && !m_password.isEmpty() && hasAppKey();
}

void
RadioReferenceModel::setPassword(const QString& password) {
    if (m_password == password) {
        return;
    }
    scrub(m_password);
    m_password = password;
    Q_EMIT credentialsChanged();
}

void
RadioReferenceModel::clearPassword() {
    if (m_password.isEmpty()) {
        return;
    }
    scrub(m_password);
    Q_EMIT credentialsChanged();
}

bool
RadioReferenceModel::fillAuth(dsd_rr_auth* auth) const {
    DSD_MEMSET(auth, 0, sizeof(*auth));
    if (m_prefs == nullptr || !credentialsReady()) {
        return false;
    }

    const QByteArray user = m_prefs->rrUsername().toUtf8();
    /* toUtf8() allocates a buffer this scope solely owns, so unlike the QString
     * it came from it can genuinely be overwritten - and it holds the password
     * in cleartext until it is. */
    QByteArray password = m_password.toUtf8();
    /* The baked key wins wherever there is one - see chooseAppKey(). Hiding the
     * field alone left a stored override still outranking the key, which made a
     * stale one - entered under a keyless build, still in the shared QSettings
     * after a keyed one is installed over it - break every request with no way
     * to reach it. Ignored rather than erased, so a keyless build run later
     * still finds the user's own key where they left it. */
    const QByteArray key = chooseAppKey(dsd_rr_builtin_app_key(), m_prefs->rrAppKey());

    const bool fits = static_cast<size_t>(user.size()) < sizeof(auth->username)
                      && static_cast<size_t>(password.size()) < sizeof(auth->password)
                      && static_cast<size_t>(key.size()) < sizeof(auth->app_key);
    if (fits) {
        DSD_MEMCPY(auth->username, user.constData(), static_cast<size_t>(user.size()));
        DSD_MEMCPY(auth->password, password.constData(), static_cast<size_t>(password.size()));
        DSD_MEMCPY(auth->app_key, key.constData(), static_cast<size_t>(key.size()));
    }
    scrub_bytes(password);
    return fits;
}

/* ------------------------------------------------------------------------- */
/* Status                                                                     */
/* ------------------------------------------------------------------------- */

void
RadioReferenceModel::setStatus(const QString& status) {
    if (m_statusText == status) {
        return;
    }
    m_statusText = status;
    Q_EMIT statusChanged();
}

void
RadioReferenceModel::setError(int kind, const QString& text) {
    if (m_errorKind == kind && m_errorText == text) {
        return;
    }
    m_errorKind = kind;
    m_errorText = text;
    Q_EMIT statusChanged();
}

/* ------------------------------------------------------------------------- */
/* Request lifecycle                                                          */
/* ------------------------------------------------------------------------- */

void
RadioReferenceModel::startBatch(const QString& status) {
    /* Retire anything still in flight: its reply carries the old generation and
     * will be dropped, and cancelling saves the transfer. */
    for (const quint64 id : m_pendingIds) {
        (void)dsd_rr_cancel(m_client, id);
    }
    m_pendingIds.clear();
    const bool wasBusy = busy();
    m_outstanding = 0;
    m_systemPending = 0;
    m_generation++;
    m_errorKind = NoError;
    m_errorText.clear();
    m_statusText = status;
    Q_EMIT statusChanged();
    if (wasBusy) {
        Q_EMIT busyChanged();
    }
    /* Whatever the user just asked for retires a refresh still in flight;
     * refreshRow() records its own state after the loadSystem() that calls this,
     * so this can only ever see the previous one. Last, so the signal it emits
     * observes a consistent batch state. */
    abandonRefresh();
}

void
RadioReferenceModel::abandonRefresh() {
    if (m_refreshRow < 0) {
        return;
    }
    QVariantMap result;
    result.insert(QStringLiteral("ok"), false);
    result.insert(QStringLiteral("error"), QStringLiteral("abandoned"));
    /* endRefresh() clears the pending state before it emits, so a handler that
     * re-enters here finds nothing left to retire. */
    endRefresh(result);
}

RadioReferenceModel::Request*
RadioReferenceModel::beginFetch(Fetch kind) {
    if (m_client == nullptr) {
        setError(UnsupportedError, tr("This build cannot reach RadioReference."));
        return nullptr;
    }

    auto* request = new Request;
    request->model = this;
    request->client = m_client;
    request->kind = kind;
    request->generation = m_generation;
    if (!fillAuth(&request->auth)) {
        delete request;
        /* Names only what can actually be missing. A key in force - baked in
         * or stored as an override - is never the gap, and a keyed build
         * offers no field to supply one, so naming it sends the user hunting
         * for something they cannot enter. */
        setError(ConfigError, hasAppKey()
                                  ? tr("Enter your RadioReference username and password first.")
                                  : tr("Enter your RadioReference username, password and application key first."));
        return nullptr;
    }
    return request;
}

void
RadioReferenceModel::endFetch(quint64 id, Request* request) {
    if (request == nullptr) {
        return;
    }
    if (id == 0U) {
        delete request;
        setError(NetworkError, tr("The RadioReference request could not be started."));
        return;
    }
    m_pendingIds.append(id);
    m_outstanding++;
    if (m_outstanding == 1) {
        Q_EMIT busyChanged();
    }
}

void
RadioReferenceModel::cancel() {
    for (const quint64 id : m_pendingIds) {
        (void)dsd_rr_cancel(m_client, id);
    }
    m_pendingIds.clear();
    const bool wasBusy = busy();
    m_outstanding = 0;
    m_systemPending = 0;
    m_generation++;
    setStatus(tr("Cancelled."));
    if (wasBusy) {
        Q_EMIT busyChanged();
    }
    /* A cancel is an action like any other: the bumped generation drops every
     * reply, so finishSystemLoad() - and with it completeRefresh() - can never
     * run for this batch. Without this the library caller waits forever. */
    abandonRefresh();
}

void
RadioReferenceModel::closeSystem() {
    /* A load still in flight carries the live generation, so its replies would
     * land after this and finishSystemLoad() would republish the system with
     * the sid clearSystem() just zeroed - a fully populated system no view can
     * show, because the screen's stage predicate is sid > 0. cancel() is the
     * retirement the rest of this class uses: it drops the replies, settles
     * busy, and answers a library refresh riding on the same batch instead of
     * leaving it waiting for a finishSystemLoad() that must not run. */
    if (m_systemPending > 0) {
        cancel();
    }
    clearSystem();
    /* The retired error described the system just closed; keeping it would put
     * a stale failure over the results list the user went back to. */
    setError(NoError, QString());
}

/* ------------------------------------------------------------------------- */
/* Requests                                                                   */
/* ------------------------------------------------------------------------- */

void
RadioReferenceModel::checkAccount() {
    startBatch(tr("Checking your RadioReference account…"));
    Request* request = beginFetch(FetchUserData);
    if (request == nullptr) {
        return;
    }
    endFetch(dsd_rr_fetch_user_data(m_client, &request->auth, &onFetchDone, request), request);
}

void
RadioReferenceModel::lookupZip(const QString& zip) {
    startBatch(tr("Looking up %1…").arg(zip));
    Request* request = beginFetch(FetchZip);
    if (request == nullptr) {
        return;
    }
    /* Checked here as well as in the client: the async entry point folds "not a
     * ZIP code" into the same 0 it returns for "queue full", and endFetch() maps
     * every 0 to "the request could not be started" - a network banner for a
     * purely local input problem. */
    bool ok = false;
    const int value = zip.trimmed().toInt(&ok);
    if (!ok || value <= 0 || value > 99999) {
        delete request;
        setError(ConfigError, tr("That is not a ZIP code."));
        return;
    }
    const QByteArray text = zip.trimmed().toUtf8();
    endFetch(dsd_rr_fetch_zipcode_info(m_client, &request->auth, text.constData(), &onFetchDone, request), request);
}

void
RadioReferenceModel::loadCountries() {
    startBatch(tr("Loading countries…"));
    Request* request = beginFetch(FetchCountries);
    if (request == nullptr) {
        return;
    }
    endFetch(dsd_rr_fetch_countries(m_client, &request->auth, &onFetchDone, request), request);
}

void
RadioReferenceModel::loadCountryStates(int coid) {
    startBatch(tr("Loading states…"));
    Request* request = beginFetch(FetchStates);
    if (request == nullptr) {
        return;
    }
    endFetch(dsd_rr_fetch_country_states(m_client, &request->auth, coid, &onFetchDone, request), request);
}

void
RadioReferenceModel::loadStateCounties(int stid) {
    startBatch(tr("Loading counties…"));
    Request* request = beginFetch(FetchCounties);
    if (request == nullptr) {
        return;
    }
    endFetch(dsd_rr_fetch_state_counties(m_client, &request->auth, stid, &onFetchDone, request), request);
}

void
RadioReferenceModel::loadStateSystems(int stid) {
    startBatch(tr("Loading systems…"));
    Request* request = beginFetch(FetchSystems);
    if (request == nullptr) {
        return;
    }
    endFetch(dsd_rr_fetch_state_trs(m_client, &request->auth, stid, &onFetchDone, request), request);
}

void
RadioReferenceModel::loadCountySystems(int ctid) {
    startBatch(tr("Loading systems…"));
    Request* request = beginFetch(FetchSystems);
    if (request == nullptr) {
        return;
    }
    endFetch(dsd_rr_fetch_county_trs(m_client, &request->auth, ctid, &onFetchDone, request), request);
}

void
RadioReferenceModel::loadSystem(int sid) {
    startBatch(tr("Loading system %1…").arg(sid));
    clearSystem();
    m_sid = sid;

    /* All four go on the worker at once; it runs them in order, so the details
     * (and with them the classification) always land before the sites they
     * describe. The preview is assembled once, when the last one arrives. */
    struct {
        Fetch kind;
        uint64_t (*fetch)(dsd_rr_client*, const dsd_rr_auth*, int, dsd_rr_done_cb, void*);
    } const calls[] = {
        {FetchDetails, &dsd_rr_fetch_trs_details},
        {FetchSites, &dsd_rr_fetch_trs_sites},
        {FetchTalkgroups, &dsd_rr_fetch_trs_talkgroups},
        {FetchTalkgroupCats, &dsd_rr_fetch_trs_talkgroup_cats},
    };

    for (const auto& call : calls) {
        Request* request = beginFetch(call.kind);
        if (request == nullptr) {
            return;
        }
        const quint64 id = call.fetch(m_client, &request->auth, sid, &onFetchDone, request);
        endFetch(id, request);
        if (id != 0U) {
            m_systemPending++;
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Completion                                                                 */
/* ------------------------------------------------------------------------- */

void
RadioReferenceModel::clearSystem() {
    m_sid = 0;
    m_protocol = DSD_RR_PROTO_UNSUPPORTED;
    m_recordSaysSimulcast = false;
    m_recordSaysEsk = false;
    dsd_rr_site_list_free(&m_siteData);
    dsd_rr_talkgroup_list_free(&m_talkgroupData);
    m_sites.clear();
    m_systemDetails.clear();
    m_talkgroupSummary.clear();
    Q_EMIT systemChanged();
}

bool
RadioReferenceModel::applySystemReply(const Reply& reply) {
    switch (reply.kind) {
        case FetchDetails:
            m_systemDetails = reply.map;
            m_protocol = static_cast<dsd_rr_protocol>(reply.map.value(QStringLiteral("protocol")).toInt());
            m_recordSaysEsk = reply.map.value(QStringLiteral("esk")).toBool();
            break;
        case FetchSites:
            if (reply.sites) {
                dsd_rr_site_list_free(&m_siteData);
                m_siteData = *reply.sites;
                /* Ownership moves here; blank the shared copy so its deleter has
                 * nothing left to release. */
                DSD_MEMSET(reply.sites.get(), 0, sizeof(*reply.sites));
            }
            break;
        case FetchTalkgroups:
            if (reply.talkgroups) {
                dsd_rr_talkgroup_list_free(&m_talkgroupData);
                m_talkgroupData = *reply.talkgroups;
                DSD_MEMSET(reply.talkgroups.get(), 0, sizeof(*reply.talkgroups));
            }
            break;
        case FetchTalkgroupCats: m_talkgroupSummary.insert(QStringLiteral("categories"), reply.list); break;
        default: return false;
    }

    /* The preview is assembled once, when the last of the four lands. */
    if (m_systemPending > 0) {
        m_systemPending--;
    }
    if (m_systemPending == 0) {
        finishSystemLoad();
    }
    return true;
}

bool
RadioReferenceModel::applyListReply(const Reply& reply) {
    switch (reply.kind) {
        case FetchUserData: setStatus(tr("Account verified.")); return true;
        case FetchCountries: m_countries = reply.list; break;
        case FetchStates: m_states = reply.list; break;
        case FetchCounties: m_counties = reply.list; break;
        case FetchSystems:
            /* A fresh results list retires the loaded system: the screen shows
             * one stage or the other, and the user who just searched asked for
             * the list. Without this the results landed invisibly behind the
             * stale system. */
            if (m_sid != 0 || !m_systemDetails.isEmpty()) {
                clearSystem();
            }
            m_systems = reply.list;
            break;
        default: return false;
    }
    Q_EMIT listsChanged();
    return true;
}

void
RadioReferenceModel::applyReply(const Reply& reply) {
    if (reply.generation != m_generation) {
        /* The user moved on; a stale reply must not overwrite fresh state. */
        return;
    }

    if (m_outstanding > 0) {
        m_outstanding--;
        if (m_outstanding == 0) {
            m_pendingIds.clear();
            Q_EMIT busyChanged();
        }
    }

    if (reply.errorKind != NoError) {
        /* One failure retires the whole batch. Its siblings are still in flight
         * and carry this generation, so applySystemReply() would run
         * finishSystemLoad() once for each of them - m_systemPending is already
         * zero, so its "the last one landed" test passes every time - publishing
         * a system assembled from whatever happened to arrive on top of the
         * error. Bump the generation to drop them, and settle the busy state
         * here because the decrements that would have done it never run. */
        m_systemPending = 0;
        for (const quint64 id : m_pendingIds) {
            (void)dsd_rr_cancel(m_client, id);
        }
        m_pendingIds.clear();
        m_generation++;
        if (m_outstanding > 0) {
            m_outstanding = 0;
            Q_EMIT busyChanged();
        }
        setError(reply.errorKind, reply.errorText);
        setStatus(QString());
        if (m_refreshRow >= 0) {
            /* finishSystemLoad() will never run for this batch, so the caller
             * would otherwise wait for an answer that never comes. */
            QVariantMap result;
            result.insert(QStringLiteral("ok"), false);
            result.insert(QStringLiteral("error"), QStringLiteral("open"));
            endRefresh(result);
        }
        return;
    }

    if (reply.kind == FetchZip) {
        /* A ZIP only resolves IDs; the useful answer is the county's system
         * list, so chain straight into it without asking the user twice. */
        setStatus(reply.map.value(QStringLiteral("city")).toString());
        loadCountySystems(reply.map.value(QStringLiteral("ctid")).toInt());
        return;
    }
    if (applyListReply(reply)) {
        return;
    }
    (void)applySystemReply(reply);
}

void
RadioReferenceModel::finishSystemLoad() {
    m_recordSaysSimulcast = false;

    QVariantList rows;
    for (size_t i = 0; i < m_siteData.count; i++) {
        const dsd_rr_site& site = m_siteData.items[i];
        QVariantMap row;
        /* siteNumber, never siteId: two database rows can share one RF site, and
         * siteId means nothing to a user. */
        row.insert(QStringLiteral("siteNumber"), site.site_number);
        row.insert(QStringLiteral("descr"), field(site.descr));
        row.insert(QStringLiteral("zoneNumber"), site.zone_number);
        row.insert(QStringLiteral("zoneDescr"), field(site.zone_descr));
        row.insert(QStringLiteral("modulation"), field(site.modulation));
        row.insert(QStringLiteral("freqCount"), static_cast<int>(site.freq_count));
        row.insert(QStringLiteral("controlFreqMhz"), hz_to_mhz_text(dsd_rr_site_control_freq_hz(&site)));
        row.insert(QStringLiteral("simulcast"), dsd_rr_site_is_simulcast(&site) != 0);
        /* Both empty for a trunked system; a conventional repeater list is
         * unreadable without them. The colour code is display-only: dsd_opts has
         * no field for it because DMR decodes it off air. */
        row.insert(QStringLiteral("freqMhz"), hz_to_mhz_text(dsd_rr_site_first_freq_hz(&site)));
        row.insert(QStringLiteral("colorCode"), site.freq_count > 0 ? field(site.freqs[0].color_code) : QString());
        if (dsd_rr_site_is_simulcast(&site) != 0) {
            m_recordSaysSimulcast = true;
        }
        rows.append(row);
    }
    m_sites = rows;

    int encCount = 0;
    int partialCount = 0;
    QVariantList sample;
    for (size_t i = 0; i < m_talkgroupData.count; i++) {
        const dsd_rr_talkgroup& tg = m_talkgroupData.items[i];
        if (tg.enc >= 2) {
            encCount++;
        } else if (tg.enc == 1) {
            partialCount++;
        }
        if (sample.size() < kTalkgroupSampleMax) {
            QVariantMap row;
            row.insert(QStringLiteral("dec"), static_cast<qulonglong>(tg.tg_dec));
            row.insert(QStringLiteral("name"), tg.alpha_tag[0] != '\0' ? field(tg.alpha_tag) : field(tg.description));
            row.insert(QStringLiteral("enc"), tg.enc);
            sample.append(row);
        }
    }
    m_talkgroupSummary.insert(QStringLiteral("count"), static_cast<int>(m_talkgroupData.count));
    m_talkgroupSummary.insert(QStringLiteral("encCount"), encCount);
    m_talkgroupSummary.insert(QStringLiteral("partialEncCount"), partialCount);
    m_talkgroupSummary.insert(QStringLiteral("sample"), sample);
    if (!m_talkgroupSummary.contains(QStringLiteral("categories"))) {
        m_talkgroupSummary.insert(QStringLiteral("categories"), QVariantList());
    }

    m_systemDetails.insert(QStringLiteral("sid"), m_sid);
    m_systemDetails.insert(QStringLiteral("simulcast"), m_recordSaysSimulcast);
    m_systemDetails.insert(QStringLiteral("conventional"), conventional());
    m_systemDetails.insert(QStringLiteral("siteCount"), static_cast<int>(m_siteData.count));

    setStatus(QString());
    Q_EMIT systemChanged();

    if (m_refreshRow >= 0) {
        completeRefresh();
    }
}

void
RadioReferenceModel::setTransportForTests(const dsd_rr_transport* transport) {
    dsd_rr_client_set_transport(m_client, transport);
}

/* ------------------------------------------------------------------------- */
/* Preview and import                                                         */
/* ------------------------------------------------------------------------- */

bool
RadioReferenceModel::generateFiles(const QList<dsd_rr_site>& chosen, bool partialEncAsDe, QVariantMap* plan,
                                   QVariantList* warnings) const {
    dsd_rr_warning_list generated;
    DSD_MEMSET(&generated, 0, sizeof(generated));

    char* chanText = nullptr;
    size_t chanLen = 0;
    if (dsd_rr_generate_chan_csv(m_protocol, chosen.constData(), static_cast<size_t>(chosen.size()), &chanText,
                                 &chanLen, &generated)
        != 0) {
        dsd_rr_warning_list_free(&generated);
        return false;
    }
    if (chanText != nullptr) {
        plan->insert(QStringLiteral("chanCsvText"), QString::fromUtf8(chanText, static_cast<int>(chanLen)));
        /* A conventional import only produces a file when two or more repeaters
         * made it through, which is exactly when -Y is right. */
        plan->insert(QStringLiteral("scanList"), conventional());
        free(chanText);
    }

    char* groupText = nullptr;
    size_t groupLen = 0;
    if (m_talkgroupData.count > 0
        && dsd_rr_generate_group_csv(m_talkgroupData.items, m_talkgroupData.count, partialEncAsDe ? 1 : 0, &groupText,
                                     &groupLen, &generated)
               == 0
        && groupText != nullptr) {
        plan->insert(QStringLiteral("groupCsvText"), QString::fromUtf8(groupText, static_cast<int>(groupLen)));
        free(groupText);
    }

    warnings->append(to_warning_list(generated));
    dsd_rr_warning_list_free(&generated);
    return true;
}

namespace {

/**
 * @brief Rebuild the classified system the plan builder takes.
 *
 * The model keeps only what take_details() put in m_systemDetails plus the two
 * fields applySystemReply() reads back, and everything the builder consults is
 * derivable from those - so the struct is rebuilt here rather than carried
 * through the reply queue.
 */
void
fill_system_info(dsd_rr_protocol protocol, bool recordSaysEsk, dsd_rr_system_info* info) {
    DSD_MEMSET(info, 0, sizeof(*info));
    /* Explicit: DSD_MEMSET leaves DSD_RR_PROTO_P25 (0) here, not UNSUPPORTED. */
    info->protocol = protocol;
    info->supported = (protocol != DSD_RR_PROTO_UNSUPPORTED) ? 1 : 0;
    info->conventional = dsd_rr_protocol_is_conventional(protocol);
    info->trunked = dsd_rr_protocol_is_trunked(protocol);
    info->record_says_esk = recordSaysEsk ? 1 : 0;
}

/**
 * @brief Apply the screen's option map over the follow-the-record sentinels.
 *
 * The caller MUST pass {-1, -1, 1}: a zeroed struct means "force simulcast off,
 * force ESK off, play partly encrypted talkgroups", which is a different import.
 * Only a key that is actually present overrides, so an absent key keeps the
 * record-derived answer exactly as options.value(key, default) did.
 */
void
fill_import_options(const QVariantMap& options, dsd_rr_import_options* opts) {
    if (options.contains(QStringLiteral("simulcast"))) {
        opts->simulcast = options.value(QStringLiteral("simulcast")).toBool() ? 1 : 0;
    }
    if (options.contains(QStringLiteral("esk"))) {
        opts->esk = options.value(QStringLiteral("esk")).toBool() ? 1 : 0;
    }
    if (options.contains(QStringLiteral("partialEncAsDe"))) {
        opts->partial_enc_as_de = options.value(QStringLiteral("partialEncAsDe")).toBool() ? 1 : 0;
    }
}

/** @brief QML's selection list as builder indexes; anything unusable becomes SIZE_MAX. */
std::vector<size_t>
selected_indexes(const QVariantList& siteIndexes) {
    std::vector<size_t> out;
    out.reserve(static_cast<size_t>(siteIndexes.size()));
    for (const QVariant& value : siteIndexes) {
        bool ok = false;
        const int index = value.toInt(&ok);
        /* SIZE_MAX is out of range for any site list, so the builder reports it
         * as "A selected site is no longer in the list and was ignored." -
         * exactly what the old selection loop did for a non-int or negative
         * entry. */
        out.push_back((ok && index >= 0) ? static_cast<size_t>(index) : SIZE_MAX);
    }
    return out;
}

/** @brief The keys buildImportPlan() has inserted before it looks at anything. */
QVariantMap
plan_base_map(dsd_rr_protocol protocol, const QVariantMap& systemDetails) {
    QVariantMap map;
    map.insert(QStringLiteral("ok"), false);
    map.insert(QStringLiteral("protocol"), static_cast<int>(protocol));
    map.insert(QStringLiteral("protocolName"), systemDetails.value(QStringLiteral("flavorDescr")));
    map.insert(QStringLiteral("conventional"), dsd_rr_protocol_is_conventional(protocol) != 0);
    map.insert(QStringLiteral("trunking"), dsd_rr_protocol_is_trunked(protocol) != 0);
    map.insert(QStringLiteral("chanNeed"), dsd_rr_chan_map_need(protocol));
    map.insert(QStringLiteral("scanList"), false);
    map.insert(QStringLiteral("groupCsvText"), QString());
    map.insert(QStringLiteral("chanCsvText"), QString());
    map.insert(QStringLiteral("freqMhz"), QString());
    map.insert(QStringLiteral("blockedReason"), QString());
    return map;
}

/**
 * @brief Fold a built plan into the map QML reads.
 *
 * The early returns are the point: a key this function skips is a key the old
 * buildImportPlan() never inserted on that path, and RadioReferenceScreen.qml
 * reads plan fields through `screen.plan[key] !== undefined`.
 */
void
plan_into_map(const dsd_rr_import_plan& plan, QVariantMap* map) {
    map->insert(QStringLiteral("blockedReason"), field(plan.blocked_reason));
    map->insert(QStringLiteral("warnings"), to_warning_list(plan.warnings));
    if (plan.protocol == DSD_RR_PROTO_UNSUPPORTED || plan.site_count <= 0) {
        return; /* blocked before a selection resolved */
    }
    map->insert(QStringLiteral("siteCount"), plan.site_count);
    map->insert(QStringLiteral("siteIds"), field(plan.site_ids));
    map->insert(QStringLiteral("partialEncAsDe"), plan.partial_enc_as_de != 0);
    if (plan.chan_csv_text != nullptr) {
        map->insert(QStringLiteral("chanCsvText"),
                    QString::fromUtf8(plan.chan_csv_text, static_cast<int>(plan.chan_csv_len)));
        map->insert(QStringLiteral("scanList"), plan.scan_list != 0);
    }
    if (plan.group_csv_text != nullptr) {
        map->insert(QStringLiteral("groupCsvText"),
                    QString::fromUtf8(plan.group_csv_text, static_cast<int>(plan.group_csv_len)));
    }
    if (plan.decode_flag[0] == '\0') {
        return; /* blocked at or before generation: decodeFlag is absent and
                * freqMhz/ok keep the base map's empty/false, exactly as the
                * old code left them when it returned early. */
    }
    map->insert(QStringLiteral("decodeFlag"), field(plan.decode_flag));
    const long long tuneHz = plan.tune_hz;
    map->insert(QStringLiteral("freqMhz"), hz_to_mhz_text(tuneHz));
    map->insert(QStringLiteral("ok"), plan.ok != 0);
}

} // namespace

QVariantMap
RadioReferenceModel::buildImportPlan(const QVariantList& siteIndexes, const QVariantMap& options) {
    dsd_rr_system_info info;
    fill_system_info(m_protocol, m_recordSaysEsk, &info);

    dsd_rr_import_options opts = {-1, -1, 1};
    fill_import_options(options, &opts);

    const std::vector<size_t> selected = selected_indexes(siteIndexes);

    dsd_rr_import_plan plan;
    DSD_MEMSET(&plan, 0, sizeof(plan));
    plan.protocol = m_protocol;

    QVariantMap map = plan_base_map(m_protocol, m_systemDetails);
    if (dsd_rr_import_plan_build(&info, m_siteData.items, m_siteData.count, selected.data(), selected.size(),
                                 m_talkgroupData.items, m_talkgroupData.count, &opts, &plan)
        != 0) {
        /* A hard builder failure is the same class of failure the generator
         * already reported this way. */
        dsd_rr_import_plan_free(&plan);
        map.insert(QStringLiteral("blockedReason"), tr("The channel map could not be generated."));
        map.insert(QStringLiteral("warnings"), QVariantList());
        return map;
    }
    plan_into_map(plan, &map);
    dsd_rr_import_plan_free(&plan);
    return map;
}

namespace {

/** @brief Write @p text to @p path atomically. */
bool
write_staging(const QString& path, const QString& text) {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    const QByteArray bytes = text.toUtf8();
    if (file.write(bytes) != bytes.size()) {
        file.cancelWriting();
        return false;
    }
    return file.commit();
}

} // namespace

void
RadioReferenceModel::unwindImport(const QStringList& paths) {
    if (m_importedFiles == nullptr) {
        return;
    }
    /* By path, not by the index the adopt returned: each remove() shifts the
     * ones after it. */
    for (const QString& path : paths) {
        const int row = m_importedFiles->rowForPath(path);
        if (row >= 0) {
            m_importedFiles->remove(row);
        }
    }
}

QVariantMap
RadioReferenceModel::performImport(const QVariantMap& plan, const QString& systemName, int savedRow) {
    QVariantMap result;
    result.insert(QStringLiteral("ok"), false);
    result.insert(QStringLiteral("error"), QStringLiteral("state"));
    if (m_importedFiles == nullptr || !plan.value(QStringLiteral("ok")).toBool()) {
        return result;
    }

    /* json_store_path() is string concatenation and creates nothing, unlike
     * json_store_save_array() which mkpaths its parent. */
    const QString dir = staging_dir_path();
    if (!QDir().mkpath(dir)) {
        return result;
    }

    QVariantMap origin;
    origin.insert(QStringLiteral("origin"), QStringLiteral("radioreference"));
    origin.insert(QStringLiteral("rrSid"), m_sid);
    origin.insert(QStringLiteral("rrSiteIds"), plan.value(QStringLiteral("siteIds")).toString());
    origin.insert(QStringLiteral("rrPartialEnc"), plan.value(QStringLiteral("partialEncAsDe"), true).toBool());

    static const struct {
        const char* planKey;
        const char* staging;
        const char* type;
        const char* resultKey;
    } kFiles[] = {
        {"groupCsvText", kStagingGroup, "group", "groupCsvPath"},
        {"chanCsvText", kStagingChan, "chan", "chanCsvPath"},
    };

    /* An import is one unit: the caller is told it failed and never saves a
     * system, so a file adopted before the failure would be a library row
     * nothing references and nothing ever prunes - and every retry would add
     * another "<System> group (2).csv". */
    QStringList adopted;
    for (const auto& spec : kFiles) {
        const QString text = plan.value(QLatin1String(spec.planKey)).toString();
        if (text.isEmpty()) {
            continue;
        }
        const QString path = dir + QLatin1Char('/') + QLatin1String(spec.staging);
        if (!write_staging(path, text)) {
            unwindImport(adopted);
            return result;
        }
        origin.insert(QStringLiteral("rrKind"), QLatin1String(spec.type));
        const QString name =
            QStringLiteral("%1 %2.csv")
                .arg(systemName.isEmpty() ? tr("RadioReference") : systemName, QLatin1String(spec.type));
        const QVariantMap imported = m_importedFiles->importGeneratedFile(path, name, QLatin1String(spec.type), origin);
        QFile::remove(path);
        if (!imported.value(QStringLiteral("ok")).toBool()) {
            unwindImport(adopted);
            result.insert(QStringLiteral("error"), QStringLiteral("import"));
            return result;
        }
        adopted.append(imported.value(QStringLiteral("path")).toString());
        result.insert(QLatin1String(spec.resultKey), imported.value(QStringLiteral("path")));
    }

    result.insert(QStringLiteral("ok"), true);
    result.insert(QStringLiteral("error"), QString());
    result.insert(QStringLiteral("name"), systemName);
    result.insert(QStringLiteral("freqMhz"), plan.value(QStringLiteral("freqMhz")));
    result.insert(QStringLiteral("decodeFlag"), plan.value(QStringLiteral("decodeFlag")));
    result.insert(QStringLiteral("trunking"), plan.value(QStringLiteral("trunking")));
    result.insert(QStringLiteral("savedRow"), savedRow);
    return result;
}

/* ------------------------------------------------------------------------- */
/* Refresh                                                                    */
/* ------------------------------------------------------------------------- */

namespace {

/**
 * @brief The sites a generated row was built from, from its `rrSiteIds` provenance.
 *
 * TrsSite.siteId, never the RF site number: a system can number several sites
 * the same - the captured SARA network numbers its 35 sites
 * 1,1,10,10,10,10,10,20,... - so a number could name the wrong tower. Blanks and
 * duplicates are dropped.
 */
QList<int>
site_ids_of(const QVariantMap& entry) {
    QList<int> out;
    const QStringList parts =
        entry.value(QStringLiteral("rrSiteIds")).toString().split(QLatin1Char(','), Qt::SkipEmptyParts);
    for (const QString& part : parts) {
        bool ok = false;
        const int value = part.trimmed().toInt(&ok);
        if (ok && !out.contains(value)) {
            out.append(value);
        }
    }
    return out;
}

} // namespace

bool
RadioReferenceModel::refreshRow(int row) {
    if (m_importedFiles == nullptr) {
        return false;
    }
    const QVariantMap entry = m_importedFiles->get(row);
    if (entry.value(QStringLiteral("origin")).toString() != QLatin1String("radioreference")) {
        setError(ConfigError, tr("That file did not come from RadioReference."));
        return false;
    }
    const int sid = entry.value(QStringLiteral("rrSid")).toInt();
    const QList<int> wanted = site_ids_of(entry);
    if (sid <= 0 || wanted.isEmpty()) {
        setError(ConfigError,
                 tr("This file does not record which system it came from. Import it again to refresh it."));
        return false;
    }
    if (!credentialsReady()) {
        /* Names only what can actually be missing. A key in force - baked in
         * or stored as an override - is never the gap, and a keyed build
         * offers no field to supply one, so naming it sends the user hunting
         * for something they cannot enter. */
        setError(ConfigError, hasAppKey()
                                  ? tr("Enter your RadioReference username and password first.")
                                  : tr("Enter your RadioReference username, password and application key first."));
        return false;
    }

    /* Replaces whatever system the RadioReference screen had loaded: one client,
     * one system at a time, and a refresh is started from the library rather
     * than from that screen. */
    loadSystem(sid);
    if (m_systemPending == 0) {
        /* Nothing was queued; beginFetch() has already reported why. */
        return false;
    }
    /* After loadSystem(), because the startBatch() inside it clears this. */
    m_refreshRow = row;
    m_refreshPath = entry.value(QStringLiteral("path")).toString();
    m_refreshKind = entry.value(QStringLiteral("rrKind")).toString();
    m_refreshPartialEnc = entry.value(QStringLiteral("rrPartialEnc"), true).toBool();
    m_refreshSiteIds = wanted;
    return true;
}

void
RadioReferenceModel::endRefresh(const QVariantMap& result) {
    const int row = m_refreshRow;
    m_refreshRow = -1;
    m_refreshPath.clear();
    m_refreshKind.clear();
    m_refreshSiteIds.clear();
    m_refreshPartialEnc = true;
    Q_EMIT refreshFinished(row, result);
}

void
RadioReferenceModel::completeRefresh() {
    QVariantMap result;
    result.insert(QStringLiteral("ok"), false);
    result.insert(QStringLiteral("error"), QStringLiteral("open"));

    /* Matched by siteId, never by index: RadioReference is free to reorder
     * getTrsSites, and an index would refresh the wrong repeater. Never by the RF
     * site number either - a system numbers several sites the same. A site that
     * is gone is dropped rather than shifting the rest of the list. */
    QList<dsd_rr_site> chosen;
    for (const int wanted : m_refreshSiteIds) {
        for (size_t i = 0; i < m_siteData.count; i++) {
            if (m_siteData.items[i].site_db_id == wanted) {
                chosen.append(m_siteData.items[i]);
                break;
            }
        }
    }
    if (chosen.isEmpty()) {
        setError(ServerError, tr("RadioReference no longer lists the site this file was built from."));
        endRefresh(result);
        return;
    }

    QVariantMap plan;
    QVariantList warnings;
    /* The answer the original import was given, read back from provenance: the
     * UI default would silently re-block every partly-encrypted talkgroup for a
     * user who had turned it off. */
    if (!generateFiles(chosen, m_refreshPartialEnc, &plan, &warnings)) {
        setError(ParseError, tr("The refreshed data could not be turned into a file."));
        endRefresh(result);
        return;
    }

    const bool isChan = (m_refreshKind == QLatin1String("chan"));
    const QString text = plan.value(isChan ? QStringLiteral("chanCsvText") : QStringLiteral("groupCsvText")).toString();
    if (text.isEmpty()) {
        setError(ParseError, tr("RadioReference has no data for this file any more."));
        endRefresh(result);
        return;
    }

    const QString dir = staging_dir_path();
    const QString path = dir + QLatin1Char('/') + QLatin1String(isChan ? kStagingChan : kStagingGroup);
    if (!QDir().mkpath(dir) || !write_staging(path, text)) {
        setError(ConfigError, tr("The refreshed file could not be written."));
        endRefresh(result);
        return;
    }

    /* Re-resolve the row by its stored path, never by the index the tap
     * recorded: the imports library stays interactive for the whole fetch, and a
     * row removed meanwhile shifts every later index down - writing to the stale
     * index would replace an unrelated file in place, which is exactly what
     * refreshGeneratedFile() is built to do. */
    const int target = m_importedFiles->rowForPath(m_refreshPath);
    if (target < 0) {
        QFile::remove(path);
        setError(ConfigError, tr("That file is no longer in your library, so there was nothing to refresh."));
        endRefresh(result);
        return;
    }
    m_refreshRow = target;

    /* Path preserved and staging pre-validated, so a fault page or a truncated
     * body leaves the stored copy byte-identical. */
    result = m_importedFiles->refreshGeneratedFile(target, path);
    QFile::remove(path);
    if (!result.value(QStringLiteral("ok")).toBool()) {
        setError(ParseError, tr("The refreshed file could not be read back, so the stored copy was kept."));
    }
    endRefresh(result);
}

} // namespace dsd_qt
