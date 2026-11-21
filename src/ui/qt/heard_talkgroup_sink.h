// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Where call history hands each verified talkgroup it ingests (issue #903).
 *
 * HeardTalkgroupStore implements it. The history only names the saved system that owns the call, the talkgroup's
 * variant (dsd_heard_tg_variant) and id, and when it was heard; whether it is kept is the sink's business.
 */

#ifndef DSD_NEO_SRC_UI_QT_HEARD_TALKGROUP_SINK_H_
#define DSD_NEO_SRC_UI_QT_HEARD_TALKGROUP_SINK_H_

#include <QString>
#include <QtGlobal>

namespace dsd_qt {

class HeardTalkgroupSink {
  public:
    virtual ~HeardTalkgroupSink() = default;

    /**
     * @brief Talkgroup @p id of @p variant was heard on saved system @p ownerUid at @p whenSecs.
     *
     * @param whenSecs Decode-domain wall seconds since the Unix epoch (the call's own stamp).
     */
    virtual void observe(const QString& ownerUid, int variant, quint32 id, qint64 whenSecs) = 0;
};

} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_HEARD_TALKGROUP_SINK_H_ */
