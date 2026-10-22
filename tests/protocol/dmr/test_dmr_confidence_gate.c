// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <assert.h>
#include <dsd-neo/core/access_code.h>
#include <dsd-neo/core/received_identity.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <math.h>
#include <stdint.h>
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
    assert(state.rx_id.valid == 0U); // a candidate is no received colour code yet
    assert(dmr_confidence_note_data_burst(&state, 0) == DMR_CONFIDENCE_LOCKED);
    assert(state.dmr_confidence_locked == 1);
    assert(state.dmr_confidence_color_code == 0);
    assert(state.dmr_confidence_mismatch_count == 0);
    assert(state.dmr_color_code == 0);
    // The lock is the carrier's received colour code, and every matching burst proves it again.
    assert(state.rx_id.family == (uint8_t)DSD_RXID_FAMILY_DMR && state.rx_id.valid == DSD_RXID_ACCESS);
    assert(state.rx_id.access_kind == (uint8_t)DSD_ACCESS_CODE_COLOR_CODE && state.rx_id.access == 0U);
    dsd_rxid_mark_stale(&state);
    assert(dmr_confidence_note_data_burst(&state, 0) == DMR_CONFIDENCE_LOCKED);
    assert(state.rx_id.fresh == DSD_RXID_ACCESS);
    // A mismatch the gate rejects names nothing.
    assert(dmr_confidence_note_data_burst(&state, 9) == DMR_CONFIDENCE_REJECT);
    assert(state.rx_id.access == 0U);
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
    assert(state.rx_id.access == 3U);
    assert(dmr_confidence_note_data_burst(&state, 7) == DMR_CONFIDENCE_LOCKED);
    assert(state.dmr_confidence_locked == 1);
    assert(state.dmr_confidence_color_code == 7);
    assert(state.dmr_color_code == 7);
    assert(state.rx_id.access == 7U); // the re-lock names the new colour code
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

#define GATE_TEST_T0_NS 1000000000000000000ULL

static void
at_ms(uint64_t ms) {
    dsd_decode_clock_test_set_ns(GATE_TEST_T0_NS + ms * 1000000ULL);
}

static int
stamped_at_ms(const dsd_state* state, uint64_t ms) {
    const double want = (double)(GATE_TEST_T0_NS + ms * 1000000ULL) / 1e9;
    return fabs(state->dmr_confidence_lock_m - want) < 1e-6;
}

static void
test_validated_content_stamps_decode_time(void) {
    // A startup control-channel hunt (issue #703) restarts its window from the last DMR content that passed this gate,
    // so every lock and every matching observation stamps the decode clock, and nothing else does.
    static dsd_state state;
    init_state(&state);
    dsd_decode_clock_use_test(GATE_TEST_T0_NS);

    at_ms(1000);
    assert(dmr_confidence_note_data_burst(&state, 5) == DMR_CONFIDENCE_PENDING);
    assert(state.dmr_confidence_lock_m < 1e-9); // one burst proves nothing
    assert(dmr_confidence_note_data_burst(&state, 5) == DMR_CONFIDENCE_LOCKED);
    assert(stamped_at_ms(&state, 1000));
    at_ms(2000);
    assert(dmr_confidence_note_data_burst(&state, 5) == DMR_CONFIDENCE_LOCKED);
    assert(stamped_at_ms(&state, 2000));
    at_ms(2500);
    assert(dmr_confidence_note_data_burst(&state, 9) == DMR_CONFIDENCE_REJECT);
    assert(stamped_at_ms(&state, 2000));
    dmr_confidence_reset(&state); // a carrier boundary does not undo content that was decoded
    assert(stamped_at_ms(&state, 2000));

    init_state(&state);
    at_ms(3000);
    dmr_confidence_note_voice_sync(&state, 1);
    assert(dmr_confidence_note_voice_burst(&state, 1, 3) == DMR_CONFIDENCE_PENDING);
    assert(state.dmr_confidence_lock_m < 1e-9);
    at_ms(3060);
    assert(dmr_confidence_note_voice_burst(&state, 1, 3) == DMR_CONFIDENCE_LOCKED);
    assert(stamped_at_ms(&state, 3060));
    dsd_decode_clock_use_system();
}

/* The lock is what tells a base station's DMR carrier from noise passing its sync word: only it marks the carrier
   two-slot TDMA (dsd_state::tdma_carrier), never a pending burst, a rejected one or a voice sync alone. After an
   acquisition reset clears the mark, the gate (which outlives the reset) marks it again on the next matching burst. */
static void
test_lock_marks_two_slot_carrier(void) {
    static dsd_state state;
    init_state(&state);

    assert(dmr_confidence_note_data_burst(&state, 3) == DMR_CONFIDENCE_PENDING);
    assert(dmr_confidence_note_data_burst(&state, 17) == DMR_CONFIDENCE_REJECT);
    dmr_confidence_note_voice_sync(&state, 1);
    assert(state.tdma_carrier == 0U);
    assert(dmr_confidence_note_data_burst(&state, 3) == DMR_CONFIDENCE_LOCKED);
    assert(state.tdma_carrier == 1U);

    state.tdma_carrier = 0U;
    assert(dmr_confidence_note_data_burst(&state, 9) == DMR_CONFIDENCE_REJECT);
    assert(state.tdma_carrier == 0U);
    assert(dmr_confidence_note_data_burst(&state, 3) == DMR_CONFIDENCE_LOCKED);
    assert(state.tdma_carrier == 1U);

    /* A voice burst that opens its slot is the same evidence. */
    init_state(&state);
    dmr_confidence_note_voice_sync(&state, 0);
    assert(dmr_confidence_note_voice_burst(&state, 0, 4) == DMR_CONFIDENCE_PENDING);
    assert(state.tdma_carrier == 0U);
    assert(dmr_confidence_note_voice_burst(&state, 0, 4) == DMR_CONFIDENCE_LOCKED);
    assert(state.tdma_carrier == 1U);
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
    test_validated_content_stamps_decode_time();
    test_lock_marks_two_slot_carrier();
    printf("DMR confidence gate: OK\n");
    return 0;
}
