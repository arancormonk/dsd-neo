// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "call_history_playlist.h"

#include <QDateTime>
#include <QList>
#include <QMap>
#include <QModelIndex>
#include <QString>
#include <QVariant>
#include <QVariantList>
#include <QtGlobal>
#include <cmath>
#include <utility>

#include "call_history_filter.h"
#include "call_history_model.h"
#include "decoder_host.h"

namespace dsd_qt {

namespace {

QVariantMap
outcome(const char* result, int generation = 0, int calls = 0) {
    return {{QStringLiteral("result"), QString::fromLatin1(result)},
            {QStringLiteral("generation"), generation},
            {QStringLiteral("calls"), calls}};
}

/* The uid QML passes as a JS number: 0 below one half (from the oldest), the nearest whole uid above, -1 for one that
 * names no row (not a number, negative, or past what a double holds exactly). */
qint64
start_uid_of(double startUid) {
    constexpr double kExactLimit = 9007199254740992.0; // 2^53
    if (!std::isfinite(startUid) || startUid < 0.0 || startUid >= kExactLimit) {
        return -1;
    }
    return startUid < 0.5 ? 0 : static_cast<qint64>(std::llround(startUid));
}

/* The source rows @p view shows, oldest first: the view never sorts, so its rows come newest first, as the model's. */
QList<int>
shown_rows_oldest_first(const CallHistoryFilterModel& view) {
    const int shown = view.rowCount();
    QList<int> rows;
    rows.reserve(shown);
    for (int row = shown - 1; row >= 0; row--) {
        const QModelIndex source = view.mapToSource(view.index(row, 0));
        if (source.isValid()) {
            rows.append(source.row());
        }
    }
    return rows;
}

/* A recording the player can take now: on storage that is there and read, with no delete waiting for it. */
bool
playable(const QVariantMap& recording) {
    return recording.value(QStringLiteral("available")).toBool()
           && !recording.value(QStringLiteral("deleteQueued")).toBool()
           && !recording.value(QStringLiteral("path")).toString().isEmpty();
}

/* The line under a call's name (the notification's subtitle): its start in local time as the detail sheet's share
 * title says it ("2026-11-24 12:04"), a date that stays true however long the playlist runs; empty with no start. */
QString
call_line(const CallHistoryModel::PlaylistRow& row) {
    return row.when > 0 ? QDateTime::fromSecsSinceEpoch(row.when).toString(QStringLiteral("yyyy-MM-dd hh:mm"))
                        : QString();
}

/* @p row's playable recordings appended to @p items as call @p call, each with its place among all the row's
 * recordings (a three-part call with only part 2 playable plays "part 2 of 3"); how many were appended. */
int
append_call(QVariantList& items, const CallHistoryModel::PlaylistRow& row, int call) {
    const int count = static_cast<int>(row.recordings.size());
    const QString line = call_line(row);
    int appended = 0;
    for (int part = 0; part < count; part++) {
        const QVariantMap recording = row.recordings.at(part).toMap();
        if (!playable(recording)) {
            continue;
        }
        items.append(QVariantMap{
            {QStringLiteral("path"), recording.value(QStringLiteral("path"))},
            {QStringLiteral("size"), recording.value(QStringLiteral("identitySize"))},
            {QStringLiteral("mtimeSec"), recording.value(QStringLiteral("mtimeSec"))},
            {QStringLiteral("mtimeNanos"), recording.value(QStringLiteral("mtimeNanos"))},
            {QStringLiteral("call"), call},
            {QStringLiteral("partOrdinal"), part + 1},
            {QStringLiteral("partCount"), count},
            {QStringLiteral("title"), row.name},
            {QStringLiteral("line"), line},
            {QStringLiteral("key"), row.uid},
        });
        appended++;
    }
    return appended;
}

/* Every playable recording of @p rows appended to @p items, the calls numbered from 0 in order; how many calls, or -1
 * when @p fromRow (Play from here) and the first row, the start, has none. */
int
append_calls(QVariantList& items, const QList<CallHistoryModel::PlaylistRow>& rows, bool fromRow) {
    int calls = 0;
    for (qsizetype i = 0; i < rows.size(); i++) {
        if (append_call(items, rows.at(i), calls) > 0) {
            calls++;
        } else if (i == 0 && fromRow) {
            return -1;
        }
    }
    return calls;
}

} // namespace

CallHistoryPlaylist::CallHistoryPlaylist(const CallHistoryFilterModel* view, const CallHistoryModel* model,
                                         DecoderHost* host, QObject* parent)
    : QObject(parent), m_view(view), m_model(model), m_host(host) {}

QVariantMap
CallHistoryPlaylist::start(double startUid) {
    // Each refusal before the host is asked changes nothing: what plays plays on.
    if (m_host == nullptr || !m_host->playbackSupported()) {
        return outcome("unsupported");
    }
    const qint64 uid = start_uid_of(startUid);
    if (m_view == nullptr || m_model == nullptr || m_view->sourceModel() != m_model) {
        return outcome(uid != 0 ? "gone" : "none");
    }
    const QList<int> shown = shown_rows_oldest_first(*m_view);
    const qsizetype from = uid == 0 ? 0 : shown.indexOf(m_model->rowForUid(uid));
    if (from < 0) {
        return outcome("gone");
    }
    // Every row from the start on, read in one store read.
    const QList<CallHistoryModel::PlaylistRow> rows = m_model->playlistRows(shown.mid(from));
    QVariantList items;
    const int calls = append_calls(items, rows, uid > 0);
    if (calls <= 0) {
        return outcome(uid > 0 ? "rowNone" : "none");
    }
    const int generation = m_host->playRecordings(items, DecoderHost::PlaybackPlaylist, QStringLiteral("history"));
    return generation > 0 ? outcome("accepted", generation, calls) : outcome("refused");
}

} // namespace dsd_qt
