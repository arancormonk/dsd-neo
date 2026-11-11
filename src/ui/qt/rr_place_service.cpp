// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "rr_place_service.h"

#include <QAbstractItemModel>
#include <QMetaObject>
#include <QModelIndex>
#include <QPair>
#include <QVariant>
#include <Qt>

#include <algorithm>
#include <memory>
#include <stdlib.h>

#include <dsd-neo/core/safe_api.h>

#include "radio_reference_model.h"
#include "saved_systems_model.h"
#include "site_groups.h"
#include "teardown_trace.h"

namespace dsd_qt {

namespace {

const QString kStates = QStringLiteral("states");
const QString kCounties = QStringLiteral("counties");
const QString kCandidates = QStringLiteral("candidateStates");
const QString kStid = QStringLiteral("stid");
const QString kCtid = QStringLiteral("ctid");
const QString kName = QStringLiteral("name");
const QString kPlace = QStringLiteral("place");

QString
c_text(const char* text, size_t capacity) {
    const char* end = std::find(text, text + capacity, '\0');
    return QString::fromUtf8(text, static_cast<qsizetype>(end - text));
}

/** @brief Whether @p place shows a place: a state with a name. */
bool
place_named(const QVariantMap& place) {
    const QVariantList states = place.value(kStates).toList();
    return std::any_of(states.cbegin(), states.cend(),
                       [](const QVariant& state) { return !state.toMap().value(kName).toString().isEmpty(); });
}

/** @brief Whether anything in @p place still lacks a name or, for a county, its state. */
bool
place_needs_names(const QVariantMap& place) {
    const QVariantList states = place.value(kStates).toList();
    const QVariantList counties = place.value(kCounties).toList();
    const bool unnamedState = std::any_of(states.cbegin(), states.cend(),
                                          [](const QVariant& state) { return !state.toMap().contains(kName); });
    const bool unnamedCounty = std::any_of(counties.cbegin(), counties.cend(), [](const QVariant& county) {
        const QVariantMap map = county.toMap();
        return !map.contains(kName) || !map.contains(kStid);
    });
    return unnamedState || unnamedCounty || (states.isEmpty() && !place.value(kCandidates).toList().isEmpty());
}

/** @brief Every state id @p place names (candidates, states, county links), each once, in that order. */
QList<int>
place_state_ids(const QVariantMap& place) {
    QList<int> out;
    const auto add = [&out](int stid) {
        if (stid > 0 && !out.contains(stid)) {
            out.append(stid);
        }
    };
    for (const QVariant& stid : place.value(kCandidates).toList()) {
        add(stid.toInt());
    }
    for (const QVariant& state : place.value(kStates).toList()) {
        add(state.toMap().value(kStid).toInt());
    }
    for (const QVariant& county : place.value(kCounties).toList()) {
        add(county.toMap().value(kStid).toInt());
    }
    return out;
}

/** @brief The entries of a place list keyed by their id. */
QHash<int, QVariantMap>
entries_by_id(const QVariantMap& place, const QString& list, const QString& idKey) {
    QHash<int, QVariantMap> out;
    for (const QVariant& entry : place.value(list).toList()) {
        const QVariantMap map = entry.toMap();
        out.insert(map.value(idKey).toInt(), map);
    }
    return out;
}

/**
 * @brief @p fresh (ids only, as RadioReference sent them now) with what @p stored already knew of the same ids: county
 *        names and state links, state names, and the states those links bring.
 *
 * So a refresh whose ids did not change keeps the row's names even when the lookup that would name them again fails.
 */
QVariantMap
keep_known_names(const QVariantMap& fresh, const QVariantMap& stored) {
    if (fresh.isEmpty()) {
        return fresh;
    }
    const QHash<int, QVariantMap> oldCounties = entries_by_id(stored, kCounties, kCtid);
    const QHash<int, QVariantMap> oldStates = entries_by_id(stored, kStates, kStid);
    QVariantList counties;
    QList<int> linked;
    for (const QVariant& value : fresh.value(kCounties).toList()) {
        QVariantMap county = value.toMap();
        const QVariantMap old = oldCounties.value(county.value(kCtid).toInt());
        for (const QString& key : {kName, kStid}) {
            if (old.contains(key)) {
                county.insert(key, old.value(key));
            }
        }
        if (county.contains(kStid) && !linked.contains(county.value(kStid).toInt())) {
            linked.append(county.value(kStid).toInt());
        }
        counties.append(county);
    }
    QVariantList states;
    QList<int> listed;
    for (const QVariant& value : fresh.value(kStates).toList()) {
        const int stid = value.toMap().value(kStid).toInt();
        states.append(oldStates.contains(stid) ? oldStates.value(stid) : value);
        listed.append(stid);
    }
    for (const int stid : linked) {
        if (!listed.contains(stid)) {
            states.append(oldStates.contains(stid) ? oldStates.value(stid) : QVariantMap{{kStid, stid}});
        }
    }
    QVariantMap out = fresh;
    out.insert(kCounties, counties);
    out.insert(kStates, states);
    return out;
}

} // namespace

/* ------------------------------------------------------------------------- */
/* Request and reply plumbing                                                 */
/* ------------------------------------------------------------------------- */

struct RrPlaceService::Reply {
    Fetch kind = FetchState;
    quint64 generation = 0;
    int key = 0;
    bool ok = false;
    QString stateName;
    QHash<int, QString> counties;
    QVariantList stateIds;
    QList<SiteRecord> sites;
};

struct RrPlaceService::Request {
    QPointer<RrPlaceService> service;
    Fetch kind = FetchState;
    quint64 generation = 0;
    int key = 0;
    dsd_rr_auth auth{};

