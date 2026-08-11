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

/* Like make_state(), but also attaches a two-slot event history array so
   staged_group_name() has something to read instead of always taking its
   NULL-history early return. */
static dsd_state*
make_state_with_history(Event_History_I** history_out) {
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    assert(state != NULL);
    assert(dsd_call_state_ensure(state) > 0);
    Event_History_I* history = (Event_History_I*)calloc(DSD_CALL_STATE_SLOT_COUNT, sizeof(Event_History_I));
    assert(history != NULL);
    state->event_history_s = history;
    *history_out = history;
    return state;
}

/* Stage a CSV-imported group name on the slot's active (index 0) history row,
   the same row staged_group_name() reads. */
static void
stage_group_name(Event_History_I* history, uint8_t slot, uint32_t target_id, const char* name) {
    Event_History* item = &history[slot].Event_History_Items[0];
    item->target_id = target_id;
    DSD_SNPRINTF(item->t_name, sizeof(item->t_name), "%s", name);
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
test_tg_id_falls_back_to_policy_target_id(void) {
    dsd_state* state = make_state();
    dsd_call_observation observation = dsd_call_observation_data(DSD_SYNC_P25P2_POS, 0U, 0U, 0U);
    observation.kind = DSD_CALL_KIND_GROUP_VOICE;
    observation.policy_target_id = 654321U;
    observation.observed_m = 10.0;
    assert(dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_BEGIN) > 0);

    dsd_app_slot_call view;
    dsd_app_slot_call_view(state, 0U, 10.5, &view);
    assert(view.state == DSD_APP_CALL_LINE_ACTIVE);
    /* No OTA target ever decoded, so tg_id falls back to the policy-resolved id
       instead of staying 0. */
    assert(view.tg_id == 654321U);
    /* tg_text still falls back to the (zero) OTA numeric id: policy resolution
       feeds tg_id/name lookups, not the OTA display text. */
    assert(strcmp(view.tg_text, "0") == 0);
    free(state);
}

static void
test_target_text_used_verbatim(void) {
    dsd_state* state = make_state();
    observe_group_call(state, 0U, 0U, 0U, 10.0);

    dsd_call_snapshot snapshot;
    assert(dsd_call_state_get(state, 0U, &snapshot) == 1);
    assert(dsd_call_state_enrich_text(state, 0U, snapshot.epoch, NULL, "DISPATCH-1", NULL, NULL, 10.05) == 1);

    dsd_app_slot_call view;
    dsd_app_slot_call_view(state, 0U, 12.0, &view);
    /* Text identity alone -- no numeric target ever decoded -- is enough to
       keep the call from reading idle, and is used verbatim over the (zero)
       numeric fallback. */
    assert(view.state == DSD_APP_CALL_LINE_ACTIVE);
    assert(strcmp(view.tg_text, "DISPATCH-1") == 0);
    assert(strcmp(view.name, "DISPATCH-1") == 0);
    free(state);
}

