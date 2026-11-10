// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_QT_CSV_TEXT_H
#define DSD_NEO_QT_CSV_TEXT_H

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

namespace dsd_qt {
namespace csv_text {

/// The UTF-8 byte order mark that starts an export file.
QByteArray bom();

/// One CSV field. A text field whose first character could start a spreadsheet formula gets a
/// leading apostrophe; every field is then quoted per RFC 4180 when it holds a comma, quote, CR or LF.
/// Numeric callers pass `text == false` with an already formatted number.
QString field(const QString& value, bool text);

/// One record, UTF-8 encoded and CRLF terminated. `text[i]` says whether `fields[i]` is a text
/// column. Mismatched sizes return an empty array.
QByteArray record(const QStringList& fields, const QVector<bool>& text);

} // namespace csv_text
} // namespace dsd_qt

#endif
