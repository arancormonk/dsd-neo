// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Unit tests: merge and re-ingest policy behind the Qt call history. The Qt
 * Quick frontend only builds for Android, so the decisions live in a Qt-free
 * header (call_history_merge.h) exactly to be testable here. Issue #799: a
 * fragment's site folds into its row only when the two agree, the seen ratchet
 * reads a site part gained or any channel change as an advance, and the merge
 * order limit (L7) is pinned as documented. Issue #800: a merged row's GPS text
 * and position come from the fragment with the greatest (run, rev) whatever the
 * order the fragments fold in, a keyed fix beats one with none, the same
 * fragment read again takes what it says now; protocol families and system
 * identities agree or are unknown; the digests and the stored-position check. */

#include <algorithm>
#include <dsd-neo/core/access_code.h>
#include <dsd-neo/core/call_channel.h>
#include <dsd-neo/core/position.h>
#include <dsd-neo/core/received_identity.h>
#include <dsd-neo/core/synctype_ids.h>
#include <stdint.h>
#include <stdio.h>

#include "call_history_merge.h"
#include "dsd-neo/core/safe_api.h"

using dsd_qt::call_history_access_code_known;
using dsd_qt::call_history_call_channel_known;
using dsd_qt::call_history_duration_secs;
using dsd_qt::call_history_fold_adopts;
using dsd_qt::call_history_freq_known;
using dsd_qt::call_history_gps_adopts;
using dsd_qt::call_history_merge_within_window;
using dsd_qt::call_history_position_digest;
using dsd_qt::call_history_position_values_valid;
using dsd_qt::call_history_protocol_family;
using dsd_qt::call_history_protocols_compatible;
using dsd_qt::call_history_provenance_compare;
using dsd_qt::call_history_scopes_compatible;
using dsd_qt::call_history_seen_absorb;
using dsd_qt::call_history_seen_absorb_fill;
using dsd_qt::call_history_seen_absorb_site;
using dsd_qt::call_history_seen_absorb_site_text;
using dsd_qt::call_history_seen_row_advanced;
using dsd_qt::call_history_site_fold;
using dsd_qt::call_history_site_text_digest;
using dsd_qt::call_history_text_digest;
using dsd_qt::CallHistoryPositionKey;
using dsd_qt::CallHistoryProvenance;
using dsd_qt::CallHistoryScope;
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

/* A P25 site of system 0x1A3: its system alone (site 0), or with RFSS 1 and @p site. */
dsd_received_identity
p25_site(uint8_t site) {
    dsd_received_identity id = {};
    id.family = DSD_RXID_FAMILY_P25;
    id.valid = DSD_RXID_P25_SYSID;
    id.p25_sysid = 0x1A3U;
    if (site != 0U) {
        id.valid |= DSD_RXID_P25_SITE;
        id.p25_rfss = 1U;
        id.p25_site = site;
    }
    return id;
}

void
test_site_fold_rules(void) {
    dsd_received_identity row = p25_site(0U);
    const dsd_received_identity site1 = p25_site(1U);
    const dsd_received_identity site2 = p25_site(2U);
    expect("a site that knows more folds in", call_history_site_fold(&row, DSD_CALL_CHANNEL_NONE, &site1, 0U), true);
    expect("and the row keeps every part", row.valid == (DSD_RXID_P25_SYSID | DSD_RXID_P25_SITE) && row.p25_site == 1U,
           true);
    expect("a conflicting site is refused", call_history_site_fold(&row, DSD_CALL_CHANNEL_NONE, &site2, 0U), false);
    expect("and the row's site is as it was", row.p25_site == 1U && row.valid == site1.valid, true);
    const dsd_received_identity none = {};
    dsd_received_identity channelOnly = {};
    expect("two channel kinds are two numberings",
           call_history_site_fold(&channelOnly, DSD_CALL_CHANNEL_LPCN, &none, DSD_CALL_CHANNEL_LSN), false);
    expect("a channel kind beside a P25 site is refused",
           call_history_site_fold(&row, DSD_CALL_CHANNEL_NONE, &none, DSD_CALL_CHANNEL_LSN), false);
    expect("a channel is known with a numbering and a number",
           call_history_call_channel_known(DSD_CALL_CHANNEL_LCN, 3U), true);
    expect("channel 0 is unknown", call_history_call_channel_known(DSD_CALL_CHANNEL_LCN, 0U), false);
    expect("no numbering is unknown", call_history_call_channel_known(DSD_CALL_CHANNEL_NONE, 7U), false);
    expect("a numbering no enum names is unknown", call_history_call_channel_known(9U, 7U), false);
}

