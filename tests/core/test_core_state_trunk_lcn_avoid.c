// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Session-only "avoid" flags for the -Y scan list: the per-row store beside the LCN
 * tail, the usable-row count the avoid command consults before refusing, and the
 * next-unavoided walk the scanner rotation and the manual cycle share. Issue #672
 * adds the revision counter, the writer helpers every LCN list writer goes through,
 * the exact avoid total and the published trunk-scan avoid list.
 */

#include <assert.h>
#include <dsd-neo/core/state.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

static dsd_state*
make_state(void) {
    dsd_state* st = (dsd_state*)calloc(1, sizeof(*st));
    assert(st != NULL);
    return st;
}

static void
free_state(dsd_state* st) {
    dsd_state_trunk_lcn_free(st);
    dsd_state_trunk_scan_avoids_free(st);
    free(st);
}

/* Rows 0..count-1 get distinct non-zero frequencies, growing the heap tail when needed. */
static void
seed_rows(dsd_state* st, int count) {
    assert(dsd_state_trunk_lcn_reserve(st, (size_t)count) == 0);
    for (int i = 0; i < count; i++) {
        *dsd_state_trunk_lcn_slot(st, i) = 851000000L + 12500L * i;
    }
    st->lcn_freq_count = count;
}

static void
test_fresh_state_has_no_avoids(void) {
    dsd_state* st = make_state();
    seed_rows(st, 3);
    assert(dsd_state_trunk_lcn_avoid_get(st, 0U) == 0);
    assert(dsd_state_trunk_lcn_avoid_get(st, 2U) == 0);
    assert(dsd_state_trunk_lcn_avoid_get(st, 4000U) == 0);
    assert(st->trunk_lcn_avoid == NULL);
    assert(st->lcn_avoid_count == 0U);
    assert(dsd_state_trunk_lcn_usable_count(st) == 3);
    free_state(st);
}

static void
test_usable_count_ignores_placeholder_rows(void) {
    dsd_state* st = make_state();
    seed_rows(st, 4);
    *dsd_state_trunk_lcn_slot(st, 1) = 0L;
    assert(dsd_state_trunk_lcn_usable_count(st) == 3);
    st->lcn_freq_count = 0;
    assert(dsd_state_trunk_lcn_usable_count(st) == 0);
    free_state(st);
}

static void
test_avoid_set_allocates_lazily_and_counts(void) {
    dsd_state* st = make_state();
    seed_rows(st, 3);
    assert(dsd_state_trunk_lcn_avoid_set(st, 1U, 1) == 0);
    assert(st->trunk_lcn_avoid != NULL);
    assert(dsd_state_trunk_lcn_avoid_get(st, 1U) == 1);
    assert(dsd_state_trunk_lcn_avoid_get(st, 0U) == 0);
    assert(st->lcn_avoid_count == 1U);
    assert(dsd_state_trunk_lcn_usable_count(st) == 2);
    /* Setting the same row again does not double count. */
    assert(dsd_state_trunk_lcn_avoid_set(st, 1U, 1) == 0);
    assert(st->lcn_avoid_count == 1U);
    assert(dsd_state_trunk_lcn_avoid_set(st, 1U, 0) == 0);
    assert(dsd_state_trunk_lcn_avoid_get(st, 1U) == 0);
    assert(st->lcn_avoid_count == 0U);
    free_state(st);
}

static void
test_avoid_store_grows_past_embedded_slots(void) {
    dsd_state* st = make_state();
    const int rows = DSD_TRUNK_LCN_EMBEDDED + 10;
    seed_rows(st, rows);
    assert(dsd_state_trunk_lcn_avoid_set(st, (size_t)rows - 1U, 1) == 0);
    assert(st->trunk_lcn_avoid_capacity >= (size_t)rows);
    assert(dsd_state_trunk_lcn_avoid_get(st, (size_t)rows - 1U) == 1);
    assert(dsd_state_trunk_lcn_avoid_get(st, (size_t)rows - 2U) == 0);
    assert(dsd_state_trunk_lcn_usable_count(st) == rows - 1);
    free_state(st);
}

