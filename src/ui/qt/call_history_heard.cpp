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
#include <algorithm>
#include <iterator>
#include <string.h>

namespace dsd_qt {

namespace {

/* Rings older sessions' calls can still come from: a run's late commits arrive while the next run starts, rarely more
   than one ring later. */
constexpr int kHeardOwnerRings = 32;

QString
pending_key(const QString& owner, int variant, quint32 id) {
    return owner + QLatin1Char('\n') + QString::number(variant) + QLatin1Char('\n') + QString::number(id);
}

/* Drop the rings bound to sessions older than every session kept, then the oldest sessions' rings past the bound. */
void
bound_rings(HeardOwners* owners) {
    if (!owners->sessions.isEmpty()) {
        const QList<qint64> kept = owners->sessions.keys();
        const qint64 oldest = *std::min_element(kept.cbegin(), kept.cend());
        for (auto it = owners->rings.begin(); it != owners->rings.end();) {
            it = it.value() < oldest ? owners->rings.erase(it) : std::next(it);
        }
    }
    while (owners->rings.size() > kHeardOwnerRings) {
        auto oldest = owners->rings.begin();
        for (auto it = owners->rings.begin(); it != owners->rings.end(); ++it) {
            if (it.value() < oldest.value()) {
                oldest = it;
            }
        }
        owners->rings.erase(oldest);
    }
}

QString
channel_of(const Event_History* item) {
    return QString::fromUtf8(item->channel_label,
                             static_cast<qsizetype>(strnlen(item->channel_label, sizeof(item->channel_label))));
}

} // namespace

qint64
heard_owners_bind_ring(HeardOwners* owners, quint64 ring, qint64 session) {
    if (owners == nullptr || ring == 0U) {
        return session;
    }
    const auto bound = owners->rings.constFind(ring);
    if (bound != owners->rings.constEnd()) {
        return bound.value();
    }
    owners->rings.insert(ring, session);
    bound_rings(owners);
    return session;
}

bool
heard_item_learnable(const Event_History* item) {
    return item->heard_variant != 0U && item->heard_id != 0U && item->live_input != 0U && item->operator_tuned == 0U;
}

QString
heard_owner_of(const HeardOwners& owners, qint64 session, const Event_History* item) {
    const auto who = owners.sessions.constFind(session);
    if (who == owners.sessions.constEnd() || item->operator_tuned != 0U) {
        return {};
    }
    // A call heard off any scan is the saved system's; one a scan heard is its target's, when one saved system owns it
    // (an unnamed -Y row, or any scan of a saved-system start, is no one's).
    if (item->channel_label[0] == '\0') {
        return item->scan_member == 0U ? who->single : QString();
    }
    return who->single.isEmpty() ? who->scan.value(channel_of(item)) : QString();
}

void
heard_owners_set_session(HeardOwners* owners, qint64 session, const HeardSessionOwners& who) {
    owners->sessions.insert(session, who);
    while (owners->sessions.size() > kHeardOwnerSessions) {
        const QList<qint64> kept = owners->sessions.keys();
        owners->sessions.remove(*std::min_element(kept.cbegin(), kept.cend()));
    }
    bound_rings(owners);
}

bool
heard_owners_adopt_rings(HeardOwners* owners, const QHash<quint64, qint64>& rings) {
    bool adopted = false;
    for (auto it = rings.constBegin(); it != rings.constEnd(); ++it) {
        if (it.key() != 0U && !owners->rings.contains(it.key())) {
            owners->rings.insert(it.key(), it.value());
            adopted = true;
        }
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
        rings.insert(QString::number(it.key(), 16), it.value());
    }
    const QJsonObject root{{QLatin1String("s"), sessions}, {QLatin1String("r"), rings}};
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
        if (ok && ring != 0U && it.value().isDouble()) {
            owners.rings.insert(ring, it.value().toInteger());
        }
    }
    while (owners.sessions.size() > kHeardOwnerSessions) {
        const QList<qint64> kept = owners.sessions.keys();
        owners.sessions.remove(*std::min_element(kept.cbegin(), kept.cend()));
    }
    bound_rings(&owners);
    return owners;
}

void
heard_pending_note(QHash<QString, HeardSighting>* pending, const QString& owner, int variant, quint32 id, qint64 when) {
    const QString key = pending_key(owner, variant, id);
    const auto held = pending->find(key);
    if (held != pending->end()) {
        held->first = qMin(held->first, when);
        held->last = qMax(held->last, when);
        return;
    }
    if (pending->size() >= kHeardPendingMax) {
        return;
    }
    HeardSighting sighting;
    sighting.owner = owner;
    sighting.variant = variant;
    sighting.id = id;
    sighting.first = when;
    sighting.last = when;
    pending->insert(key, sighting);
}

} // namespace dsd_qt
