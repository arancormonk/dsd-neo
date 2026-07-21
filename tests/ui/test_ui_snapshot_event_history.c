// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <assert.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/input_level.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/runtime/trunk_cc_candidates.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../src/app_control/snapshot_internal.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

int
dsd_event_state_copy_snapshot_incremental(dsd_state* dst, const dsd_state* src, Event_History_I event_history[2],
                                          const uint64_t source_revisions[2], int force_copy, uint8_t copied[2]) {
    (void)dsd_call_state_copy_to_state(dst, src);
    copied[0] = 0U;
    copied[1] = 0U;
    if (!src || !src->event_history_s) {
        return 0;
    }
    for (size_t slot = 0; slot < 2U; slot++) {
        if (force_copy || source_revisions == NULL || source_revisions[slot] != src->event_history_s[slot].revision) {
            DSD_MEMCPY(&event_history[slot], &src->event_history_s[slot], sizeof(Event_History_I));
            copied[slot] = 1U;
        }
    }
    return 1;
}

int
dsd_event_state_copy_snapshot(dsd_state* dst, const dsd_state* src, Event_History_I event_history[2]) {
    uint8_t copied[2];
    return dsd_event_state_copy_snapshot_incremental(dst, src, event_history, NULL, 1, copied);
}

static void
assert_slot_tail(const dsd_state* snap, uint32_t slot0_src, uint32_t slot1_src) {
    assert(snap != NULL);
    assert(snap->event_history_s != NULL);
    assert(snap->event_history_s[0].Event_History_Items[1].source_id == slot0_src);
    assert(snap->event_history_s[1].Event_History_Items[1].source_id == slot1_src);
}

static void
mark_history(Event_History_I* history) {
    history->revision++;
    if (history->revision == 0U) {
        history->revision = 1U;
    }
}

static void
assert_history_copy_counts(uint64_t source_slot0, uint64_t source_slot1, uint64_t consumer_slot0,
                           uint64_t consumer_slot1) {
    dsd_app_snapshot_event_history_copy_counts counts;
    dsd_app_snapshot_test_get_event_history_copy_counts(&counts);
    assert(counts.source_to_published[0] == source_slot0);
    assert(counts.source_to_published[1] == source_slot1);
    assert(counts.published_to_consumer[0] == consumer_slot0);
    assert(counts.published_to_consumer[1] == consumer_slot1);
}

static void
assert_render_fields(const dsd_state* snap) {
    dsd_tg_policy_lookup lookup;
    assert(snap != NULL);
    assert(snap->lasttg == 321);
    assert(snap->trunk_chan_map[0x1234] == 769768750L);
    assert(snap->trunk_chan_map_used_count == 1U);
    assert(snap->trunk_chan_map_used[0] == 0x1234U);
    assert(dsd_tg_policy_lookup_id(snap, 321U, &lookup) == 0);
    assert(lookup.match == DSD_TG_POLICY_MATCH_EXACT);
    assert(strcmp(lookup.entry.name, "DISPATCH") == 0);
    assert(strcmp(snap->ui_msg, "snapshot ready") == 0);
    assert(snap->input_level.status == DSD_INPUT_LEVEL_CLIPPING);
    assert(snap->input_level.source == DSD_INPUT_LEVEL_SOURCE_RTL_CU8);
    assert(snap->input_level.sample_count == 2048U);
    assert(snap->input_level_last_toast_time == 1234);
    assert(snap->input_level_last_toast_status == DSD_INPUT_LEVEL_CLIPPING);
    assert(snap->input_level_last_toast_source == DSD_INPUT_LEVEL_SOURCE_RTL_CU8);
    assert(snap->p25_cc_prot_valid == 1U);
    assert(snap->p25_cc_prot_algid == 0x84U);
    assert(snap->p25_sys_time_valid == 1U);
    assert((int64_t)snap->p25_sys_time == 1770000000);
    assert(snap->p25_sys_time_offset_valid == 1U);
    assert(snap->p25_sys_time_offset == -300);
    assert(snap->p25_sys_services_valid == 1U);
    assert(snap->p25_sys_services_available == 0x00ABCDEFU);
    assert(snap->p25_sys_services_supported == 0x00123456U);
    assert(snap->p25_sys_services_request_priority == 7U);
    assert(snap->p25_site_lra_valid == 1U);
    assert(snap->p25_site_lra == 0x22U);
    assert(snap->p25_site_network_active_valid == 1U);
    assert(snap->p25_site_network_active == 1U);
    assert(snap->rkey_array[7] == 0ULL);
}

static void
assert_cc_candidates(const dsd_state* snap) {
    const dsd_trunk_cc_candidates* cc = dsd_trunk_cc_candidates_peek(snap);
    assert(cc != NULL);
    assert(cc->count == 2);
    assert(cc->idx == 1);
    assert(cc->candidates[0] == 851006250L);
    assert(cc->candidates[1] == 852006250L);
    assert(cc->cool_until[1] == 12.5);
    assert(cc->added == 2U);
    assert(cc->used == 3U);
}

