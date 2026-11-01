// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Issue #666: the scan list's position records, apart from its cursor (lcn_freq_roll). The row heard is the last
 * non-zero row an accepted step selected, while that row still holds the frequency it was recorded with; before any
 * step recorded one, the row before the cursor unless it is a placeholder, and none after a hard-failed step. The
 * backward-retry row follows the same validity rule, and a new row heard drops it.
 */

#include <assert.h>
#include <dsd-neo/core/state.h>
#include <stdlib.h>
#include "dsd-neo/core/state_fwd.h"

static dsd_state*
make_state(void) {
    dsd_state* st = (dsd_state*)calloc(1, sizeof(*st));
    assert(st != NULL);
    dsd_state_lcn_nav_reset(st);
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

/* A zero-filled state (calloc, a snapshot of one) names no row: row 0 at 0 Hz is no record. */
static void
test_zeroed_state_records_nothing(void) {
    dsd_state* st = (dsd_state*)calloc(1, sizeof(*st));
    assert(st != NULL);
    seed_rows(st, 2);
    assert(dsd_state_lcn_on_air_row(st) == -1);
    assert(dsd_state_lcn_back_retry_row(st) == -1);
    assert(dsd_state_lcn_heard_row(st) == -1);
    assert(dsd_state_lcn_on_air_row(NULL) == -1);
    assert(dsd_state_lcn_heard_row(NULL) == -1);
    assert(dsd_state_lcn_back_retry_row(NULL) == -1);
    dsd_state_lcn_on_air_note(NULL, 0);
    dsd_state_lcn_back_retry_note(NULL, 0);
    dsd_state_lcn_nav_reset(NULL);
    free_state(st);
}

/* Before a step records a row, the row heard is the one before the cursor, as every reader took it before the split,
   except a placeholder (parked over) and a cursor outside the list. */
static void
test_heard_row_falls_back_to_the_cursor(void) {
    dsd_state* st = make_state();
    seed_rows(st, 3);
    st->lcn_freq_roll = 0;
    assert(dsd_state_lcn_heard_row(st) == -1);
    st->lcn_freq_roll = 2;
    assert(dsd_state_lcn_heard_row(st) == 1);
    st->lcn_freq_roll = 3;
    assert(dsd_state_lcn_heard_row(st) == 2);
    st->lcn_freq_roll = 4;
    assert(dsd_state_lcn_heard_row(st) == -1);
    st->lcn_freq_roll = -1;
    assert(dsd_state_lcn_heard_row(st) == -1);
    st->lcn_freq_roll = 2;
    *dsd_state_trunk_lcn_slot(st, 1) = 0L;
    assert(dsd_state_lcn_heard_row(st) == -1);
    free_state(st);
}

/* A recorded row stands while the cursor moves on: past a skipped row, a wrap, a placeholder park, a step waiting on
   its tune. */
static void
test_recorded_row_outlives_the_cursor(void) {
    dsd_state* st = make_state();
    seed_rows(st, 4);
    *dsd_state_trunk_lcn_slot(st, 2) = 0L;
    dsd_state_lcn_on_air_note(st, 1);
    for (int roll = 0; roll <= 4; roll++) {
        st->lcn_freq_roll = roll;
        assert(dsd_state_lcn_on_air_row(st) == 1);
        assert(dsd_state_lcn_heard_row(st) == 1);
    }
    /* A placeholder and a row outside the list record nothing: the row heard stays. */
    dsd_state_lcn_on_air_note(st, 2);
    assert(dsd_state_lcn_heard_row(st) == 1);
    dsd_state_lcn_on_air_note(st, 4);
    dsd_state_lcn_on_air_note(st, -1);
    assert(dsd_state_lcn_heard_row(st) == 1);
    dsd_state_lcn_on_air_note(st, 3);
    assert(dsd_state_lcn_heard_row(st) == 3);
    free_state(st);
}

/* A recorded row that no longer describes the list reads as none, and the cursor rule takes over: its frequency
   edited, the row made a placeholder, the list shortened past it. Put back as it was, it reads again. */
static void
test_recorded_row_validates_against_the_list(void) {
    dsd_state* st = make_state();
    seed_rows(st, 3);
    dsd_state_lcn_on_air_note(st, 2);
    st->lcn_freq_roll = 1;
    *dsd_state_trunk_lcn_slot(st, 2) = 862000000L;
    assert(dsd_state_lcn_on_air_row(st) == -1);
    assert(dsd_state_lcn_heard_row(st) == 0);
    *dsd_state_trunk_lcn_slot(st, 2) = 0L;
    assert(dsd_state_lcn_on_air_row(st) == -1);
    *dsd_state_trunk_lcn_slot(st, 2) = 851025000L;
    assert(dsd_state_lcn_on_air_row(st) == 2);
    st->lcn_freq_count = 2;
    assert(dsd_state_lcn_on_air_row(st) == -1);
    assert(dsd_state_lcn_heard_row(st) == 0);
    st->lcn_freq_count = 3;
    assert(dsd_state_lcn_heard_row(st) == 2);
    dsd_state_lcn_on_air_clear(st);
    assert(dsd_state_lcn_on_air_row(st) == -1);
    assert(dsd_state_lcn_heard_row(st) == 0);
    free_state(st);
}

/* Rows in the heap tail (past the embedded slots) are recorded and validated the same way. */
static void
test_recorded_row_in_the_heap_tail(void) {
    dsd_state* st = make_state();
    seed_rows(st, 40);
    dsd_state_lcn_on_air_note(st, 33);
    dsd_state_lcn_back_retry_note(st, 30);
    assert(dsd_state_lcn_on_air_row(st) == 33);
    assert(dsd_state_lcn_back_retry_row(st) == 30);
    st->lcn_freq_count = 31;
    assert(dsd_state_lcn_on_air_row(st) == -1);
    assert(dsd_state_lcn_back_retry_row(st) == 30);
    free_state(st);
}

/* The backward-retry row: same validity rule; a row heard drops it, and so does a reset, which drops both. */
static void
test_back_retry_row(void) {
    dsd_state* st = make_state();
    seed_rows(st, 3);
    *dsd_state_trunk_lcn_slot(st, 0) = 0L;
    dsd_state_lcn_back_retry_note(st, 0);
    assert(dsd_state_lcn_back_retry_row(st) == -1);
    dsd_state_lcn_back_retry_note(st, 2);
    assert(dsd_state_lcn_back_retry_row(st) == 2);
    *dsd_state_trunk_lcn_slot(st, 2) = 862000000L;
    assert(dsd_state_lcn_back_retry_row(st) == -1);
    *dsd_state_trunk_lcn_slot(st, 2) = 851025000L;
    assert(dsd_state_lcn_back_retry_row(st) == 2);
    dsd_state_lcn_back_retry_clear(st);
    assert(dsd_state_lcn_back_retry_row(st) == -1);

    dsd_state_lcn_back_retry_note(st, 2);
    dsd_state_lcn_on_air_note(st, 1);
    assert(dsd_state_lcn_back_retry_row(st) == -1);
    assert(dsd_state_lcn_on_air_row(st) == 1);

    dsd_state_lcn_back_retry_note(st, 2);
    dsd_state_lcn_nav_reset(st);
    assert(dsd_state_lcn_on_air_row(st) == -1);
    assert(dsd_state_lcn_back_retry_row(st) == -1);
    assert(st->lcn_on_air_row == -1 && st->lcn_back_retry_row == -1);
    free_state(st);
}

/* A hard-failed step with no row recorded makes the row heard none, not the row before a cursor the failure moved past
   a row it never tuned; it stays none as the cursor moves, until a row is recorded or the records are reset, when the
   cursor rule applies again. A record that still describes the list is left alone, and one that stopped describing it
   counts as none. */
static void
test_refused_step_leaves_no_row_heard(void) {
    dsd_state* st = make_state();
    seed_rows(st, 3);
    st->lcn_freq_roll = 1;
    assert(dsd_state_lcn_heard_row(st) == 0);
    dsd_state_lcn_on_air_note_refused(st);
    assert(dsd_state_lcn_heard_row(st) == -1 && dsd_state_lcn_on_air_row(st) == -1);
    for (int roll = 0; roll <= 3; roll++) {
        st->lcn_freq_roll = roll;
        assert(dsd_state_lcn_heard_row(st) == -1);
    }
    dsd_state_lcn_on_air_note(st, 1);
    assert(dsd_state_lcn_heard_row(st) == 1);
    dsd_state_lcn_on_air_note_refused(st);
    assert(dsd_state_lcn_heard_row(st) == 1 && dsd_state_lcn_on_air_row(st) == 1);

    *dsd_state_trunk_lcn_slot(st, 1) = 862000000L;
    st->lcn_freq_roll = 3;
    assert(dsd_state_lcn_heard_row(st) == 2);
    dsd_state_lcn_on_air_note_refused(st);
    assert(dsd_state_lcn_heard_row(st) == -1);
    /* Put back as it was, the old record does not come back: the refusal replaced it. */
    *dsd_state_trunk_lcn_slot(st, 1) = 851012500L;
    assert(dsd_state_lcn_heard_row(st) == -1);

    dsd_state_lcn_nav_reset(st);
    assert(dsd_state_lcn_heard_row(st) == 2);
    dsd_state_lcn_on_air_note_refused(NULL);
    free_state(st);
}

int
main(void) {
    test_zeroed_state_records_nothing();
    test_heard_row_falls_back_to_the_cursor();
    test_recorded_row_outlives_the_cursor();
    test_recorded_row_validates_against_the_list();
    test_recorded_row_in_the_heap_tail();
    test_back_retry_row();
    test_refused_step_leaves_no_row_heard();
    return 0;
}
