// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <assert.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/runtime/trunk_cc_candidates.h>
#include <stdint.h>
#include <stdlib.h>

#include "dsd-neo/core/state_ext.h"
#include "dsd-neo/core/state_fwd.h"

static void
test_public_guards(void) {
    dsd_state* st = calloc(1, sizeof(*st));
    assert(st != NULL);
    long out = 1234;

    assert(dsd_trunk_cc_candidates_get(NULL) == NULL);
    assert(dsd_trunk_cc_candidates_peek(NULL) == NULL);
    assert(dsd_trunk_cc_candidates_add(NULL, 100, 1, DSD_TRUNK_CC_CANDIDATE_CURRENT_SITE) == 0);
    assert(dsd_trunk_cc_candidates_add(st, 0, 1, DSD_TRUNK_CC_CANDIDATE_CURRENT_SITE) == 0);
    assert(dsd_trunk_cc_candidates_next(NULL, 0.0, 0, &out) == 0);
    assert(out == 1234);
    assert(dsd_trunk_cc_candidates_next(st, 0.0, 0, NULL) == 0);
    assert(dsd_trunk_cc_candidates_next(st, 0.0, 0, &out) == 0);
    assert(out == 1234);

    dsd_trunk_cc_candidates_set_cooldown(NULL, 100, 1.0);
    dsd_trunk_cc_candidates_set_cooldown(st, 100, 1.0);

    dsd_state_ext_free_all(st);
    free(st);
}

static void
test_add_dedup_rollover(void) {
    dsd_state* st = calloc(1, sizeof(*st));
    assert(st != NULL);

    assert(dsd_trunk_cc_candidates_peek(st) == NULL);

    assert(dsd_trunk_cc_candidates_add(st, 100, 0, DSD_TRUNK_CC_CANDIDATE_CURRENT_SITE) == 1);
    const dsd_trunk_cc_candidates* cc = dsd_trunk_cc_candidates_peek(st);
    assert(cc != NULL);
    assert(cc->count == 1);
    assert(cc->candidates[0] == 100);
    assert((cc->flags[0] & DSD_TRUNK_CC_CANDIDATE_CURRENT_SITE) != 0);
    assert(cc->added == 0);

    assert(dsd_trunk_cc_candidates_add(st, 100, 1, DSD_TRUNK_CC_CANDIDATE_CURRENT_SITE) == 0);
    assert(cc->added == 0);

    assert(dsd_trunk_cc_candidates_add(st, 200, 1, DSD_TRUNK_CC_CANDIDATE_CURRENT_SITE) == 1);
    assert(cc->count == 2);
    assert(cc->candidates[0] == 100);
    assert(cc->candidates[1] == 200);
    assert((cc->flags[1] & DSD_TRUNK_CC_CANDIDATE_CURRENT_SITE) != 0);
    assert(cc->added == 1);

    dsd_state* st2 = calloc(1, sizeof(*st2));
    assert(st2 != NULL);
    for (long f = 1; f <= DSD_TRUNK_CC_CANDIDATES_MAX; f++) {
        assert(dsd_trunk_cc_candidates_add(st2, f, 0, DSD_TRUNK_CC_CANDIDATE_CURRENT_SITE) == 1);
    }
    dsd_trunk_cc_candidates* cc2 = dsd_trunk_cc_candidates_get(st2);
    assert(cc2 != NULL);
    assert(cc2->count == DSD_TRUNK_CC_CANDIDATES_MAX);
    assert(cc2->candidates[0] == 1);
    assert(cc2->candidates[DSD_TRUNK_CC_CANDIDATES_MAX - 1] == DSD_TRUNK_CC_CANDIDATES_MAX);

    cc2->idx = 5;
    assert(dsd_trunk_cc_candidates_add(st2, 17, 0, DSD_TRUNK_CC_CANDIDATE_CURRENT_SITE) == 1);
    assert(cc2->count == DSD_TRUNK_CC_CANDIDATES_MAX);
    assert(cc2->candidates[0] == 2);
    assert(cc2->candidates[DSD_TRUNK_CC_CANDIDATES_MAX - 2] == 16);
    assert(cc2->candidates[DSD_TRUNK_CC_CANDIDATES_MAX - 1] == 17);
    assert(cc2->idx == 4);

    dsd_state_ext_free_all(st2);
    free(st2);
    dsd_state_ext_free_all(st);
    free(st);
}

