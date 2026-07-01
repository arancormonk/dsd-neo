// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <assert.h>
#include <dsd-neo/app_control/snapshot.h>
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

void ui_terminal_telemetry_publish_snapshot(const dsd_state* state);

static void
assert_slot_tail(const dsd_state* snap, uint32_t slot0_src, uint32_t slot1_src) {
    assert(snap != NULL);
    assert(snap->event_history_s != NULL);
    assert(snap->event_history_s[0].Event_History_Items[1].source_id == slot0_src);
    assert(snap->event_history_s[1].Event_History_Items[1].source_id == slot1_src);
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

static void
assert_frontend_snapshot_fields(const dsd_frontend_snapshot* snap) {
    assert(snap != NULL);
    assert(snap->has_state == 1);
    assert(snap->has_options == 0);
    assert(snap->status.p2_wacn == 0xbee00ULL);
    assert(snap->status.lasttg == 321U);
    assert(strcmp(snap->ui_message.text, "snapshot ready") == 0);
    assert(snap->ui_message.present == 1);
    assert(snap->ui_message.severity == DSD_FRONTEND_EVENT_SEVERITY_INFO);
    assert(snap->ui_message.category == DSD_FRONTEND_EVENT_CATEGORY_STATUS);
    assert(strcmp(snap->ui_message.source, "decoder") == 0);
    assert(snap->ui_message.slot == -1);
    assert(snap->ui_message.expire_unix_s == 4321);
    assert(snap->input_level.status == DSD_INPUT_LEVEL_CLIPPING);
    assert(snap->input_level.source == DSD_INPUT_LEVEL_SOURCE_RTL_CU8);
    assert(snap->input_level.sample_count == 2048U);
    assert(snap->input_level_last_toast_time == 1234);
    assert(snap->input_level_last_toast_status == DSD_INPUT_LEVEL_CLIPPING);
    assert(snap->input_level_last_toast_source == DSD_INPUT_LEVEL_SOURCE_RTL_CU8);

    assert(snap->slots[0].last_tg == 321U);
    assert(snap->slots[0].last_src == 654U);
    assert(snap->slots[0].payload_algid == 0x80U);
    assert(snap->slots[0].payload_keyid == 0x1234U);
    assert(snap->slots[0].audio_allowed == 1);
    assert(snap->slots[0].active_call == 1);
    assert(strcmp(snap->slots[0].call_string, "slot zero active") == 0);
    assert(snap->slots[1].last_tg == 987U);
    assert(snap->slots[1].last_src == 765U);
    assert(snap->slots[1].payload_algid == 0x81U);
    assert(snap->slots[1].payload_keyid == 0x5678U);
    assert(snap->slots[1].active_call == 1);
    assert(strcmp(snap->slots[1].call_string, "slot one active") == 0);

    assert(snap->p25.p2_wacn == 0xbee00ULL);
    assert(snap->p25.p2_sysid == 0x123ULL);
    assert(snap->p25.p2_cc == 0x456ULL);
    assert(snap->p25.trunk_cc_freq == 769006250L);
    assert(snap->p25.trunk_vc_freq == 769506250L);
    assert(snap->p25.p25_cc_freq == 851006250L);
    assert(snap->p25.p25_vc_freq == 851506250L);
    assert(snap->p25.p25_cc_is_tdma == 1);
    assert(snap->p25.p25_p2_active_slot == 1);
    assert(snap->p25.p25_p2_audio_ring_count[0] == 3);
    assert(snap->p25.p25_p2_audio_ring_count[1] == 5);
    assert(snap->p25.p25_p1_fec_ok == 11U);
    assert(snap->p25.p25_p1_fec_err == 2U);
    assert(snap->p25.p25_p2_facch_ok == 13U);
    assert(snap->p25.p25_p2_facch_err == 4U);
    assert(snap->p25.p25_p2_sacch_ok == 17U);
    assert(snap->p25.p25_p2_sacch_err == 6U);
    assert(snap->p25.p25_p2_voice_err == 15U);

    assert(snap->trunk_channel_count == 1U);
    assert(snap->trunk_channels[0].channel == 0x1234U);
    assert(snap->trunk_channels[0].freq_hz == 769768750L);
    assert(snap->trunk_channel_sequence != 0U);
    assert(snap->trunk_cc_candidates.count == 2);
    assert(snap->trunk_cc_candidates.index == 1);
    assert(snap->trunk_cc_candidates.candidates[0].freq_hz == 851006250L);
    assert(snap->trunk_cc_candidates.candidates[1].freq_hz == 852006250L);
    assert(snap->trunk_cc_candidates.candidates[1].cool_until_monotonic_s == 12.5);
    assert(snap->trunk_cc_candidates.added == 2U);
    assert(snap->trunk_cc_candidates.used == 3U);

    assert(snap->event_history_present == 1);
    assert(snap->event_history_sequence != 0U);
    assert(snap->event_history_slot_count == DSD_FRONTEND_EVENT_HISTORY_SLOTS);
    assert(snap->event_history_items_per_slot == DSD_FRONTEND_EVENT_HISTORY_ITEMS);
    _Static_assert(sizeof(dsd_frontend_snapshot) < ((size_t)128U * 1024U), "frontend snapshot must stay cheap to poll");
}

static void
assert_frontend_history_api(uint64_t sequence) {
    dsd_frontend_event_history_summary summaries[2];
    dsd_frontend_event_history_page_info info;
    dsd_frontend_event_history_query query;
    DSD_MEMSET(summaries, 0, sizeof summaries);
    DSD_MEMSET(&info, 0, sizeof info);
    DSD_MEMSET(&query, 0, sizeof query);
    query.slot = 0;
    query.offset = 1;
    query.limit = 2;
    assert(dsd_app_frontend_event_history_page_get(&query, summaries, 2U, &info) == 0);
    assert(info.present == 1);
    assert(info.unchanged == 0);
    assert(info.sequence == sequence);
    assert(info.total_items == DSD_FRONTEND_EVENT_HISTORY_ITEMS);
    assert(info.returned_items == 2U);
    assert(summaries[0].present == 1);
    assert(summaries[0].slot == 0U);
    assert(summaries[0].severity == DSD_FRONTEND_EVENT_SEVERITY_WARNING);
    assert(summaries[0].category == DSD_FRONTEND_EVENT_CATEGORY_CONTROL);
    assert(summaries[0].protocol == DSD_FRONTEND_PROTOCOL_P25);
    assert(summaries[0].encryption_state == DSD_FRONTEND_ENCRYPTION_ENCRYPTED);
    assert(summaries[0].source_id == 123U);
    assert(summaries[0].target_id == 321U);
    assert(summaries[0].channel == 0x1234U);
    assert(summaries[0].timestamp_unix_s == 1770001234);
    assert(strcmp(summaries[0].source_text, "RADIO-123") == 0);
    assert(strcmp(summaries[0].target_text, "TG-321") == 0);
    assert(strcmp(summaries[0].source_label, "Unit 123") == 0);
    assert(strcmp(summaries[0].target_label, "Dispatch") == 0);
    assert(strcmp(summaries[0].system_label, "WACN BEE00 SYS 123") == 0);
    assert(strcmp(summaries[0].summary_text, "voice grant") == 0);
    assert(strcmp(summaries[0].detail_text, "grant detail") == 0);

    query.known_sequence = sequence;
    DSD_MEMSET(&info, 0, sizeof info);
    DSD_MEMSET(summaries, 0, sizeof summaries);
    assert(dsd_app_frontend_event_history_page_get(&query, summaries, 2U, &info) == 0);
    assert(info.unchanged == 1);
    assert(info.returned_items == 0U);

    dsd_frontend_event_history_item detail;
    uint64_t detail_sequence = 0;
    DSD_MEMSET(&detail, 0, sizeof detail);
    assert(dsd_app_frontend_event_history_item_get(0U, 1U, &detail, &detail_sequence) == 0);
    assert(detail_sequence == sequence);
    assert(detail.present == 1);
    assert(detail.slot == 0U);
    assert(detail.subtype == 7);
    assert(detail.encryption_alg_id == 0x80U);
    assert(detail.encryption_key_id == 0x1234U);
    assert(detail.encryption_message_indicator == 0x456789ULL);
    assert(detail.service_options == 0x20U);
    assert(detail.group_individual == 1);
    assert(detail.system_id[0] == 0xbee00U);
    assert(detail.system_id[1] == 0x123U);
    assert(detail.system_id[2] == 0x456U);
    assert(detail.channel == 0x1234U);
    assert(detail.timestamp_unix_s == 1770001234);
    assert(strcmp(detail.source_text, "RADIO-123") == 0);
    assert(strcmp(detail.target_text, "TG-321") == 0);
    assert(strcmp(detail.source_label, "Unit 123") == 0);
    assert(strcmp(detail.target_label, "Dispatch") == 0);
    assert(strcmp(detail.source_mode, "A") == 0);
    assert(strcmp(detail.target_mode, "D") == 0);
    assert(strcmp(detail.system_label, "WACN BEE00 SYS 123") == 0);
    assert(strcmp(detail.summary_text, "voice grant") == 0);
    assert(strcmp(detail.detail_text, "grant detail") == 0);
    assert(strcmp(detail.gps_text, "GPS detail") == 0);
    assert(strcmp(detail.text_message, "Text detail") == 0);
    assert(strcmp(detail.alias, "Alias detail") == 0);
    assert(detail.pdu_len == 4U);
    assert(detail.pdu[0] == 0x12U);
    assert(detail.pdu[3] == 0x34U);
    assert(dsd_app_frontend_event_history_item_get(1U, 1U, &detail, NULL) == 0);
    assert(detail.source_id == 456U);
}

int
main(void) {
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    Event_History_I* history = (Event_History_I*)calloc(2u, sizeof(*history));
    if (!state || !history) {
        DSD_FPRINTF(stderr, "allocation failed\n");
        free(history);
        free(state);
        return 1;
    }
    state->event_history_s = history;
    state->lasttg = 321;
    state->lastsrc = 654;
    state->lasttgR = 987;
    state->lastsrcR = 765;
    state->payload_algid = 0x80;
    state->payload_keyid = 0x1234;
    state->payload_algidR = 0x81;
    state->payload_keyidR = 0x5678;
    DSD_SNPRINTF(state->call_string[0], sizeof state->call_string[0], "%s", "slot zero active");
    DSD_SNPRINTF(state->call_string[1], sizeof state->call_string[1], "%s", "slot one active");
    state->p2_wacn = 0xbee00U;
    state->p2_sysid = 0x123U;
    state->p2_cc = 0x456U;
    state->trunk_cc_freq = 769006250L;
    state->trunk_vc_freq[0] = 769506250L;
    state->p25_cc_freq = 851006250L;
    state->p25_vc_freq[0] = 851506250L;
    state->p25_cc_is_tdma = 1;
    state->p25_p2_active_slot = 1;
    state->p25_p2_audio_ring_count[0] = 3;
    state->p25_p2_audio_ring_count[1] = 5;
    state->p25_p2_audio_allowed[0] = 1;
    state->p25_p1_fec_ok = 11U;
    state->p25_p1_fec_err = 2U;
    state->p25_p2_rs_facch_ok = 13U;
    state->p25_p2_rs_facch_err = 4U;
    state->p25_p2_rs_sacch_ok = 17U;
    state->p25_p2_rs_sacch_err = 6U;
    state->p25_p2_voice_err_hist_sum[0] = 7U;
    state->p25_p2_voice_err_hist_sum[1] = 8U;
    dsd_state_set_trunk_chan_freq(state, 0x1234U, 769768750L);
    dsd_tg_policy_entry entry;
    assert(dsd_tg_policy_make_exact_entry(321U, "A", "DISPATCH", DSD_TG_POLICY_SOURCE_IMPORTED, &entry) == 0);
    assert(dsd_tg_policy_append_exact(state, &entry) == 0);
    DSD_SNPRINTF(state->ui_msg, sizeof(state->ui_msg), "%s", "snapshot ready");
    state->ui_msg_expire = 4321;
    state->input_level.status = DSD_INPUT_LEVEL_CLIPPING;
    state->input_level.source = DSD_INPUT_LEVEL_SOURCE_RTL_CU8;
    state->input_level.rms_dbfs = -12.5;
    state->input_level.peak_dbfs = -0.2;
    state->input_level.clip_pct = 0.5;
    state->input_level.sample_count = 2048U;
    state->input_level.updated = 1200;
    state->input_level_last_toast_time = 1234;
    state->input_level_last_toast_status = DSD_INPUT_LEVEL_CLIPPING;
    state->input_level_last_toast_source = DSD_INPUT_LEVEL_SOURCE_RTL_CU8;
    state->rkey_array[7] = 0x12345678ULL;
    assert(dsd_trunk_cc_candidates_add(state, 851006250L, 1) == 1);
    assert(dsd_trunk_cc_candidates_add(state, 852006250L, 1) == 1);
    dsd_trunk_cc_candidates* cc = dsd_trunk_cc_candidates_get(state);
    assert(cc != NULL);
    cc->idx = 1;
    cc->cool_until[1] = 12.5;
    cc->used = 3U;

    Event_History* hist0_item = &history[0].Event_History_Items[1];
    hist0_item->source_id = 123U;
    hist0_item->target_id = 321U;
    hist0_item->systype = 0;
    hist0_item->subtype = 7;
    hist0_item->gi = 1;
    hist0_item->enc = 1;
    hist0_item->enc_alg = 0x80U;
    hist0_item->enc_key = 0x1234U;
    hist0_item->mi = 0x456789ULL;
    hist0_item->svc = 0x20U;
    hist0_item->sys_id1 = 0xbee00U;
    hist0_item->sys_id2 = 0x123U;
    hist0_item->sys_id3 = 0x456U;
    hist0_item->channel = 0x1234U;
    hist0_item->event_time = 1770001234;
    hist0_item->pdu[0] = 0x12U;
    hist0_item->pdu[3] = 0x34U;
    dsd_event_history_item_set_metadata(hist0_item, DSD_EVENT_SEVERITY_WARNING, DSD_EVENT_CATEGORY_CONTROL);
    DSD_SNPRINTF(hist0_item->src_str, sizeof hist0_item->src_str, "%s", "RADIO-123");
    DSD_SNPRINTF(hist0_item->tgt_str, sizeof hist0_item->tgt_str, "%s", "TG-321");
    DSD_SNPRINTF(hist0_item->s_name, sizeof hist0_item->s_name, "%s", "Unit 123");
    DSD_SNPRINTF(hist0_item->t_name, sizeof hist0_item->t_name, "%s", "Dispatch");
    DSD_SNPRINTF(hist0_item->s_mode, sizeof hist0_item->s_mode, "%s", "A");
    DSD_SNPRINTF(hist0_item->t_mode, sizeof hist0_item->t_mode, "%s", "D");
    DSD_SNPRINTF(hist0_item->sysid_string, sizeof hist0_item->sysid_string, "%s", "WACN BEE00 SYS 123");
    DSD_SNPRINTF(hist0_item->event_string, sizeof hist0_item->event_string, "%s", "voice grant");
    DSD_SNPRINTF(hist0_item->internal_str, sizeof hist0_item->internal_str, "%s", "grant detail");
    DSD_SNPRINTF(hist0_item->gps_s, sizeof hist0_item->gps_s, "%s", "GPS detail");
    DSD_SNPRINTF(hist0_item->text_message, sizeof hist0_item->text_message, "%s", "Text detail");
    DSD_SNPRINTF(hist0_item->alias, sizeof hist0_item->alias, "%s", "Alias detail");
    history[1].Event_History_Items[1].source_id = 456U;
    ui_terminal_telemetry_publish_snapshot(state);
    history[0].Event_History_Items[1].source_id = 999U;
    DSD_SNPRINTF(history[0].Event_History_Items[1].src_str, sizeof history[0].Event_History_Items[1].src_str, "%s",
                 "MUTATED");
    cc->count = 1;
    cc->candidates[0] = 999999999L;
    cc->added = 99U;
    const dsd_state* snap = dsd_app_get_latest_snapshot();
    assert_slot_tail(snap, 123U, 456U);
    assert_render_fields(snap);
    assert_cc_candidates(snap);
    dsd_frontend_snapshot* frontend_snap = (dsd_frontend_snapshot*)calloc(1, sizeof(*frontend_snap));
    assert(frontend_snap != NULL);
    assert(dsd_app_frontend_snapshot_get(frontend_snap) == 0);
    const uint64_t initial_event_history_sequence = frontend_snap->event_history_sequence;
    assert_frontend_snapshot_fields(frontend_snap);
    assert_frontend_history_api(frontend_snap->event_history_sequence);
    free(frontend_snap);

    // Update only non-head rows; this must still refresh the snapshot copy.
    history[0].Event_History_Items[1].source_id = 789U;
    history[1].Event_History_Items[1].source_id = 987U;
    ui_terminal_telemetry_publish_snapshot(state);
    assert_slot_tail(dsd_app_get_latest_snapshot(), 789U, 987U);
    dsd_frontend_event_history_item updated_detail;
    uint64_t updated_sequence = 0;
    assert(dsd_app_frontend_event_history_item_get(0U, 1U, &updated_detail, &updated_sequence) == 0);
    assert(updated_detail.source_id == 789U);
    assert(updated_sequence != initial_event_history_sequence);

    // Reset-like clear with unchanged head rows must also be reflected.
    DSD_MEMSET(history, 0, 2u * sizeof(*history));
    ui_terminal_telemetry_publish_snapshot(state);
    assert_slot_tail(dsd_app_get_latest_snapshot(), 0U, 0U);
    assert(dsd_app_frontend_event_history_item_get(0U, 1U, &updated_detail, NULL) == 0);
    assert(updated_detail.present == 0U);

    assert(dsd_tg_policy_make_exact_entry(7777U, "B", "POLICY-ONLY", DSD_TG_POLICY_SOURCE_IMPORTED, &entry) == 0);
    assert(dsd_tg_policy_append_exact(state, &entry) == 0);
    ui_terminal_telemetry_publish_snapshot(state);
    snap = dsd_app_get_latest_snapshot();
    dsd_tg_policy_lookup lookup;
    assert(dsd_tg_policy_lookup_id(snap, 7777U, &lookup) == 0);
    assert(lookup.match == DSD_TG_POLICY_MATCH_EXACT);
    assert(strcmp(lookup.entry.name, "POLICY-ONLY") == 0);

    printf("UI_SNAPSHOT_EVENT_HISTORY: OK\n");
    dsd_state_ext_free_all(state);
    free(history);
    free(state);
    return 0;
}
