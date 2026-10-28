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
#include <QString>
#include <QStringList>
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

  protected:
    CallRecordingProvider() = default;
    CallRecordingProvider(const CallRecordingProvider&) = default;
    CallRecordingProvider& operator=(const CallRecordingProvider&) = default;
};

} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_CALL_RECORDING_PROVIDER_H_ */