void
test_seen_site_ratchet(void) {
    uint32_t mask = 0U;
    uint32_t channel = 0U;
    uint8_t kind = DSD_CALL_CHANNEL_NONE;
    expect("nothing read is no advance", call_history_seen_absorb_site(&mask, &channel, &kind, 0U, 0U, 0U), false);
    expect("a part gained advances", call_history_seen_absorb_site(&mask, &channel, &kind, DSD_RXID_P25_SYSID, 0U, 0U),
           true);
    expect("a part read again does not",
           call_history_seen_absorb_site(&mask, &channel, &kind, DSD_RXID_P25_SYSID, 0U, 0U), false);
    expect("a part lost is not recorded", call_history_seen_absorb_site(&mask, &channel, &kind, 0U, 0U, 0U), false);
    expect_int("and the recorded parts stay", static_cast<int>(mask), static_cast<int>(DSD_RXID_P25_SYSID));
    expect("a channel filled advances",
           call_history_seen_absorb_site(&mask, &channel, &kind, mask, 12U, DSD_CALL_CHANNEL_LPCN), true);
    expect("another channel number advances",
           call_history_seen_absorb_site(&mask, &channel, &kind, mask, 14U, DSD_CALL_CHANNEL_LPCN), true);
    expect("so does another numbering",
           call_history_seen_absorb_site(&mask, &channel, &kind, mask, 14U, DSD_CALL_CHANNEL_LCN), true);
    expect_int("the channel read is recorded", static_cast<int>(channel), 14);
    expect_int("with its numbering", kind, DSD_CALL_CHANNEL_LCN);
    expect("the same channel again is no advance",
           call_history_seen_absorb_site(&mask, &channel, &kind, mask, 14U, DSD_CALL_CHANNEL_LCN), false);
}

/* The site text digest: FNV-1a over the text while it is valid, 0 otherwise; any change from the digest last read is
 * an advance, and a digest not known on either side is none. */
void
test_seen_site_text_digest(void) {
    dsd_received_identity site = {};
    DSD_SNPRINTF(site.dmr_site_text, sizeof(site.dmr_site_text), "%s", "a");
    expect_int("a text not valid digests to 0", static_cast<int>(call_history_site_text_digest(&site)), 0);
    site.valid = DSD_RXID_DMR_SITE_TEXT;
    expect("FNV-1a of \"a\"", call_history_site_text_digest(&site) == 0xE40C292CU, true);
    site.dmr_site_text[0] = '\0';
    expect("an empty valid text is the offset basis", call_history_site_text_digest(&site) == 2166136261U, true);
    DSD_MEMSET(site.dmr_site_text, 'x', sizeof(site.dmr_site_text));
    const uint32_t full = call_history_site_text_digest(&site);
    site.dmr_site_text[sizeof(site.dmr_site_text) - 1U] = '\0';
    expect("a text with no NUL stops at the field's end", full != call_history_site_text_digest(&site), true);

    uint32_t stored = 0U;
    bool known = false;
    expect("an unknown read is no advance", call_history_seen_absorb_site_text(&stored, &known, 7U, false), false);
    expect("and records nothing", !known && stored == 0U, true);
    expect("an unknown stored digest takes the one read", call_history_seen_absorb_site_text(&stored, &known, 7U, true),
           false);
    expect("and knows it", known && stored == 7U, true);
    expect("the same digest is no advance", call_history_seen_absorb_site_text(&stored, &known, 7U, true), false);
    expect("another digest advances", call_history_seen_absorb_site_text(&stored, &known, 9U, true), true);
    expect("so does 0, a text no longer read", call_history_seen_absorb_site_text(&stored, &known, 0U, true), true);
    expect("an unknown read leaves the digest", call_history_seen_absorb_site_text(&stored, &known, 9U, false), false);
    expect_int("as it was", static_cast<int>(stored), 0);
}

