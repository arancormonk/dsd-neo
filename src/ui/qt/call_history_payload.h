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

#include <QJsonObject>
#include <QJsonValue>
#include <QLatin1String>
#include <QSet>
#include <QString>
#include <QtGlobal>
#include <dsd-neo/core/state.h>
#include <limits>
#include <stdint.h>

#include "call_history_merge.h"
#include "call_history_model.h"

template <typename Key, typename T>
class QHash;

namespace dsd_qt {

/** @brief A stored integer, or @p fallback when the key is missing (a store an older build wrote). */
qint64 json_i64_or(const QJsonObject& obj, const char* key, qint64 fallback);

/**
 * @brief A stored integer that fits @p T, or false when the key is missing, holds no integer or does not fit.
 *
 * For values a ring row carries in fields of that width, so a hand-edited store cannot wrap one into another value.
 * Whether the value is an integer is judged apart from its range: no stand-in value means "none", so a 64-bit field
 * cannot take one for a stored value.
 */
template <typename T>
bool
json_field_fits(const QJsonObject& obj, const char* key, T* out) {
    const QJsonValue value = obj.value(QLatin1String(key));
    // toInteger() gives its fallback for anything but a whole number a qint64 holds: two fallbacks tell that apart.
    const qint64 n = value.toInteger(0);
    if (!value.isDouble() || n != value.toInteger(1) || n < static_cast<qint64>(std::numeric_limits<T>::min())
        || n > static_cast<qint64>(std::numeric_limits<T>::max())) {
        return false;
    }
    *out = static_cast<T>(n);
    return true;
}

/** @brief A stored system identity under @p key (`rx`, `sscope`), or none when it is missing or invalid. */
CallHistoryScope stored_scope_from_json(const QJsonObject& obj, const char* key);

/** @brief Store @p scope under @p key; nothing for an unknown one. */
void stored_scope_to_json(const CallHistoryScope& scope, const char* key, QJsonObject& obj);

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

/** @brief A row that names its source keeps no anonymous position (issue #800, spec 2.14): an anonymous report and its
 * GPS text go once the row names a source, as the core drops them (dsd_event_pos_requalify_row()). */
void drop_anonymous_gps_of_named(CallHistoryModel::Row& row);

/** @brief The key of the fragment a GPS unit came from, as drop_gps_units_of_stamps() reads it: its run's session
 * ordinal @p ord (0 when not known), its slot and its push stamp (call_history_same_fragment()). */
QString gps_fragment_stamp(qint64 ord, int slot, qulonglong seq);

/** @brief Take from @p row the GPS units of the fragments whose keys (gps_fragment_stamp()) are in @p stamps (issue
 * #800): the row shows the best remaining one, or none. */
void drop_gps_units_of_stamps(CallHistoryModel::Row& row, const QSet<QString>& stamps);

/** @brief Whether two raw texts (callsigns, dial strings) are known and differ, runs of spaces as one, trimmed. */
bool raw_texts_differ(const QString& a, const QString& b);

/** @brief Whether a row (or fragment) names its sender: a source ID or a source text. */
bool row_names_source(const CallHistoryModel::Row& row);

/** @brief Whether two fragments are one receive service (issue #637): their confirmed access codes or tones, their
 * labels and their proven over-the-air slots agree. */
bool receive_services_compatible(const CallHistoryModel::Row& a, const CallHistoryModel::Row& b);

/** @brief Whether fragment @p row is of @p existing's call: one receive service (receive_services_compatible()), one
 * protocol family, a reception and sender system the row's are compatible with (call_history_scopes_compatible()), and
 * no other raw target text. */
bool fragment_call_compatible(const CallHistoryModel::Row& existing, const CallHistoryModel::Row& row);

/** @brief Whether fragment @p row is of @p existing's radio, so it may give the row its texts, position and source
 * label: one protocol family and sender system, no other source number or text, and not anonymous beside a named row. */
bool fragment_identity_compatible(const CallHistoryModel::Row& existing, const CallHistoryModel::Row& row);

/** @brief Whether fragment @p row was heard where @p existing was, as the core's position writers require (issue #800,
 * fail closed): one protocol class (call_history_position_class(): the two P25 phases apart, a sync type known on
 * both), and one reception by the reception rule (call_history_same_reception()). Gates a fix and a scope a row learns,
 * never the merge of a conversation. */
bool fragment_same_reception(const CallHistoryModel::Row& existing, const CallHistoryModel::Row& row);

/** @brief Whether @p held learns a stronger reception scope from fragment @p seen: heard where it was
 * (fragment_same_reception()), with a scope that names more (call_history_scope_learns()). */
bool reception_learns(const CallHistoryModel::Row& held, const CallHistoryModel::Row& seen);

/** @brief Give @p to the reception of @p from: its scope, epoch and ring. */
void take_reception(CallHistoryModel::Row& to, const CallHistoryModel::Row& from);

/** @brief Whether two rows' known identities agree: protocol family, reception and sender system, source and target
 * text. */
bool rows_identity_compatible(const CallHistoryModel::Row& existing, const CallHistoryModel::Row& row);
/** @brief Whether two calls' kinds (Row::gi: 0 group, 1 private, -1 not known) are both known and differ: a group call
 *         and a private call to the same number are two calls (issue #800). */
bool call_kinds_differ(int a, int b);
/** @brief A stored `gi` as Row::gi: 0 or 1, anything else (absent, an older file) -1. */
qint8 stored_gi(qint64 value);

/** @brief Whether a row has a GPS unit: GPS text, a position, or both. */
bool row_gps_known(const CallHistoryModel::Row& row);

/** @brief Give @p to the GPS text and position of @p from, with the fragment they came from: always together. */
void take_gps(CallHistoryModel::Row& to, const CallHistoryModel::Row& from);

/** @brief Give @p to the text message of @p from, with the fragment it came from. */
void take_message(CallHistoryModel::Row& to, const CallHistoryModel::Row& from);

/** @brief Fold the text message of @p row, a fragment of @p existing's radio, into @p existing (call before its
 * source). */
void fold_message(CallHistoryModel::Row& existing, const CallHistoryModel::Row& row);

/** @brief Fold @p row's GPS text with its position into @p existing when @p sender (the reading names the row's radio),
 * else take back the GPS unit of @p row's fragment; @p learnsSource: the row named no source before this reading. */
void fold_gps(CallHistoryModel::Row& existing, const CallHistoryModel::Row& row, bool sender, bool learnsSource);

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
