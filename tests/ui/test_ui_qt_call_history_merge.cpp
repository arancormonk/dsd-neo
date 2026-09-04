// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Unit tests: merge and re-ingest policy behind the Qt call history. The Qt
 * Quick frontend only builds for Android, so the decisions live in a Qt-free
 * header (call_history_merge.h) exactly to be testable here. */

#include <stdint.h>
#include <stdio.h>

#include "call_history_merge.h"
#include "dsd-neo/core/safe_api.h"

using dsd_qt::call_history_duration_secs;
using dsd_qt::call_history_merge_within_window;
using dsd_qt::call_history_seen_absorb;
using dsd_qt::call_history_seen_row_advanced;
using dsd_qt::kCallHistoryMaxPlausibleDurationSecs;
using dsd_qt::kCallMergeWindowSrcMatchedSecs;
using dsd_qt::kCallMergeWindowSrcUnknownSecs;

namespace {

int g_failures = 0;

void
expect(const char* what, bool got, bool want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %d want %d\n", what, got ? 1 : 0, want ? 1 : 0);
        g_failures++;
    }
}

void
test_merge_window(void) {
    /* A retune fragment chains a couple of seconds off the previous fragment's
     * end; it must merge whether or not the src was ever learned. */
    expect("src-unknown fragment 2s after end merges", call_history_merge_within_window(100, 105, 107, 110, false),
           true);
    expect("src-matched fragment 2s after end merges", call_history_merge_within_window(100, 105, 107, 110, true),
           true);

    /* Two distinct back-to-back calls on one talkgroup, src never learned: a
     * 15 s gap is a new conversation, not a fragment (the regression this
     * policy exists for — the old 20 s wildcard window collapsed them). */
    expect("src-unknown call 15s after end stays distinct", call_history_merge_within_window(100, 105, 120, 128, false),
           false);
    /* The same 15 s gap from the same unit is one conversation resuming. */
    expect("src-matched call 15s after end merges", call_history_merge_within_window(100, 105, 120, 128, true), true);
    /* But even a matched src eventually times out. */
    expect("src-matched call past the window stays distinct",
           call_history_merge_within_window(100, 105, 105 + kCallMergeWindowSrcMatchedSecs + 1, 140, true), false);

    /* Symmetric: a row that ends just before the existing one starts. */
    expect("src-unknown earlier fragment merges",
           call_history_merge_within_window(100, 105, 90, 100 - kCallMergeWindowSrcUnknownSecs, false), true);
    expect("src-unknown earlier row outside window stays distinct",
           call_history_merge_within_window(100, 105, 80, 100 - kCallMergeWindowSrcUnknownSecs - 1, false), false);

    /* Containment (an update re-read of the same row) always overlaps. */
    expect("same-start update merges", call_history_merge_within_window(100, 105, 100, 145, false), true);
}

void
test_seen_row_advanced(void) {
    /* Nothing changed: the common rescan path must stay quiet. */
    expect("unchanged row is not re-read", call_history_seen_row_advanced(105, 1234, false, 105, 1234, false), false);

    /* The core's reacquisition merge extends the committed row's end in place. */
    expect("extended end re-reads", call_history_seen_row_advanced(105, 1234, false, 145, 1234, false), true);

    /* A late-decoded src fills 0 -> real without moving the end. */
    expect("learned src re-reads", call_history_seen_row_advanced(105, 0, false, 105, 1234, false), true);

    /* The crypto verdict can arrive with a reacquired segment's header. */
    expect("enc flip re-reads", call_history_seen_row_advanced(105, 1234, false, 105, 1234, true), true);

    /* One-way ratchets: src never un-learns, enc never clears, end never
     * retreats — a stale snapshot must not thrash updates. */
    expect("earlier end does not re-read", call_history_seen_row_advanced(145, 1234, true, 105, 1234, true), false);
    expect("src change does not re-read", call_history_seen_row_advanced(105, 1234, false, 105, 5678, false), false);
    expect("enc clear does not re-read", call_history_seen_row_advanced(105, 1234, true, 105, 1234, false), false);
}

void
expect_int(const char* what, int got, int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %d want %d\n", what, got, want);
        g_failures++;
    }
}

void
test_seen_absorb(void) {
    /* The absorb ratchet is the write half of the advance test: same one-way
     * rules, applied to the stored values. */
    int64_t end = 105;
    uint64_t src = 0;
    bool enc = false;

    expect("unchanged read absorbs nothing", call_history_seen_absorb(&end, &src, &enc, 105, 0, false), false);
    expect("advanced read absorbs", call_history_seen_absorb(&end, &src, &enc, 145, 1234, true), true);
    expect_int("end ratchets forward", (int)end, 145);
    expect_int("src fills once", (int)src, 1234);
    expect("enc latches on", enc, true);

    /* A stale snapshot must not unwind any of it. */
    expect("stale read absorbs nothing", call_history_seen_absorb(&end, &src, &enc, 105, 5678, false), false);
    expect_int("end never retreats", (int)end, 145);
    expect_int("src never re-learns", (int)src, 1234);
    expect("enc never clears", enc, true);
}

void
test_duration(void) {
    expect_int("measured span is the duration", call_history_duration_secs(100, 145), 45);
    expect_int("zero-length call is zero, not unknown", call_history_duration_secs(100, 100), 0);
    expect_int("missing start reads unknown", call_history_duration_secs(0, 145), -1);
    expect_int("end before start reads unknown", call_history_duration_secs(145, 100), -1);
    expect_int("implausible span reads unknown",
               call_history_duration_secs(100, 100 + kCallHistoryMaxPlausibleDurationSecs + 1), -1);
    expect_int("longest plausible span is kept",
               call_history_duration_secs(100, 100 + kCallHistoryMaxPlausibleDurationSecs),
               (int)kCallHistoryMaxPlausibleDurationSecs);
}

} // namespace

int
main(void) {
    test_merge_window();
    test_seen_row_advanced();
    test_seen_absorb();
    test_duration();
    if (g_failures != 0) {
        DSD_FPRINTF(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    DSD_FPRINTF(stderr, "OK\n");
    return 0;
}
