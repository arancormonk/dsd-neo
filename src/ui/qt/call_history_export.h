// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The History export file: the rows History shows, as a CSV for a PC spreadsheet.
 *
 * A pure builder: it reads only the rows and the time zone it is given (CallHistoryModel::exportRows() and the
 * device's zone), never a clock. The file starts with a UTF-8 byte order mark, ends every record with CRLF, writes
 * every number and time in the same digits whatever the locale, and guards the text columns against spreadsheet
 * formulas (csv_text).
 */

#ifndef DSD_NEO_SRC_UI_QT_CALL_HISTORY_EXPORT_H_
#define DSD_NEO_SRC_UI_QT_CALL_HISTORY_EXPORT_H_

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>
#include <QTimeZone>

#include "call_history_model.h"

namespace dsd_qt {
namespace call_history_export {

/**
 * @brief The file: a header, then one record per row in the order given.
 *
 * Columns: `started_local,utc_offset,started_utc,duration_s,type,name,talkgroup,radio_id,radio_name,system,channel,`
 * `frequency_hz,access_code,tone,site,analog_mode,encrypted,emergency,message,recordings`. The start is written in
 * @p zone at that instant (`yyyy-MM-dd HH:mm:ss`), with that instant's offset (`UTC+05:30`), and in UTC (ISO 8601,
 * `Z`); all three are empty for a row with no start.
 */
QByteArray file(const QList<CallHistoryModel::ExportRow>& rows, const QTimeZone& zone);

/**
 * @brief The `recordings` cell: @p names joined by " | ", each backslash in a name written as two and each pipe as a
 *        backslash and a pipe, so splitRecordingNames() gives the names back whatever they hold.
 */
QString joinRecordingNames(const QStringList& names);

/**
 * @brief The names in a `recordings` cell: split on each " | " whose pipe is not escaped, then unescaped.
 *
 * For readers of the file (tests, docs); the app only writes the cell. An empty cell holds no names.
 */
QStringList splitRecordingNames(const QString& cell);

} // namespace call_history_export
} // namespace dsd_qt

#endif // DSD_NEO_SRC_UI_QT_CALL_HISTORY_EXPORT_H_