    Request() = default;
    Request(const Request&) = delete;
    Request& operator=(const Request&) = delete;

    ~Request() { DSD_SECURE_ZERO(&auth, sizeof(auth)); }
};

void
RrPlaceService::convertResult(Fetch kind, void* result, Reply* reply) {
    /* WORKER THREAD. Converted and released here, so a dropped queued call leaks nothing. */
    switch (kind) {
        case FetchState: {
            auto* info = static_cast<dsd_rr_state_info*>(result);
            reply->stateName = c_text(info->state_name, sizeof(info->state_name));
            for (size_t i = 0; i < info->counties.count; i++) {
                const dsd_rr_county& county = info->counties.items[i];
                reply->counties.insert(county.ctid, c_text(county.county_name, sizeof(county.county_name)));
            }
            dsd_rr_state_info_free(info);
            free(info);
            break;
        }
        case FetchDetails: {
            auto* details = static_cast<dsd_rr_trs_details*>(result);
            for (size_t i = 0; i < details->state_count; i++) {
                reply->stateIds.append(details->state_ids[i]);
            }
            dsd_rr_trs_details_free(details);
            free(details);
            break;
        }
        case FetchSites: {
            auto* list = static_cast<dsd_rr_site_list*>(result);
            for (size_t i = 0; i < list->count; i++) {
                const dsd_rr_site& site = list->items[i];
                SiteRecord record;
                record.siteId = site.site_db_id;
                record.countyId = site.county_id;
                record.name = c_text(site.descr, sizeof(site.descr));
                record.positioned = site.has_position != 0 && site_position_valid(site.lat, site.lon);
                record.lat = record.positioned ? site.lat : 0;
                record.lon = record.positioned ? site.lon : 0;
                reply->sites.append(record);
            }
            dsd_rr_site_list_free(list);
            free(list);
            break;
        }
    }
}

void
RrPlaceService::onFetchDone(void* user, dsd_rr_status status, const dsd_rr_error* err, void* result) {
    /* WORKER THREAD. No member is touched here; the error detail is not kept, so nothing a server echoed can reach a
     * string. */
    (void)err;
    std::unique_ptr<Request> request(static_cast<Request*>(user));
    if (!request) {
        return;
    }
    Reply reply;
    reply.kind = request->kind;
    reply.generation = request->generation;
    reply.key = request->key;
    reply.ok = status == DSD_RR_OK && result != nullptr;
    if (result != nullptr) {
        convertResult(request->kind, result, &reply);
    }
    QPointer<RrPlaceService> service = request->service;
    QMetaObject::invokeMethod(
        service,
        [service, reply]() {
            if (!service.isNull()) {
                service->applyReply(reply);
            }
        },
        Qt::QueuedConnection);
}

/* ------------------------------------------------------------------------- */
/* Construction                                                               */
/* ------------------------------------------------------------------------- */

RrPlaceService::RrPlaceService(RadioReferenceModel* rr, SavedSystemsModel* systems, QObject* parent)
    : QObject(parent), m_rr(rr), m_systems(systems) {
    m_client = dsd_rr_client_create(nullptr);
    if (m_systems != nullptr) {
        m_systems->setPlaceNameCache([this](QVariantMap& place) { fillPlaceNames(place); });
        connect(m_systems, &QAbstractItemModel::rowsInserted, this, &RrPlaceService::recountUnplaced);
        connect(m_systems, &QAbstractItemModel::rowsRemoved, this, &RrPlaceService::recountUnplaced);
        connect(m_systems, &QAbstractItemModel::modelReset, this, &RrPlaceService::recountUnplaced);
        connect(m_systems, &QAbstractItemModel::dataChanged, this, &RrPlaceService::recountUnplaced);
        recountUnplaced();
    }
    if (m_rr != nullptr) {
        /* Queued: the lookups start after the load that announced them has finished publishing. */
        connect(m_rr, &RadioReferenceModel::systemPlaceIdsReady, this, &RrPlaceService::resolveStates,
                Qt::QueuedConnection);
        connect(m_rr, &RadioReferenceModel::credentialsChanged, this, &RrPlaceService::onCredentialsChanged);
    }
}

RrPlaceService::~RrPlaceService() {
    const TeardownStage stage("~RrPlaceService");
    /* First: cancels what is in flight and joins the worker, so no callback runs into the members below. */
    dsd_rr_client_destroy(m_client);
    m_client = nullptr;
    if (!m_systems.isNull()) {
        m_systems->setPlaceNameCache(SavedSystemsModel::PlaceNameFill());
    }
}

void
RrPlaceService::setTransportForTests(const dsd_rr_transport* transport) {
    dsd_rr_client_set_transport(m_client, transport);
}

bool
RrPlaceService::signedIn() const {
    return !m_rr.isNull() && m_rr->credentialsReady();
}

/* ------------------------------------------------------------------------- */
/* Requests                                                                   */
/* ------------------------------------------------------------------------- */

bool
RrPlaceService::submit(Fetch kind, int key) {
    if (m_client == nullptr || m_rr.isNull()) {
        return false;
    }
    auto request = std::make_unique<Request>();
    request->service = this;
    request->kind = kind;
    request->generation = m_generation;
    request->key = key;
    if (!m_rr->fillAuth(&request->auth)) {
        return false;
    }
    quint64 id = 0;
    switch (kind) {
        case FetchState:
            id = dsd_rr_fetch_state_info(m_client, &request->auth, key, &onFetchDone, request.get());
            break;
        case FetchDetails:
            id = dsd_rr_fetch_trs_details(m_client, &request->auth, key, &onFetchDone, request.get());
            break;
        case FetchSites: id = dsd_rr_fetch_trs_sites(m_client, &request->auth, key, &onFetchDone, request.get()); break;
    }
    if (id == 0U) {
        return false;
    }
    /* The callback owns it now. */
    (void)request.release();
    m_inFlight = id;
    m_inFlightKind = kind;
    m_inFlightKey = key;
    return true;
}

void
RrPlaceService::submitNext() {
    while (m_inFlight == 0U) {
        if (m_updating && !m_update.namesPhase) {
            if (m_update.next >= m_update.sids.size()) {
                beginNamesPhase();
                continue;
            }
            const int sid = m_update.sids.at(m_update.next);
            if (!submit(m_update.detailsRead ? FetchSites : FetchDetails, sid)) {
                applySitesReply(sid, false, QList<SiteRecord>());
            }
            continue;
        }
        if (m_stateQueue.isEmpty()) {
            return;
        }
        const int stid = m_stateQueue.takeFirst();
        if (!submit(FetchState, stid)) {
            m_failedStates.insert(stid);
            noteStateSettled(stid);
        }
    }
}

void
RrPlaceService::queueState(int stid) {
    const bool asking = m_inFlight != 0U && m_inFlightKind == FetchState && m_inFlightKey == stid;
    if (!asking && !m_stateQueue.contains(stid)) {
        m_stateQueue.append(stid);
    }
}

void
RrPlaceService::stopRequests() {
    if (m_inFlight != 0U) {
        (void)dsd_rr_cancel(m_client, m_inFlight);
    }
    m_inFlight = 0;
    m_stateQueue.clear();
    m_generation++;
}

void
RrPlaceService::applyReply(const Reply& reply) {
    if (reply.generation != m_generation || m_inFlight == 0U) {
        return;
    }
    m_inFlight = 0;
    switch (reply.kind) {
        case FetchState: applyStateReply(reply); break;
        case FetchDetails:
            if (reply.ok) {
                m_update.stateIds = reply.stateIds;
                m_update.detailsRead = true;
            } else {
                applySitesReply(reply.key, false, QList<SiteRecord>());
            }
            break;
        case FetchSites: applySitesReply(reply.key, reply.ok, reply.sites); break;
    }
    submitNext();
}

/* ------------------------------------------------------------------------- */
/* Names                                                                      */
/* ------------------------------------------------------------------------- */

void
RrPlaceService::resolveStates(const QVariantList& stids) {
    if (!signedIn()) {
        return;
    }
    retryUnapplied();
    QList<int> wanted;
    for (const QVariant& value : stids) {
        bool ok = false;
        const int stid = value.toInt(&ok);
        if (ok && stid > 0 && !wanted.contains(stid)) {
            wanted.append(stid);
        }
        if (wanted.size() == kMaxStatesPerCall) {
            break;
        }
    }
    for (const int stid : wanted) {
        if (m_states.contains(stid)) {
            applyCachedState(stid);
        } else {
            m_failedStates.remove(stid);
            queueState(stid);
        }
    }
    submitNext();
}

void
RrPlaceService::applyStateReply(const Reply& reply) {
    const int stid = reply.key;
    if (!reply.ok) {
        m_failedStates.insert(stid);
        noteStateSettled(stid);
        return;
    }
    m_states.insert(stid, StateEntry{reply.stateName, reply.counties});
    m_failedStates.remove(stid);
    retryUnapplied();
    applyCachedState(stid);
    noteStateSettled(stid);
}

void
RrPlaceService::applyCachedState(int stid) {
    const auto entry = m_states.constFind(stid);
    if (m_systems.isNull() || entry == m_states.cend()) {
        return;
    }
    if (m_systems->applyPlaceNames(stid, entry->name, entry->counties)) {
        m_unappliedStates.remove(stid);
    } else {
        m_unappliedStates.insert(stid);
    }
}

void
RrPlaceService::retryUnapplied() {
    const QSet<int> pending = m_unappliedStates;
    for (const int stid : pending) {
        applyCachedState(stid);
    }
}

void
RrPlaceService::fillPlaceNames(QVariantMap& place) const {
    for (auto it = m_states.cbegin(); it != m_states.cend(); ++it) {
        (void)SavedSystemsModel::namePlace(place, it.key(), it->name, it->counties);
    }
}

void
RrPlaceService::onCredentialsChanged() {
    /* Signed out, or another account: nothing learned or asked under the old one carries over. */
    stopRequests();
    m_states.clear();
    m_failedStates.clear();
    m_unappliedStates.clear();
    if (m_updating) {
        finishUpdate(false);
    }
}

/* ------------------------------------------------------------------------- */
/* Update from RadioReference                                                 */
/* ------------------------------------------------------------------------- */

bool
RrPlaceService::updateFromRadioReference(const QStringList& uids) {
    if (!signedIn()) {
        return false;
    }
    if (m_updating || m_systems.isNull()) {
        return true;
    }
    Update update;
    QSet<QString> seen;
    for (const QString& uid : uids) {
        const int row = m_systems->rowForUid(uid);
        if (row < 0 || seen.contains(uid)) {
            continue;
        }
        seen.insert(uid);
        const QModelIndex index = m_systems->index(row);
        const int sid = m_systems->data(index, SavedSystemsModel::RrSidRole).toInt();
        if (site_grouped(sid, m_systems->data(index, SavedSystemsModel::RrSiteIdRole).toInt())) {
            if (!update.sids.contains(sid)) {
                update.sids.append(sid);
            }
            update.siteUids[sid].append(uid);
            update.uids.append(uid);
        } else if (!m_systems->data(index, SavedSystemsModel::PlaceRole).toMap().isEmpty()) {
            /* A conventional repeater list: no site to re-read, only its place names. */
            update.placeUids.append(uid);
            update.uids.append(uid);
        } else {
            continue;
        }
        update.total++;
    }
    m_update = update;
    m_updateSerial++;
    m_updating = true;
    m_summaryText.clear();
    Q_EMIT updatingChanged();
    Q_EMIT summaryTextChanged();
    setProgress();
    submitNext();
    return true;
}

void
RrPlaceService::cancelUpdate() {
    if (!m_updating) {
        return;
    }
    stopRequests();
    finishUpdate(false);
}

void
RrPlaceService::applySitesReply(int sid, bool ok, const QList<SiteRecord>& sites) {
    const QStringList uids = m_update.siteUids.value(sid);
    const quint64 serial = m_updateSerial;
    const QPair<int, int> counted = ok ? refreshSystemRows(sid, sites) : qMakePair(0, static_cast<int>(uids.size()));
    if (serial != m_updateSerial) {
        /* A handler of the saved rows' signals ended this update (or began another) while they were saved. */
        return;
    }
    m_update.updated += counted.first;
    m_update.unreachable += counted.second;
    m_update.done += static_cast<int>(uids.size());
    m_update.next++;
    m_update.detailsRead = false;
    m_update.stateIds.clear();
    setProgress();
}

QPair<int, int>
RrPlaceService::refreshSystemRows(int sid, const QList<SiteRecord>& sites) {
    QList<SavedSystemsModel::SiteRefresh> refreshes;
    int unlisted = 0;
    for (const QString& uid : m_update.siteUids.value(sid)) {
        const QVariantMap stored = m_systems->getByUid(uid);
        /* Gone, or edited onto another system since the update began: no longer this system's to refresh. */
        if (stored.value(QStringLiteral("rrSid")).toInt() != sid) {
            continue;
        }
        const int siteId = stored.value(QStringLiteral("rrSiteId")).toInt();
        const auto site = std::find_if(sites.cbegin(), sites.cend(),
                                       [siteId](const SiteRecord& record) { return record.siteId == siteId; });
        if (site == sites.cend()) {
            /* RadioReference no longer lists it: left as it is, and counted. */
            unlisted++;
            continue;
        }
        SavedSystemsModel::SiteRefresh refresh;
        refresh.uid = uid;
        refresh.rrSid = sid;
        refresh.rrSiteId = siteId;
        refresh.fields.insert(QStringLiteral("siteName"), site->name);
        refresh.fields.insert(QStringLiteral("hasSitePos"), site->positioned);
        refresh.fields.insert(QStringLiteral("siteLat"), site->lat);
        refresh.fields.insert(QStringLiteral("siteLon"), site->lon);
        refresh.fields.insert(
            kPlace, keep_known_names(rr_site_place({site->countyId}, m_update.stateIds), stored.value(kPlace).toMap()));
        refreshes.append(refresh);
    }
    const int applied = refreshes.isEmpty() ? 0 : m_systems->refreshSites(refreshes);
    /* A failed save counts its rows as not updated. */
    return applied < 0 ? qMakePair(0, unlisted + static_cast<int>(refreshes.size())) : qMakePair(applied, unlisted);
}

void
RrPlaceService::beginNamesPhase() {
    m_update.namesPhase = true;
    QList<int> stids;
    for (const QString& uid : m_update.uids) {
        const QVariantMap place = m_systems.isNull() ? QVariantMap() : m_systems->getByUid(uid).value(kPlace).toMap();
        if (!place_needs_names(place)) {
            continue;
        }
        for (const int stid : place_state_ids(place)) {
            if (!stids.contains(stid)) {
                stids.append(stid);
            }
        }
    }
    const quint64 serial = m_updateSerial;
    for (const int stid : stids) {
        if (m_states.contains(stid)) {
            applyCachedState(stid);
            if (serial != m_updateSerial) {
                return; /* ended from a handler of the saved rows' signals */
            }
        } else {
            m_failedStates.remove(stid);
            m_update.awaitedStates.insert(stid);
            queueState(stid);
        }
    }
    if (m_update.awaitedStates.isEmpty()) {
        finishUpdate(true);
    }
}

void
RrPlaceService::noteStateSettled(int stid) {
    if (m_updating && m_update.namesPhase && m_update.awaitedStates.remove(stid) && m_update.awaitedStates.isEmpty()) {
        finishUpdate(true);
    }
}

void
RrPlaceService::finishUpdate(bool namesDone) {
    if (namesDone && !m_systems.isNull()) {
        /* A row with no site counts as updated once every state its place names was answered. */
        for (const QString& uid : m_update.placeUids) {
            const QList<int> stids = place_state_ids(m_systems->getByUid(uid).value(kPlace).toMap());
            const bool reached =
                std::none_of(stids.cbegin(), stids.cend(), [this](int stid) { return m_failedStates.contains(stid); });
            (reached ? m_update.updated : m_update.unreachable)++;
        }
    }
    const int updated = m_update.updated;
    const int unreachable = m_update.unreachable;
    m_update = Update();
    m_updateSerial++;
    m_updating = false;
    m_summaryText = tr("Updated %1 systems.").arg(updated);
    if (unreachable > 0) {
        m_summaryText += QLatin1Char(' ') + tr("%1 could not be reached.").arg(unreachable);
    }
    setProgress();
    Q_EMIT updatingChanged();
    Q_EMIT summaryTextChanged();
    recountUnplaced();
    Q_EMIT updateFinished(updated, unreachable);
}

void
RrPlaceService::setProgress() {
    const QString text =
        m_updating ? tr("Updating %1 of %2").arg(qMin(m_update.done + 1, qMax(m_update.total, 1))).arg(m_update.total)
                   : QString();
    if (text != m_progressText) {
        m_progressText = text;
        Q_EMIT progressTextChanged();
    }
}

void
RrPlaceService::recountUnplaced() {
    int count = 0;
    if (!m_systems.isNull()) {
        for (int row = 0; row < m_systems->rowCount(); row++) {
            const QModelIndex index = m_systems->index(row);
            const QVariantMap place = m_systems->data(index, SavedSystemsModel::PlaceRole).toMap();
            const bool site = site_grouped(m_systems->data(index, SavedSystemsModel::RrSidRole).toInt(),
                                           m_systems->data(index, SavedSystemsModel::RrSiteIdRole).toInt());
            if ((site || !place.isEmpty()) && !place_named(place)) {
                count++;
            }
        }
    }
    if (count != m_unplacedCount) {
        m_unplacedCount = count;
        Q_EMIT unplacedCountChanged();
    }
}

} // namespace dsd_qt
