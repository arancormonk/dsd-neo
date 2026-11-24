// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Typed read of the talkgroups a history heard, for TalkgroupListModel.
 *
 * The talkgroup list reads its heard set from the whole history on every history change.
 * Through the item-model interface that is three QVariant data() lookups per row, which at a
 * full log (1000 rows) cost the UI thread milliseconds per call commit (issue #718). A history
 * that implements this interface (CallHistoryModel) hands over the ids from its own row storage.
 * It is a Qt interface (qobject_cast), so the talkgroup list does not link the history: a history
 * model without it, such as a QML test fixture, is still read through its role names.
 */

#ifndef DSD_NEO_SRC_UI_QT_HEARD_TALKGROUP_SOURCE_H_
#define DSD_NEO_SRC_UI_QT_HEARD_TALKGROUP_SOURCE_H_

#include <QList>
#include <QObject> // IWYU pragma: keep -- Q_DECLARE_INTERFACE
#include <QString>
#include <QtGlobal>
#include <stdint.h>

namespace dsd_qt {

/** @brief One talkgroup a history heard (HeardTalkgroupSource::heardTalkgroups()). */
struct HeardTalkgroupRow {
    uint32_t id = 0;
    /* The newest of its fragments' times (each its end, else its start), in decode-domain wall seconds. */
    qint64 last = 0;
    /* The newest of its fragments' ingest serials (HeardTalkgroupSource::heardSerial()); 0 for a fragment this process
       did not ingest (one the saved log reloaded). */
    quint64 serial = 0;
};

/**
 * @brief The scan target a session's heard talkgroups are scoped to (HeardTalkgroupSource::heardTalkgroups()): a
 *        trunk-scan target by its id, a `-Y` scan-list row by the row itself (its label is only a display name: two
 *        rows may share one, a frequency, or neither), or none, off any scan target (a paused scan among them).
 */
struct HeardTarget {
    /* The channel label a call heard on it takes (a row's ChannelRole): the trunk-scan target id, the -Y row's name
       (empty for an unnamed row), empty off any scan target. */
    QString channel;
    /* The -Y scan-list row a call heard on it is stamped with (Event_History::scan_list_row, scan_list_rev): 1 + the
       row and its list's identity; 0 and 0 off any -Y row. */
    quint32 scanListRow = 0;
    quint64 scanListRev = 0;
};

inline bool
operator==(const HeardTarget& a, const HeardTarget& b) {
    return a.channel == b.channel && a.scanListRow == b.scanListRow && a.scanListRev == b.scanListRev;
}

inline bool
operator!=(const HeardTarget& a, const HeardTarget& b) {
    return !(a == b);
}

class HeardTalkgroupSource {
  public:
    virtual ~HeardTalkgroupSource() = default;

    /**
     * @brief The distinct talkgroups of the voice rows logged by decode session @p forSession (0 = every session)
     *        on scan target @p target (a row's ChannelRole id, and the -Y scan-list row each fragment was heard on,
     *        none off it; empty: off any scan target) whose fragments the saved system @p owner owns (empty: the fragments no
     *        saved system owns), in no order.
     *
     * A fragment's owner is resolved where its ring was read, from the ring's own session (issue #903), whether or not
     * its talkgroup may be learnt. Ids of 0, ids that do not fit 32 bits and private calls' radios are left out.
     */
    virtual QList<HeardTalkgroupRow> heardTalkgroups(qint64 forSession, const QString& owner,
                                                     const HeardTarget& target) const = 0;

    /** @brief The newest ingest serial given (0 before any): a fragment ingested later has a greater one. */
    virtual quint64 heardSerial() const = 0;

    /**
     * @brief The saved system a call heard now would be owned by (empty for none), by the owner rule a fragment's owner
     *        follows: in the session decoder ring @p ring (Event_History_I::instance; 0: none) is bound to, else the
     *        current one, with the tuner stamps a call would take now (@p operatorTuned, @p scanMember, @p channel).
     */
    virtual QString heardOwnerOnAir(quint64 ring, bool operatorTuned, bool scanMember,
                                    const QString& channel) const = 0;
};

} // namespace dsd_qt

#define DSD_QT_HEARD_TALKGROUP_SOURCE_IID "org.dsd-neo.ui.HeardTalkgroupSource/5"
Q_DECLARE_INTERFACE(dsd_qt::HeardTalkgroupSource, DSD_QT_HEARD_TALKGROUP_SOURCE_IID)

#endif /* DSD_NEO_SRC_UI_QT_HEARD_TALKGROUP_SOURCE_H_ */
