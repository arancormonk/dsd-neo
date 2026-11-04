// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Scan-list position records: the row heard and the backward-retry row (issue #666).
 *
 * Apart from dsd_state_trunk_lcn.c so the targets that read the row heard (the terminal
 * printer's tests among them) attach the real rule with no store or policy dependency.
 */

#include <dsd-neo/core/state.h>

#include "dsd-neo/core/state_fwd.h"

/* lcn_on_air_row after a refused step left no row recorded: none, with no fallback to the row before the cursor, which
   the refusal moved past the row it never tuned. Any row note or a reset replaces it. */
#define LCN_NAV_ROW_REFUSED (-2)

/* Whether scan-list row @p row is in the list and carries a frequency (not a 0 Hz placeholder), which @p freq gets. */
static int
lcn_nav_row_freq(const dsd_state* state, int row, long int* freq) {
    if (row < 0 || row >= state->lcn_freq_count) {
        return 0;
    }
    *freq = *dsd_state_trunk_lcn_slot_const(state, row);
    return *freq != 0;
}

/* A record describes the list while its row is in it and still holds the frequency it held when recorded: an edit, a
 * removal or a shorter list makes it none, by frequency alone (no list revision, dsd_state::lcn_list_rev, is kept). */
static int
lcn_nav_row_valid(const dsd_state* state, int row, long int recorded_freq) {
    long int freq = 0;
    return lcn_nav_row_freq(state, row, &freq) && freq == recorded_freq;
}

void
dsd_state_lcn_on_air_note(dsd_state* state, int row) {
    long int freq = 0;
    if (!state || !lcn_nav_row_freq(state, row, &freq)) {
        return;
    }
    state->lcn_on_air_row = row;
    state->lcn_on_air_freq = freq;
    dsd_state_lcn_back_retry_clear(state);
}

void
dsd_state_lcn_on_air_note_refused(dsd_state* state) {
    if (!state || dsd_state_lcn_on_air_row(state) >= 0) {
        return;
    }
    state->lcn_on_air_row = LCN_NAV_ROW_REFUSED;
    state->lcn_on_air_freq = 0;
}

void
dsd_state_lcn_on_air_clear(dsd_state* state) {
    if (!state) {
        return;
    }
    state->lcn_on_air_row = -1;
    state->lcn_on_air_freq = 0;
}

int
dsd_state_lcn_on_air_row(const dsd_state* state) {
    if (!state || !lcn_nav_row_valid(state, state->lcn_on_air_row, state->lcn_on_air_freq)) {
        return -1;
    }
    return state->lcn_on_air_row;
}

int
dsd_state_lcn_heard_row(const dsd_state* state) {
    const int recorded = dsd_state_lcn_on_air_row(state);
    if (recorded >= 0 || !state || state->lcn_on_air_row == LCN_NAV_ROW_REFUSED) {
        return recorded;
    }
    /* No step recorded one yet (a list just loaded, a state restored from before the record existed): the row before
       the cursor, as every reader took it before the split, but never a placeholder, which is parked over. Not after a
       refused step, whose cursor stands past a row never tuned. */
    const int roll = state->lcn_freq_roll;
    if (roll > 0 && roll <= state->lcn_freq_count && *dsd_state_trunk_lcn_slot_const(state, roll - 1) != 0) {
        return roll - 1;
    }
    return -1;
}

void
dsd_state_lcn_back_retry_note(dsd_state* state, int row) {
    long int freq = 0;
    if (!state || !lcn_nav_row_freq(state, row, &freq)) {
        return;
    }
    state->lcn_back_retry_row = row;
    state->lcn_back_retry_freq = freq;
}

void
dsd_state_lcn_back_retry_clear(dsd_state* state) {
    if (!state) {
        return;
    }
    state->lcn_back_retry_row = -1;
    state->lcn_back_retry_freq = 0;
}

int
dsd_state_lcn_back_retry_row(const dsd_state* state) {
    if (!state || !lcn_nav_row_valid(state, state->lcn_back_retry_row, state->lcn_back_retry_freq)) {
        return -1;
    }
    return state->lcn_back_retry_row;
}

void
dsd_state_lcn_nav_reset(dsd_state* state) {
    dsd_state_lcn_on_air_clear(state);
    dsd_state_lcn_back_retry_clear(state);
}