static void
test_clearing_an_unset_row_past_capacity_allocates_nothing(void) {
    dsd_state* st = make_state();
    seed_rows(st, 3);
    assert(dsd_state_trunk_lcn_avoid_set(st, 2U, 0) == 0);
    assert(st->trunk_lcn_avoid == NULL);
    assert(st->trunk_lcn_avoid_capacity == 0U);
    free_state(st);
}

static void
test_avoid_clear_reports_how_many_and_keeps_capacity(void) {
    dsd_state* st = make_state();
    seed_rows(st, 5);
    assert(dsd_state_trunk_lcn_avoid_set(st, 0U, 1) == 0);
    assert(dsd_state_trunk_lcn_avoid_set(st, 3U, 1) == 0);
    const size_t capacity = st->trunk_lcn_avoid_capacity;
    assert(dsd_state_trunk_lcn_avoid_clear(st) == 2);
    assert(st->lcn_avoid_count == 0U);
    assert(dsd_state_trunk_lcn_avoid_get(st, 0U) == 0);
    assert(dsd_state_trunk_lcn_avoid_get(st, 3U) == 0);
    assert(st->trunk_lcn_avoid_capacity == capacity);
    assert(dsd_state_trunk_lcn_usable_count(st) == 5);
    assert(dsd_state_trunk_lcn_avoid_clear(st) == 0);
    free_state(st);
}

static void
test_avoid_count_is_bounded_by_the_list(void) {
    dsd_state* st = make_state();
    seed_rows(st, 5);
    assert(dsd_state_trunk_lcn_avoid_set(st, 4U, 1) == 0);
    assert(dsd_state_trunk_lcn_avoid_set(st, 1U, 1) == 0);
    assert(st->lcn_avoid_count == 2U);
    /* A flag past the (shrunk) list is not counted once the store is touched again. */
    st->lcn_freq_count = 3;
    assert(dsd_state_trunk_lcn_avoid_set(st, 0U, 1) == 0);
    assert(st->lcn_avoid_count == 2U);
    assert(dsd_state_trunk_lcn_usable_count(st) == 1);
    free_state(st);
}

static void
test_next_unavoided_walks_and_wraps(void) {
    dsd_state* st = make_state();
    seed_rows(st, 4);
    assert(dsd_state_trunk_lcn_next_unavoided(st, 0) == 0);
    assert(dsd_state_trunk_lcn_avoid_set(st, 1U, 1) == 0);
    assert(dsd_state_trunk_lcn_avoid_set(st, 2U, 1) == 0);
    assert(dsd_state_trunk_lcn_next_unavoided(st, 1) == 3);
    assert(dsd_state_trunk_lcn_avoid_set(st, 3U, 1) == 0);
    /* Only row 0 remains: from row 1 the walk wraps back to it. */
    assert(dsd_state_trunk_lcn_next_unavoided(st, 1) == 0);
    /* An out-of-range start is treated as the head of the list. */
    assert(dsd_state_trunk_lcn_next_unavoided(st, 4) == 0);
    assert(dsd_state_trunk_lcn_next_unavoided(st, -1) == 0);
    free_state(st);
}

static void
test_next_unavoided_reports_none_when_all_avoided_or_empty(void) {
    dsd_state* st = make_state();
    assert(dsd_state_trunk_lcn_next_unavoided(st, 0) == -1);
    seed_rows(st, 2);
    assert(dsd_state_trunk_lcn_avoid_set(st, 0U, 1) == 0);
    assert(dsd_state_trunk_lcn_avoid_set(st, 1U, 1) == 0);
    assert(dsd_state_trunk_lcn_next_unavoided(st, 0) == -1);
    free_state(st);
}

