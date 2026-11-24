// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Which saved system owns a verified talkgroup that call history hears (issue #903).
 *
 * A History session (CallHistoryModel::session()) is told at its accepted start who owns its calls: the saved system
 * of a saved-system start, or for a scan-list start the saved system behind each scan target id. A row's owners are
 * those of the session that owns the ring generation it was pushed in: a decoder ring (Event_History_I::instance) is
 * bound to the session it is first read in, and a run that reuses the last run's state (its ring with it) opens a new
 * generation of that ring at the position read when its session began. So the last call of system A, read while B
 * starts, keeps A's owners, whatever session the UI has moved on to, and B's calls on A's reused ring are B's. A ring is
 * not read before a session owns it (the flush before a start leaves a ring no session owns unread). What the flush
 * after the host accepted a start finds pushed on a ring a session owns could be the old run's last commit or the new
 * run's first on the ring it reused: nothing tells which, so those rows are no one's (HeardRingWindow). Each row's own
 * stamps then decide: a call the operator tuned to, a scan member with no target, a target no single saved system owns,
 * or a row of no known session has no owner.
 */

#ifndef DSD_NEO_SRC_UI_QT_CALL_HISTORY_HEARD_H_
#define DSD_NEO_SRC_UI_QT_CALL_HISTORY_HEARD_H_

#include <QHash>
#include <QList>
#include <QString>
#include <QtGlobal>
#include <dsd-neo/core/state.h>
#include <limits>
#include <stdint.h>