static void
test_invalid_count_is_cleared_before_failure(void) {
    dsd_state* st = calloc(1, sizeof(*st));
    assert(st != NULL);

    assert(dsd_trunk_cc_candidates_add(st, 100, 0, DSD_TRUNK_CC_CANDIDATE_CURRENT_SITE) == 1);
    dsd_trunk_cc_candidates* cc = dsd_trunk_cc_candidates_get(st);
    assert(cc != NULL);

    cc->count = DSD_TRUNK_CC_CANDIDATES_MAX + 1;
    cc->idx = 9;
    cc->candidates[0] = 100;
    cc->flags[0] = DSD_TRUNK_CC_CANDIDATE_CURRENT_SITE;
    cc->cool_until[0] = 20.0;
    assert(dsd_trunk_cc_candidates_add(st, 200, 1, DSD_TRUNK_CC_CANDIDATE_CURRENT_SITE) == 0);
    assert(cc->count == 0);
    assert(cc->idx == 0);
    assert(cc->candidates[0] == 0);
    assert(cc->flags[0] == 0);
    assert(cc->cool_until[0] == 0.0);

    assert(dsd_trunk_cc_candidates_add(st, 300, 0, DSD_TRUNK_CC_CANDIDATE_CURRENT_SITE) == 1);
    cc->count = -1;
    cc->idx = 3;
    cc->candidates[0] = 300;
    long out = 0;
    assert(dsd_trunk_cc_candidates_next(st, 0.0, 0, &out) == 0);
    assert(out == 0);
    assert(cc->count == 0);
    assert(cc->idx == 0);
    assert(cc->candidates[0] == 0);

    dsd_state_ext_free_all(st);
    free(st);
}

static void
test_filtered_iteration(void) {
    dsd_state* st = calloc(1, sizeof(*st));
    assert(st != NULL);

    assert(dsd_trunk_cc_candidates_add(st, 300, 0, 0) == 1);
    long out = 0;
    assert(dsd_trunk_cc_candidates_next(st, 0.0, DSD_TRUNK_CC_CANDIDATE_CURRENT_SITE, &out) == 0);

    out = 0;
    assert(dsd_trunk_cc_candidates_next(st, 0.0, 0, &out) == 1);
    assert(out == 300);

    assert(dsd_trunk_cc_candidates_add(st, 300, 0, DSD_TRUNK_CC_CANDIDATE_CURRENT_SITE) == 0);
    out = 0;
    assert(dsd_trunk_cc_candidates_next(st, 0.0, DSD_TRUNK_CC_CANDIDATE_CURRENT_SITE, &out) == 1);
    assert(out == 300);

    dsd_state_ext_free_all(st);
    free(st);
}

