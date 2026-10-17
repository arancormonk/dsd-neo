// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Typed read of the talkgroups a history heard, for TalkgroupListModel.
 *
 * The talkgroup list rebuilds its heard set from the whole history on every history change.
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
#include <QtGlobal>
#include <stdint.h>

namespace dsd_qt {

class HeardTalkgroupSource {
  public:
    virtual ~HeardTalkgroupSource() = default;

    /**
     * @brief The distinct talkgroups of the voice rows logged by decode session @p forSession
     *        (0 = every session), newest row first.
     *
     * Ids of 0, and ids that do not fit 32 bits, are left out.
     */
    virtual QList<uint32_t> heardTalkgroups(qint64 forSession) const = 0;
};

} // namespace dsd_qt

#define DSD_QT_HEARD_TALKGROUP_SOURCE_IID "org.dsd-neo.ui.HeardTalkgroupSource/1"
Q_DECLARE_INTERFACE(dsd_qt::HeardTalkgroupSource, DSD_QT_HEARD_TALKGROUP_SOURCE_IID)

#endif /* DSD_NEO_SRC_UI_QT_HEARD_TALKGROUP_SOURCE_H_ */
