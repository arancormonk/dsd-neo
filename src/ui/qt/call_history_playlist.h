// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief History's playlist (context property `historyPlaylist`, issue #909): the recorded calls a History view shows,
 * played one after another.
 *
 * A press (Play all, Play from here) takes the rows the view shows at that moment, oldest first, reads every row's
 * recordings in one store read (CallHistoryModel::playlistRows()), and hands the host the playable ones in order
 * (DecoderHost::playRecordings(), PlaybackPlaylist). Fixed at the press: later filters, merges and new calls do not
 * change it. It holds no files: a recording gone or changed by its turn is skipped by the player. GUI thread only.
 */

#ifndef DSD_NEO_SRC_UI_QT_CALL_HISTORY_PLAYLIST_H_
#define DSD_NEO_SRC_UI_QT_CALL_HISTORY_PLAYLIST_H_

#include <QObject>
#include <QVariantMap>

namespace dsd_qt {

class CallHistoryFilterModel;
class CallHistoryModel;
class DecoderHost;

class CallHistoryPlaylist : public QObject {
    Q_OBJECT

  public:
    /** @brief The playlist of the rows @p view (a filter over @p model) shows, played through @p host. */
    CallHistoryPlaylist(const CallHistoryFilterModel* view, const CallHistoryModel* model, DecoderHost* host,
                        QObject* parent);

    CallHistoryPlaylist(const CallHistoryPlaylist&) = delete;
    CallHistoryPlaylist& operator=(const CallHistoryPlaylist&) = delete;

    /**
     * @brief Play the recorded calls the view shows now, oldest first, from the row whose uid is @p startUid (0: from
     *        the oldest).
     *
     * A call plays each of its recordings that can be played now (its storage there and read, no delete waiting),
     * oldest first; a call with none is left out.
     *
     * @return `{result, generation, calls}`: result `accepted` (the host took it: generation above 0, calls the
     *         number of calls in it), `none` (no shown call has a recording that can be played now), `gone` (no shown
     *         row has @p startUid), `rowNone` (the row of @p startUid is shown but has no recording that can be played
     *         now), `unsupported` (the host has no player) or `refused` (the host has a player but did not take the
     *         items: generation 0). Every result but `accepted` leaves what plays playing, with generation and calls
     *         0. Each item's `line` is the call's start in local time, "yyyy-MM-dd hh:mm", empty with no start.
     */
    Q_INVOKABLE QVariantMap start(double startUid);

  private:
    const CallHistoryFilterModel* m_view;
    const CallHistoryModel* m_model;
    DecoderHost* m_host;
};

} // namespace dsd_qt

#endif // DSD_NEO_SRC_UI_QT_CALL_HISTORY_PLAYLIST_H_