static void
test_next_and_cooldown(void) {
    dsd_state* st = calloc(1, sizeof(*st));
    assert(st != NULL);
    assert(dsd_trunk_cc_candidates_add(st, 100, 0, DSD_TRUNK_CC_CANDIDATE_CURRENT_SITE) == 1);
    assert(dsd_trunk_cc_candidates_add(st, 200, 0, DSD_TRUNK_CC_CANDIDATE_CURRENT_SITE) == 1);

    dsd_trunk_cc_candidates* cc = dsd_trunk_cc_candidates_get(st);
    assert(cc != NULL);

    // Skip current CC frequency when set
    st->trunk_cc_freq = 100;
    cc->idx = 0;
    cc->used = 0;
    long out = 0;
    assert(dsd_trunk_cc_candidates_next(st, 0.0, 0, &out) == 1);
    assert(out == 200);
    assert(cc->used == 1);

    // Cooldown skips a candidate until it expires
    st->trunk_cc_freq = 0;
    st->p25_cc_freq = 0;
    cc->idx = 0;
    cc->used = 0;
    dsd_trunk_cc_candidates_set_cooldown(st, 100, 10.0);

    out = 0;
    assert(dsd_trunk_cc_candidates_next(st, 0.0, 0, &out) == 1);
    assert(out == 200);
    assert(cc->used == 1);

    out = 0;
    assert(dsd_trunk_cc_candidates_next(st, 0.0, 0, &out) == 1);
    assert(out == 200);
    assert(cc->used == 2);

    out = 0;
    assert(dsd_trunk_cc_candidates_next(st, 11.0, 0, &out) == 1);
    assert(out == 100);
    assert(cc->used == 3);

    dsd_state_ext_free_all(st);
    free(st);
}

static void
test_cooldown_ignores_zero_and_missing_frequencies(void) {
    dsd_state* st = calloc(1, sizeof(*st));
    assert(st != NULL);
    assert(dsd_trunk_cc_candidates_add(st, 100, 0, DSD_TRUNK_CC_CANDIDATE_CURRENT_SITE) == 1);

    dsd_trunk_cc_candidates* cc = dsd_trunk_cc_candidates_get(st);
    assert(cc != NULL);
    assert(cc->cool_until[0] == 0.0);

    dsd_trunk_cc_candidates_set_cooldown(st, 0, 5.0);
    assert(cc->cool_until[0] == 0.0);
    dsd_trunk_cc_candidates_set_cooldown(st, 200, 5.0);
    assert(cc->cool_until[0] == 0.0);

    dsd_state_ext_free_all(st);
    free(st);
}

/* Issue #666 (B6): a search that finds nothing leaves the walk where it was. From index 0 a full pass used to end at
   count, which reads as "the last candidate was handed out" to anything stepping backward from the walk. */
static void
test_fruitless_search_keeps_the_walk(void) {
    dsd_state* st = calloc(1, sizeof(*st));
    assert(st != NULL);
    assert(dsd_trunk_cc_candidates_add(st, 100, 0, 0) == 1);
    assert(dsd_trunk_cc_candidates_add(st, 200, 0, 0) == 1);
    dsd_trunk_cc_candidates* cc = dsd_trunk_cc_candidates_get(st);
    assert(cc != NULL);

    long out = 0;
    assert(dsd_trunk_cc_candidates_next(st, 0.0, DSD_TRUNK_CC_CANDIDATE_CURRENT_SITE, &out) == 0);
    assert(out == 0);
    assert(cc->idx == 0);
    assert(cc->used == 0);

    assert(dsd_trunk_cc_candidates_next(st, 0.0, 0, &out) == 1);
    assert(out == 100);
    assert(cc->idx == 1);
    assert(dsd_trunk_cc_candidates_next(st, 0.0, DSD_TRUNK_CC_CANDIDATE_CURRENT_SITE, &out) == 0);
    assert(cc->idx == 1);
    assert(cc->used == 1);

    dsd_state_ext_free_all(st);
    free(st);
}

/* Issue #666 (B5): a checkpoint taken before a pick puts the walk back, so a deferred tune retries the same
   candidate and is not counted as a use. */