static void
test_free_releases_the_store(void) {
    dsd_state* st = make_state();
    seed_rows(st, 3);
    assert(dsd_state_trunk_lcn_avoid_set(st, 2U, 1) == 0);
    dsd_state_trunk_lcn_avoid_free(st);
    assert(st->trunk_lcn_avoid == NULL);
    assert(st->trunk_lcn_avoid_capacity == 0U);
    assert(dsd_state_trunk_lcn_avoid_get(st, 2U) == 0);
    /* The umbrella free covers it too, so every existing teardown site releases it. */
    assert(dsd_state_trunk_lcn_avoid_set(st, 2U, 1) == 0);
    dsd_state_trunk_lcn_free(st);
    assert(st->trunk_lcn_avoid == NULL);
    free(st);
}

static void
test_null_state_is_harmless(void) {
    assert(dsd_state_trunk_lcn_avoid_set(NULL, 0U, 1) == -1);
    assert(dsd_state_trunk_lcn_avoid_get(NULL, 0U) == 0);
    assert(dsd_state_trunk_lcn_avoid_clear(NULL) == 0);
    assert(dsd_state_trunk_lcn_usable_count(NULL) == 0);
    assert(dsd_state_trunk_lcn_next_unavoided(NULL, 0) == -1);
    dsd_state_trunk_lcn_avoid_free(NULL);
}

/* The four stamps a writer may move, read together so a test can say which moved. */
typedef struct {
    uint64_t instance;
    uint64_t list;
    uint64_t avoid;
    uint64_t trunk_scan;
} stamps;

static stamps
read_stamps(const dsd_state* st) {
    stamps out = {st->state_instance, st->lcn_list_rev, st->lcn_avoid_rev, st->trunk_scan_avoid_rev};
    return out;
}

static int
stamps_equal(stamps a, stamps b) {
    return a.instance == b.instance && a.list == b.list && a.avoid == b.avoid && a.trunk_scan == b.trunk_scan;
}

static void
test_rev_counter_never_repeats_or_returns_zero(void) {
    uint64_t prev = dsd_state_rev_next();
    assert(prev != 0U);
    for (int i = 0; i < 1000; i++) {
        const uint64_t next = dsd_state_rev_next();
        assert(next > prev);
        prev = next;
    }
}

/* A wholesale replacement drops the flags and both totals and moves the list identity and the content revision. */
static void
test_replace_begin_drops_flags_and_stamps_both(void) {
    dsd_state* st = make_state();
    seed_rows(st, 4);
    assert(dsd_state_trunk_lcn_avoid_set(st, 1U, 1) == 0);
    assert(dsd_state_trunk_lcn_avoid_set(st, 3U, 1) == 0);
    const stamps before = read_stamps(st);
    dsd_state_trunk_lcn_replace_begin(st);
    assert(st->trunk_lcn_avoid == NULL);
    assert(st->lcn_avoid_count == 0U && st->lcn_avoid_total == 0U);
    assert(dsd_state_trunk_lcn_avoid_get(st, 1U) == 0);
    assert(st->lcn_list_rev > before.list && st->lcn_avoid_rev > before.avoid);
    assert(st->lcn_list_rev != st->lcn_avoid_rev);
    assert(st->state_instance == before.instance && st->trunk_scan_avoid_rev == before.trunk_scan);
    /* An empty list is replaced the same way: the identity still moves. */
    const uint64_t list = st->lcn_list_rev;
    dsd_state_trunk_lcn_replace_begin(st);
    assert(st->lcn_list_rev > list);
    dsd_state_trunk_lcn_replace_begin(NULL);
    free_state(st);
}