/* One History row as the fold below builds it: its site and the fragments (by bit) it holds. */
struct SiteRow {
    dsd_received_identity site;
    int fragments;
};

/* Fold @p count fragments in @p order as tryMerge() does: each joins the newest compatible row, else starts its own.
 * Returns the rows, newest last. */
int
fold_sites(const dsd_received_identity* fragments, const int* order, int count, SiteRow* rows) {
    int made = 0;
    for (int i = 0; i < count; i++) {
        const int fragment = order[i];
        int joined = -1;
        for (int r = made - 1; r >= 0 && joined < 0; r--) {
            if (call_history_site_fold(&rows[r].site, DSD_CALL_CHANNEL_NONE, &fragments[fragment],
                                       DSD_CALL_CHANNEL_NONE)) {
                joined = r;
            }
        }
        if (joined < 0) {
            rows[made].site = fragments[fragment];
            rows[made].fragments = 0;
            joined = made++;
        }
        rows[joined].fragments |= 1 << fragment;
    }
    return made;
}

/* Limit L7, as documented on call_history_site_fold(): with a split on conflicting parts, a fragment that knows fewer
 * parts joins whichever compatible row it meets first, so a live session (oldest first) and a backlog (newest first)
 * group the same three fragments differently. Pinned so a change to the rule is a decision, not an accident. */
void
test_site_fold_order_limit(void) {
    const int live[] = {0, 1, 2};
    const int backlog[] = {2, 1, 0};
    SiteRow rows[3];
    /* A on site 1, B with no site, C on site 2. */
    const dsd_received_identity unsited[] = {p25_site(1U), dsd_received_identity{}, p25_site(2U)};
    expect_int("live: two rows", fold_sites(unsited, live, 3, rows), 2);
    expect_int("live: the site-less fragment joins the first site", rows[0].fragments, 0x3);
    expect_int("live: the second site stands alone", rows[1].fragments, 0x4);
    expect_int("backlog: two rows", fold_sites(unsited, backlog, 3, rows), 2);
    expect_int("backlog: the site-less fragment joins the second site", rows[0].fragments, 0x6);
    expect_int("backlog: the first site stands alone", rows[1].fragments, 0x1);
    /* A knows the system only: which site its row ends up naming is the order's. */
    const dsd_received_identity systemOnly[] = {p25_site(0U), p25_site(1U), p25_site(2U)};
    expect_int("live: two rows again", fold_sites(systemOnly, live, 3, rows), 2);
    expect_int("live: the system-only fragment's row names site 1", rows[0].site.p25_site, 1);
    expect_int("live: it holds the system-only fragment and site 1", rows[0].fragments, 0x3);
    const int systemFirst[] = {2, 0, 1};
    expect_int("site 2 first: two rows", fold_sites(systemOnly, systemFirst, 3, rows), 2);
    expect_int("site 2 first: the system-only fragment's row names site 2", rows[0].site.p25_site, 2);
    expect_int("site 2 first: it holds the system-only fragment and site 2", rows[0].fragments, 0x5);
}

/* A fragment's GPS unit as the fold sees it: its key, its provenance, and a name to tell it by. */
struct GpsFragment {
    const char* name;
    bool known;
    CallHistoryPositionKey key;
    CallHistoryProvenance from;
};

