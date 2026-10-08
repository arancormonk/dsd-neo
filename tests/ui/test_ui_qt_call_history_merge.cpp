// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Unit tests: merge and re-ingest policy behind the Qt call history. The Qt
 * Quick frontend only builds for Android, so the decisions live in a Qt-free
 * header (call_history_merge.h) exactly to be testable here. */

#include <algorithm>
#include <dsd-neo/core/access_code.h>
#include <stdint.h>
#include <stdio.h>

#include "call_history_merge.h"
#include "dsd-neo/core/safe_api.h"

using dsd_qt::call_history_access_code_known;
using dsd_qt::call_history_duration_secs;
using dsd_qt::call_history_fold_adopts;
using dsd_qt::call_history_freq_known;
using dsd_qt::call_history_merge_within_window;
using dsd_qt::call_history_provenance_compare;
using dsd_qt::call_history_seen_absorb;
using dsd_qt::call_history_seen_absorb_fill;
using dsd_qt::call_history_seen_row_advanced;
using dsd_qt::CallHistoryProvenance;
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
    bool emergency = false;
    expect("emergency-only enrichment advances",
           call_history_seen_absorb(&end, &src, &enc, end, src, enc, &emergency, true), true);
    expect("emergency latches on", emergency, true);
    expect("stale ordinary read does not advance",
           call_history_seen_absorb(&end, &src, &enc, end, src, enc, &emergency, false), false);
    expect("emergency never clears", emergency, true);
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

void
expect_i64(const char* what, int64_t got, int64_t want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %lld want %lld\n", what, (long long)got, (long long)want);
        g_failures++;
    }
}

CallHistoryProvenance
prov(int64_t when, uint64_t seq, int slot) {
    CallHistoryProvenance from;
    from.when = when;
    from.seq = seq;
    from.slot = slot;
    return from;
}

void
test_provenance_compare(void) {
    /* The start decides; the push stamp breaks a same-second tie, then the slot. */
    expect_int("later start is newer", call_history_provenance_compare(prov(101, 1, 0), prov(100, 9, 1)), 1);
    expect_int("earlier start is older", call_history_provenance_compare(prov(100, 9, 1), prov(101, 1, 0)), -1);
    expect_int("same second: later push is newer", call_history_provenance_compare(prov(100, 6, 0), prov(100, 5, 1)),
               1);
    expect_int("same second: earlier push is older", call_history_provenance_compare(prov(100, 5, 1), prov(100, 6, 0)),
               -1);
    expect_int("same push: higher slot is newer", call_history_provenance_compare(prov(100, 5, 1), prov(100, 5, 0)), 1);
    expect_int("same push: lower slot is older", call_history_provenance_compare(prov(100, 5, 0), prov(100, 5, 1)), -1);
    expect_int("the same fragment is equal", call_history_provenance_compare(prov(100, 5, 1), prov(100, 5, 1)), 0);
}

/* merge_source_label() as it read before the per-field fold: whether a fragment's label replaces the stored one. */
bool
legacy_source_label_adopts(bool stored_known, const CallHistoryProvenance& stored, bool incoming_known,
                           const CallHistoryProvenance& incoming) {
    if (!incoming_known) {
        return false;
    }
    if (stored_known) {
        if (incoming.when != stored.when) {
            if (incoming.when < stored.when) {
                return false;
            }
        } else if (incoming.seq != stored.seq) {
            if (incoming.seq < stored.seq) {
                return false;
            }
        } else if (incoming.slot < stored.slot) {
            return false;
        }
    }
    return true;
}