/* In-place writes and appends keep the list identity; only a changed avoided row moves the content revision. */
static void
test_freq_write_stamps_only_a_changed_avoided_row(void) {
    dsd_state* st = make_state();
    seed_rows(st, 3);
    assert(dsd_state_trunk_lcn_avoid_set(st, 1U, 1) == 0);
    stamps before = read_stamps(st);

    assert(dsd_state_trunk_lcn_freq_write(st, 0, 852000000L) == 0);
    assert(*dsd_state_trunk_lcn_slot(st, 0) == 852000000L);
    assert(stamps_equal(read_stamps(st), before));

    const long same = *dsd_state_trunk_lcn_slot(st, 1);
    assert(dsd_state_trunk_lcn_freq_write(st, 1, same) == 0);
    assert(stamps_equal(read_stamps(st), before));

    assert(dsd_state_trunk_lcn_freq_write(st, 1, 853000000L) == 0);
    assert(*dsd_state_trunk_lcn_slot(st, 1) == 853000000L);
    assert(st->lcn_avoid_rev > before.avoid && st->lcn_list_rev == before.list);
    before = read_stamps(st);

    /* A 0 Hz avoided slot filled in place (the EDACS capture) counts as a change too. */
    assert(dsd_state_trunk_lcn_freq_write(st, 1, 0L) == 0);
    assert(st->lcn_avoid_rev > before.avoid);
    before = read_stamps(st);

    /* An append past the embedded slots grows the tail, leaves the count to the caller and stamps nothing. */
    const int row = DSD_TRUNK_LCN_EMBEDDED + 4;
    assert(dsd_state_trunk_lcn_freq_write(st, row, 860000000L) == 0);
    assert(*dsd_state_trunk_lcn_slot(st, row) == 860000000L);
    assert(st->lcn_freq_count == 3);
    assert(stamps_equal(read_stamps(st), before));

    assert(dsd_state_trunk_lcn_freq_write(st, -1, 1L) == -1);
    assert(dsd_state_trunk_lcn_freq_write(NULL, 0, 1L) == -1);
    free_state(st);
}

/* Raising the count never lowers it; avoid flags it brings into the list are counted and move the revision. */
static void
test_count_raise_counts_flags_it_brings_in(void) {
    dsd_state* st = make_state();
    seed_rows(st, 2);
    stamps before = read_stamps(st);
    dsd_state_trunk_lcn_count_raise(st, 3);
    assert(st->lcn_freq_count == 3);
    dsd_state_trunk_lcn_count_raise(st, 1);
    assert(st->lcn_freq_count == 3);
    assert(stamps_equal(read_stamps(st), before));

    /* A flag past the list (left there by a shrink) joins the counts when an append reaches it. */
    assert(dsd_state_trunk_lcn_avoid_set(st, 5U, 1) == 0);
    assert(st->lcn_avoid_total == 0U && st->lcn_avoid_count == 0U);
    before = read_stamps(st);
    dsd_state_trunk_lcn_count_raise(st, 5);
    assert(st->lcn_avoid_total == 0U);
    assert(stamps_equal(read_stamps(st), before));
    dsd_state_trunk_lcn_count_raise(st, 6);
    assert(st->lcn_avoid_total == 1U && st->lcn_avoid_count == 1U);
    assert(st->lcn_avoid_rev > before.avoid && st->lcn_list_rev == before.list);

    /* The DMR learner's negative-count repair: raising to 0. */
    st->lcn_freq_count = -1;
    dsd_state_trunk_lcn_count_raise(st, 0);
    assert(st->lcn_freq_count == 0);
    dsd_state_trunk_lcn_count_raise(NULL, 1);
    free_state(st);
}

/* A name write moves the content revision only for an avoided row whose name changes. */
static void
test_name_set_stamps_only_a_renamed_avoided_row(void) {
    dsd_state* st = make_state();
    seed_rows(st, 3);
    assert(dsd_state_trunk_lcn_avoid_set(st, 2U, 1) == 0);
    stamps before = read_stamps(st);
    assert(dsd_state_trunk_lcn_name_set(st, 0U, "Dispatch") == 0);
    assert(stamps_equal(read_stamps(st), before));
    assert(dsd_state_trunk_lcn_name_set(st, 2U, "Fire") == 0);
    assert(st->lcn_avoid_rev > before.avoid);
    before = read_stamps(st);
    assert(dsd_state_trunk_lcn_name_set(st, 2U, "  Fire  ") == 0);
    assert(stamps_equal(read_stamps(st), before));
    free_state(st);
}

