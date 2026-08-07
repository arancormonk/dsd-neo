// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Merge and re-ingest policy for the Qt call history, free of Qt.
 *
 * The Qt Quick frontend only builds for Android, so the decisions that make or
 * break the history — when two committed ring rows are one conversation, and
 * when an already-ingested row has learned enough to be worth re-reading — live
 * here as plain functions the host test suite can exercise.
 */

#ifndef DSD_NEO_SRC_UI_QT_CALL_HISTORY_MERGE_H_
#define DSD_NEO_SRC_UI_QT_CALL_HISTORY_MERGE_H_

#include <stdint.h>

namespace dsd_qt {

/* Trunk-following mints a committed row per tune attempt, so one keyed-up
 * talkgroup can shed several near-simultaneous fragments. Within this window a
 * same-target row is the same conversation, not a new call. (Plain constexpr,
 * not C++17 inline variables: the host test suite builds as C++14.) */
constexpr int64_t kCallMergeWindowSrcMatchedSecs = 20;

/* When either side never learned its source id the window has to be tight:
 * src==0 is treated as a wildcard, and on a busy talkgroup two genuinely
 * distinct back-to-back calls routinely land within 20 s of each other. A
 * retune fragment chains off the previous fragment's end within a few seconds;
 * a different unit answering usually does not. */
constexpr int64_t kCallMergeWindowSrcUnknownSecs = 6;

/**
 * @brief Whether two same-target voice rows are close enough to be one call.
 *
 * The window extends off each fragment's end, not its start: one keyed-up
 * talkgroup sheds a fragment per retune, and a fixed window off the first
 * start would leak a duplicate row every window-length of activity.
 *
 * @param src_known_match Both rows carry the same nonzero source id.
 */
inline bool
call_history_merge_within_window(int64_t existing_start, int64_t existing_end, int64_t row_start, int64_t row_end,
                                 bool src_known_match) {
    const int64_t window = src_known_match ? kCallMergeWindowSrcMatchedSecs : kCallMergeWindowSrcUnknownSecs;
    return row_start <= existing_end + window && existing_start <= row_end + window;
}

/**
 * @brief Whether a ring row already ingested has since learned something.
 *
 * The core merges a reacquired segment into its committed row in place: the end
 * stamp extends, a late-decoded source id fills 0 -> real, the crypto verdict
 * can flip on. The row's key does not change when that happens, so this
 * comparison — against what was last read, not against presence in a seen set —
 * is the only way those merges ever reach the display and the persisted log.
 */
inline bool
call_history_seen_row_advanced(int64_t stored_end, uint64_t stored_src, bool stored_enc, int64_t end, uint64_t src,
                               bool enc) {
    return end > stored_end || (stored_src == 0U && src != 0U) || (!stored_enc && enc);
}

/**
 * @brief Fold a fresh read of a seen ring row into what was last recorded.
 *
 * One-way ratchets, matching the core's own merge semantics: the end never
 * retreats, a learned source id never un-learns, the crypto verdict never
 * clears. This is the single definition both the live noteSeen() path and its
 * tests share, so the ratchet cannot drift from the advance test above.
 *
 * @return true when the row had advanced and the stored values were updated.
 */
inline bool
call_history_seen_absorb(int64_t* stored_end, uint64_t* stored_src, bool* stored_enc, int64_t end, uint64_t src,
                         bool enc) {
    if (!call_history_seen_row_advanced(*stored_end, *stored_src, *stored_enc, end, src, enc)) {
        return false;
    }
    if (end > *stored_end) {
        *stored_end = end;
    }
    if (*stored_src == 0U && src != 0U) {
        *stored_src = src;
    }
    *stored_enc = *stored_enc || enc;
    return true;
}

/* Sanity bound on a row's start/end stamps: a span longer than this is a corrupt
 * or clock-shifted row, not a measured call, so it renders as unknown. */
constexpr int64_t kCallHistoryMaxPlausibleDurationSecs = 3600;

/**
 * @brief Measured duration from a row's stamped ends, or -1 when unknown.
 *
 * The single definition of "plausible" shared by first ingest and every later
 * merge, so a duration a fresh row would refuse cannot sneak in via a merge.
 */
inline int
call_history_duration_secs(int64_t start, int64_t end) {
    if (start <= 0 || end < start || end - start > kCallHistoryMaxPlausibleDurationSecs) {
        return -1;
    }
    return static_cast<int>(end - start);
}

} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_CALL_HISTORY_MERGE_H_ */