void
test_source_label_rule_is_unchanged(void) {
    /* Every older/equal/newer pairing of start, push stamp and slot, known or not on either side. */
    const int64_t whens[] = {99, 100, 101};
    const uint64_t seqs[] = {4U, 5U, 6U};
    const int slots[] = {0, 1};
    int mismatches = 0;
    int cases = 0;
    for (int64_t sw : whens) {
        for (uint64_t sq : seqs) {
            for (int ss : slots) {
                for (int64_t iw : whens) {
                    for (uint64_t iq : seqs) {
                        for (int is : slots) {
                            for (int known = 0; known < 4; known++) {
                                const bool storedKnown = (known & 1) != 0;
                                const bool incomingKnown = (known & 2) != 0;
                                const CallHistoryProvenance stored = prov(sw, sq, ss);
                                const CallHistoryProvenance incoming = prov(iw, iq, is);
                                cases++;
                                if (call_history_fold_adopts(storedKnown, stored, incomingKnown, incoming)
                                    != legacy_source_label_adopts(storedKnown, stored, incomingKnown, incoming)) {
                                    mismatches++;
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    expect_int("the fold decides every label merge as merge_source_label did", mismatches, 0);
    expect_int("every pairing was tried", cases, 18 * 18 * 4);
}

/* One fragment of a call: its frequency and access code (0 = unknown), and where it came from. */
struct Fragment {
    int64_t freqHz;
    int acKind;
    int ac;
    CallHistoryProvenance from;
};

/* The absorbing row as the model keeps it: each field with the provenance of the fragment it took. */
struct Folded {
    int64_t freqHz;
    CallHistoryProvenance freqFrom;
    int acKind;
    int ac;
    CallHistoryProvenance acFrom;
};

/* The first fragment logs the row; every later one folds into it, field by field. */
void
fold_fragment(Folded* row, const Fragment& fragment, bool first) {
    if (first) {
        row->freqHz = fragment.freqHz;
        row->freqFrom = fragment.from;
        row->acKind = fragment.acKind;
        row->ac = fragment.ac;
        row->acFrom = fragment.from;
        return;
    }
    if (call_history_fold_adopts(call_history_freq_known(row->freqHz), row->freqFrom,
                                 call_history_freq_known(fragment.freqHz), fragment.from)) {
        row->freqHz = fragment.freqHz;
        row->freqFrom = fragment.from;
    }
    if (call_history_fold_adopts(call_history_access_code_known(row->acKind), row->acFrom,
                                 call_history_access_code_known(fragment.acKind), fragment.from)) {
        row->acKind = fragment.acKind;
        row->ac = fragment.ac;
        row->acFrom = fragment.from;
    }
}

Folded
fold_in_order(const Fragment* fragments, const int* order, int count) {
    Folded row = {};
    for (int i = 0; i < count; i++) {
        fold_fragment(&row, fragments[order[i]], i == 0);
    }
    return row;
}

void
test_fold_is_order_independent(void) {
    /* A trunked call over three voice channels. The newest fragment never learned its code and the oldest never
     * learned its frequency, so each field's newest known value comes from a different fragment. A fourth fragment
     * shares the middle one's second and is told apart by its push stamp. */
    const Fragment fragments[] = {
        {0, DSD_ACCESS_CODE_NAC, 0x293, prov(100, 10, 0)},
        {851012500, DSD_ACCESS_CODE_NAC, 0x294, prov(104, 11, 0)},
        {851037500, DSD_ACCESS_CODE_NONE, 0, prov(108, 12, 1)},
        {852000000, DSD_ACCESS_CODE_NAC, 0x295, prov(104, 13, 1)},
    };
    const int count = 4;
    int order[] = {0, 1, 2, 3};
    int permutations = 0;
    int wrong = 0;
    do {
        const Folded row = fold_in_order(fragments, order, count);
        permutations++;
        if (row.freqHz != 851037500 || row.acKind != DSD_ACCESS_CODE_NAC || row.ac != 0x295
            || call_history_provenance_compare(row.freqFrom, prov(108, 12, 1)) != 0
            || call_history_provenance_compare(row.acFrom, prov(104, 13, 1)) != 0) {
            DSD_FPRINTF(stderr, "order %d%d%d%d folded to %lld Hz, code %d/%d\n", order[0], order[1], order[2],
                        order[3], (long long)row.freqHz, row.acKind, row.ac);
            wrong++;
        }
    } while (std::next_permutation(order, order + count));
    expect_int("every arrival order folds to the same row", wrong, 0);
    expect_int("all 24 orders were folded", permutations, 24);
}

void
test_unknown_never_erases(void) {
    const Fragment known = {851012500, DSD_ACCESS_CODE_COLOR_CODE, 1, prov(100, 10, 0)};
    const Fragment newerUnknown = {0, DSD_ACCESS_CODE_NONE, 0, prov(200, 20, 1)};
    Folded row = {};
    fold_fragment(&row, known, true);
    fold_fragment(&row, newerUnknown, false);
    expect_i64("a newer fragment without a frequency keeps the known one", row.freqHz, 851012500);
    expect_int("a newer fragment without a code keeps the known one", row.acKind, DSD_ACCESS_CODE_COLOR_CODE);
    expect_int("and its value", row.ac, 1);
    expect("unknown never erases through the rule itself",
           call_history_fold_adopts(true, prov(100, 10, 0), false, prov(200, 20, 1)), false);
    /* A known value fills an unknown one whatever their order. */
    expect("an older known value fills an unknown one",
           call_history_fold_adopts(false, prov(200, 20, 1), true, prov(100, 10, 0)), true);
    /* An older known value never replaces a newer known one. */
    expect("an older known value does not replace a newer one",
           call_history_fold_adopts(true, prov(200, 20, 1), true, prov(100, 10, 0)), false);
}

void
test_equal_provenance_adopts(void) {
    /* The same fragment read again (an in-place update) fills itself in. */
    expect("equal provenance adopts", call_history_fold_adopts(true, prov(100, 10, 0), true, prov(100, 10, 0)), true);
    Folded row = {};
    fold_fragment(&row, Fragment{0, DSD_ACCESS_CODE_NONE, 0, prov(100, 10, 0)}, true);
    fold_fragment(&row, Fragment{851012500, DSD_ACCESS_CODE_RAN, 5, prov(100, 10, 0)}, false);
    expect_i64("the fragment fills its own frequency", row.freqHz, 851012500);
    expect_int("and its own code", row.acKind, DSD_ACCESS_CODE_RAN);
    expect_int("and the code's value", row.ac, 5);
}

void
test_known_predicates(void) {
    expect("0 Hz is unknown", call_history_freq_known(0), false);
    expect("a negative frequency is unknown", call_history_freq_known(-5), false);
    expect("a frequency is known", call_history_freq_known(851012500), true);
    expect("NONE is no code", call_history_access_code_known(DSD_ACCESS_CODE_NONE), false);
    expect("CC 0 is a code", call_history_access_code_known(DSD_ACCESS_CODE_COLOR_CODE), true);
}

void
test_seen_fill_ratchet(void) {
    int64_t freqHz = 0;
    int acKind = DSD_ACCESS_CODE_NONE;
    int ac = 0;
    expect("nothing learned reads unchanged", call_history_seen_absorb_fill(&freqHz, &acKind, &ac, 0, 0, 0), false);
    expect("a learned frequency advances",
           call_history_seen_absorb_fill(&freqHz, &acKind, &ac, 851012500, DSD_ACCESS_CODE_NONE, 0), true);
    expect_i64("and is recorded", freqHz, 851012500);
    expect("a learned code advances",
           call_history_seen_absorb_fill(&freqHz, &acKind, &ac, 851012500, DSD_ACCESS_CODE_COLOR_CODE, 0), true);
    expect_int("and its kind is recorded", acKind, DSD_ACCESS_CODE_COLOR_CODE);
    expect_int("with its value", ac, 0);

    /* The core only fills: a known value read as another known one is not an advance, and is not recorded. */
    expect("a changed frequency does not advance",
           call_history_seen_absorb_fill(&freqHz, &acKind, &ac, 852000000, DSD_ACCESS_CODE_COLOR_CODE, 0), false);
    expect_i64("the recorded frequency stays", freqHz, 851012500);
    expect("a changed code does not advance",
           call_history_seen_absorb_fill(&freqHz, &acKind, &ac, 851012500, DSD_ACCESS_CODE_COLOR_CODE, 3), false);
    expect("nor does a changed kind",
           call_history_seen_absorb_fill(&freqHz, &acKind, &ac, 851012500, DSD_ACCESS_CODE_NAC, 0x293), false);
    expect_int("the recorded kind stays", acKind, DSD_ACCESS_CODE_COLOR_CODE);
    expect_int("the recorded code stays", ac, 0);
    expect("unknown never erases what was recorded",
           call_history_seen_absorb_fill(&freqHz, &acKind, &ac, 0, DSD_ACCESS_CODE_NONE, 0), false);
    expect_i64("the frequency survives an unknown read", freqHz, 851012500);
    expect_int("the code survives an unknown read", acKind, DSD_ACCESS_CODE_COLOR_CODE);
}

} // namespace

int
main(void) {
    test_merge_window();
    test_seen_row_advanced();
    test_seen_absorb();
    test_duration();
    test_provenance_compare();
    test_source_label_rule_is_unchanged();
    test_fold_is_order_independent();
    test_unknown_never_erases();
    test_equal_provenance_adopts();
    test_known_predicates();
    test_seen_fill_ratchet();
    if (g_failures != 0) {
        DSD_FPRINTF(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    DSD_FPRINTF(stderr, "OK\n");
    return 0;
}
