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
test_single_data_burst_does_not_lock(void) {
    static dsd_state state;
    init_state(&state);

    assert(dmr_confidence_note_data_burst(&state, 11) == DMR_CONFIDENCE_PENDING);

    assert(state.dmr_confidence_locked == 0);
    assert(state.dmr_color_code == 16);
}

static void
test_repeated_data_bursts_lock_color_code(void) {
    // Regression for issue #348: a dedicated TIII TSCC carries continuous CSBK
    // signalling (C_ALOHA) and may never transmit IDLE bursts or voice, so
    // consistent data bursts of any type must be able to lock the gate. CC 0
    // is a valid colour code (and the Tier III default, ETSI TS 102 361-4
    // clause 6.2.1.3) and must not be confused with the "unknown" sentinel.
    static dsd_state state;
    init_state(&state);

    assert(dmr_confidence_note_data_burst(&state, 0) == DMR_CONFIDENCE_PENDING);
    assert(dmr_confidence_note_data_burst(&state, 0) == DMR_CONFIDENCE_LOCKED);
    assert(state.dmr_confidence_locked == 1);
    assert(state.dmr_confidence_color_code == 0);
    assert(state.dmr_confidence_mismatch_count == 0);
    assert(state.dmr_color_code == 0);
}

static void
test_inconsistent_cc_restarts_candidate_count(void) {
    static dsd_state state;
    init_state(&state);

    assert(dmr_confidence_note_data_burst(&state, 5) == DMR_CONFIDENCE_PENDING);
    assert(dmr_confidence_note_data_burst(&state, 7) == DMR_CONFIDENCE_PENDING);
    assert(dmr_confidence_note_data_burst(&state, 7) == DMR_CONFIDENCE_LOCKED);
    assert(state.dmr_confidence_color_code == 7);
    assert(state.dmr_color_code == 7);
}

static void
test_locked_color_code_rejects_mismatch(void) {
    static dsd_state state;
    init_state(&state);

    assert(dmr_confidence_note_data_burst(&state, 3) == DMR_CONFIDENCE_PENDING);
    assert(dmr_confidence_note_data_burst(&state, 3) == DMR_CONFIDENCE_LOCKED);
    assert(dmr_confidence_note_data_burst(&state, 11) == DMR_CONFIDENCE_REJECT);
    assert(state.dmr_confidence_mismatch_count == 1);
    assert(state.dmr_confidence_color_code == 3);
}

static void
test_sustained_mismatch_relocks_to_new_color_code(void) {
    // A locked gate must not reject forever: if the channel now carries a
    // consistent different colour code (site change without carrier loss),
    // sustained evidence re-locks instead of dropping every burst until the
    // next no-carrier reset.
    static dsd_state state;
    init_state(&state);

    assert(dmr_confidence_note_data_burst(&state, 3) == DMR_CONFIDENCE_PENDING);
    assert(dmr_confidence_note_data_burst(&state, 3) == DMR_CONFIDENCE_LOCKED);

    assert(dmr_confidence_note_data_burst(&state, 7) == DMR_CONFIDENCE_REJECT);
    assert(dmr_confidence_note_data_burst(&state, 7) == DMR_CONFIDENCE_REJECT);
    assert(dmr_confidence_note_data_burst(&state, 7) == DMR_CONFIDENCE_REJECT);
    assert(dmr_confidence_note_data_burst(&state, 7) == DMR_CONFIDENCE_LOCKED);
    assert(state.dmr_confidence_locked == 1);
    assert(state.dmr_confidence_color_code == 7);
    assert(state.dmr_color_code == 7);
}

