// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Which saved system owns a verified talkgroup that call history hears (issue #903).
 *
 * A History session (CallHistoryModel::session()) is told at its accepted start who owns its calls: the saved system
 * of a saved-system start, or for a scan-list start the saved system behind each scan target id. A decoder ring
 * (Event_History_I::instance) belongs to the session it was first read in, so a ring's late commits (the last call of
 * system A, published while B starts) keep A's owners, whatever session the UI has moved on to. Each row's own stamps
 * then decide: a call the operator tuned to, a scan member with no target, a target no single saved system owns, or a
 * row of no known session has no owner.
 */

#ifndef DSD_NEO_SRC_UI_QT_CALL_HISTORY_HEARD_H_
#define DSD_NEO_SRC_UI_QT_CALL_HISTORY_HEARD_H_

#include <QHash>
#include <QString>
#include <QtGlobal>
#include <dsd-neo/core/state.h>

namespace dsd_qt {

/** @brief Who owns the calls one History session hears. */
struct HeardSessionOwners {
    /* The saved system a saved-system start runs; empty for every other start. */
    QString single;
    /* For a scan-list start: scan target id (the entry's uid, Event_History::channel_label) to the saved system it
       runs. Only ids exactly one saved system gives, and no imported target file or frequency entry does. */
    QHash<QString, QString> scan;
};

/** @brief The owners of the recent History sessions, and the session each decoder ring was first read in. */
struct HeardOwners {
    QHash<qint64, HeardSessionOwners> sessions;
    QHash<quint64, qint64> rings;
};

/** @brief The most sessions (the newest) whose owners are kept. */
constexpr int kHeardOwnerSessions = 8;

/** @brief One talkgroup's observations waiting for the sink (CallHistoryModel::setHeardSink()). */
struct HeardSighting {
    QString owner;
    int variant = 0;
    quint32 id = 0;
    qint64 first = 0;
    qint64 last = 0;
};

/** @brief The most distinct talkgroups held for a sink not yet set; further ones are dropped. */
constexpr int kHeardPendingMax = 20000;

/**
 * @brief The session ring @p ring belongs to: the one it is bound to, else @p session, which it is bound to now. A ring
 * of no identity (0), or no @p owners, binds nothing and reads @p session.
 */
qint64 heard_owners_bind_ring(HeardOwners* owners, quint64 ring, qint64 session);

/** @brief Whether @p item carries a proof that may be learnt: a verified talkgroup, live input, not operator tuned. */
bool heard_item_learnable(const Event_History* item);

/** @brief The saved system that owns @p item, heard in History session @p session, or empty for none. */
QString heard_owner_of(const HeardOwners& owners, qint64 session, const Event_History* item);

/** @brief Set @p session's owners, keeping the newest kHeardOwnerSessions sessions and their rings. */
void heard_owners_set_session(HeardOwners* owners, qint64 session, const HeardSessionOwners& who);

/** @brief Bind each ring of @p rings @p owners has no session for (the background tap's); @return whether any was. */
bool heard_owners_adopt_rings(HeardOwners* owners, const QHash<quint64, qint64>& rings);

/** @brief @p owners as compact JSON text, for the settings. */
QString heard_owners_to_text(const HeardOwners& owners);

/** @brief What heard_owners_to_text() wrote; anything it cannot read is left out. */
HeardOwners heard_owners_from_text(const QString& text);

/** @brief Note @p id of @p variant heard on @p owner at @p when in @p pending (bounded by kHeardPendingMax). */
void heard_pending_note(QHash<QString, HeardSighting>* pending, const QString& owner, int variant, quint32 id,
                        qint64 when);

} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_CALL_HISTORY_HEARD_H_ */
