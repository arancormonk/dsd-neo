// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "recording_journal.h"

#include <QChar>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QLatin1String>
#include <QList>
#include <QStringView>
#include <algorithm>
#include <utility>

namespace dsd_qt {

namespace {

/* 16 uppercase hex digits: a journal token or ring instance. */
bool
is_hex16(QStringView text) {
    return text.size() == 16 && std::all_of(text.begin(), text.end(), [](QChar c) {
               return (c >= QLatin1Char('0') && c <= QLatin1Char('9'))
                      || (c >= QLatin1Char('A') && c <= QLatin1Char('F'));
           });
}

bool
is_digits(QStringView text) {
    return !text.isEmpty() && std::all_of(text.begin(), text.end(), [](QChar c) {
        return c >= QLatin1Char('0') && c <= QLatin1Char('9');
    });
}

/* A file name the store may resolve against the journal's folder: relative, '/'-separated, no way out. */
bool
plain_relative_name(const QString& file) {
    if (file.isEmpty() || file.startsWith(QLatin1Char('/')) || file.contains(QLatin1Char('\\'))
        || file.contains(QLatin1Char(':')) || file.contains(QChar(0))) {
        return false;
    }
    const QList<QStringView> parts = QStringView(file).split(QLatin1Char('/'));
    return std::none_of(parts.cbegin(), parts.cend(), [](QStringView part) {
        return part.isEmpty() || part == QLatin1String(".") || part == QLatin1String("..");
    });
}

/* A non-negative JSON integer, else @p fallback. */
qint64
integer_or(const QJsonValue& value, qint64 fallback) {
    if (!value.isDouble()) {
        return fallback;
    }
    const qint64 n = value.toInteger(-1);
    return n >= 0 ? n : fallback;
}

/* The ring, slot and row a publish or link names. */
void
read_owner(const QJsonObject& object, RecordingJournalRecord* out) {
    const QString instance = object.value(QLatin1String("instance")).toString();
    if (is_hex16(instance)) {
        out->hasInstance = true;
        out->instance = instance.toULongLong(nullptr, 16);
    }
    const qint64 slot = integer_or(object.value(QLatin1String("slot")), -1);
    out->slot = slot == 0 || slot == 1 ? static_cast<int>(slot) : -1;
    const qint64 row = integer_or(object.value(QLatin1String("row")), -1);
    if (row >= 0) {
        out->hasRow = true;
        out->row = static_cast<qulonglong>(row);
    }
}

/* A publish's file and the call it records; false for a file outside the journal's folder. */
bool
read_publish(const QJsonObject& object, RecordingJournalRecord* record) {
    record->file = object.value(QLatin1String("file")).toString();
    if (!plain_relative_name(record->file)) {
        return false;
    }
    record->start = integer_or(object.value(QLatin1String("start")), 0);
    record->durationMs = integer_or(object.value(QLatin1String("duration_ms")), 0);
    record->rate = static_cast<int>(qMin<qint64>(integer_or(object.value(QLatin1String("rate")), 0), 1000000));
    read_owner(object, record);
    record->pending = object.value(QLatin1String("pending")).toBool(false);
    record->epoch = static_cast<qulonglong>(integer_or(object.value(QLatin1String("epoch")), 0));
    record->rdio = object.value(QLatin1String("rdio")).toBool(false);
    record->failed = object.value(QLatin1String("failed")).toBool(false);
    record->system = object.value(QLatin1String("system")).toString();
    record->target = object.value(QLatin1String("target")).toString();
    record->source = object.value(QLatin1String("source")).toString();
    return true;
}

/* A link's call; false unless it names a recording and the whole of its row. */
bool
read_link(const QJsonObject& object, RecordingJournalRecord* record) {
    read_owner(object, record);
    return !record->id.isEmpty() && record->hasInstance && record->slot >= 0 && record->hasRow;
}

bool
type_of(const QString& type, RecordingJournalRecord::Type* out) {
    if (type == QLatin1String("publish")) {
        *out = RecordingJournalRecord::Publish;
    } else if (type == QLatin1String("link")) {
        *out = RecordingJournalRecord::Link;
    } else if (type == QLatin1String("unlink")) {
        *out = RecordingJournalRecord::Unlink;
    } else if (type == QLatin1String("discard")) {
        *out = RecordingJournalRecord::Discard;
    } else if (type == QLatin1String("error")) {
        *out = RecordingJournalRecord::Error;
    } else {
        return false;
    }
    return true;
}

} // namespace

bool
recording_journal_parse(const QByteArray& line, RecordingJournalRecord* out) {
    if (out == nullptr) {
        return false;
    }
    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(line, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        return false;
    }
    const QJsonObject object = document.object();
    if (integer_or(object.value(QLatin1String("v")), 0) != 1) {
        return false;
    }
    RecordingJournalRecord record;
    if (!type_of(object.value(QLatin1String("type")).toString(), &record.type)) {
        return false;
    }
    record.t = integer_or(object.value(QLatin1String("t")), 0);
    record.id = object.value(QLatin1String("id")).toString();
    switch (record.type) {
        case RecordingJournalRecord::Publish:
            if (!read_publish(object, &record)) {
                return false;
            }
            break;
        case RecordingJournalRecord::Link:
            if (!read_link(object, &record)) {
                return false;
            }
            break;
        case RecordingJournalRecord::Unlink:
            if (record.id.isEmpty()) {
                return false;
            }
            break;
        case RecordingJournalRecord::Discard:
        case RecordingJournalRecord::Error: break;
    }
    *out = std::move(record);
    return true;
}

QByteArray
recording_journal_publish_line(const RecordingJournalRecord& record) {
    QJsonObject object;
    object.insert(QLatin1String("v"), 1);
    object.insert(QLatin1String("type"), QLatin1String("publish"));
    object.insert(QLatin1String("t"), record.t);
    object.insert(QLatin1String("id"), record.id.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(record.id));
    object.insert(QLatin1String("file"), record.file);
    object.insert(QLatin1String("start"), record.start);
    object.insert(QLatin1String("duration_ms"), record.durationMs);
    object.insert(QLatin1String("rate"), record.rate);
    object.insert(QLatin1String("instance"),
                  record.hasInstance
                      ? QJsonValue(QStringLiteral("%1").arg(record.instance, 16, 16, QLatin1Char('0')).toUpper())
                      : QJsonValue(QJsonValue::Null));
    object.insert(QLatin1String("slot"), record.slot >= 0 ? QJsonValue(record.slot) : QJsonValue(QJsonValue::Null));
    object.insert(QLatin1String("row"),
                  record.hasRow ? QJsonValue(static_cast<qint64>(record.row)) : QJsonValue(QJsonValue::Null));
    object.insert(QLatin1String("pending"), false);
    object.insert(QLatin1String("epoch"), 0);
    object.insert(QLatin1String("rdio"), record.rdio);
    object.insert(QLatin1String("failed"), record.failed);
    object.insert(QLatin1String("system"), record.system);
    object.insert(QLatin1String("target"), record.target);
    object.insert(QLatin1String("source"), record.source);
    QByteArray line = QJsonDocument(object).toJson(QJsonDocument::Compact);
    line.append('\n');
    return line;
}

bool
recording_temp_name_token(const QString& fileName, QString* token) {
    QStringView name(fileName);
    if (!name.startsWith(QLatin1String("TEMP_"))) {
        return false;
    }
    if (name.endsWith(QLatin1String(".wav"))) {
        name.chop(4);
    }
    const QList<QStringView> parts = name.split(QLatin1Char('_'));
    if (parts.size() != 5 || parts.at(1).size() != 8 || !is_digits(parts.at(1)) || parts.at(2).size() != 6
        || !is_digits(parts.at(2)) || !is_hex16(parts.at(3)) || !is_digits(parts.at(4))) {
        return false;
    }
    if (token != nullptr) {
        *token = parts.at(3).toString();
    }
    return true;
}

} // namespace dsd_qt
