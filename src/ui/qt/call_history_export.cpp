// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "call_history_export.h"

#include <QByteArray>
#include <QChar>
#include <QDateTime>
#include <QLatin1String>
#include <QList>
#include <QString>
#include <QStringList>
#include <QStringView>
#include <QTimeZone>
#include <QVector>
#include <Qt>
#include <QtGlobal>
#include "call_history_model.h"
#include "csv_text.h"

namespace dsd_qt {
namespace call_history_export {

namespace {

/* The file's columns, in order; `true` marks a text column, which gets the formula guard. */
const struct {
    const char* name;
    bool text;
} kColumns[] = {
    {"started_local", false}, {"utc_offset", false}, {"started_utc", false}, {"duration_s", false},
    {"type", false},          {"name", true},        {"talkgroup", false},   {"radio_id", false},
    {"radio_name", true},     {"system", true},      {"channel", true},      {"frequency_hz", false},
    {"access_code", true},    {"tone", true},        {"site", true},         {"analog_mode", false},
    {"encrypted", false},     {"emergency", false},  {"message", true},      {"recordings", true},
};

const QLatin1String kSeparator(" | ");

QStringList
header() {
    QStringList names;
    for (const auto& column : kColumns) {
        names.append(QLatin1String(column.name));
    }
    return names;
}

QVector<bool>
textColumns() {
    QVector<bool> text;
    for (const auto& column : kColumns) {
        text.append(column.text);
    }
    return text;
}

/* An offset in seconds as `UTC+hh:mm` / `UTC-hh:mm`; the same digits in every locale. */
QString
offsetText(int seconds) {
    const int magnitude = seconds < 0 ? -seconds : seconds;
    return QString::asprintf("UTC%c%02d:%02d", seconds < 0 ? '-' : '+', magnitude / 3600, (magnitude % 3600) / 60);
}

/* The three time fields of a start: local in @p zone, that instant's offset, UTC; all empty with no start. */
QStringList
timeFields(qint64 when, const QTimeZone& zone) {
    if (when <= 0) {
        return {QString(), QString(), QString()};
    }
    const QDateTime local = QDateTime::fromSecsSinceEpoch(when, zone);
    const QDateTime utc = QDateTime::fromSecsSinceEpoch(when, QTimeZone::UTC);
    if (!local.isValid() || !utc.isValid()) {
        return {QString(), QString(), QString()};
    }
    // toString() with a format names and numbers in the C locale whatever the default locale.
    return {local.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")), offsetText(local.offsetFromUtc()),
            utc.toString(Qt::ISODate)};
}

/* A number that 0 means "not known" for: empty then. */
QString
knownNumber(qulonglong value) {
    return value != 0U ? QString::number(value) : QString();
}

QString
flag(bool on) {
    return on ? QStringLiteral("1") : QStringLiteral("0");
}

/* `NFM` or `AM` as heard, `analog` for an analog row whose mode is not known, empty for a digital row. */
QString
analogModeText(const CallHistoryModel::ExportRow& row) {
    if (!row.analog) {
        return QString();
    }
    return row.analogMode.isEmpty() ? QStringLiteral("analog") : row.analogMode;
}

QStringList
fields(const CallHistoryModel::ExportRow& row, const QTimeZone& zone) {
    QStringList out = timeFields(row.when, zone);
    out << (row.durationSecs >= 0 ? QString::number(row.durationSecs) : QString())
        << (row.kind == CallHistoryModel::KindNotice ? QStringLiteral("message") : QStringLiteral("call")) << row.name
        << knownNumber(row.tg) << knownNumber(row.src) << row.sourceName << row.systemName << row.channel
        << (row.freqHz > 0 ? QString::number(row.freqHz) : QString()) << row.accessCode << row.receivedTone
        << row.siteText << analogModeText(row) << flag(row.enc) << flag(row.emergency) << row.detail
        << joinRecordingNames(row.recordingNames);
    return out;
}

} // namespace

QByteArray
file(const QList<CallHistoryModel::ExportRow>& rows, const QTimeZone& zone) {
    const QVector<bool> text = textColumns();
    QByteArray out = csv_text::bom();
    out += csv_text::record(header(), QVector<bool>(text.size(), false));
    for (const CallHistoryModel::ExportRow& row : rows) {
        out += csv_text::record(fields(row, zone), text);
    }
    return out;
}

QString
joinRecordingNames(const QStringList& names) {
    QStringList escaped;
    escaped.reserve(names.size());
    for (QString name : names) {
        name.replace(QLatin1Char('\\'), QLatin1String("\\\\"));
        name.replace(QLatin1Char('|'), QLatin1String("\\|"));
        escaped.append(name);
    }
    return escaped.join(kSeparator);
}

QStringList
splitRecordingNames(const QString& cell) {
    QStringList names;
    if (cell.isEmpty()) {
        return names;
    }
    QString name;
    for (qsizetype i = 0; i < cell.size();) {
        if (cell.at(i) == QLatin1Char('\\') && i + 1 < cell.size()) {
            // An escaped character (a backslash or a pipe) is part of the name.
            name.append(cell.at(i + 1));
            i += 2;
        } else if (QStringView(cell).mid(i, kSeparator.size()) == kSeparator) {
            names.append(name);
            name.clear();
            i += kSeparator.size();
        } else {
            name.append(cell.at(i));
            i++;
        }
    }
    names.append(name);
    return names;
}

} // namespace call_history_export
} // namespace dsd_qt
