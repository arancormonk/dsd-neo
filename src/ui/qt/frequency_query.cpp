// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "frequency_query.h"

#include <QByteArray>
#include <QChar>
#include <QLatin1String>
#include <algorithm>
#include <dsd-neo/core/parse.h>
#include <utility>

namespace dsd_qt {

namespace {

constexpr int kMaxMhzDigits = 5;
constexpr int kMaxFractionDigits = 6;

/** @brief Whether @p text is ASCII digits only (an empty string is). */
bool
all_ascii_digits(const QString& text) {
    return std::all_of(text.cbegin(), text.cend(),
                       [](QChar ch) { return ch >= QLatin1Char('0') && ch <= QLatin1Char('9'); });
}

/** @brief Whether @p fraction begins @p digits, the row's fraction written @p width digits wide. */
bool
fraction_begins(const QString& fraction, qint64 digits, int width) {
    return QString::number(digits).rightJustified(width, QLatin1Char('0')).startsWith(fraction);
}

} // namespace

FrequencyQuery
frequency_query_parse(const QString& text) {
    QString body = text.trimmed();
    bool unit = false;
    if (body.endsWith(QLatin1String("mhz"), Qt::CaseInsensitive)) {
        body.chop(3);
        body = body.trimmed();
        unit = true;
    }
    const qsizetype point = body.indexOf(QLatin1Char('.'));
    if (point < 0 && !unit) {
        return {};
    }
    const QString whole = point < 0 ? body : body.left(point);
    QString fraction = point < 0 ? QString() : body.mid(point + 1);
    if (whole.isEmpty() || whole.size() > kMaxMhzDigits) {
        return {};
    }
    if (!all_ascii_digits(whole) || !all_ascii_digits(fraction)) {
        return {};
    }
    while (fraction.size() > kMaxFractionDigits && fraction.endsWith(QLatin1Char('0'))) {
        fraction.chop(1);
    }
    if (fraction.size() > kMaxFractionDigits) {
        return {};
    }
    int mhz = 0;
    if (dsd_parse_int_strict(whole.toLatin1().constData(), 10, 0, 99999, &mhz) != 0) {
        return {};
    }
    FrequencyQuery query;
    query.valid = true;
    query.mhz = mhz;
    query.fraction = std::move(fraction);
    return query;
}

bool
frequency_query_matches(const FrequencyQuery& query, qint64 hz) {
    if (!query.valid || hz <= 0) {
        return false;
    }
    // To the Hz: six decimals, and in its first five the frequency rounded down.
    if (hz / 1000000 == query.mhz && fraction_begins(query.fraction, hz % 1000000, kMaxFractionDigits)) {
        return true;
    }
    if (query.fraction.size() > kMaxFractionDigits - 1) {
        return false;
    }
    // To 10 Hz, half up: the frequency rounded up wherever the row's text was. Quotient
    // and remainder, so no positive qint64 overflows.
    const qint64 tens = hz / 10 + (hz % 10 >= 5 ? 1 : 0);
    return tens / 100000 == query.mhz && fraction_begins(query.fraction, tens % 100000, kMaxFractionDigits - 1);
}

} // namespace dsd_qt
