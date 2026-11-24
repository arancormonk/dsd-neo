// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "call_history_heard.h"

#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLatin1String>
#include <QList>
#include <QPair>
#include <QString>
#include <algorithm>
#include <iterator>
#include <limits>
#include <string.h>

namespace dsd_qt {

namespace {

/* Rings older sessions' calls can still come from: a run's late commits arrive while the next run starts, rarely more
   than one ring later. */
constexpr int kHeardOwnerRings = 32;

/* A ring generation's whole form in the settings: [session, from slot 0, from slot 1]. */
constexpr qsizetype kHeardGenerationFields = 3;

/* The newest session a ring's generations name. */
qint64
newest_session(const QList<HeardRingGeneration>& generations) {
    return generations.isEmpty() ? std::numeric_limits<qint64>::min() : generations.constLast().session;
}

/* Drop the generations of sessions older than every session kept, a ring left with none, then the rings whose newest
   session is oldest past the bound. */
void
bound_rings(HeardOwners* owners) {
    if (!owners->sessions.isEmpty()) {
        const QList<qint64> kept = owners->sessions.keys();
        const qint64 oldest = *std::min_element(kept.cbegin(), kept.cend());
        for (auto it = owners->rings.begin(); it != owners->rings.end();) {
            it->removeIf([oldest](const HeardRingGeneration& generation) { return generation.session < oldest; });
            it = it->isEmpty() ? owners->rings.erase(it) : std::next(it);
        }
    }
    while (owners->rings.size() > kHeardOwnerRings) {
        auto oldest = owners->rings.begin();
        for (auto it = owners->rings.begin(); it != owners->rings.end(); ++it) {
            if (newest_session(it.value()) < newest_session(oldest.value())) {
                oldest = it;
            }
        }
        owners->rings.erase(oldest);
    }
}

/* Keep the newest kHeardOwnerSessions sessions. */
void
prune_sessions(HeardOwners* owners) {
    while (owners->sessions.size() > kHeardOwnerSessions) {
        const QList<qint64> kept = owners->sessions.keys();
        owners->sessions.remove(*std::min_element(kept.cbegin(), kept.cend()));
    }
}

QString
channel_of(const Event_History* item) {
    return QString::fromUtf8(item->channel_label,
                             static_cast<qsizetype>(strnlen(item->channel_label, sizeof(item->channel_label))));
}

/* A generation as heard_owners_to_text() writes it; false for anything else. */
bool
generation_from_json(const QJsonValue& value, HeardRingGeneration* generation) {
    const QJsonArray fields = value.toArray();
    if (fields.size() != kHeardGenerationFields) {
        return false;
    }
    if (!std::all_of(fields.cbegin(), fields.cend(),
                     [](const QJsonValue& field) { return field.isDouble() && field.toInteger(-1) >= 0; })) {
        return false;
    }
    generation->session = fields.at(0).toInteger();
    generation->from[0] = static_cast<quint64>(fields.at(1).toInteger());
    generation->from[1] = static_cast<quint64>(fields.at(2).toInteger());
    return true;
}

/* A ring's generations as heard_owners_to_text() writes them, oldest first; empty for anything else. */
QList<HeardRingGeneration>
generations_from_json(const QJsonValue& value) {
    QList<HeardRingGeneration> generations;
    for (const auto& entry : value.toArray()) {
        HeardRingGeneration generation;
        if (!generation_from_json(entry, &generation)
            || (!generations.isEmpty() && generation.session <= generations.constLast().session)) {
            return {};
        }
        generations.append(generation);
    }
    return generations;
}

QJsonValue
mark_to_json(const HeardRingMark& mark) {
    if (mark.ring == 0U) {
        return QJsonValue();
    }
    return QJsonObject{
        {QLatin1String("r"), QString::number(mark.ring, 16)},
        {QLatin1String("n"), mark.session},
        {QLatin1String("t"), QJsonArray{static_cast<qint64>(mark.through[0]), static_cast<qint64>(mark.through[1])}}};
}

HeardRingMark
mark_from_json(const QJsonValue& value) {
    const QJsonObject entry = value.toObject();
    const QJsonArray through = entry.value(QLatin1String("t")).toArray();
    bool ok = false;
    const quint64 ring = entry.value(QLatin1String("r")).toString().toULongLong(&ok, 16);
    HeardRingMark mark;
    if (!ok || ring == 0U || !entry.value(QLatin1String("n")).isDouble() || through.size() != 2
        || through.at(0).toInteger(-1) < 0 || through.at(1).toInteger(-1) < 0) {
        return mark;
    }
    mark.ring = ring;
    mark.session = entry.value(QLatin1String("n")).toInteger();
    mark.through[0] = static_cast<quint64>(through.at(0).toInteger());
    mark.through[1] = static_cast<quint64>(through.at(1).toInteger());
    return mark;
}

} // namespace

bool
operator==(const HeardRingGeneration& a, const HeardRingGeneration& b) {
    return a.session == b.session && a.from[0] == b.from[0] && a.from[1] == b.from[1];
}

bool
operator==(const HeardRingMark& a, const HeardRingMark& b) {
    return a.ring == b.ring && a.session == b.session && a.through[0] == b.through[0] && a.through[1] == b.through[1];
}

void
heard_owners_mark_start(HeardOwners* owners, qint64 session, quint64 ring, const quint64 through[2]) {
    owners->mark = HeardRingMark();
    if (ring == 0U || !owners->rings.contains(ring)) {
        return;
    }
    owners->mark.ring = ring;
    owners->mark.session = session;
    owners->mark.through[0] = through[0];
    owners->mark.through[1] = through[1];
}

bool
heard_owners_ring_bound(const HeardOwners& owners, quint64 ring) {
    return ring == 0U || owners.rings.contains(ring);
}

void
heard_owners_bind_ring(HeardOwners* owners, quint64 ring, int slot, quint64 pushSeq, qint64 session) {
    if (owners == nullptr || ring == 0U || slot < 0 || slot > 1) {
        return;
    }
    const bool marked = owners->mark.ring != 0U && owners->mark.session == session;
    const auto bound = owners->rings.find(ring);
    if (bound == owners->rings.end()) {
        owners->rings.insert(ring, {HeardRingGeneration{session, {0U, 0U}}});
    } else if (marked && owners->mark.ring == ring && pushSeq > owners->mark.through[slot]) {
        // The run reuses the ring the last read found: what it pushes after that read is its own.
        if (newest_session(bound.value()) < session) {
            bound->append(HeardRingGeneration{session, {owners->mark.through[0], owners->mark.through[1]}});
        }
    } else {
        return;
    }
    if (marked) {
        owners->mark = HeardRingMark();
    }
    bound_rings(owners);
}

qint64
heard_owners_session_of(const HeardOwners* owners, quint64 ring, int slot, quint64 seq, qint64 session) {
    if (owners == nullptr || ring == 0U) {
        return session;
    }
    const auto bound = owners->rings.constFind(ring);
    if (bound == owners->rings.constEnd() || slot < 0 || slot > 1) {
        return kHeardNoSession;
    }
    for (auto it = bound->crbegin(); it != bound->crend(); ++it) {
        if (seq > it->from[slot]) {
            return it->session;
        }
    }
    return kHeardNoSession;
}

qint64
heard_owners_on_air_session(const HeardOwners& owners, quint64 ring, qint64 current) {
    if (ring == 0U || (owners.mark.ring == ring && owners.mark.session == current)) {
        return current;
    }
    const auto bound = owners.rings.constFind(ring);
    return bound == owners.rings.constEnd() || bound->isEmpty() ? current : bound->constLast().session;
}

HeardProof
heard_item_proof(const Event_History* item) {
    HeardProof proof;
    if (item->heard_variant != 0U && item->heard_id != 0U) {
        proof.variant = item->heard_variant;
        proof.id = item->heard_id;
    }
    return proof;
}

bool
heard_item_learnable(const Event_History* item) {
    return item->heard_variant != 0U && item->heard_id != 0U && item->live_input != 0U && item->operator_tuned == 0U;
}

QString
heard_owner_for(const HeardOwners& owners, qint64 session, bool operatorTuned, bool scanMember,
                const QString& channel) {
    const auto who = owners.sessions.constFind(session);
    if (who == owners.sessions.constEnd() || operatorTuned) {
        return {};
    }
    // A call heard off any scan is the saved system's; one a scan heard is its target's, when one saved system owns it
    // (an unnamed -Y row, or any scan of a saved-system start, is no one's).
    if (channel.isEmpty()) {
        return scanMember ? QString() : who->single;
    }
    return who->single.isEmpty() ? who->scan.value(channel) : QString();
}

QString
heard_owner_of(const HeardOwners& owners, qint64 session, const Event_History* item) {
    return heard_owner_for(owners, session, item->operator_tuned != 0U, item->scan_member != 0U, channel_of(item));
}

void
heard_owners_set_session(HeardOwners* owners, qint64 session, const HeardSessionOwners& who) {
    owners->sessions.insert(session, who);
    prune_sessions(owners);
    bound_rings(owners);
}

bool
heard_owners_adopt_rings(HeardOwners* owners, const HeardOwners& tapped) {
    bool adopted = false;
    for (auto it = tapped.rings.constBegin(); it != tapped.rings.constEnd(); ++it) {
        if (it.key() == 0U || it->isEmpty()) {
            continue;
        }
        QList<HeardRingGeneration>& generations = owners->rings[it.key()];
        for (const HeardRingGeneration& generation : it.value()) {
            if (!generations.contains(generation)) {
                generations.append(generation);
                adopted = true;
            }
        }
        std::sort(generations.begin(), generations.end(),
                  [](const HeardRingGeneration& a, const HeardRingGeneration& b) { return a.session < b.session; });
    }
    // The model changes no session while the tap runs (it hands the tap off first): a mark the tap left otherwise is
    // one its reads consumed.
    if (!(owners->mark == tapped.mark)) {
        owners->mark = tapped.mark;
        adopted = true;
    }
    if (adopted) {
        bound_rings(owners);
    }
    return adopted;
}

QString
heard_owners_to_text(const HeardOwners& owners) {
    QJsonArray sessions;
    for (auto it = owners.sessions.constBegin(); it != owners.sessions.constEnd(); ++it) {
        QJsonObject scan;
        for (auto target = it->scan.constBegin(); target != it->scan.constEnd(); ++target) {
            scan.insert(target.key(), target.value());
        }
        sessions.append(QJsonObject{
            {QLatin1String("n"), it.key()}, {QLatin1String("one"), it->single}, {QLatin1String("scan"), scan}});
    }
    // Ring identities in hex: a nonce can use all 64 bits, more than a JSON number holds exactly.
    QJsonObject rings;
    for (auto it = owners.rings.constBegin(); it != owners.rings.constEnd(); ++it) {
        QJsonArray generations;
        for (const HeardRingGeneration& generation : it.value()) {
            generations.append(QJsonArray{generation.session, static_cast<qint64>(generation.from[0]),
                                          static_cast<qint64>(generation.from[1])});
        }
        rings.insert(QString::number(it.key(), 16), generations);
    }
    QJsonObject root{{QLatin1String("s"), sessions}, {QLatin1String("r"), rings}};
    const QJsonValue mark = mark_to_json(owners.mark);
    if (!mark.isNull()) {
        root.insert(QLatin1String("m"), mark);
    }
    return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact));
}