static void
test_checkpoint_restore(void) {
    dsd_trunk_cc_candidates_cursor cursor = {.present = 1, .idx = 7, .used = 7U};
    dsd_trunk_cc_candidates_checkpoint(NULL, &cursor);
    assert(cursor.present == 0);
    dsd_trunk_cc_candidates_checkpoint(NULL, NULL);
    dsd_trunk_cc_candidates_restore(NULL, &cursor);

    dsd_state* st = calloc(1, sizeof(*st));
    assert(st != NULL);
    cursor.present = 1;
    dsd_trunk_cc_candidates_checkpoint(st, &cursor);
    assert(cursor.present == 0);
    dsd_trunk_cc_candidates_restore(st, &cursor);
    dsd_trunk_cc_candidates_restore(st, NULL);
    assert(dsd_trunk_cc_candidates_peek(st) == NULL); /* neither allocates a list */

    assert(dsd_trunk_cc_candidates_add(st, 100, 0, 0) == 1);
    assert(dsd_trunk_cc_candidates_add(st, 200, 0, 0) == 1);
    assert(dsd_trunk_cc_candidates_add(st, 300, 0, 0) == 1);
    dsd_trunk_cc_candidates* cc = dsd_trunk_cc_candidates_get(st);
    assert(cc != NULL);
    long out = 0;
    assert(dsd_trunk_cc_candidates_next(st, 0.0, 0, &out) == 1 && out == 100);

    dsd_trunk_cc_candidates_checkpoint(st, &cursor);
    assert(cursor.present == 1 && cursor.idx == 1 && cursor.used == 1U);
    assert(dsd_trunk_cc_candidates_next(st, 0.0, 0, &out) == 1 && out == 200);
    assert(cc->idx == 2 && cc->used == 2U);
    dsd_trunk_cc_candidates_restore(st, &cursor);
    assert(cc->idx == 1 && cc->used == 1U);
    out = 0;
    assert(dsd_trunk_cc_candidates_next(st, 0.0, 0, &out) == 1 && out == 200);

    /* The last candidate handed out wraps; putting it back hands it out again. */
    assert(dsd_trunk_cc_candidates_next(st, 0.0, 0, &out) == 1 && out == 300);
    dsd_trunk_cc_candidates_checkpoint(st, &cursor);
    assert(cursor.idx == 3);
    assert(dsd_trunk_cc_candidates_next(st, 0.0, 0, &out) == 1 && out == 100);
    dsd_trunk_cc_candidates_restore(st, &cursor);
    assert(cc->idx == 3 && cc->used == 3U);
    out = 0;
    assert(dsd_trunk_cc_candidates_next(st, 0.0, 0, &out) == 1 && out == 100);

    dsd_state_ext_free_all(st);
    free(st);
}

/* Issue #666 (D7): the walk backward. With nothing handed out it starts at the last candidate; otherwise at the one
   before the last handed out, wrapping, and that one itself last. What it hands out is the last handed out, so the
   next walk either way goes on from it, and each pick counts a use. It skips what the forward walk skips (the control
   channel, a cooling candidate, a missing flag), finds the only eligible one even when it is the last handed out, and
   finding none leaves the walk where it was. An eviction of the oldest keeps naming the same candidate. */
