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

class HeardTalkgroupSource {
  public:
    virtual ~HeardTalkgroupSource() = default;

    /**
     * @brief The distinct talkgroups of the voice rows logged by decode session @p forSession (0 = every session)
     *        on scan target @p channel (a row's ChannelRole id; empty: off any scan target) whose fragments the saved
     *        system @p owner owns (empty: the fragments no saved system owns), in no order.
     *
     * A fragment's owner is resolved where its ring was read, from the ring's own session (issue #903), whether or not
     * its talkgroup may be learnt. Ids of 0, ids that do not fit 32 bits and private calls' radios are left out.
     */
    virtual QList<HeardTalkgroupRow> heardTalkgroups(qint64 forSession, const QString& owner,
                                                     const QString& channel) const = 0;

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

#define DSD_QT_HEARD_TALKGROUP_SOURCE_IID "org.dsd-neo.ui.HeardTalkgroupSource/3"
Q_DECLARE_INTERFACE(dsd_qt::HeardTalkgroupSource, DSD_QT_HEARD_TALKGROUP_SOURCE_IID)

#endif /* DSD_NEO_SRC_UI_QT_HEARD_TALKGROUP_SOURCE_H_ */
