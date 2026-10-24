// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief A History search read as a frequency in MHz, and the rows it finds.
 *
 * A query is a frequency only when it says so: digits with a decimal point
 * ("851.0125", "851."), optionally followed by "MHz", or whole MHz with the unit
 * ("851 MHz"). A bare number stays an ID search, because three- and four-digit
 * talkgroups collide with MHz values (TG 851 on an 800 MHz system).
 *
 * A frequency query finds a row when its digits begin the row's frequency as the
 * app writes it, in either spelling: rounded to 10 Hz (Util.mhzText, the row's own
 * text) or to the Hz (the six decimals the Site and Network sheets print).
 * Util.mhzText formats the inexact double hz / 1e6 with toFixed(5), so it rounds a
 * frequency down or up; down is a prefix of the six-decimal spelling and up is
 * the rounded one. Whatever the row prints, typing it finds the row.
 */

#ifndef DSD_NEO_SRC_UI_QT_FREQUENCY_QUERY_H_
#define DSD_NEO_SRC_UI_QT_FREQUENCY_QUERY_H_

#include <QString>
#include <QtGlobal>

namespace dsd_qt {

/** @brief A search text parsed as a frequency; @c valid is false for anything else. */
struct FrequencyQuery {
    bool valid = false;
    qint64 mhz = 0;   /**< whole MHz, 0..99999 */
    QString fraction; /**< the digits after the point, 0..6 of them */
};

/**
 * @brief Read @p text (already trimmed or not) as a frequency query.
 *
 * Grammar: `digits "." digits* [ws] ["MHz"]` or `digits [ws] "MHz"`, the unit in any
 * case. At most five MHz digits; fraction digits past the sixth must be zeros and
 * are dropped. No sign, no decimal comma, no locale.
 */
FrequencyQuery frequency_query_parse(const QString& text);

/** @brief Whether @p query finds a row heard on @p hz; never for an invalid query or @p hz <= 0. */
bool frequency_query_matches(const FrequencyQuery& query, qint64 hz);

} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_FREQUENCY_QUERY_H_ */