/* Fold @p count fragments in the order given, as foldRow() does, and name the one the row keeps. */
const char*
fold_gps(const GpsFragment* const* order, int count) {
    const GpsFragment* kept = nullptr;
    for (int i = 0; i < count; i++) {
        const GpsFragment* f = order[i];
        const bool keptKnown = kept != nullptr && kept->known;
        const CallHistoryPositionKey none = {false, 0, 0U, 0U};
        const CallHistoryProvenance nowhere = {0, 0U, 0};
        if (call_history_gps_adopts(keptKnown, kept != nullptr ? kept->key : none,
                                    kept != nullptr ? kept->from : nowhere, f->known, f->key, f->from)) {
            kept = f;
        }
    }
    return kept != nullptr ? kept->name : "";
}

/* 2.7: three fragments from two runs, folded in every order, keep the later run's fix though the earlier run's revs
 * are higher and its stamps later (a backward clock step); a keyed fix beats a newer one with no key, and among
 * fragments with none the newest wins, as before. */
void
test_gps_fold_total_order(void) {
    const GpsFragment a = {"A", true, {true, 1, 5U, 0U}, {200, 1U, 0}};
    const GpsFragment b = {"B", true, {true, 1, 9U, 0U}, {300, 2U, 0}};
    const GpsFragment c = {"C", true, {true, 2, 2U, 0U}, {100, 1U, 0}};
    const GpsFragment* const perms[6][3] = {{&a, &b, &c}, {&a, &c, &b}, {&b, &a, &c},
                                            {&b, &c, &a}, {&c, &a, &b}, {&c, &b, &a}};
    for (const auto& perm : perms) {
        expect("every order keeps the later run's fix", fold_gps(perm, 3)[0] == 'C', true);
    }
    const GpsFragment* const sameRun[2][2] = {{&a, &b}, {&b, &a}};
    for (const auto& perm : sameRun) {
        expect("within a run the higher rev wins", fold_gps(perm, 2)[0] == 'B', true);
    }
    const GpsFragment text = {"T", true, {false, 2, 0U, 0U}, {900, 9U, 1}};
    const GpsFragment* const keyedFirst[2][2] = {{&a, &text}, {&text, &a}};
    for (const auto& perm : keyedFirst) {
        expect("a keyed fix beats a newer text-only line", fold_gps(perm, 2)[0] == 'A', true);
    }
    const GpsFragment old1 = {"O", true, {false, 0, 0U, 0U}, {100, 1U, 0}};
    const GpsFragment old2 = {"P", true, {false, 0, 0U, 0U}, {200, 2U, 0}};
    const GpsFragment* const unkeyed[2][2] = {{&old1, &old2}, {&old2, &old1}};
    for (const auto& perm : unkeyed) {
        expect("among unkeyed fragments the newest wins", fold_gps(perm, 2)[0] == 'P', true);
    }
    const GpsFragment nothing = {"N", false, {false, 0, 0U, 0U}, {999, 9U, 1}};
    const GpsFragment* const empty[1][2] = {{&a, &nothing}};
    expect("nothing never erases a fix", fold_gps(empty[0], 2)[0] == 'A', true);
    // The same fragment read again (a text-only write drops its position) takes what it says now.
    const GpsFragment again = {"S", true, {false, 1, 0U, 0U}, {200, 1U, 0}};
    const GpsFragment* const sameFragment[1][2] = {{&a, &again}};
    expect("the same fragment read again takes its new text", fold_gps(sameFragment[0], 2)[0] == 'S', true);
    // The same fragment's older revision read again (a spill line replayed over a newer save) keeps the newer one, in
    // either order; its newer revision read again replaces the older.
    const GpsFragment rev9 = {"9", true, {true, 1, 9U, 0U}, {200, 1U, 0}};
    const GpsFragment rev5 = {"5", true, {true, 1, 5U, 0U}, {200, 1U, 0}};
    const GpsFragment* const revisions[2][2] = {{&rev9, &rev5}, {&rev5, &rev9}};
    for (const auto& perm : revisions) {
        expect("the same fragment keeps its newest revision", fold_gps(perm, 2)[0] == '9', true);
    }
    // Readings of one fragment that carry the ring's commit_rev are ordered by it, with or without a position: an
    // older text-only reading (a spill line from before the fix) never replaces the saved rev 9, in either order,
    // while a later text-only reading (the core's text-only write) does.
    const GpsFragment saved9 = {"9", true, {true, 1, 9U, 20U}, {200, 1U, 0}};
    const GpsFragment olderText = {"o", true, {false, 1, 0U, 12U}, {200, 1U, 0}};
    const GpsFragment laterText = {"l", true, {false, 1, 0U, 25U}, {200, 1U, 0}};
    const GpsFragment* const olderFirst[2][2] = {{&saved9, &olderText}, {&olderText, &saved9}};
    for (const auto& perm : olderFirst) {
        expect("an older text-only reading never replaces a newer fix", fold_gps(perm, 2)[0] == '9', true);
    }
    const GpsFragment* const laterOrders[2][2] = {{&saved9, &laterText}, {&laterText, &saved9}};
    for (const auto& perm : laterOrders) {
        expect("a later text-only reading replaces the fix", fold_gps(perm, 2)[0] == 'l', true);
    }
    const GpsFragment rev5Older = {"5", true, {true, 1, 5U, 10U}, {200, 1U, 0}};
    const GpsFragment* const keyedOrders[2][2] = {{&saved9, &rev5Older}, {&rev5Older, &saved9}};
    for (const auto& perm : keyedOrders) {
        expect("an older keyed reading never replaces a newer one", fold_gps(perm, 2)[0] == '9', true);
    }
}

