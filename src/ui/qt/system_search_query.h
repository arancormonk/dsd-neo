// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief How the saved-systems search reads its terms: frequencies in MHz or kHz, and folded text.
 *
 * History's frequency_query reads a bare number as an ID, because talkgroups collide with MHz values there. A saved
 * system has no IDs to search, so this adapter also reads a bare number of three or more digits as whole MHz ("851"
 * finds 851.0125) and a kHz term as MHz ("460000 kHz" is 460.000 MHz), then matches with frequency_query_matches().
 * A saved frequency itself is read as the tuner reads it, so every spelling that tunes is found. History is
 * unchanged.
 */

#ifndef DSD_NEO_SRC_UI_QT_SYSTEM_SEARCH_QUERY_H_
#define DSD_NEO_SRC_UI_QT_SYSTEM_SEARCH_QUERY_H_

#include <QString>
#include <QStringList>
#include <QtGlobal>

#include "frequency_query.h"

namespace dsd_qt {

/**
 * @brief The terms of a saved-systems search: blank-separated, with a number and the unit token after it ("851 MHz",
 *        "460 kHz") kept as one term. Every term must match.
 */
QStringList system_search_terms(const QString& query);

/** @brief @p term read as a frequency: a bare number of 3+ digits or a decimal is MHz, "N kHz" is N / 1000 MHz. */
FrequencyQuery system_search_frequency(const QString& term);

/**
 * @brief A saved system's freqMhz text in Hz as the tuner reads it (session_args_freq_hz()), or 0 when it does not
 *        tune. Only query terms use the stricter search grammar above.
 */
qint64 system_search_hz(const QString& freqMhz);

/** @brief Whether @p term, read as a frequency, finds any of @p freqsMhz (saved freqMhz texts). */
bool system_search_matches(const QString& term, const QStringList& freqsMhz);

/**
 * @brief @p text as a text term or haystack compares it: text_fold(), then the separators an import writes
 *        (— – - · ,) as blanks, and blank runs as one.
 */
QString system_search_text(const QString& text);

} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_SYSTEM_SEARCH_QUERY_H_ */
