// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Shared slot-call display decision, independent of any frontend.
 */

#include <assert.h>
#include <dsd-neo/app_control/call_view.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static dsd_state*
make_state(void) {
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    assert(state != NULL);
    assert(dsd_call_state_ensure(state) > 0);
    return state;
}

static void
observe_group_call(dsd_state* state, uint8_t slot, uint64_t target, uint64_t source, double now_m) {
    dsd_call_observation observation = dsd_call_observation_data(DSD_SYNC_P25P2_POS, slot, source, target);
    observation.kind = DSD_CALL_KIND_GROUP_VOICE;
    observation.observed_m = now_m;
    assert(dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_BEGIN) > 0);
}

static void
test_idle_slot_reports_none(void) {
    dsd_state* state = make_state();
    dsd_app_slot_call view;
    dsd_app_slot_call_view(state, 0U, 10.0, &view);
    assert(view.state == DSD_APP_CALL_LINE_NONE);
    assert(view.tg_text[0] == '\0');
    free(state);
}

static void
test_active_call_reports_identity(void) {
    dsd_state* state = make_state();
    observe_group_call(state, 0U, 51023U, 1234567U, 10.0);

    dsd_app_slot_call view;
    dsd_app_slot_call_view(state, 0U, 12.0, &view);
    assert(view.state == DSD_APP_CALL_LINE_ACTIVE);
    assert(strcmp(view.tg_text, "51023") == 0);
    assert(strcmp(view.src_text, "1234567") == 0);
    /* No CSV group name staged, so the name falls back to the talkgroup text. */
    assert(strcmp(view.name, "51023") == 0);
    assert(view.tg_id == 51023U);
    assert(view.enc == 0U);
    assert(view.elapsed_ms >= 1900U && view.elapsed_ms <= 2100U);
    free(state);
}

static void
test_ended_call_holds_then_expires(void) {
    dsd_state* state = make_state();
    observe_group_call(state, 0U, 51023U, 1234567U, 10.0);
    assert(dsd_call_state_end_ex(state, 0U, 12.0, DSD_CALL_END_TERMINATOR) > 0);

    dsd_app_slot_call held;
    dsd_app_slot_call_view(state, 0U, 13.0, &held);
    assert(held.state == DSD_APP_CALL_LINE_ENDED);
    /* Elapsed freezes at the end, it does not keep counting. */
    assert(held.elapsed_ms >= 1900U && held.elapsed_ms <= 2100U);

    dsd_app_slot_call expired;
    dsd_app_slot_call_view(state, 0U, 12.0 + DSD_APP_CALL_LINE_ENDED_HOLD_S + 0.5, &expired);
    assert(expired.state == DSD_APP_CALL_LINE_IDLE);
    free(state);
}

static void
test_identityless_epoch_reads_idle(void) {
    dsd_state* state = make_state();
    /* A frame that synced far enough to be a frame and no further. */
    observe_group_call(state, 0U, 0U, 0U, 10.0);

    dsd_app_slot_call view;
    dsd_app_slot_call_view(state, 0U, 10.5, &view);
    assert(view.state == DSD_APP_CALL_LINE_IDLE);
    free(state);
}

static void
test_x2tdma_identityless_epoch_still_shows(void) {
    dsd_state* state = make_state();
    dsd_call_observation observation = dsd_call_observation_data(DSD_SYNC_X2TDMA_VOICE_POS, 0U, 0U, 0U);
    observation.kind = DSD_CALL_KIND_VOICE;
    observation.observed_m = 10.0;
    assert(dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_BEGIN) > 0);

    dsd_app_slot_call view;
    dsd_app_slot_call_view(state, 0U, 10.5, &view);
    /* X2-TDMA never parses voice into a talkgroup, so an identity-less epoch is
       the whole story the protocol has. Suppressing it would hide real traffic. */
    assert(view.state == DSD_APP_CALL_LINE_ACTIVE);
    free(state);
}

static void
test_encrypted_call_carries_alg_and_kid(void) {
    dsd_state* state = make_state();
    observe_group_call(state, 0U, 51023U, 1234567U, 10.0);

    dsd_call_crypto_update crypto;
    DSD_MEMSET(&crypto, 0, sizeof(crypto));
    crypto.classification = DSD_CALL_CRYPTO_ENCRYPTED;
    crypto.algid = 0x84U;
    crypto.kid = 0x0101U;
    crypto.observed_m = 10.1;
    assert(dsd_call_state_update_crypto(state, 0U, &crypto) > 0);

    dsd_app_slot_call view;
    dsd_app_slot_call_view(state, 0U, 11.0, &view);
    assert(view.enc == 1U);
    assert(view.algid == 0x84U);
    assert(view.kid == 0x0101U);
    free(state);
}

static void
test_call_line_state_boundaries(void) {
    dsd_call_snapshot call;
    DSD_MEMSET(&call, 0, sizeof(call));

    /* A non-positive lookup means no epoch on the slot at all. */
    assert(dsd_app_call_line_state(0, &call, 10.0, 3.0) == DSD_APP_CALL_LINE_NONE);

    call.phase = DSD_CALL_PHASE_ACTIVE;
    assert(dsd_app_call_line_state(1, &call, 10.0, 3.0) == DSD_APP_CALL_LINE_ACTIVE);

    call.phase = DSD_CALL_PHASE_IDLE;
    assert(dsd_app_call_line_state(1, &call, 10.0, 3.0) == DSD_APP_CALL_LINE_IDLE);

    call.phase = DSD_CALL_PHASE_ENDED;
    call.ended_m = 10.0;
    assert(dsd_app_call_line_state(1, &call, 12.0, 3.0) == DSD_APP_CALL_LINE_ENDED);
    assert(dsd_app_call_line_state(1, &call, 14.0, 3.0) == DSD_APP_CALL_LINE_IDLE);
    /* An end stamped a hair ahead of the poll counts as fresh, not as an expiry. */
    assert(dsd_app_call_line_state(1, &call, 9.9, 3.0) == DSD_APP_CALL_LINE_ENDED);
}

static void
test_null_arguments_are_safe(void) {
    dsd_app_slot_call view;
    dsd_app_slot_call_view(NULL, 0U, 10.0, &view);
    assert(view.state == DSD_APP_CALL_LINE_NONE);
    dsd_app_slot_call_view(NULL, 0U, 10.0, NULL);
}

int
main(void) {
    test_idle_slot_reports_none();
    test_active_call_reports_identity();
    test_ended_call_holds_then_expires();
    test_identityless_epoch_reads_idle();
    test_x2tdma_identityless_epoch_still_shows();
    test_encrypted_call_carries_alg_and_kid();
    test_call_line_state_boundaries();
    test_null_arguments_are_safe();
    printf("APP_CONTROL_CALL_VIEW ok\n");
    return 0;
}