static void
test_source_text_used_verbatim(void) {
    dsd_state* state = make_state();
    observe_group_call(state, 0U, 0U, 0U, 10.0);

    dsd_call_snapshot snapshot;
    assert(dsd_call_state_get(state, 0U, &snapshot) == 1);
    assert(dsd_call_state_enrich_text(state, 0U, snapshot.epoch, "N0CALL", NULL, NULL, NULL, 10.05) == 1);

    dsd_app_slot_call view;
    dsd_app_slot_call_view(state, 0U, 12.0, &view);
    assert(view.state == DSD_APP_CALL_LINE_ACTIVE);
    assert(strcmp(view.src_text, "N0CALL") == 0);
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
test_route_text_leg0_confers_identity(void) {
    dsd_state* state = make_state();
    /* Same identity-less baseline as test_identityless_epoch_reads_idle(): no
       numeric ids, no source/target text. Without the enrichment below this
       reads idle. */
    observe_group_call(state, 0U, 0U, 0U, 10.0);

    dsd_call_snapshot snapshot;
    assert(dsd_call_state_get(state, 0U, &snapshot) == 1);
    /* D-STAR/NXDN/YSF can decode the first repeater leg before either callsign;
       that alone must count as a named transmission, matching
       dsd_call_state_snapshot_has_identity(). */
    assert(dsd_call_state_enrich_text(state, 0U, snapshot.epoch, NULL, NULL, "RPT1 GATEWAY", NULL, 10.05) == 1);

    dsd_app_slot_call view;
    dsd_app_slot_call_view(state, 0U, 10.5, &view);
    assert(view.state == DSD_APP_CALL_LINE_ACTIVE);
    free(state);
}

static void
test_route_text_leg1_confers_identity(void) {
    dsd_state* state = make_state();
    observe_group_call(state, 0U, 0U, 0U, 10.0);

    dsd_call_snapshot snapshot;
    assert(dsd_call_state_get(state, 0U, &snapshot) == 1);
    /* Same as leg0, but only the second repeater leg decoded. Either leg alone
       must be enough -- call_has_no_identity() ANDs both being empty. */
    assert(dsd_call_state_enrich_text(state, 0U, snapshot.epoch, NULL, NULL, NULL, "RPT2 GATEWAY", 10.05) == 1);

    dsd_app_slot_call view;
    dsd_app_slot_call_view(state, 0U, 10.5, &view);
    assert(view.state == DSD_APP_CALL_LINE_ACTIVE);
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

/* call_reads_encrypted() treats ENCRYPTED, ENCRYPTED_PENDING and DECRYPTABLE as
   equally "encrypted over the air" -- the same three-way definition the event
   ring stamps on history rows. Exercised for all three so a regression that
   narrows the check to plain ENCRYPTED fails here. */
static void
test_crypto_classification_reads_encrypted(dsd_call_crypto_state classification) {
    dsd_state* state = make_state();
    observe_group_call(state, 0U, 51023U, 1234567U, 10.0);

    dsd_call_crypto_update crypto;
    DSD_MEMSET(&crypto, 0, sizeof(crypto));
    crypto.classification = classification;
    crypto.algid = 0xAAU;
    crypto.kid = 0x2222U;
    crypto.observed_m = 10.1;
    assert(dsd_call_state_update_crypto(state, 0U, &crypto) > 0);

    dsd_app_slot_call view;
    dsd_app_slot_call_view(state, 0U, 11.0, &view);
    assert(view.enc == 1U);
    assert(view.algid == 0xAAU);
    assert(view.kid == 0x2222U);
    free(state);
}

static void
test_staged_group_name_used_when_target_matches(void) {
    Event_History_I* history = NULL;
    dsd_state* state = make_state_with_history(&history);
    observe_group_call(state, 0U, 51023U, 1234567U, 10.0);
    stage_group_name(history, 0U, 51023U, "Metro Fire Dispatch");

    dsd_app_slot_call view;
    dsd_app_slot_call_view(state, 0U, 12.0, &view);
    assert(view.state == DSD_APP_CALL_LINE_ACTIVE);
    assert(strcmp(view.name, "Metro Fire Dispatch") == 0);
    /* The staged name replaces the display name; it does not touch tg_text. */
    assert(strcmp(view.tg_text, "51023") == 0);
    free(history);
    free(state);
}

static void
test_staged_group_name_ignored_for_other_talkgroup(void) {
    Event_History_I* history = NULL;
    dsd_state* state = make_state_with_history(&history);
    observe_group_call(state, 0U, 51023U, 1234567U, 10.0);
    /* Staged row describes a previous call on a different talkgroup and must
       not caption this one -- the guarantee staged_group_name()'s doc comment
       makes, and the most valuable case here: a broken target_id comparison
       would let a stale name bleed into the next call. */
    stage_group_name(history, 0U, 99999U, "Old Talkgroup");

    dsd_app_slot_call view;
    dsd_app_slot_call_view(state, 0U, 12.0, &view);
    assert(strcmp(view.name, "51023") == 0);
    free(history);
    free(state);
}

static void
test_staged_group_name_ignored_when_empty(void) {
    Event_History_I* history = NULL;
    dsd_state* state = make_state_with_history(&history);
    observe_group_call(state, 0U, 51023U, 1234567U, 10.0);
    /* target_id matches, but the row's t_name was never populated (no CSV
       import staged one). */
    history[0].Event_History_Items[0].target_id = 51023U;

    dsd_app_slot_call view;
    dsd_app_slot_call_view(state, 0U, 12.0, &view);
    assert(strcmp(view.name, "51023") == 0);
    free(history);
    free(state);
}

static void
test_staged_group_name_respects_slot(void) {
    Event_History_I* history = NULL;
    dsd_state* state = make_state_with_history(&history);
    observe_group_call(state, 0U, 51023U, 1234567U, 10.0);
    /* Same talkgroup id staged on the *other* slot must not caption slot 0:
       a regression that dropped the slot index from staged_group_name()'s
       lookup would pass every other test in this file. */
    stage_group_name(history, 1U, 51023U, "Wrong Slot");

    dsd_app_slot_call view;
    dsd_app_slot_call_view(state, 0U, 12.0, &view);
    assert(strcmp(view.name, "51023") == 0);
    free(history);
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
    /* Exactly at hold_s the comparison is strict less-than, not less-or-equal,
       so equality already reads idle rather than getting one extra tick of
       grace. */
    assert(dsd_app_call_line_state(1, &call, 13.0, 3.0) == DSD_APP_CALL_LINE_IDLE);
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

static void
test_seconds_truncation_matches_qt_adapter(void) {
    /* Qt renders whole seconds as (int)(elapsed_ms / 1000). Pin the boundaries so the
       adapter's integer division cannot drift from the old (int)elapsed_seconds. */
    dsd_state* state = make_state();
    observe_group_call(state, 0U, 51023U, 1234567U, 10.0);

    dsd_app_slot_call just_under;
    dsd_app_slot_call_view(state, 0U, 12.999, &just_under);
    assert(just_under.elapsed_ms / 1000U == 2U);

    dsd_app_slot_call just_over;
    dsd_app_slot_call_view(state, 0U, 13.001, &just_over);
    assert(just_over.elapsed_ms / 1000U == 3U);
    free(state);
}

int
main(void) {
    test_idle_slot_reports_none();
    test_active_call_reports_identity();
    test_tg_id_falls_back_to_policy_target_id();
    test_target_text_used_verbatim();
    test_source_text_used_verbatim();
    test_ended_call_holds_then_expires();
    test_identityless_epoch_reads_idle();
    test_route_text_leg0_confers_identity();
    test_route_text_leg1_confers_identity();
    test_x2tdma_identityless_epoch_still_shows();
    test_encrypted_call_carries_alg_and_kid();
    test_crypto_classification_reads_encrypted(DSD_CALL_CRYPTO_ENCRYPTED_PENDING);
    test_crypto_classification_reads_encrypted(DSD_CALL_CRYPTO_DECRYPTABLE);
    test_staged_group_name_used_when_target_matches();
    test_staged_group_name_ignored_for_other_talkgroup();
    test_staged_group_name_ignored_when_empty();
    test_staged_group_name_respects_slot();
    test_call_line_state_boundaries();
    test_null_arguments_are_safe();
    test_seconds_truncation_matches_qt_adapter();
    printf("APP_CONTROL_CALL_VIEW ok\n");
    return 0;
}