static void
test_prev_walks_back(void) {
    dsd_state* st = calloc(1, sizeof(*st));
    assert(st != NULL);
    long out = 1234;
    assert(dsd_trunk_cc_candidates_prev(NULL, 0.0, 0, &out) == 0 && out == 1234);
    assert(dsd_trunk_cc_candidates_prev(st, 0.0, 0, NULL) == 0);
    assert(dsd_trunk_cc_candidates_prev(st, 0.0, 0, &out) == 0 && out == 1234);
    assert(dsd_trunk_cc_candidates_peek(st) == NULL);

    for (long f = 100; f <= 400; f += 100) {
        assert(dsd_trunk_cc_candidates_add(st, f, 0, DSD_TRUNK_CC_CANDIDATE_CURRENT_SITE) == 1);
    }
    dsd_trunk_cc_candidates* cc = dsd_trunk_cc_candidates_get(st);
    assert(cc != NULL);
    assert(dsd_trunk_cc_candidates_prev(st, 0.0, 0, &out) == 1 && out == 400);
    assert(cc->idx == 4 && cc->used == 1U);
    assert(dsd_trunk_cc_candidates_prev(st, 0.0, 0, &out) == 1 && out == 300);
    assert(cc->idx == 3 && cc->used == 2U);
    /* Forward goes on from there, and back again. */
    assert(dsd_trunk_cc_candidates_next(st, 0.0, 0, &out) == 1 && out == 400);
    assert(dsd_trunk_cc_candidates_prev(st, 0.0, 0, &out) == 1 && out == 300);
    assert(dsd_trunk_cc_candidates_prev(st, 0.0, 0, &out) == 1 && out == 200);
    assert(dsd_trunk_cc_candidates_prev(st, 0.0, 0, &out) == 1 && out == 100);
    assert(cc->idx == 1);
    assert(dsd_trunk_cc_candidates_prev(st, 0.0, 0, &out) == 1 && out == 400); /* wrapping */
    assert(cc->idx == 4 && cc->used == 7U);

    /* Skips: the control channel, a cooling candidate. */
    st->trunk_cc_freq = 300;
    dsd_trunk_cc_candidates_set_cooldown(st, 200, 10.0);
    assert(dsd_trunk_cc_candidates_prev(st, 0.0, 0, &out) == 1 && out == 100);
    assert(cc->idx == 1);
    assert(dsd_trunk_cc_candidates_prev(st, 11.0, 0, &out) == 1 && out == 400);
    assert(dsd_trunk_cc_candidates_prev(st, 11.0, 0, &out) == 1 && out == 200);
    st->trunk_cc_freq = 0;

    /* The only eligible candidate is the one last handed out: found again, last. */
    dsd_trunk_cc_candidates_set_cooldown(st, 100, 50.0);
    dsd_trunk_cc_candidates_set_cooldown(st, 300, 50.0);
    dsd_trunk_cc_candidates_set_cooldown(st, 400, 50.0);
    const unsigned int used = cc->used;
    assert(dsd_trunk_cc_candidates_prev(st, 20.0, 0, &out) == 1 && out == 200);
    assert(cc->idx == 2 && cc->used == used + 1U);
    /* None eligible: nothing moves. */
    out = 1234;
    assert(dsd_trunk_cc_candidates_prev(st, 20.0, (uint8_t)(DSD_TRUNK_CC_CANDIDATE_CURRENT_SITE << 1), &out) == 0);
    assert(out == 1234 && cc->idx == 2 && cc->used == used + 1U);

    /* An eviction moves the index with the list, so it still names 200; back from it now wraps to the newest. */
    for (int k = 0; k < DSD_TRUNK_CC_CANDIDATES_MAX; k++) {
        cc->cool_until[k] = 0.0;
    }
    for (long f = 500; f < 500 + 100 * (DSD_TRUNK_CC_CANDIDATES_MAX - 4); f += 100) {
        assert(dsd_trunk_cc_candidates_add(st, f, 0, DSD_TRUNK_CC_CANDIDATE_CURRENT_SITE) == 1);
    }
    assert(cc->count == DSD_TRUNK_CC_CANDIDATES_MAX && cc->idx == 2);
    assert(dsd_trunk_cc_candidates_add(st, 9900, 0, DSD_TRUNK_CC_CANDIDATE_CURRENT_SITE) == 1); /* 100 goes */
    assert(cc->idx == 1 && cc->candidates[0] == 200);
    assert(dsd_trunk_cc_candidates_prev(st, 0.0, 0, &out) == 1 && out == 9900); /* before 200, wrapping */

    dsd_state_ext_free_all(st);
    free(st);
}

int
main(void) {
    test_public_guards();
    test_add_dedup_rollover();
    test_invalid_count_is_cleared_before_failure();
    test_filtered_iteration();
    test_next_and_cooldown();
    test_cooldown_ignores_zero_and_missing_frequencies();
    test_fruitless_search_keeps_the_walk();
    test_checkpoint_restore();
    test_prev_walks_back();
    return 0;
}