void
test_identity_compatibility(void) {
    expect("P25 phases are one family", call_history_protocols_compatible(DSD_SYNC_P25P1_POS, DSD_SYNC_P25P2_NEG),
           true);
    expect("DMR base and mobile are one family",
           call_history_protocols_compatible(DSD_SYNC_DMR_BS_VOICE_POS, DSD_SYNC_DMR_MS_VOICE), true);
    expect("YSF polarities are one family", call_history_protocols_compatible(DSD_SYNC_YSF_POS, DSD_SYNC_YSF_NEG),
           true);
    expect("P25 and DMR are two", call_history_protocols_compatible(DSD_SYNC_P25P1_POS, DSD_SYNC_DMR_BS_VOICE_POS),
           false);
    expect("an unknown protocol is compatible", call_history_protocols_compatible(-1, DSD_SYNC_NXDN_POS), true);
    expect("no protocol has no family", call_history_protocol_family(DSD_SYNC_NONE) < 0, true);
    CallHistoryScope none;
    CallHistoryScope tuned1;
    tuned1.kind = DSD_POS_SCOPE_FEED_TUNED_FREQ;
    tuned1.a = 446006250U;
    CallHistoryScope tuned2 = tuned1;
    tuned2.a = 446093750U;
    CallHistoryScope nac1;
    nac1.kind = DSD_POS_SCOPE_P25_NAC;
    nac1.a = 0x293U;
    nac1.freqHz = 155100000;
    CallHistoryScope nac2 = nac1;
    nac2.freqHz = 155400000;
    expect("an unknown system is compatible with any", call_history_scopes_compatible(none, tuned1), true);
    expect("one system is compatible with itself", call_history_scopes_compatible(tuned1, tuned1), true);
    expect("two tuned frequencies are two systems", call_history_scopes_compatible(tuned1, tuned2), false);
    expect("one NAC on two frequencies is two systems", call_history_scopes_compatible(nac1, nac2), false);
    CallHistoryScope p25;
    p25.kind = DSD_POS_SCOPE_P25;
    p25.a = 0xBEE00U;
    p25.b = 0x1A3U;
    CallHistoryScope otherP25 = p25;
    otherP25.b = 0x1A4U;
    CallHistoryScope capPlus;
    capPlus.kind = DSD_POS_SCOPE_DMR;
    capPlus.a = (static_cast<uint32_t>(DSD_RXID_DMR_TRUNK_CAPACITY_PLUS) << 8);
    capPlus.b = 0x105U;
    CallHistoryScope tier3;
    tier3.kind = DSD_POS_SCOPE_DMR;
    tier3.a = (static_cast<uint32_t>(DSD_RXID_DMR_TRUNK_TIER3) << 8) | 1U;
    tier3.b = 0x22U;
    expect("a NAC joins its system's full scope", call_history_scopes_compatible(nac1, p25), true);
    expect("a full scope joins a NAC", call_history_scopes_compatible(p25, nac2), true);
    expect("a tuned frequency joins a full scope", call_history_scopes_compatible(tuned1, p25), true);
    expect("two full P25 scopes that differ are two systems", call_history_scopes_compatible(p25, otherP25), false);
    expect("a NAC is no DMR system", call_history_scopes_compatible(nac1, tier3), false);
    expect("Capacity Plus is a weak DMR scope", dsd_qt::call_history_scope_strength(capPlus) == 2, true);
    expect("a Tier III network is a full DMR scope", dsd_qt::call_history_scope_strength(tier3) == 3, true);
    expect("a weak DMR scope joins a full one", call_history_scopes_compatible(capPlus, tier3), true);
}

