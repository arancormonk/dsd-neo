// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "map_tile_http.h"

#include <algorithm>
#include <iterator>

namespace dsd_qt {

namespace {

/* RFC 9111 §1.2.2: a delta-seconds value past what the cache can hold is taken as 2^31. */
constexpr qint64 kDeltaSecondsCap = 2147483648LL;

bool
is_ows(char c) {
    return c == ' ' || c == '\t';
}

bool
is_digit(char c) {
    return c >= '0' && c <= '9';
}

bool
is_tchar(char c) {
    if (is_digit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
        return true;
    }
    static const char kExtra[] = "!#$%&'*+-.^_`|~";
    for (const char* p = kExtra; *p != '\0'; ++p) {
        if (*p == c) {
            return true;
        }
    }
    return false;
}

/* Read @p count digits at @p pos; @return false when any is not a digit. */
bool
read_digits(const QByteArray& text, int pos, int count, int* value) {
    if (pos < 0 || pos + count > text.size()) {
        return false;
    }
    int v = 0;
    for (int i = 0; i < count; ++i) {
        const char c = text.at(pos + i);
        if (!is_digit(c)) {
            return false;
        }
        v = v * 10 + (c - '0');
    }
    *value = v;
    return true;
}

int
month_index(const QByteArray& name) {
    static const char* const kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                          "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    for (int i = 0; i < 12; ++i) {
        if (name == kMonths[i]) {
            return i + 1;
        }
    }
    return 0;
}

bool
is_short_day(const QByteArray& name) {
    static const char* const kDays[] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
    return std::any_of(std::begin(kDays), std::end(kDays), [&name](const char* day) { return name == day; });
}

bool
is_long_day(const QByteArray& name) {
    static const char* const kDays[] = {"Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"};
    return std::any_of(std::begin(kDays), std::end(kDays), [&name](const char* day) { return name == day; });
}

bool
is_leap(int year) {
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

int
days_in_month(int year, int month) {
    static const int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return month == 2 && is_leap(year) ? 29 : kDays[month - 1];
}

/* Days since 1970-01-01 of a proleptic Gregorian date (H. Hinnant's days_from_civil). */
qint64
days_from_civil(qint64 y, int m, int d) {
    y -= m <= 2 ? 1 : 0;
    const qint64 era = (y >= 0 ? y : y - 399) / 400;
    const qint64 yoe = y - era * 400;
    const qint64 doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const qint64 doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

/* The civil year of @p days since 1970-01-01. */
qint64
year_from_days(qint64 days) {
    const qint64 z = days + 719468;
    const qint64 era = (z >= 0 ? z : z - 146096) / 146097;
    const qint64 doe = z - era * 146097;
    const qint64 yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const qint64 doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const qint64 mp = (5 * doy + 2) / 153;
    const qint64 month = mp < 10 ? mp + 3 : mp - 9;
    return yoe + era * 400 + (month <= 2 ? 1 : 0);
}

/* "hh:mm:ss" at @p pos. */
bool
read_time(const QByteArray& text, int pos, int* seconds) {
    int h = 0;
    int m = 0;
    int s = 0;
    if (!read_digits(text, pos, 2, &h) || text.at(pos + 2) != ':' || !read_digits(text, pos + 3, 2, &m)
        || text.at(pos + 5) != ':' || !read_digits(text, pos + 6, 2, &s)) {
        return false;
    }
    if (h > 23 || m > 59 || s > 60) {
        return false;
    }
    *seconds = h * 3600 + m * 60 + s;
    return true;
}

struct CivilDate {
    int year = 0;
    int month = 0;
    int day = 0;
    int seconds = 0;
};

bool
civil_to_secs(const CivilDate& date, qint64* secs) {
    if (date.month < 1 || date.month > 12 || date.day < 1 || date.day > days_in_month(date.year, date.month)) {
        return false;
    }
    *secs = days_from_civil(date.year, date.month, date.day) * 86400 + date.seconds;
    return true;
}

/* IMF-fixdate: "Sun, 06 Nov 1994 08:49:37 GMT". */
bool
parse_imf_fixdate(const QByteArray& t, qint64* secs) {
    CivilDate d;
    if (t.size() != 29 || !is_short_day(t.left(3)) || t.at(3) != ',' || t.at(4) != ' ' || t.at(7) != ' '
        || t.at(11) != ' ' || t.at(16) != ' ' || t.at(25) != ' ' || t.mid(26) != "GMT") {
        return false;
    }
    d.month = month_index(t.mid(8, 3));
    if (!read_digits(t, 5, 2, &d.day) || !read_digits(t, 12, 4, &d.year) || !read_time(t, 17, &d.seconds)) {
        return false;
    }
    return civil_to_secs(d, secs);
}

/* RFC 850: "Sunday, 06-Nov-94 08:49:37 GMT"; a two-digit year more than 50 years ahead is the century before. */
bool
parse_rfc850_date(const QByteArray& t, qint64 nowSecs, qint64* secs) {
    const int comma = t.indexOf(',');
    if (comma < 0 || !is_long_day(t.left(comma))) {
        return false;
    }
    const QByteArray rest = t.mid(comma + 1);
    CivilDate d;
    int yy = 0;
    if (rest.size() != 23 || rest.at(0) != ' ' || rest.at(3) != '-' || rest.at(7) != '-' || rest.at(10) != ' '
        || rest.at(19) != ' ' || rest.mid(20) != "GMT") {
        return false;
    }
    d.month = month_index(rest.mid(4, 3));
    if (!read_digits(rest, 1, 2, &d.day) || !read_digits(rest, 8, 2, &yy) || !read_time(rest, 11, &d.seconds)) {
        return false;
    }
    const qint64 nowYear = year_from_days(nowSecs / 86400);
    qint64 year = (nowYear / 100) * 100 + yy;
    if (year > nowYear + 50) {
        year -= 100;
    }
    d.year = static_cast<int>(year);
    return civil_to_secs(d, secs);
}

/* asctime: "Sun Nov  6 08:49:37 1994". */
bool
parse_asctime_date(const QByteArray& t, qint64* secs) {
    CivilDate d;
    if (t.size() != 24 || !is_short_day(t.left(3)) || t.at(3) != ' ' || t.at(7) != ' ' || t.at(10) != ' '
        || t.at(19) != ' ') {
        return false;
    }
    d.month = month_index(t.mid(4, 3));
    const bool dayOk = t.at(8) == ' ' ? read_digits(t, 9, 1, &d.day) : read_digits(t, 8, 2, &d.day);
    if (!dayOk || !read_time(t, 11, &d.seconds) || !read_digits(t, 20, 4, &d.year)) {
        return false;
    }
    return civil_to_secs(d, secs);
}

/* Skip OWS from @p i. */
int
skip_ows(const QByteArray& v, int i) {
    while (i < v.size() && is_ows(v.at(i))) {
        ++i;
    }
    return i;
}

/* A quoted-string starting at @p i (the opening quote); @return the index past the closing quote, or -1. */
int
read_quoted(const QByteArray& v, int i, QByteArray* out) {
    ++i;
    while (i < v.size()) {
        const char c = v.at(i);
        if (c == '"') {
            return i + 1;
        }
        if (c == '\\') {
            ++i;
            if (i >= v.size()) {
                return -1;
            }
        }
        out->append(v.at(i));
        ++i;
    }
    return -1;
}

void
apply_directive(map_tile_http::CacheControl* cc, const QByteArray& name, bool hasArg, bool argOk,
                const QByteArray& arg) {
    if (name == "no-store") {
        cc->noStore = true;
    } else if (name == "no-cache") {
        cc->noCache = true;
    } else if (name == "must-revalidate") {
        cc->mustRevalidate = true;
    } else if (name == "max-age") {
        qint64 value = 0;
        if (!hasArg || !argOk || !map_tile_http::parseDeltaSeconds(arg, &value)) {
            cc->maxAgeMalformed = true;
        } else if (cc->hasMaxAge && cc->maxAge != value) {
            cc->maxAgeMalformed = true;
        } else {
            cc->hasMaxAge = true;
            cc->maxAge = value;
        }
    }
}

/* Read one directive's argument after its '='; @return the index after it, or -1 for an unterminated quote. */
int
read_argument(const QByteArray& v, int i, QByteArray* arg, bool* argOk) {
    i = skip_ows(v, i);
    if (i < v.size() && v.at(i) == '"') {
        i = read_quoted(v, i, arg);
        if (i < 0) {
            return -1;
        }
        i = skip_ows(v, i);
        if (i < v.size() && v.at(i) != ',') {
            *argOk = false;
        }
    } else {
        const int start = i;
        while (i < v.size() && v.at(i) != ',') {
            ++i;
        }
        *arg = v.mid(start, i - start).trimmed();
    }
    while (i < v.size() && v.at(i) != ',') {
        ++i;
    }
    return i;
}

} // namespace

namespace map_tile_http {

bool
parseDeltaSeconds(const QByteArray& text, qint64* seconds) {
    if (text.isEmpty()) {
        return false;
    }
    qint64 value = 0;
    for (const char c : text) {
        if (!is_digit(c)) {
            return false;
        }
        value = value * 10 + (c - '0');
        if (value > kDeltaSecondsCap) {
            value = kDeltaSecondsCap;
        }
    }
    *seconds = value;
    return true;
}

bool
parseHttpDate(const QByteArray& text, qint64 nowSecs, qint64* secs) {
    return parse_imf_fixdate(text, secs) || parse_rfc850_date(text, nowSecs, secs) || parse_asctime_date(text, secs);
}

CacheControl
parseCacheControl(const QByteArray& value) {
    CacheControl cc;
    int i = 0;
    while (i < value.size()) {
        while (i < value.size() && (is_ows(value.at(i)) || value.at(i) == ',')) {
            ++i;
        }
        if (i >= value.size()) {
            break;
        }
        const int start = i;
        while (i < value.size() && value.at(i) != '=' && value.at(i) != ',') {
            ++i;
        }
        const QByteArray name = value.mid(start, i - start).trimmed().toLower();
        QByteArray arg;
        bool argOk = true;
        const bool hasArg = i < value.size() && value.at(i) == '=';
        if (hasArg) {
            i = read_argument(value, i + 1, &arg, &argOk);
            if (i < 0) {
                cc.syntaxError = true;
                return cc;
            }
        }
        apply_directive(&cc, name, hasArg, argOk, arg);
    }
    return cc;
}

QList<QByteArray>
parseVary(const QByteArray& value, bool* star) {
    QList<QByteArray> names;
    *star = false;
    const QList<QByteArray> parts = value.split(',');
    for (const QByteArray& part : parts) {
        const QByteArray name = part.trimmed();
        if (name.isEmpty()) {
            continue;
        }
        if (name == "*" || !std::all_of(name.cbegin(), name.cend(), is_tchar)) {
            *star = true;
            continue;
        }
        names.append(name.toLower());
    }
    return names;
}

} // namespace map_tile_http

} // namespace dsd_qt
