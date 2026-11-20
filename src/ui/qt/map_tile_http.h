// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The HTTP field parsers the map's tile cache and tile worker share (issue #800): HTTP-date, delta-seconds,
 *        Cache-Control and Vary (RFC 9110, RFC 9111). Pure functions, no Qt beyond QByteArray and QList.
 */

#ifndef DSD_NEO_SRC_UI_QT_MAP_TILE_HTTP_H_
#define DSD_NEO_SRC_UI_QT_MAP_TILE_HTTP_H_

#include <QByteArray>
#include <QList>
#include <QtGlobal>

namespace dsd_qt {

namespace map_tile_http {

/** @brief The response directives a private tile cache acts on. Unknown directives are ignored. */
struct CacheControl {
    bool noStore = false;
    bool noCache = false;
    bool mustRevalidate = false;
    bool hasMaxAge = false;
    /** `max-age` without a valid delta-seconds value, or two `max-age` values that differ. */
    bool maxAgeMalformed = false;
    /** The value could not be split into directives (an unterminated quoted string): nothing in it can be trusted. */
    bool syntaxError = false;
    qint64 maxAge = 0;
};

/** @brief Parse a (possibly joined) Cache-Control value; names are case-insensitive, arguments token or quoted. */
CacheControl parseCacheControl(const QByteArray& value);

/**
 * @brief Parse delta-seconds (RFC 9111 §1.2.2): one or more digits, nothing else.
 *
 * A value past 2^31 reads as 2^31, as the RFC requires. @return false for anything else.
 */
bool parseDeltaSeconds(const QByteArray& text, qint64* seconds);

/**
 * @brief Parse an HTTP-date in any of its three forms (IMF-fixdate, RFC 850, asctime; RFC 9110 §5.6.7).
 *
 * @param nowSecs Wall-clock now, for the RFC 850 two-digit year (a year more than 50 years ahead is the century
 *                before). @return false when the text is not a valid date ("0" included).
 */
bool parseHttpDate(const QByteArray& text, qint64 nowSecs, qint64* secs);

/**
 * @brief Split a Vary value into lowercase field names.
 *
 * @param star Set when the value holds `*`, or anything that is not a field name (failing closed: never stored).
 */
QList<QByteArray> parseVary(const QByteArray& value, bool* star);

} // namespace map_tile_http

} // namespace dsd_qt

#endif // DSD_NEO_SRC_UI_QT_MAP_TILE_HTTP_H_