namespace dsd_qt {

/** @brief Who owns the calls one History session hears. */
struct HeardSessionOwners {
    /* The saved system a saved-system start runs; empty for every other start. */
    QString single;
    /* For a scan-list start: scan target id (the entry's uid, Event_History::channel_label) to the saved system it
       runs. Only ids exactly one saved system gives, and no imported target file or frequency entry does. */
    QHash<QString, QString> scan;
};

/** @brief One generation of a decoder ring: the rows pushed in slot `s` after push stamp `from[s]` (up to the next
 * generation's) are History session `session`'s. A ring's first generation starts at 0. */
struct HeardRingGeneration {
    qint64 session = 0;
    quint64 from[2] = {0U, 0U};
};

/** @brief Where the ring read last stood when History session `session` began (heard_owners_mark_start()): the
 * position its own generation of that ring starts after, should its run have reused the ring. */
struct HeardRingMark {
    quint64 ring = 0U; // 0: none
    qint64 session = 0;
    quint64 through[2] = {0U, 0U};
};

/** @brief Rows of a decoder ring that no session owns: those pushed in slot `s` after push stamp `after[s]`, up to and
 * including `through[s]` (heard_owners_note_unowned()). */
struct HeardRingWindow {
    quint64 after[2] = {0U, 0U};
    quint64 through[2] = {0U, 0U};
};

bool operator==(const HeardRingGeneration& a, const HeardRingGeneration& b);
bool operator==(const HeardRingMark& a, const HeardRingMark& b);
bool operator==(const HeardRingWindow& a, const HeardRingWindow& b);

/** @brief The owners of the recent History sessions, the generations of each decoder ring (oldest first), the rows of
 * each ring no session owns, and the mark of the session begun last. */
struct HeardOwners {
    QHash<qint64, HeardSessionOwners> sessions;
    QHash<quint64, QList<HeardRingGeneration>> rings;
    QHash<quint64, QList<HeardRingWindow>> unowned;
    HeardRingMark mark;
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

/** @brief The sightings waiting for the sink: owner to entry key (variant << 32 | id) to its sighting. */
using HeardPending = QHash<QString, QHash<quint64, HeardSighting>>;

/** @brief The most sightings of one owner held for a sink not yet set (HeardTalkgroupStore::kMaxEntriesPerOwner). */
constexpr int kHeardPendingPerOwner = 2000;

/** @brief The session of a row no ring generation holds: none, so it has no owner. */
constexpr qint64 kHeardNoSession = std::numeric_limits<qint64>::min();

/** @brief Note that History session @p session begins with ring @p ring read last at @p through (per slot), or with no
 * ring read (@p ring 0, or one no session owns): its first read of that ring opens its generation there. */
void heard_owners_mark_start(HeardOwners* owners, qint64 session, quint64 ring, const quint64 through[2]);

/** @brief Whether ring @p ring may be read without binding it: a ring of no identity (0), or one a session owns. */
bool heard_owners_ring_bound(const HeardOwners& owners, quint64 ring);

/**
 * @brief Note that the rows ring @p ring pushed after @p after up to @p through (per slot) are no one's: what a read
 * between a start's accept and its session finds on a ring a session owns (an old run's last commits, or a new run's on
 * the ring it reused). A ring of no identity (0), one no session owns, or a window with nothing in it notes nothing.
 * @return whether anything was noted.
 */
bool heard_owners_note_unowned(HeardOwners* owners, quint64 ring, const quint64 after[2], const quint64 through[2]);

/**
 * @brief Bind ring @p ring as session @p session reads its slot @p slot at push count @p pushSeq: a ring no session
 * owns becomes @p session's from its start; the ring @p session's start marked (heard_owners_mark_start()), once the
 * slot has pushed past the mark (the run reused it), gets @p session's generation after the mark. Either consumes the
 * mark (a session's run has one ring); a read of the marked ring with nothing new leaves it. A ring of no identity
 * (0), or no @p owners, binds nothing.
 */
void heard_owners_bind_ring(HeardOwners* owners, quint64 ring, int slot, quint64 pushSeq, qint64 session);

/** @brief The session that owns the row ring @p ring pushed in slot @p slot at push stamp @p seq: its generation's,
 * kHeardNoSession for none and for a row no session owns (heard_owners_note_unowned()); @p session for a ring of no
 * identity (0) or no @p owners. */
qint64 heard_owners_session_of(const HeardOwners* owners, quint64 ring, int slot, quint64 seq, qint64 session);

/** @brief The session a call ring @p ring commits now would be read in: the one @p current's start marked it for, else
 * its newest generation's, else (an unbound ring, or none) @p current. */
qint64 heard_owners_on_air_session(const HeardOwners& owners, quint64 ring, qint64 current);

/** @brief Whether @p item carries a proof that may be learnt: a verified talkgroup, live input, not operator tuned. */
bool heard_item_learnable(const Event_History* item);

/**
 * @brief The saved system that owns a call heard in History session @p session, or empty for none: with
 * @p operatorTuned none; off any scan (@p channel empty, not @p scanMember) the session's single owner; on a scan
 * target @p channel of a session with no single owner, that target's owner; else none.
 */
QString heard_owner_for(const HeardOwners& owners, qint64 session, bool operatorTuned, bool scanMember,
                        const QString& channel);

/** @brief The saved system that owns @p item, heard in History session @p session, or empty for none
 * (heard_owner_for() with the item's own stamps). */
QString heard_owner_of(const HeardOwners& owners, qint64 session, const Event_History* item);

/** @brief Set @p session's owners, keeping the newest kHeardOwnerSessions sessions and their rings. */
void heard_owners_set_session(HeardOwners* owners, qint64 session, const HeardSessionOwners& who);

/** @brief Take in what the background tap bound in its copy @p tapped of @p owners: the ring generations @p owners
 * lacks, and the mark as the tap left it (the tap notes no unowned rows); @return whether anything changed. */
bool heard_owners_adopt_rings(HeardOwners* owners, const HeardOwners& tapped);

/** @brief @p owners as compact JSON text, for the settings. */
QString heard_owners_to_text(const HeardOwners& owners);

/** @brief What heard_owners_to_text() wrote; anything it cannot read is left out. */
HeardOwners heard_owners_from_text(const QString& text);

/** @brief A ring row's verified talkgroup proof: a variant and a non-zero id together, else none (0 and 0). */
struct HeardProof {
    uint8_t variant = 0U;
    quint32 id = 0U;
};

/** @brief The proof @p item carries (HeardProof). */
HeardProof heard_item_proof(const Event_History* item);

/** @brief Note @p id of @p variant heard on @p owner at @p when in @p pending. Each owner keeps its newest sightings,
 * as the store does: past twice kHeardPendingPerOwner, the oldest last heard go first (ties: lower variant, then lower
 * id) down to kHeardPendingPerOwner. */
void heard_pending_note(HeardPending* pending, const QString& owner, int variant, quint32 id, qint64 when);

} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_CALL_HISTORY_HEARD_H_ */