HeardOwners
heard_owners_from_text(const QString& text) {
    HeardOwners owners;
    const QJsonObject root = QJsonDocument::fromJson(text.toUtf8()).object();
    for (const auto& value : root.value(QLatin1String("s")).toArray()) {
        const QJsonObject entry = value.toObject();
        const QJsonValue session = entry.value(QLatin1String("n"));
        if (!session.isDouble()) {
            continue;
        }
        HeardSessionOwners who;
        who.single = entry.value(QLatin1String("one")).toString();
        const QJsonObject scan = entry.value(QLatin1String("scan")).toObject();
        for (auto it = scan.constBegin(); it != scan.constEnd(); ++it) {
            if (!it.key().isEmpty() && !it.value().toString().isEmpty()) {
                who.scan.insert(it.key(), it.value().toString());
            }
        }
        owners.sessions.insert(session.toInteger(), who);
    }
    const QJsonObject rings = root.value(QLatin1String("r")).toObject();
    for (auto it = rings.constBegin(); it != rings.constEnd(); ++it) {
        bool ok = false;
        const quint64 ring = it.key().toULongLong(&ok, 16);
        const QList<HeardRingGeneration> generations = generations_from_json(it.value());
        if (ok && ring != 0U && !generations.isEmpty()) {
            owners.rings.insert(ring, generations);
        }
    }
    owners.mark = mark_from_json(root.value(QLatin1String("m")));
    prune_sessions(&owners);
    bound_rings(&owners);
    return owners;
}

