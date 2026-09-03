// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Session-only "avoid" flags for the -Y scan list: the per-row store beside the LCN
 * tail, the usable-row count the avoid command consults before refusing, and the
 * next-unavoided walk the scanner rotation and the manual cycle share.
 */

#include <assert.h>
#include <dsd-neo/core/state.h>
#include <stdlib.h>
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
    return 0;
}