void
test_digests_and_stored_position(void) {
    expect("no text has digest 0", call_history_text_digest("", 8U) == 0U, true);
    expect("a text has a digest", call_history_text_digest("GPS", 8U) != 0U, true);
    expect("two texts differ", call_history_text_digest("GPS: 1", 8U) != call_history_text_digest("GPS: 2", 8U), true);
    const char unterminated[4] = {'A', 'B', 'C', 'D'};
    expect("a text is bounded by its field",
           call_history_text_digest(unterminated, 2U) == call_history_text_digest("AB", 8U), true);
    expect("no position has digest 0", call_history_position_digest(false, 1, 2, 3U, 0U, 1U, 4U, 5U) == 0U, true);
    expect("a later rev changes the digest",
           call_history_position_digest(true, 1, 2, 3U, 0U, 1U, 4U, 5U)
               != call_history_position_digest(true, 1, 2, 3U, 0U, 1U, 5U, 5U),
           true);
    expect("a position in range is valid", call_history_position_values_valid(225000000, -450000000, 3, 0), true);
    expect("latitude past 90 is no position", call_history_position_values_valid(900000001, 0, 3, 0), false);
    expect("longitude past 180 is no position", call_history_position_values_valid(0, -1800000001, 3, 0), false);
    expect("(0, 0) is no position", call_history_position_values_valid(0, 0, 3, 0), false);
    expect("kind 0 is no position", call_history_position_values_valid(1, 1, 0, 0), false);
    expect("a kind past the last is no position",
           call_history_position_values_valid(1, 1, static_cast<int64_t>(DSD_POS_KIND_LAST) + 1, 0), false);
    expect("an undefined flag is no position",
           call_history_position_values_valid(1, 1, 3, static_cast<int64_t>(DSD_POS_FLAG_ALL) + 1), false);
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
    test_site_fold_rules();
    test_seen_site_ratchet();
    test_seen_site_text_digest();
    test_site_fold_order_limit();
    test_gps_fold_total_order();
    test_identity_compatibility();
    test_digests_and_stored_position();
    if (g_failures != 0) {
        DSD_FPRINTF(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    DSD_FPRINTF(stderr, "OK\n");
    return 0;
}