int
main(void) {
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    Event_History_I* history = (Event_History_I*)calloc(2U, sizeof(*history));
    Event_History_I* replacement = (Event_History_I*)calloc(2U, sizeof(*replacement));
    if (!state || !history || !replacement) {
        DSD_FPRINTF(stderr, "allocation failed\n");
        free(replacement);
        free(history);
        free(state);
        return 1;
    }

    state->event_history_s = history;
    state->lasttg = 321;
    dsd_state_set_trunk_chan_freq(state, 0x1234U, 769768750L);

    dsd_call_observation observation = {0};
    observation.protocol = 35;
    observation.slot = 0U;
    observation.kind = DSD_CALL_KIND_GROUP_VOICE;
    observation.ota_target_id = 321U;
    observation.policy_target_id = 321U;
    observation.group_id = 321U;
    observation.source_id = 654U;
    observation.frequency_hz = 769768750L;
    observation.observed_m = 1.0;
    assert(dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_BEGIN) == 1);
    dsd_call_crypto_update crypto = {
        .classification = DSD_CALL_CRYPTO_DECRYPTABLE,
        .algid = 0x84U,
        .kid = 0x1234U,
        .mi = UINT64_C(0x1122334455667788),
        .audio_permitted = 1U,
        .observed_m = 1.1,
    };
    assert(dsd_call_state_update_crypto(state, 0U, &crypto) == 1);
    assert(dsd_recent_activity_set_at(state, 0U, "Active Ch: 1234 TG: 321; ", 1000U) == 1);

    dsd_tg_policy_entry entry;
    assert(dsd_tg_policy_make_exact_entry(321U, "A", "DISPATCH", DSD_TG_POLICY_SOURCE_IMPORTED, &entry) == 0);
    assert(dsd_tg_policy_append_exact(state, &entry) == 0);

    DSD_SNPRINTF(state->ui_msg, sizeof(state->ui_msg), "%s", "snapshot ready");
    state->input_level.status = DSD_INPUT_LEVEL_CLIPPING;
    state->input_level.source = DSD_INPUT_LEVEL_SOURCE_RTL_CU8;
    state->input_level.sample_count = 2048U;
    state->input_level_last_toast_time = 1234;
    state->input_level_last_toast_status = DSD_INPUT_LEVEL_CLIPPING;
    state->input_level_last_toast_source = DSD_INPUT_LEVEL_SOURCE_RTL_CU8;
    state->p25_cc_prot_valid = 1U;
    state->p25_cc_prot_algid = 0x84U;
    state->p25_sys_time_valid = 1U;
    state->p25_sys_time = 1770000000;
    state->p25_sys_time_offset_valid = 1U;
    state->p25_sys_time_offset = -300;
    state->p25_sys_services_valid = 1U;
    state->p25_sys_services_available = 0x00ABCDEFU;
    state->p25_sys_services_supported = 0x00123456U;
    state->p25_sys_services_request_priority = 7U;
    state->p25_site_lra_valid = 1U;
    state->p25_site_lra = 0x22U;
    state->p25_site_network_active_valid = 1U;
    state->p25_site_network_active = 1U;
    state->rkey_array[7] = 0x12345678ULL;

    assert(dsd_trunk_cc_candidates_add(state, 851006250L, 1, DSD_TRUNK_CC_CANDIDATE_CURRENT_SITE) == 1);
    assert(dsd_trunk_cc_candidates_add(state, 852006250L, 1, DSD_TRUNK_CC_CANDIDATE_CURRENT_SITE) == 1);
    dsd_trunk_cc_candidates* cc = dsd_trunk_cc_candidates_get(state);
    assert(cc != NULL);
    cc->idx = 1;
    cc->cool_until[1] = 12.5;
    cc->used = 3U;

    history[0].Event_History_Items[1].source_id = 123U;
    history[1].Event_History_Items[1].source_id = 456U;
    DSD_SNPRINTF(history[0].Event_History_Items[1].src_str, sizeof history[0].Event_History_Items[1].src_str, "%s",
                 "RADIO-123");
    mark_history(&history[0]);
    mark_history(&history[1]);

    dsd_app_snapshot_test_reset_event_history_copy_counts();
    dsd_app_telemetry_publish_snapshot(state);
    observation.source_id = 999U;
    observation.observed_m = 2.0;
    assert(dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_CONTINUE) == 1);
    crypto.classification = DSD_CALL_CRYPTO_ENCRYPTED;
    crypto.audio_permitted = 0U;
    crypto.observed_m = 2.1;
    assert(dsd_call_state_update_crypto(state, 0U, &crypto) == 1);
    assert(dsd_recent_activity_set_at(state, 0U, "mutated TG: 999; ", 2000U) == 1);
    history[0].Event_History_Items[1].source_id = 999U;
    DSD_SNPRINTF(history[0].Event_History_Items[1].src_str, sizeof history[0].Event_History_Items[1].src_str, "%s",
                 "MUTATED");
    cc->count = 1;
    cc->candidates[0] = 999999999L;
    cc->added = 99U;

    const dsd_state* snap = dsd_app_get_latest_snapshot();
    assert_slot_tail(snap, 123U, 456U);
    assert(strcmp(snap->event_history_s[0].Event_History_Items[1].src_str, "RADIO-123") == 0);
    assert_render_fields(snap);
    assert_cc_candidates(snap);
    dsd_call_snapshot call;
    assert(dsd_call_state_get(snap, 0U, &call) == 1);
    assert(call.source_id == 654U);
    assert(call.crypto == DSD_CALL_CRYPTO_DECRYPTABLE);
    assert(call.algid == 0x84U);
    dsd_recent_activity_snapshot recent;
    assert(dsd_recent_activity_copy_snapshot(snap, &recent) == 1);
    assert(strcmp(recent.entries[0].text, "Active Ch: 1234 TG: 321; ") == 0);
    assert_history_copy_counts(1U, 1U, 1U, 1U);

    // Repeated publications with unchanged revisions do not copy either history slot.
    dsd_app_snapshot_test_reset_event_history_copy_counts();
    dsd_app_telemetry_publish_snapshot(state);
    assert_slot_tail(dsd_app_get_latest_snapshot(), 123U, 456U);
    assert_history_copy_counts(0U, 0U, 0U, 0U);

    // A slot-0-only mutation copies only slot 0 at both snapshot stages.
    history[0].Event_History_Items[1].source_id = 789U;
    mark_history(&history[0]);
    dsd_app_snapshot_test_reset_event_history_copy_counts();
    dsd_app_telemetry_publish_snapshot(state);
    assert_slot_tail(dsd_app_get_latest_snapshot(), 789U, 456U);
    assert_history_copy_counts(1U, 0U, 1U, 0U);

    // Slot 1 advances independently from slot 0.
    history[1].Event_History_Items[1].source_id = 987U;
    mark_history(&history[1]);
    dsd_app_snapshot_test_reset_event_history_copy_counts();
    dsd_app_telemetry_publish_snapshot(state);
    assert_slot_tail(dsd_app_get_latest_snapshot(), 789U, 987U);
    assert_history_copy_counts(0U, 1U, 0U, 1U);

    // Resetting both histories publishes both cleared slots.
    DSD_MEMSET(history[0].Event_History_Items, 0, sizeof(history[0].Event_History_Items));
    DSD_MEMSET(history[1].Event_History_Items, 0, sizeof(history[1].Event_History_Items));
    mark_history(&history[0]);
    mark_history(&history[1]);
    dsd_app_snapshot_test_reset_event_history_copy_counts();
    dsd_app_telemetry_publish_snapshot(state);
    assert_slot_tail(dsd_app_get_latest_snapshot(), 0U, 0U);
    assert_history_copy_counts(1U, 1U, 1U, 1U);

    // Present-to-null does not copy stale backing storage.
    state->event_history_s = NULL;
    dsd_app_snapshot_test_reset_event_history_copy_counts();
    dsd_app_telemetry_publish_snapshot(state);
    assert(dsd_app_get_latest_snapshot()->event_history_s == NULL);
    assert_history_copy_counts(0U, 0U, 0U, 0U);

    // Null-to-present forces both slots, regardless of their last observed revisions.
    history[0].Event_History_Items[1].source_id = 111U;
    history[1].Event_History_Items[1].source_id = 222U;
    mark_history(&history[0]);
    mark_history(&history[1]);
    state->event_history_s = history;
    dsd_app_snapshot_test_reset_event_history_copy_counts();
    dsd_app_telemetry_publish_snapshot(state);
    assert_slot_tail(dsd_app_get_latest_snapshot(), 111U, 222U);
    assert_history_copy_counts(1U, 1U, 1U, 1U);

    // Replacing the source pointer forces both slots even when revisions coincide.
    replacement[0].revision = history[0].revision;
    replacement[1].revision = history[1].revision;
    replacement[0].Event_History_Items[1].source_id = 333U;
    replacement[1].Event_History_Items[1].source_id = 444U;
    state->event_history_s = replacement;
    dsd_app_snapshot_test_reset_event_history_copy_counts();
    dsd_app_telemetry_publish_snapshot(state);
    assert_slot_tail(dsd_app_get_latest_snapshot(), 333U, 444U);
    assert_history_copy_counts(1U, 1U, 1U, 1U);

    assert(dsd_tg_policy_make_exact_entry(7777U, "B", "POLICY-ONLY", DSD_TG_POLICY_SOURCE_IMPORTED, &entry) == 0);
    assert(dsd_tg_policy_append_exact(state, &entry) == 0);
    dsd_app_telemetry_publish_snapshot(state);
    snap = dsd_app_get_latest_snapshot();
    dsd_tg_policy_lookup lookup;
    assert(dsd_tg_policy_lookup_id(snap, 7777U, &lookup) == 0);
    assert(lookup.match == DSD_TG_POLICY_MATCH_EXACT);
    assert(strcmp(lookup.entry.name, "POLICY-ONLY") == 0);

    puts("UI_SNAPSHOT_EVENT_HISTORY: OK");
    dsd_state_ext_free_all(state);
    free(replacement);
    free(history);
    free(state);
    return 0;
}