/* Set and clear move the content revision only when a flag actually changes. */
static void
test_avoid_set_and_clear_stamp_only_real_changes(void) {
    dsd_state* st = make_state();
    seed_rows(st, 4);
    stamps before = read_stamps(st);
    assert(dsd_state_trunk_lcn_avoid_set(st, 3U, 0) == 0);
    assert(stamps_equal(read_stamps(st), before));
    assert(dsd_state_trunk_lcn_avoid_clear(st) == 0);
    assert(stamps_equal(read_stamps(st), before));

    assert(dsd_state_trunk_lcn_avoid_set(st, 2U, 1) == 0);
    assert(st->lcn_avoid_rev > before.avoid && st->lcn_list_rev == before.list);
    assert(st->lcn_avoid_total == 1U);
    before = read_stamps(st);
    assert(dsd_state_trunk_lcn_avoid_set(st, 2U, 1) == 0);
    assert(stamps_equal(read_stamps(st), before));
    /* Clearing an unset row inside the store's capacity changes nothing either. */
    assert(dsd_state_trunk_lcn_avoid_set(st, 0U, 0) == 0);
    assert(stamps_equal(read_stamps(st), before));

    assert(dsd_state_trunk_lcn_avoid_set(st, 2U, 0) == 0);
    assert(st->lcn_avoid_rev > before.avoid && st->lcn_avoid_total == 0U);
    before = read_stamps(st);
    /* A store holding only cleared flags has nothing to clear. */
    assert(dsd_state_trunk_lcn_avoid_clear(st) == 0);
    assert(stamps_equal(read_stamps(st), before));

    assert(dsd_state_trunk_lcn_avoid_set(st, 1U, 1) == 0);
    before = read_stamps(st);
    assert(dsd_state_trunk_lcn_avoid_clear(st) == 1);
    assert(st->lcn_avoid_rev > before.avoid && st->lcn_avoid_total == 0U);
    free_state(st);
}

/* lcn_avoid_count saturates for the status line; lcn_avoid_total counts every avoided row in the list. */
static void
test_avoid_total_does_not_saturate(void) {
    dsd_state* st = make_state();
    const int rows = 70000;
    seed_rows(st, rows);
    assert(dsd_state_trunk_lcn_avoid_reserve(st, (size_t)rows) == 0);
    /* Fill the flags directly, then let one helper call recount, instead of 70,000 recounting calls. */
    DSD_MEMSET(st->trunk_lcn_avoid, 1, (size_t)rows - 1U);
    assert(dsd_state_trunk_lcn_avoid_set(st, (size_t)rows - 1U, 1) == 0);
    assert(st->lcn_avoid_total == (size_t)rows);
    assert(st->lcn_avoid_count == UINT16_MAX);
    assert(dsd_state_trunk_lcn_avoid_set(st, 0U, 0) == 0);
    assert(st->lcn_avoid_total == (size_t)rows - 1U);
    assert(st->lcn_avoid_count == UINT16_MAX);
    /* The clear's return keeps its saturated meaning; the total is what a view reads. */
    assert(dsd_state_trunk_lcn_avoid_clear(st) == (int)UINT16_MAX);
    assert(st->lcn_avoid_total == 0U && st->lcn_avoid_count == 0U);
    free_state(st);
}

