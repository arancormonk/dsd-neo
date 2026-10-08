// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Merge and re-ingest policy for the Qt call history, free of Qt.
 *
 * The Qt Quick frontend only builds for Android, so the decisions that make or
 * break the history — when two committed ring rows are one conversation, which
 * fragment's source label, frequency and access code a merged row shows, and
 * when an already-ingested row has learned enough to be worth re-reading — live
 * here as plain functions the host test suite can exercise.
 */

#ifndef DSD_NEO_SRC_UI_QT_CALL_HISTORY_MERGE_H_
#define DSD_NEO_SRC_UI_QT_CALL_HISTORY_MERGE_H_

#include <dsd-neo/core/access_code.h>
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
 * stamp extends, a late-decoded source id fills 0 -> real, and the crypto verdict
 * and emergency can flip on. The row's key does not change when that happens, so
 * this comparison — against what was last read, not against presence in a seen set —
 * is the only way those merges ever reach the display and the persisted log.
 */
inline bool
call_history_seen_row_advanced(int64_t stored_end, uint64_t stored_src, bool stored_enc, int64_t end, uint64_t src,
                               bool enc, bool stored_emergency = false, bool emergency = false) {
    return end > stored_end || (stored_src == 0U && src != 0U) || (!stored_enc && enc)
           || (!stored_emergency && emergency);
}

/**
 * @brief Fold a fresh read of a seen ring row into what was last recorded.
 *
 * One-way ratchets, matching the core's own merge semantics: the end never
 * retreats, a learned source id never un-learns, the crypto verdict never
 * clears; emergency also latches on. This is the single definition both the live
 * noteSeen() path and its tests share, so the ratchet cannot drift from the
 * advance test above.
 *
 * @return true when the row had advanced and the stored values were updated.
 */
inline bool
call_history_seen_absorb(int64_t* stored_end, uint64_t* stored_src, bool* stored_enc, int64_t end, uint64_t src,
                         bool enc, bool* stored_emergency = nullptr, bool emergency = false) {
    if (!call_history_seen_row_advanced(*stored_end, *stored_src, *stored_enc, end, src, enc,
                                        stored_emergency ? *stored_emergency : false, emergency)) {
        return false;
    }
    if (end > *stored_end) {
        *stored_end = end;
    }
    if (*stored_src == 0U && src != 0U) {
        *stored_src = src;
    }
    *stored_enc = *stored_enc || enc;
    if (stored_emergency) {
        *stored_emergency = *stored_emergency || emergency;
    }
    return true;
}

/**
 * @brief Which fragment a merged row's field came from: its start, push stamp and slot.
 *
 * Fragments reach the merge in whichever order a refresh happens to collect them -- a
 * backlog walks newest-first, a live session arrives oldest-first -- so a field has to be
 * chosen by the fragment it came from rather than by arrival. Each field keeps its own:
 * the newest fragment that knew a source label need not be the newest that knew the
 * frequency.
 */
struct CallHistoryProvenance {
    int64_t when; /**< The fragment's start. */
    uint64_t seq; /**< Its push stamp, which breaks a same-second tie. */
    int slot;     /**< Its TDMA slot, which breaks a tie between the slots' rings. */
};

/**
 * @brief Order two fragments: by start, then push stamp, then slot.
 * @return -1 when @p a is older than @p b, 1 when newer, 0 for the same fragment.
 */
inline int
call_history_provenance_compare(const CallHistoryProvenance& a, const CallHistoryProvenance& b) {
    if (a.when != b.when) {
        return a.when < b.when ? -1 : 1;
    }
    if (a.seq != b.seq) {
        return a.seq < b.seq ? -1 : 1;
    }
    if (a.slot != b.slot) {
        return a.slot < b.slot ? -1 : 1;
    }
    return 0;
}

/**
 * @brief Whether a fragment's value of one field replaces the merged row's.
 *
 * The per-field fold behind the source label, the frequency and the access code: the
 * newest fragment with a known value wins, unknown never erases known, and a known value
 * fills an unknown one whatever its age. Equal provenance adopts, since that is the same
 * fragment filling itself in (an in-place update). So fragments folded in any order give
 * the same row. The caller takes the incoming provenance with the value.
 *
 * @param stored_known   The row holds a known value.
 * @param stored         The fragment the row's value came from.
 * @param incoming_known The fragment carries a known value.
 * @param incoming       The fragment.
 */
inline bool
call_history_fold_adopts(bool stored_known, const CallHistoryProvenance& stored, bool incoming_known,
                         const CallHistoryProvenance& incoming) {
    if (!incoming_known) {
        return false;
    }
    return !stored_known || call_history_provenance_compare(incoming, stored) >= 0;
}

/** @brief Whether a row's frequency is known: 0 (and anything not positive) is unknown. */
inline bool
call_history_freq_known(int64_t freq_hz) {
    return freq_hz > 0;
}

/** @brief Whether a row's access code is known: any kind but DSD_ACCESS_CODE_NONE. */
inline bool
call_history_access_code_known(int kind) {
    return kind != static_cast<int>(DSD_ACCESS_CODE_NONE);
}

/**
 * @brief Fold a fresh read of a seen ring row's frequency and access code into what was last recorded.
 *
 * The core fills a committed row's frequency or code only while it is unknown (a
 * reacquisition merge learning one), so unknown -> known is the one change that makes the
 * row worth re-reading, and it is recorded. A read that changes a known value into another
 * is ignored and recorded nowhere, and unknown never erases.
 *
 * @return true when either field was learned.
 */
inline bool
call_history_seen_absorb_fill(int64_t* stored_freq_hz, int* stored_ac_kind, int* stored_ac, int64_t freq_hz,
                              int ac_kind, int ac) {
    bool learned = false;
    if (!call_history_freq_known(*stored_freq_hz) && call_history_freq_known(freq_hz)) {
        *stored_freq_hz = freq_hz;
        learned = true;
    }
    if (!call_history_access_code_known(*stored_ac_kind) && call_history_access_code_known(ac_kind)) {
        *stored_ac_kind = ac_kind;
        *stored_ac = ac;
        learned = true;
    }
    return learned;
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
