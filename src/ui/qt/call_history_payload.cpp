// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "call_history_payload.h"

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QJsonValue>
#include <QLatin1String>
#include <QString>
#include <QtGlobal>
#include <dsd-neo/core/position.h>
#include <dsd-neo/core/state.h>
#include <limits>
#include <stddef.h>
#include <stdint.h>

#include "call_history_merge.h"
#include "call_history_model.h"

namespace dsd_qt {

namespace {

/* How many rings' ordinals are kept, the newest. Runs are sequential: only the running one, or one an embedding host
   reuses, is read again, and a spill line carries its own. */
constexpr int kMaxSessionOrdinals = 64;

/** @brief A ring item's text field of @p capacity bytes, up to its NUL. */
QString
item_text(const char* text, size_t capacity) {
    return QString::fromUtf8(text, static_cast<qsizetype>(qstrnlen(text, capacity)));
}

/** @brief A ring item's sender identity (issue #800): pos.scope with its discriminator, set with or without a
 * position. */
CallHistoryScope
item_sender_scope(const Event_History* item) {
    CallHistoryScope scope;
    scope.kind = item->pos.scope.kind;
    scope.a = item->pos.scope.a;
    scope.b = item->pos.scope.b;
    scope.freqHz = item->pos.identity_freq_hz;
    return call_history_scope_known(scope) ? scope : CallHistoryScope();
}

/** @brief Where a ring item was heard (issue #800): its rx_scope once resolved, else none. */
CallHistoryScope
item_rx_scope(const Event_History* item) {
    if (item->rx_scope.resolved == 0U) {
        return CallHistoryScope();
    }
    CallHistoryScope scope;
    scope.kind = item->rx_scope.scope.kind;
    scope.a = item->rx_scope.scope.a;
    scope.b = item->rx_scope.scope.b;
    scope.freqHz = item->rx_scope.identity_freq_hz;
    return call_history_scope_known(scope) ? scope : CallHistoryScope();
}

/** @brief Whether a row (or fragment) names its sender: a source ID or a source text. */
bool
row_names_source(const CallHistoryModel::Row& row) {
    return row.src != 0 || !row.sourceText.simplified().isEmpty();
}

/**
 * @brief Whether fragment @p row may give @p existing its text message, GPS text and position (issue #800, B2): one
 * protocol family and sender system, and the same radio.
 *
 * An anonymous fragment never enriches a row that names its source, and a fragment that names another source than the
 * row's keeps them to itself: the row keeps its original radio (foldRow()), so a position never crosses radios.
 */
bool
fragment_identity_compatible(const CallHistoryModel::Row& existing, const CallHistoryModel::Row& row) {
    if (!call_history_protocols_compatible(existing.protocol, row.protocol)
        || !call_history_scopes_compatible(existing.sourceScope, row.sourceScope)) {
        return false;
    }
    if (row_names_source(existing) && !row_names_source(row)) {
        return false;
    }
    return !(existing.src != 0 && row.src != 0 && existing.src != row.src)
           && !raw_texts_differ(existing.sourceText, row.sourceText);
}

/** @brief Where a row's GPS unit stands in the order that picks a merged row's one (call_history_gps_adopts()). */
CallHistoryPositionKey
gps_key(const CallHistoryModel::Row& row) {
    CallHistoryPositionKey key;
    key.keyed = row.pos.valid && row.gpsOrd > 0 && row.pos.rev > 0U;
    key.ord = row.gpsOrd;
    key.rev = row.pos.rev;
    return key;
}

/** @brief Whether @p kind is a dsd_position_scope_kind a record can hold: an alias scope kind or a feed-only one. */
bool
scope_kind_valid(uint8_t kind) {
    return (kind >= static_cast<uint8_t>(DSD_POS_SCOPE_P25) && kind <= static_cast<uint8_t>(DSD_POS_SCOPE_P25_NAC))
           || (kind >= static_cast<uint8_t>(DSD_POS_SCOPE_FEED_EDACS)
               && kind <= static_cast<uint8_t>(DSD_POS_SCOPE_FEED_TUNED_FREQ));
}

/** @brief A stored identity (issue #800) under @p key, or none when it is missing or no record could hold it. */
CallHistoryScope
scope_from_json(const QJsonObject& obj, const char* key) {
    const QJsonValue value = obj.value(QLatin1String(key));
    const QJsonObject stored = value.toObject();
    CallHistoryScope scope;
    if (!value.isObject() || !json_field_fits(stored, "k", &scope.kind) || !scope_kind_valid(scope.kind)
        || !json_field_fits(stored, "a", &scope.a) || !json_field_fits(stored, "b", &scope.b)
        || !json_field_fits(stored, "f", &scope.freqHz)) {
        return CallHistoryScope();
    }
    return scope;
}

void
scope_to_json(const CallHistoryScope& scope, const char* key, QJsonObject& obj) {
    if (!call_history_scope_known(scope)) {
        return;
    }
    QJsonObject stored;
    stored.insert(QLatin1String("k"), scope.kind);
    stored.insert(QLatin1String("a"), static_cast<qint64>(scope.a));
    stored.insert(QLatin1String("b"), static_cast<qint64>(scope.b));
    stored.insert(QLatin1String("f"), static_cast<qint64>(scope.freqHz));
    obj.insert(QLatin1String(key), stored);
}

/** @brief A stored 64-bit value written as hex text (a JSON number holds no more than 53 bits exactly), or 0. */
quint64
json_hex_u64(const QJsonObject& obj, const char* key) {
    bool ok = false;
    const quint64 value = obj.value(QLatin1String(key)).toString().toULongLong(&ok, 16);
    return ok ? value : 0U;
}

/** @brief A stored position (issue #800), or none when it is missing or holds values no report could (B2). */
CallHistoryModel::Position
position_from_json(const QJsonObject& obj) {
    const QJsonValue value = obj.value(QLatin1String("pos"));
    const QJsonObject stored = value.toObject();
    CallHistoryModel::Position pos;
    qint32 lat = 0;
    qint32 lon = 0;
    if (!value.isObject() || !json_field_fits(stored, "lat", &lat) || !json_field_fits(stored, "lon", &lon)
        || !json_field_fits(stored, "err", &pos.errM) || !json_field_fits(stored, "flags", &pos.flags)
        || !json_field_fits(stored, "kind", &pos.kind)
        || !call_history_position_values_valid(lat, lon, pos.kind, pos.flags)) {
        return CallHistoryModel::Position();
    }
    pos.valid = true;
    pos.latE7 = lat;
    pos.lonE7 = lon;
    pos.rev = json_hex_u64(stored, "rev");
    pos.procInstance = json_hex_u64(stored, "inst");
    pos.rxTimeS = json_i64_or(stored, "rxt", 0);
    return pos;
}

void
position_to_json(const CallHistoryModel::Position& pos, QJsonObject& obj) {
    if (!pos.valid) {
        return;
    }
    QJsonObject stored;
    stored.insert(QLatin1String("lat"), pos.latE7);
    stored.insert(QLatin1String("lon"), pos.lonE7);
    stored.insert(QLatin1String("err"), static_cast<qint64>(pos.errM));
    stored.insert(QLatin1String("flags"), pos.flags);
    stored.insert(QLatin1String("kind"), pos.kind);
    stored.insert(QLatin1String("rev"), QString::number(pos.rev, 16));
    stored.insert(QLatin1String("inst"), QString::number(pos.procInstance, 16));
    stored.insert(QLatin1String("rxt"), pos.rxTimeS);
    obj.insert(QLatin1String("pos"), stored);
}

} // namespace

/** @brief Keep the newest kMaxSessionOrdinals of @p rings, by ordinal. */
void
bound_session_ordinals(QHash<quint64, qint64>& rings) {
    while (rings.size() > kMaxSessionOrdinals) {
        auto oldest = rings.begin();
        for (auto it = rings.begin(); it != rings.end(); ++it) {
            if (it.value() < oldest.value()) {
                oldest = it;
            }
        }
        rings.erase(oldest);
    }
}

qint64
CallHistoryModel::sessionOrdinalFor(SessionOrdinals& ordinals, quint64 ring) {
    if (ring == 0U) {
        return 0;
    }
    const auto known = ordinals.rings.constFind(ring);
    if (known != ordinals.rings.constEnd()) {
        return known.value();
    }
    if (ordinals.next < 1 || ordinals.next >= ordinals.limit) {
        return 0;
    }
    const qint64 ordinal = ordinals.next++;
    ordinals.rings.insert(ring, ordinal);
    bound_session_ordinals(ordinals.rings);
    return ordinal;
}

CallHistoryProvenance
provenance(qint64 when, qulonglong seq, int slot) {
    CallHistoryProvenance from;
    from.when = static_cast<int64_t>(when);
    from.seq = static_cast<uint64_t>(seq);
    from.slot = slot;
    return from;
}

/** @brief A stored integer, or @p fallback when the key is missing (a store an older build wrote). */
qint64
json_i64_or(const QJsonObject& obj, const char* key, qint64 fallback) {
    return obj.contains(QLatin1String(key)) ? obj.value(QLatin1String(key)).toVariant().toLongLong() : fallback;
}

/**
 * @brief The row's detail line (issue #800, B2): its text message and its GPS text, each when it has one, joined; the
 * detail a store an older build wrote when it has neither.
 */
QString
row_detail_text(const CallHistoryModel::Row& row) {
    if (row.messageText.isEmpty() && row.gpsText.isEmpty()) {
        return row.legacyDetail;
    }
    if (row.messageText.isEmpty() || row.gpsText.isEmpty()) {
        return row.messageText.isEmpty() ? row.gpsText : row.messageText;
    }
    return row.messageText + QStringLiteral(" \u00B7 ") + row.gpsText;
}

/** @brief Whether a ring item carries an admitted position whose values a report could hold. */
bool
item_position_valid(const Event_History* item) {
    return item->pos.valid == 1U
           && call_history_position_values_valid(item->pos.lat_e7, item->pos.lon_e7, item->pos.kind, item->pos.flags);
}

/**
 * @brief Give @p row its item's raw identity, text message, GPS text and position (issue #800), each from the row's
 * own fragment. Pure, as everything the background tap's walk calls must be.
 */
void
row_take_payload(CallHistoryModel::Row& row, const Event_History* item) {
    row.protocol = item->systype;
    row.sourceText = item_text(item->src_str, sizeof(item->src_str));
    row.targetText = item_text(item->tgt_str, sizeof(item->tgt_str));
    row.sourceScope = item_sender_scope(item);
    row.rxScope = item_rx_scope(item);
    row.messageText = item_text(item->text_message, sizeof(item->text_message));
    row.msgWhen = row.when;
    row.msgSeq = row.seq;
    row.msgSlot = row.slot;
    row.gpsText = item_text(item->gps_s, sizeof(item->gps_s));
    if (item_position_valid(item)) {
        row.pos.valid = true;
        row.pos.latE7 = item->pos.lat_e7;
        row.pos.lonE7 = item->pos.lon_e7;
        row.pos.errM = item->pos.err_m;
        row.pos.flags = item->pos.flags;
        row.pos.kind = item->pos.kind;
        row.pos.rev = item->pos.rev;
        row.pos.procInstance = item->pos.proc_instance;
        row.pos.rxTimeS = item->pos.rx_time_s;
    }
    row.gpsWhen = row.when;
    row.gpsSeq = row.seq;
    row.gpsSlot = row.slot;
}

/** @brief Whether two raw texts (callsigns, dial strings) are known and differ, compared as the call layer does: runs
 * of spaces as one, trimmed. */
bool
raw_texts_differ(const QString& a, const QString& b) {
    const QString x = a.simplified();
    const QString y = b.simplified();
    return !x.isEmpty() && !y.isEmpty() && x != y;
}

/**
 * @brief Whether two rows' known identities agree (issue #800): one protocol family, one reception system and one
 * sender system (B5: two conventional systems on two frequencies, or two channels of one NAC, are two calls; a trunked
 * call on two voice channels is one), and one source text (B3: two callsign callers to one destination are two calls,
 * whatever their display names). An identity either row does not know is no conflict, which keeps the merge older rows
 * always had.
 */
bool
rows_identity_compatible(const CallHistoryModel::Row& existing, const CallHistoryModel::Row& row) {
    return call_history_protocols_compatible(existing.protocol, row.protocol)
           && call_history_scopes_compatible(existing.rxScope, row.rxScope)
           && call_history_scopes_compatible(existing.sourceScope, row.sourceScope)
           && !raw_texts_differ(existing.sourceText, row.sourceText);
}

/** @brief Whether a row has a GPS unit: GPS text, a position, or both. */
bool
row_gps_known(const CallHistoryModel::Row& row) {
    return !row.gpsText.isEmpty() || row.pos.valid;
}

/** @brief Give @p to the GPS text and position of @p from, with the fragment they came from: always together. */
void
take_gps(CallHistoryModel::Row& to, const CallHistoryModel::Row& from) {
    to.gpsText = from.gpsText;
    to.pos = from.pos;
    to.gpsWhen = from.gpsWhen;
    to.gpsSeq = from.gpsSeq;
    to.gpsSlot = from.gpsSlot;
    to.gpsOrd = from.gpsOrd;
}

void
take_message(CallHistoryModel::Row& to, const CallHistoryModel::Row& from) {
    to.messageText = from.messageText;
    to.msgWhen = from.msgWhen;
    to.msgSeq = from.msgSeq;
    to.msgSlot = from.msgSlot;
}

/**
 * @brief Fold @p row's text message, and its GPS text with its position, into @p existing (issue #800, B2 and 2.7).
 *
 * Each only from a fragment of the row's radio (fragment_identity_compatible()). A row that names no source yet and
 * learns one from @p row first forgets what its anonymous fragments gave it: those were no messages or positions of the
 * radio it now names. So any order of the same fragments gives the same row. The message is the newest fragment's, as
 * the source label is; the GPS text and position are the fragment's with the greatest session ordinal and rev
 * (call_history_gps_adopts()).
 */
void
fold_payload(CallHistoryModel::Row& existing, const CallHistoryModel::Row& row) {
    if (!row_names_source(existing) && row_names_source(row)) {
        existing.messageText.clear();
        existing.gpsText.clear();
        existing.pos = CallHistoryModel::Position();
    }
    if (!fragment_identity_compatible(existing, row)) {
        return;
    }
    if (call_history_fold_adopts(!existing.messageText.isEmpty(),
                                 provenance(existing.msgWhen, existing.msgSeq, existing.msgSlot),
                                 !row.messageText.isEmpty(), provenance(row.msgWhen, row.msgSeq, row.msgSlot))) {
        take_message(existing, row);
    }
    if (call_history_gps_adopts(row_gps_known(existing), gps_key(existing),
                                provenance(existing.gpsWhen, existing.gpsSeq, existing.gpsSlot), row_gps_known(row),
                                gps_key(row), provenance(row.gpsWhen, row.gpsSeq, row.gpsSlot))) {
        take_gps(existing, row);
    }
}

/**
 * @brief Fill what @p existing does not know of its raw identity from @p row (issue #800): the reception system when
 * @p row names it more strongly (call_history_scope_strength()); and, only from a fragment of the row's own radio
 * (fragment_identity_compatible()), the sync type, the target text, the source text and a stronger sender system.
 * Other known values stay: a merge joins only rows whose known ones agree (rows_identity_compatible()).
 */
void
fill_identity(CallHistoryModel::Row& existing, const CallHistoryModel::Row& row) {
    // Where the row was heard is the reception's, whoever sent the fragment: the stronger of the two the merge found
    // compatible (a trunked call's NAC gives way to its system).
    if (call_history_scope_strength(row.rxScope) > call_history_scope_strength(existing.rxScope)) {
        existing.rxScope = row.rxScope;
    }
    // The sender's identity only from a fragment of the row's own radio, as its texts and position (fold_payload()).
    if (!fragment_identity_compatible(existing, row)) {
        return;
    }
    if (call_history_protocol_family(existing.protocol) < 0) {
        existing.protocol = row.protocol;
    }
    if (call_history_scope_strength(row.sourceScope) > call_history_scope_strength(existing.sourceScope)) {
        existing.sourceScope = row.sourceScope;
    }
    if (existing.targetText.simplified().isEmpty()) {
        existing.targetText = row.targetText;
    }
    if (existing.sourceText.simplified().isEmpty() && (existing.src == 0 || existing.src == row.src)) {
        existing.sourceText = row.sourceText;
    }
}

/** @brief Whether two positions are one fix as a reader sees it: either has none, or the same values (the rev and the
 * process differ when a notice is decoded again). */
bool
positions_same_fix(const CallHistoryModel::Position& a, const CallHistoryModel::Position& b) {
    return !a.valid || !b.valid
           || (a.latE7 == b.latE7 && a.lonE7 == b.lonE7 && a.errM == b.errM && a.flags == b.flags && a.kind == b.kind);
}

/** @brief The digest a ring walk takes of a row's text (call_history_text_digest()), from the text the row keeps. */
uint32_t
row_text_digest(const QString& text) {
    const QByteArray bytes = text.toUtf8();
    return call_history_text_digest(bytes.constData(), static_cast<size_t>(bytes.size()));
}

/** @brief The digest a ring walk takes of a row's position (call_history_position_digest()). */
uint32_t
row_position_digest(const CallHistoryModel::Position& pos) {
    return call_history_position_digest(pos.valid, pos.latE7, pos.lonE7, pos.errM, pos.flags, pos.kind, pos.rev,
                                        pos.procInstance);
}

/**
 * @brief Read a stored row's raw identity, text message, GPS text and position (issue #800), and the fragment each
 * came from. A store an older build wrote has none of them: its row keeps the detail it stored (legacyDetail), and a
 * text written without its provenance takes the row's own, as the frequency's does.
 */
void
row_payload_from_json(CallHistoryModel::Row& row, const QJsonObject& obj) {
    const qint64 stored = json_i64_or(obj, "proto", -1);
    const int protocol = stored >= 0 && stored <= INT8_MAX ? static_cast<int>(stored) : -1;
    row.protocol = call_history_protocol_family(protocol) >= 0 ? protocol : -1;
    row.sourceText = obj.value(QLatin1String("srcText")).toString();
    row.targetText = obj.value(QLatin1String("tgtText")).toString();
    row.sourceScope = scope_from_json(obj, "sscope");
    row.rxScope = scope_from_json(obj, "rx");
    row.messageText = obj.value(QLatin1String("msg")).toString();
    row.msgWhen = json_i64_or(obj, "msgWhen", row.when);
    row.msgSeq = static_cast<qulonglong>(json_i64_or(obj, "msgSeq", static_cast<qint64>(row.seq)));
    row.msgSlot = static_cast<int>(json_i64_or(obj, "msgSlot", row.slot));
    row.gpsText = obj.value(QLatin1String("gps")).toString();
    row.pos = position_from_json(obj);
    row.gpsWhen = json_i64_or(obj, "gpsWhen", row.when);
    row.gpsSeq = static_cast<qulonglong>(json_i64_or(obj, "gpsSeq", static_cast<qint64>(row.seq)));
    row.gpsSlot = static_cast<int>(json_i64_or(obj, "gpsSlot", row.slot));
    row.gpsOrd = qMax<qint64>(json_i64_or(obj, "gpsOrd", 0), 0);
    if (row.messageText.isEmpty() && row.gpsText.isEmpty()) {
        row.legacyDetail = obj.value(QLatin1String("detail")).toString();
    }
}

/** @brief Write a row's raw identity, text message, GPS text and position (issue #800); nothing a row does not know. */
void
row_payload_to_json(const CallHistoryModel::Row& row, QJsonObject& obj) {
    if (call_history_protocol_family(row.protocol) >= 0) {
        obj.insert(QLatin1String("proto"), row.protocol);
    }
    if (!row.sourceText.isEmpty()) {
        obj.insert(QLatin1String("srcText"), row.sourceText);
    }
    if (!row.targetText.isEmpty()) {
        obj.insert(QLatin1String("tgtText"), row.targetText);
    }
    scope_to_json(row.sourceScope, "sscope", obj);
    scope_to_json(row.rxScope, "rx", obj);
    if (!row.messageText.isEmpty()) {
        obj.insert(QLatin1String("msg"), row.messageText);
        obj.insert(QLatin1String("msgWhen"), row.msgWhen);
        obj.insert(QLatin1String("msgSeq"), static_cast<qint64>(row.msgSeq));
        obj.insert(QLatin1String("msgSlot"), row.msgSlot);
    }
    if (row_gps_known(row)) {
        if (!row.gpsText.isEmpty()) {
            obj.insert(QLatin1String("gps"), row.gpsText);
        }
        position_to_json(row.pos, obj);
        obj.insert(QLatin1String("gpsWhen"), row.gpsWhen);
        obj.insert(QLatin1String("gpsSeq"), static_cast<qint64>(row.gpsSeq));
        obj.insert(QLatin1String("gpsSlot"), row.gpsSlot);
        obj.insert(QLatin1String("gpsOrd"), row.gpsOrd);
    }
    if (row.messageText.isEmpty() && row.gpsText.isEmpty() && !row.legacyDetail.isEmpty()) {
        obj.insert(QLatin1String("detail"), row.legacyDetail);
    }
}

} // namespace dsd_qt