void
heard_pending_note(HeardPending* pending, const QString& owner, int variant, quint32 id, qint64 when) {
    QHash<quint64, HeardSighting>& sightings = (*pending)[owner];
    const quint64 key = (static_cast<quint64>(static_cast<quint32>(variant)) << 32) | id;
    const auto held = sightings.find(key);
    if (held != sightings.end()) {
        held->first = qMin(held->first, when);
        held->last = qMax(held->last, when);
        return;
    }
    HeardSighting sighting;
    sighting.owner = owner;
    sighting.variant = variant;
    sighting.id = id;
    sighting.first = when;
    sighting.last = when;
    sightings.insert(key, sighting);
    if (sightings.size() <= qsizetype{2} * kHeardPendingPerOwner) {
        return;
    }
    // Down to the cap at once, so a long spill pays for one selection per kHeardPendingPerOwner new talkgroups. What
    // goes has the cap's worth of newer ones, which only grow newer: the store would keep none of it either.
    QList<QPair<qint64, quint64>> order;
    order.reserve(sightings.size());
    for (auto it = sightings.cbegin(); it != sightings.cend(); ++it) {
        order.append(qMakePair(it->last, it.key()));
    }
    const qsizetype excess = sightings.size() - kHeardPendingPerOwner;
    std::nth_element(order.begin(), order.begin() + (excess - 1), order.end());
    for (qsizetype i = 0; i < excess; ++i) {
        sightings.remove(order.at(i).second);
    }
}

} // namespace dsd_qt