static void
test_matching_burst_restarts_relock_streak(void) {
    // Sporadic co-channel interference must not accumulate toward a re-lock:
    // seeing the locked colour code again restarts the mismatch streak.
    static dsd_state state;
    init_state(&state);

    assert(dmr_confidence_note_data_burst(&state, 3) == DMR_CONFIDENCE_PENDING);
    assert(dmr_confidence_note_data_burst(&state, 3) == DMR_CONFIDENCE_LOCKED);

    assert(dmr_confidence_note_data_burst(&state, 7) == DMR_CONFIDENCE_REJECT);
    assert(dmr_confidence_note_data_burst(&state, 7) == DMR_CONFIDENCE_REJECT);
    assert(dmr_confidence_note_data_burst(&state, 7) == DMR_CONFIDENCE_REJECT);
    assert(dmr_confidence_note_data_burst(&state, 3) == DMR_CONFIDENCE_LOCKED);

    assert(dmr_confidence_note_data_burst(&state, 7) == DMR_CONFIDENCE_REJECT);
    assert(dmr_confidence_note_data_burst(&state, 7) == DMR_CONFIDENCE_REJECT);
    assert(dmr_confidence_note_data_burst(&state, 7) == DMR_CONFIDENCE_REJECT);
    assert(state.dmr_confidence_locked == 1);
    assert(state.dmr_confidence_color_code == 3);
    assert(state.dmr_color_code == 3);
}

static void
test_relock_closes_voice_gates(void) {
    // Re-locking means the previous system identity is gone; open voice
    // slots must re-qualify under the new colour code before audio resumes.
    static dsd_state state;
    init_state(&state);

    dmr_confidence_note_voice_sync(&state, 0);
    assert(dmr_confidence_note_voice_burst(&state, 0, 3) == DMR_CONFIDENCE_PENDING);
    assert(dmr_confidence_note_voice_burst(&state, 0, 3) == DMR_CONFIDENCE_LOCKED);
    assert(dmr_confidence_voice_slot_open(&state, 0) == 1);

    assert(dmr_confidence_note_data_burst(&state, 7) == DMR_CONFIDENCE_REJECT);
    assert(dmr_confidence_note_data_burst(&state, 7) == DMR_CONFIDENCE_REJECT);
    assert(dmr_confidence_note_data_burst(&state, 7) == DMR_CONFIDENCE_REJECT);
    assert(dmr_confidence_note_data_burst(&state, 7) == DMR_CONFIDENCE_LOCKED);
    assert(state.dmr_confidence_color_code == 7);
    assert(dmr_confidence_voice_slot_open(&state, 0) == 0);
    assert(dmr_confidence_any_voice_open(&state) == 0);
}

static void
test_invalid_color_code_rejected(void) {
    static dsd_state state;
    init_state(&state);

    assert(dmr_confidence_note_data_burst(&state, 16) == DMR_CONFIDENCE_REJECT);
    assert(state.dmr_confidence_locked == 0);
    assert(state.dmr_color_code == 16);
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

    dmr_confidence_note_voice_sync(&state, 0);
    assert(dmr_confidence_note_voice_burst(&state, 0, 3) == DMR_CONFIDENCE_PENDING);
    assert(dmr_confidence_note_voice_burst(&state, 0, 3) == DMR_CONFIDENCE_LOCKED);
    assert(dmr_confidence_voice_slot_open(&state, 0) == 1);

    dmr_confidence_reset_slot(&state, 1);
    assert(dmr_confidence_voice_slot_open(&state, 1) == 0);
    assert(dmr_confidence_voice_slot_open(&state, 0) == 1);
    assert(dmr_confidence_any_voice_open(&state) == 1);

    dmr_confidence_reset_slot(&state, 7);
    assert(dmr_confidence_voice_slot_open(&state, 0) == 1);
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
    test_single_data_burst_does_not_lock();
    test_repeated_data_bursts_lock_color_code();
    test_inconsistent_cc_restarts_candidate_count();
    test_locked_color_code_rejects_mismatch();
    test_sustained_mismatch_relocks_to_new_color_code();
    test_matching_burst_restarts_relock_streak();
    test_relock_closes_voice_gates();
    test_invalid_color_code_rejected();
    test_voice_requires_voice_sync_before_open();
    test_reset_clears_gate_state();
    printf("DMR confidence gate: OK\n");
    return 0;
}
