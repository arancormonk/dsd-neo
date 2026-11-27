// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief What the call history asks of the call recording store (issue #708).
 *
 * History rows never store recordings. A row knows the ring fragments it absorbed (CallHistoryModel::Row::fragments),
 * and the store knows which fragment each recording belongs to (the recording journal's `instance`, `slot` and `row`).
 * The history's recording roles ask the store by fragment, and the store tells the history which fragments changed
 * (CallHistoryModel::recordingFragmentsChanged()). A journal line read before its row exists waits in the store and
 * shows once the row arrives; a deleted recording leaves the store, so nothing in History can bring it back.
 */

#ifndef DSD_NEO_SRC_UI_QT_CALL_RECORDING_PROVIDER_H_
#define DSD_NEO_SRC_UI_QT_CALL_RECORDING_PROVIDER_H_

#include <QChar>
#include <QList>
#include <QString>
#include <QStringList>
#include <QVariant> // IWYU pragma: keep -- QVariantList's element; IWYU's answer differs by TU.
#include <QVariantList>
#include <QtGlobal>

namespace dsd_qt {

/**
 * @brief The key of one ring fragment: the ring (Event_History_I::instance), the slot and the row's push stamp.
 *
 * `<16 uppercase hex instance>:<slot>:<decimal stamp>`, the journal's `instance`, `slot` and `row` fields joined, so
 * the store keys a `publish` or `link` record without knowing anything else about History.
 */
inline QString
call_history_fragment_key(quint64 ring, int slot, qulonglong stamp) {
    return QStringLiteral("%1:%2:%3").arg(ring, 16, 16, QLatin1Char('0')).toUpper().arg(slot).arg(stamp);
}

/**
 * @brief The recordings of a History row's fragments, as the call recording store indexes them.
 *
 * Read on the GUI thread, from the history's data(). The history holds a provider it does not own; without one
 * (before the store is wired, and in tests) every row reports no recordings.
 */
class CallRecordingProvider {
  public:
    virtual ~CallRecordingProvider() = default;

    /** @brief The recordings of @p fragments (call_history_fragment_key()), oldest first, one map per recording. */
    virtual QVariantList recordingsForFragments(const QStringList& fragments) const = 0;

    /** @brief Whether any of @p fragments has a recording; what recordingsForFragments() reports, without the list. */
    virtual bool hasRecordingForFragments(const QStringList& fragments) const = 0;

    /**
     * @brief For each row's fragments in @p fragmentsPerRow, the names its recordings take in an Export as zip
     * (`<folder name>/<file>`), in recordingsForFragments()' order, each recording once; one list per row, in order.
     *
     * One read for every row, so a History export takes the provider's lock once. A provider without recordings names
     * none: one empty list per row.
     */
    virtual QList<QStringList>
    recordingEntryNames(const QList<QStringList>& fragmentsPerRow) const {
        return QList<QStringList>(fragmentsPerRow.size());
    }

    /**
     * @brief For each row's fragments in @p fragmentsPerRow, its recordings as recordingsForFragments() lists them
     * (oldest first within the row, by start then publish order, each recording once), one list per row, in order.
     *
     * One read for every row, so a History playlist (issue #909) takes the provider's lock once. Each map carries
     * `id`, `path`, `identitySize`, `mtimeSec` and `mtimeNanos` (the file as the provider indexed it), `durationMs`,
     * `available` and `deleteQueued` (a delete asked for it has not run yet). A provider without recordings lists none:
     * one empty list per row. The read may probe the storage roots it admits from (the recording store: one stat each,
     * under its lock), marking a root found gone as seen away and asking for a root back to be read again, so it is for
     * a playlist's admission (Play all, Play from here), not for per-refresh reads.
     */
    virtual QList<QVariantList>
    recordingsForFragmentLists(const QList<QStringList>& fragmentsPerRow) const {
        return QList<QVariantList>(fragmentsPerRow.size());
    }

  protected:
    CallRecordingProvider() = default;
    CallRecordingProvider(const CallRecordingProvider&) = default;
    CallRecordingProvider& operator=(const CallRecordingProvider&) = default;
};

} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_CALL_RECORDING_PROVIDER_H_ */
