// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Issue #800: what a History row carries besides its call, and how fragments share it.
 *
 * The row's text message, GPS text and admitted position, its raw identity (sync type, source and target text, sender
 * and reception systems), how a fragment gives them to the row it folds into, how they are stored, and the session
 * ordinals that order positions across decoder runs. The Qt-free order and compatibility rules are in
 * call_history_merge.h; CallHistoryModel calls these.
 */

#ifndef DSD_NEO_SRC_UI_QT_CALL_HISTORY_PAYLOAD_H_
#define DSD_NEO_SRC_UI_QT_CALL_HISTORY_PAYLOAD_H_

#include <QHash>
#include <QJsonObject>
#include <QJsonValue>
#include <QLatin1String>
#include <QString>
#include <QtGlobal>
#include <dsd-neo/core/state.h>
#include <limits>
#include <stdint.h>

#include "call_history_merge.h"
#include "call_history_model.h"

namespace dsd_qt {

/** @brief A stored integer, or @p fallback when the key is missing (a store an older build wrote). */
qint64 json_i64_or(const QJsonObject& obj, const char* key, qint64 fallback);

/**
 * @brief A stored integer that fits @p T, or false when the key is missing, holds no integer or does not fit.
 *
 * For values a ring row carries in fields of that width, so a hand-edited store cannot wrap one into another value.
 */
template <typename T>
bool
json_field_fits(const QJsonObject& obj, const char* key, T* out) {
    constexpr qint64 kNone = std::numeric_limits<qint64>::min();
    const QJsonValue value = obj.value(QLatin1String(key));
    const qint64 n = value.isDouble() ? value.toInteger(kNone) : kNone;
    if (n < static_cast<qint64>(std::numeric_limits<T>::min())
        || n > static_cast<qint64>(std::numeric_limits<T>::max())) {
        return false;
    }
    *out = static_cast<T>(n);
    return true;
}

/** @brief Keep the newest of @p rings' session ordinals (CallHistoryModel::SessionOrdinals), by ordinal. */
void bound_session_ordinals(QHash<quint64, qint64>& rings);

/** @brief The fragment a value came from: its start, push stamp and slot (call_history_fold_adopts()). */
CallHistoryProvenance provenance(qint64 when, qulonglong seq, int slot);

/** @brief The row's detail line: its text message and GPS text joined, else the detail an older store kept. */
QString row_detail_text(const CallHistoryModel::Row& row);

/** @brief Whether a ring item carries an admitted position whose values a report could hold. */
bool item_position_valid(const Event_History* item);

/** @brief Give @p row its item's raw identity, text message, GPS text and position, from the row's own fragment. */
void row_take_payload(CallHistoryModel::Row& row, const Event_History* item);

/** @brief Whether two raw texts (callsigns, dial strings) are known and differ, runs of spaces as one, trimmed. */
bool raw_texts_differ(const QString& a, const QString& b);

/** @brief Whether two rows' known identities agree: protocol family, reception and sender system, source text. */
bool rows_identity_compatible(const CallHistoryModel::Row& existing, const CallHistoryModel::Row& row);

/** @brief Whether a row has a GPS unit: GPS text, a position, or both. */
bool row_gps_known(const CallHistoryModel::Row& row);

/** @brief Give @p to the GPS text and position of @p from, with the fragment they came from: always together. */
void take_gps(CallHistoryModel::Row& to, const CallHistoryModel::Row& from);

/** @brief Give @p to the text message of @p from, with the fragment it came from. */
void take_message(CallHistoryModel::Row& to, const CallHistoryModel::Row& from);

/** @brief Fold @p row's text message, and its GPS text with its position, into @p existing (call before its source). */
void fold_payload(CallHistoryModel::Row& existing, const CallHistoryModel::Row& row);

/** @brief Fill what @p existing does not know of its raw identity from @p row (call before its source). */
void fill_identity(CallHistoryModel::Row& existing, const CallHistoryModel::Row& row);

/** @brief Whether two positions are one fix as a reader sees it (either has none, or the same values). */
bool positions_same_fix(const CallHistoryModel::Position& a, const CallHistoryModel::Position& b);

/** @brief The digest a ring walk takes of a row's text (call_history_text_digest()), from the text the row keeps. */
uint32_t row_text_digest(const QString& text);

/** @brief The digest a ring walk takes of a row's position (call_history_position_digest()). */
uint32_t row_position_digest(const CallHistoryModel::Position& pos);

/** @brief Read a stored row's raw identity, texts and position, and the fragment each came from. */
void row_payload_from_json(CallHistoryModel::Row& row, const QJsonObject& obj);

/** @brief Write a row's raw identity, texts and position; nothing a row does not know. */
void row_payload_to_json(const CallHistoryModel::Row& row, QJsonObject& obj);

} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_CALL_HISTORY_PAYLOAD_H_ */
