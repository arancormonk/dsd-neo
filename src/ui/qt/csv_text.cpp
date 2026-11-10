// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "csv_text.h"

#include <QChar>
#include <QtGlobal>

namespace dsd_qt {
namespace csv_text {

namespace {

bool
startsFormula(QChar c) {
    switch (c.unicode()) {
        case u'=':
        case u'+':
        case u'-':
        case u'@':
        case u'\t':
        case u'\r':
        case u'\n':
        case 0xFF1D: // full-width =
        case 0xFF0B: // full-width +
        case 0xFF0D: // full-width -
        case 0xFF20: // full-width @
            return true;
        default: return false;
    }
}

} // namespace

QByteArray
bom() {
    return QByteArray("\xEF\xBB\xBF", 3);
}

QString
field(const QString& value, bool text) {
    QString out = value;
    if (text && !out.isEmpty() && startsFormula(out.at(0))) {
        out.prepend(QLatin1Char('\''));
    }
    if (out.contains(QLatin1Char(',')) || out.contains(QLatin1Char('"')) || out.contains(QLatin1Char('\r'))
        || out.contains(QLatin1Char('\n'))) {
        out.replace(QLatin1Char('"'), QStringLiteral("\"\""));
        out = QLatin1Char('"') + out + QLatin1Char('"');
    }
    return out;
}

QByteArray
record(const QStringList& fields, const QVector<bool>& text) {
    if (fields.size() != text.size()) {
        return QByteArray();
    }
    QStringList cells;
    cells.reserve(fields.size());
    for (qsizetype i = 0; i < fields.size(); ++i) {
        cells.append(field(fields.at(i), text.at(i)));
    }
    return cells.join(QLatin1Char(',')).toUtf8() + QByteArray("\r\n");
}

} // namespace csv_text
} // namespace dsd_qt
