// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The Signal search export files (#826): the detections and avoids CSV files, built from snapshots.
 *
 * Pure builders: they read only what they are given, so SignalSearchModel feeds them the store's snapshots and the
 * tests feed them records of their own. Both files start with a UTF-8 byte order mark, end every record with CRLF and
 * format every number with a `.` decimal point whatever the locale (csv_text).
 */

#ifndef DSD_NEO_SRC_UI_QT_SIGNAL_SEARCH_EXPORT_H_
#define DSD_NEO_SRC_UI_QT_SIGNAL_SEARCH_EXPORT_H_

#include <QByteArray>
#include <QString>
#include <dsd-neo/runtime/signal_search.h>
#include <functional>
#include <stddef.h>
#include <stdint.h>
#include <vector>

namespace dsd_qt {
namespace signal_search_export {

/** A detection's saved names, joined with " / " (empty when none): the `saved_names` column. */
using SavedNames = std::function<QString(const dsd_signal_search_hit&)>;

/**
 * @brief The detections file: a header, then one record per hit, in the order given (the store's newest first).
 *
 * The receiver settings a hit carries (input spec, PPM, bias tee, gain, Airspy settings) and its ids are left out.
 */
QByteArray detections(const dsd_signal_search_hit* hits, size_t count, const SavedNames& names);

/** Where the avoid lists are read from: the store's calls, or a test's. */
struct AvoidSource {
    std::function<uint64_t()> revision;
    std::function<size_t(uint32_t*, size_t)> permanent;
    std::function<size_t(uint32_t*, size_t)> session;
};

/** The store's avoid calls (dsd_app_signal_search_avoids_revision(), _avoids_get(), _session_avoids_get()). */
AvoidSource storeAvoids();

/** How many times readAvoids() reads both lists before it gives up on them changing. */
constexpr int kAvoidReadTries = 32;

/**
 * @brief Both avoid lists as one consistent pair: revision, permanent list, session list, revision, again until the
 *        two revisions are equal.
 * @return false when they still differed after kAvoidReadTries reads (the lists kept changing).
 */
bool readAvoids(const AvoidSource& source, std::vector<uint32_t>* permanent, std::vector<uint32_t>* session);

/**
 * @brief The avoids file: a header, the permanent list in its saved order (`permanent`), then the session list
 *        (`session`) without the frequencies the permanent list holds.
 * @param rows Set to the number of records under the header.
 */
QByteArray avoids(const std::vector<uint32_t>& permanent, const std::vector<uint32_t>& session, int* rows);

} // namespace signal_search_export
} // namespace dsd_qt

#endif // DSD_NEO_SRC_UI_QT_SIGNAL_SEARCH_EXPORT_H_
