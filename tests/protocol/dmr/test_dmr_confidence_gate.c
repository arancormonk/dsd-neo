// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <assert.h>
#include <dsd-neo/core/state.h>
#include <stdio.h>
#include "dmr_confidence.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

static void
init_state(dsd_state* state) {
    DSD_MEMSET(state, 0, sizeof(*state));
    state->dmr_color_code = 16;
    state->dmr_confidence_color_code = 16;
    state->dmr_confidence_candidate_cc = 16;
}

static void
test_non_idle_data_does_not_lock_from_fresh_start(void) {
    static dsd_state state;
    init_state(&state);

    for (int i = 0; i < 8; i++) {
        assert(dmr_confidence_note_data_burst(&state, 11, 1) == DMR_CONFIDENCE_PENDING);
    }

    assert(state.dmr_confidence_locked == 0);
    assert(state.dmr_color_code == 16);
}

static void
test_idle_data_can_lock_color_code(void) {
    static dsd_state state;
    init_state(&state);

    assert(dmr_confidence_note_data_burst(&state, 3, 9) == DMR_CONFIDENCE_PENDING);
    assert(dmr_confidence_note_data_burst(&state, 3, 9) == DMR_CONFIDENCE_LOCKED);
    assert(state.dmr_confidence_locked == 1);
    assert(state.dmr_confidence_color_code == 3);
    assert(state.dmr_color_code == 3);
}

static void
test_locked_color_code_rejects_mismatch(void) {
    static dsd_state state;
    init_state(&state);

    assert(dmr_confidence_note_data_burst(&state, 3, 9) == DMR_CONFIDENCE_PENDING);
    assert(dmr_confidence_note_data_burst(&state, 3, 9) == DMR_CONFIDENCE_LOCKED);
    assert(dmr_confidence_note_data_burst(&state, 11, 9) == DMR_CONFIDENCE_REJECT);
    assert(state.dmr_confidence_mismatch_count == 1);
    assert(state.dmr_confidence_color_code == 3);
}

static void
test_voice_requires_voice_sync_before_open(void) {
    static dsd_state state;
    init_state(&state);

    assert(dmr_confidence_note_voice_burst(&state, 1, 3) == DMR_CONFIDENCE_PENDING);
    assert(dmr_confidence_note_voice_burst(&state, 1, 3) == DMR_CONFIDENCE_PENDING);
    assert(dmr_confidence_voice_slot_open(&state, 1) == 0);

    dmr_confidence_note_voice_sync(&state, 1);
    assert(dmr_confidence_note_voice_burst(&state, 1, 3) == DMR_CONFIDENCE_PENDING);
    assert(dmr_confidence_note_voice_burst(&state, 1, 3) == DMR_CONFIDENCE_LOCKED);
    assert(dmr_confidence_voice_slot_open(&state, 1) == 1);
    assert(dmr_confidence_any_voice_open(&state) == 1);
}

static void
test_reset_clears_gate_state(void) {
    static dsd_state state;
    init_state(&state);

    dmr_confidence_note_voice_sync(&state, 0);
    assert(dmr_confidence_note_voice_burst(&state, 0, 3) == DMR_CONFIDENCE_PENDING);
    assert(dmr_confidence_note_voice_burst(&state, 0, 3) == DMR_CONFIDENCE_LOCKED);
    dmr_confidence_reset(&state);

    assert(state.dmr_confidence_locked == 0);
    assert(state.dmr_confidence_color_code == 16);
    assert(state.dmr_confidence_candidate_cc == 16);
    assert(dmr_confidence_any_voice_open(&state) == 0);
}

int
main(void) {
    test_non_idle_data_does_not_lock_from_fresh_start();
    test_idle_data_can_lock_color_code();
    test_locked_color_code_rejects_mismatch();
    test_voice_requires_voice_sync_before_open();
    test_reset_clears_gate_state();
    printf("DMR confidence gate: OK\n");
    return 0;
}
