// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The record DocumentCopies.probeImportDocument() answers over JNI (#831), and its reader, free of JNI so the
 * desktop host test reads it as it ships.
 *
 * The record is `code;total;header;name`: the DocumentTransfer result code, the counted length (-1 when not counted),
 * the header as lowercase hex (at most dsd_qt::kProbeHeaderBytes bytes), and the provider's display name, last and
 * verbatim, so a name may hold any character, a ';' included; an empty name is none.
 */

#ifndef DSD_NEO_ANDROID_DOCUMENT_PROBE_RECORD_H_
#define DSD_NEO_ANDROID_DOCUMENT_PROBE_RECORD_H_

#include <QByteArray>
#include <QChar>
#include <QString>
#include <QStringView>
#include <QtGlobal>
#include <algorithm>
#include <utility>

#include "document_import_transport.h"

namespace dsd_android {

/** @brief A probe record's fields. */
struct DocumentProbeRecord {
    int code = -1;
    qint64 total = -1;
    QByteArray header;
    QString name;
};

namespace probe_record_detail {

/* A plain decimal of at most 18 digits (no sign, space or '+'); -1 only when @p allowMinusOne. */
inline bool
parse_count(QStringView field, bool allowMinusOne, qint64* out) {
    if (allowMinusOne && field == u"-1") {
        *out = -1;
        return true;
    }
    if (field.isEmpty() || field.size() > 18) {
        return false;
    }
    qint64 value = 0;
    for (const QChar c : field) {
        if (c < u'0' || c > u'9') {
            return false;
        }
        value = value * 10 + (c.unicode() - u'0');
    }
    *out = value;
    return true;
}

/* Lowercase or uppercase hex pairs, at most dsd_qt::kProbeHeaderBytes bytes of them. */
inline bool
parse_hex(QStringView field, QByteArray* out) {
    if (field.size() % 2 != 0 || field.size() > qsizetype{2} * dsd_qt::kProbeHeaderBytes) {
        return false;
    }
    const auto is_hex = [](QChar c) {
        const char16_t u = c.unicode();
        return (u >= u'0' && u <= u'9') || (u >= u'a' && u <= u'f') || (u >= u'A' && u <= u'F');
    };
    if (!std::all_of(field.begin(), field.end(), is_hex)) {
        return false;
    }
    *out = QByteArray::fromHex(field.toLatin1());
    return true;
}

} // namespace probe_record_detail

/**
 * @brief Read @p record into @p out; false, with @p out untouched, when it is not a whole record: an empty answer (a
 * JNI call that failed), a field that is not a plain number or hex, a header longer than a probe reads, or a counted
 * length shorter than the header it counted from.
 */
inline bool
parse_document_probe_record(const QString& record, DocumentProbeRecord* out) {
    const QStringView view(record);
    const qsizetype first = view.indexOf(u';');
    const qsizetype second = first < 0 ? -1 : view.indexOf(u';', first + 1);
    const qsizetype third = second < 0 ? -1 : view.indexOf(u';', second + 1);
    if (third < 0 || out == nullptr) {
        return false;
    }
    DocumentProbeRecord parsed;
    qint64 code = 0;
    if (!probe_record_detail::parse_count(view.first(first), false, &code) || code > 0x7fffffff
        || !probe_record_detail::parse_count(view.sliced(first + 1, second - first - 1), true, &parsed.total)
        || !probe_record_detail::parse_hex(view.sliced(second + 1, third - second - 1), &parsed.header)
        || (parsed.total >= 0 && parsed.total < parsed.header.size())) {
        return false;
    }
    parsed.code = static_cast<int>(code);
    parsed.name = view.sliced(third + 1).toString();
    *out = std::move(parsed);
    return true;
}

} // namespace dsd_android

#endif // DSD_NEO_ANDROID_DOCUMENT_PROBE_RECORD_H_
