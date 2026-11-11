// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "system_search_query.h"

#include <QChar>
#include <QLatin1Char>
#include <QLatin1String>
#include <QList>
#include <algorithm>

#include "frequency_query.h"
#include "text_order.h"

namespace dsd_qt {

namespace {

constexpr int kMinBareMhzDigits = 3;
constexpr int kKhzPerMhzDigits = 3;
constexpr int kHzFractionDigits = 6;

bool
ascii_digits(const QString& text) {
    return !text.isEmpty() && std::all_of(text.cbegin(), text.cend(), [](QChar ch) {
        return ch >= QLatin1Char('0') && ch <= QLatin1Char('9');
    });
}

/* Digits, optionally with one point and more digits after it ("851", "851.", "851.0125"). */
bool
is_number(const QString& text) {
    const qsizetype point = text.indexOf(QLatin1Char('.'));
    if (point < 0) {
        return ascii_digits(text);
    }
    const QString fraction = text.mid(point + 1);
    return ascii_digits(text.left(point)) && (fraction.isEmpty() || ascii_digits(fraction));
}

bool
is_unit(const QString& token) {
    return token.compare(QLatin1String("mhz"), Qt::CaseInsensitive) == 0
           || token.compare(QLatin1String("khz"), Qt::CaseInsensitive) == 0;
}

/* "460000" kHz as MHz text, "460.000"; a fraction of a kHz carries on past the third decimal. */
FrequencyQuery
khz_frequency(const QString& body) {
    if (!is_number(body)) {
        return {};
    }
    const qsizetype point = body.indexOf(QLatin1Char('.'));
    const QString whole = point < 0 ? body : body.left(point);
    const QString fraction = point < 0 ? QString() : body.mid(point + 1);
    const QString padded = whole.rightJustified(kKhzPerMhzDigits + 1, QLatin1Char('0'));
    const qsizetype split = padded.size() - kKhzPerMhzDigits;
    return frequency_query_parse(padded.left(split) + QLatin1Char('.') + padded.mid(split) + fraction);
}

} // namespace

QStringList
system_search_terms(const QString& query) {
    const QStringList tokens = query.simplified().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    QStringList terms;
    for (qsizetype i = 0; i < tokens.size(); ++i) {
        if (i + 1 < tokens.size() && is_number(tokens.at(i)) && is_unit(tokens.at(i + 1))) {
            terms.append(tokens.at(i) + QLatin1Char(' ') + tokens.at(i + 1));
            ++i;
        } else {
            terms.append(tokens.at(i));
        }
    }
    return terms;
}

FrequencyQuery
system_search_frequency(const QString& term) {
    const QString text = term.trimmed();
    if (text.endsWith(QLatin1String("khz"), Qt::CaseInsensitive)) {
        return khz_frequency(text.left(text.size() - 3).trimmed());
    }
    if (ascii_digits(text) && text.size() >= kMinBareMhzDigits) {
        return frequency_query_parse(text + QLatin1String(" MHz"));
    }
    return frequency_query_parse(text);
}

qint64
system_search_hz(const QString& freqMhz) {
    const QString text = freqMhz.trimmed();
    const bool spelled = text.contains(QLatin1Char('.')) || text.endsWith(QLatin1String("mhz"), Qt::CaseInsensitive);
    const FrequencyQuery parsed = frequency_query_parse(spelled ? text : text + QLatin1String(" MHz"));
    if (!parsed.valid) {
        return 0;
    }
    bool ok = false;
    const qint64 fraction = parsed.fraction.leftJustified(kHzFractionDigits, QLatin1Char('0')).toLongLong(&ok);
    return ok ? parsed.mhz * 1000000 + fraction : 0;
}

bool
system_search_matches(const QString& term, const QStringList& freqsMhz) {
    const FrequencyQuery query = system_search_frequency(term);
    return query.valid && std::any_of(freqsMhz.cbegin(), freqsMhz.cend(), [&query](const QString& freq) {
               return frequency_query_matches(query, system_search_hz(freq));
           });
}

QString
system_search_text(const QString& text) {
    QString folded = text_fold(text);
    for (QChar& ch : folded) {
        const char16_t unit = ch.unicode();
        if (unit == u'—' || unit == u'–' || unit == u'-' || unit == u'·' || unit == u',') {
            ch = QLatin1Char(' ');
        }
    }
    return folded.simplified();
}

} // namespace dsd_qt