/* What the snapshot copies and the teardown paths use moves no stamp. */
static void
test_storage_helpers_stamp_nothing(void) {
    dsd_state* st = make_state();
    seed_rows(st, 3);
    assert(dsd_state_trunk_lcn_avoid_set(st, 1U, 1) == 0);
    assert(dsd_state_trunk_lcn_name_set(st, 1U, "Ops") == 0);
    const stamps before = read_stamps(st);
    assert(dsd_state_trunk_lcn_reserve(st, (size_t)DSD_TRUNK_LCN_EMBEDDED + 40U) == 0);
    assert(dsd_state_trunk_lcn_name_reserve(st, 64U) == 0);
    assert(dsd_state_trunk_lcn_avoid_reserve(st, 64U) == 0);
    const dsd_trunk_scan_avoid_entry entry = {.ordinal = 1U, .frequency_hz = 851000000U, .id = "A"};
    assert(dsd_state_trunk_scan_avoids_reserve(st, 8U) == 0);
    assert(dsd_state_trunk_scan_avoids_set(st, &entry, 1U) == 0);
    dsd_state_trunk_scan_avoids_free(st);
    dsd_state_trunk_lcn_name_free(st);
    dsd_state_trunk_lcn_avoid_free(st);
    dsd_state_trunk_lcn_free(st);
    assert(stamps_equal(read_stamps(st), before));
    free(st);
}

static void
test_trunk_scan_avoids_set_copies_and_free_releases(void) {
    dsd_state* st = make_state();
    assert(st->trunk_scan_avoids == NULL && st->trunk_scan_avoid_count == 0U);
    dsd_trunk_scan_avoid_entry entries[40];
    DSD_MEMSET(entries, 0, sizeof(entries));
    for (uint32_t i = 0; i < 40U; i++) {
        entries[i].ordinal = i + 1U;
        entries[i].frequency_hz = 851000000U + 12500U * i;
        entries[i].id[0] = (char)('A' + (char)(i % 26U));
    }
    assert(dsd_state_trunk_scan_avoids_set(st, entries, 3U) == 0);
    assert(st->trunk_scan_avoid_count == 3U && st->trunk_scan_avoid_capacity >= 3U);
    assert(st->trunk_scan_avoids != entries);
    assert(memcmp(st->trunk_scan_avoids, entries, 3U * sizeof(entries[0])) == 0);
    /* The copy is the state's own: changing the source changes nothing. */
    entries[0].ordinal = 99U;
    assert(st->trunk_scan_avoids[0].ordinal == 1U);
    entries[0].ordinal = 1U;
    /* Growing past the first capacity keeps a faithful copy. */
    assert(dsd_state_trunk_scan_avoids_set(st, entries, 40U) == 0);
    assert(st->trunk_scan_avoid_count == 40U && st->trunk_scan_avoid_capacity >= 40U);
    assert(memcmp(st->trunk_scan_avoids, entries, sizeof(entries)) == 0);
    /* An empty list keeps the allocation and holds nothing. */
    const size_t capacity = st->trunk_scan_avoid_capacity;
    assert(dsd_state_trunk_scan_avoids_set(st, NULL, 0U) == 0);
    assert(st->trunk_scan_avoid_count == 0U && st->trunk_scan_avoid_capacity == capacity);
    assert(dsd_state_trunk_scan_avoids_set(st, NULL, 2U) == -1);
    assert(dsd_state_trunk_scan_avoids_set(NULL, entries, 1U) == -1);
    assert(dsd_state_trunk_scan_avoids_reserve(NULL, 1U) == -1);
    assert(dsd_state_trunk_scan_avoids_set(st, entries, 2U) == 0);
    dsd_state_trunk_scan_avoids_free(st);
    assert(st->trunk_scan_avoids == NULL && st->trunk_scan_avoid_count == 0U && st->trunk_scan_avoid_capacity == 0U);
    /* The LCN umbrella free leaves the list as it is: emptying it is the coordinator's (with a new revision) or
       initState()'s/freeState()'s (with a new instance), so a revision-gated snapshot copy never keeps stale targets. */
    assert(dsd_state_trunk_scan_avoids_set(st, entries, 2U) == 0);
    const dsd_trunk_scan_avoid_entry* kept = st->trunk_scan_avoids;
    dsd_state_trunk_lcn_free(st);
    assert(st->trunk_scan_avoids == kept && st->trunk_scan_avoid_count == 2U);
    assert(memcmp(st->trunk_scan_avoids, entries, 2U * sizeof(entries[0])) == 0);
    dsd_state_trunk_scan_avoids_free(st);
    dsd_state_trunk_scan_avoids_free(NULL);
    free(st);
}

/* The test seam fails the reserve helpers' allocations: a failed set keeps no stale entries, the other stores keep
   what they hold, and disarming the seam restores normal growth. */
static void
test_reserve_failure_seam(void) {
    dsd_state* st = make_state();
    seed_rows(st, 3);
    dsd_trunk_scan_avoid_entry entries[20];
    DSD_MEMSET(entries, 0, sizeof(entries));
    assert(dsd_state_trunk_scan_avoids_set(st, entries, 2U) == 0);
    const size_t capacity = st->trunk_scan_avoid_capacity;

    dsd_state_trunk_lcn_test_alloc_fail_after(0);
    /* Within capacity no allocation is needed, so nothing fails. */
    assert(dsd_state_trunk_scan_avoids_set(st, entries, 1U) == 0);
    assert(dsd_state_trunk_scan_avoids_set(st, entries, capacity + 1U) == -1);
    assert(st->trunk_scan_avoid_count == 0U && st->trunk_scan_avoid_capacity == capacity);
    assert(dsd_state_trunk_lcn_avoid_set(st, 1U, 1) == -1);
    assert(st->trunk_lcn_avoid == NULL && st->lcn_avoid_total == 0U);
    assert(dsd_state_trunk_lcn_reserve(st, (size_t)DSD_TRUNK_LCN_EMBEDDED + 1U) == -1);
    assert(dsd_state_trunk_lcn_freq_write(st, DSD_TRUNK_LCN_EMBEDDED, 1L) == -1);
    assert(dsd_state_trunk_lcn_name_reserve(st, 1U) == -1);

    /* One more allowed allocation, then failure again. */
    dsd_state_trunk_lcn_test_alloc_fail_after(1);
    assert(dsd_state_trunk_lcn_avoid_set(st, 1U, 1) == 0);
    assert(dsd_state_trunk_lcn_name_reserve(st, 1U) == -1);

    dsd_state_trunk_lcn_test_alloc_fail_after(-1);
    assert(dsd_state_trunk_lcn_name_reserve(st, 1U) == 0);
    assert(dsd_state_trunk_scan_avoids_set(st, entries, capacity + 1U) == 0);
    assert(st->trunk_scan_avoid_count == capacity + 1U);
    free_state(st);
}

int
main(void) {
    test_fresh_state_has_no_avoids();
    test_usable_count_ignores_placeholder_rows();
    test_avoid_set_allocates_lazily_and_counts();
    test_avoid_store_grows_past_embedded_slots();
    test_clearing_an_unset_row_past_capacity_allocates_nothing();
    test_avoid_clear_reports_how_many_and_keeps_capacity();
    test_avoid_count_is_bounded_by_the_list();
    test_next_unavoided_walks_and_wraps();
    test_next_unavoided_reports_none_when_all_avoided_or_empty();
    test_free_releases_the_store();
    test_null_state_is_harmless();
    test_rev_counter_never_repeats_or_returns_zero();
    test_replace_begin_drops_flags_and_stamps_both();
    test_freq_write_stamps_only_a_changed_avoided_row();
    test_count_raise_counts_flags_it_brings_in();
    test_name_set_stamps_only_a_renamed_avoided_row();
    test_avoid_set_and_clear_stamp_only_real_changes();
    test_avoid_total_does_not_saturate();
    test_storage_helpers_stamp_nothing();
    test_trunk_scan_avoids_set_copies_and_free_releases();
    test_reserve_failure_seam();
    return 0;
}
