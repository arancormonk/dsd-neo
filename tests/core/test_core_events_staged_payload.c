// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */

#include <assert.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

void
// NOLINTNEXTLINE(bugprone-reserved-identifier,misc-use-internal-linkage)
__wrap_beeper(dsd_opts* opts, dsd_state* state, int lr, int id, int ad, int len) {
    (void)opts;
    (void)state;
    (void)lr;
    (void)id;
    (void)ad;
    (void)len;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif

static dsd_opts opts;
static dsd_state state;
static Event_History_I history[2];

static void
reset_fixture(void) {
    dsd_state_ext_free_all(&state);
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&state, 0, sizeof state);
    DSD_MEMSET(history, 0, sizeof history);
    state.event_history_s = history;
    state.lastsynctype = DSD_SYNC_DMR_BS_VOICE_POS;
    init_event_history(&history[0], 0, DSD_EVENT_HISTORY_LEN);
    init_event_history(&history[1], 0, DSD_EVENT_HISTORY_LEN);
}

static void
seed_enriched_call(void) {
    const dsd_call_observation observation = {
        .protocol = DSD_SYNC_DMR_BS_VOICE_POS,
        .slot = 0,
        .kind = DSD_CALL_KIND_GROUP_VOICE,
        .ota_source_id = 111,
        .ota_target_id = 1201,
        .policy_target_id = 1201,
        .observed_m = 1.0,
    };
    assert(dsd_call_state_observe(&state, &observation, DSD_CALL_BOUNDARY_BEGIN) == 1);
    dsd_event_sync_slot(&opts, &state, 0);
    dsd_call_snapshot call;
    assert(dsd_call_state_get(&state, 0, &call) == 1);
    assert(dsd_event_enrich_gps(&state, 0, call.epoch, "GPS A") == 1);
    assert(dsd_event_enrich_text(&state, 0, call.epoch, "text A") == 1);
}

static int
expect_text(const char* tag, const char* got, const char* expected) {
    if (strcmp(got, expected) != 0) {
        DSD_FPRINTF(stderr, "%s: got '%s', want '%s'\n", tag, got, expected);
        return 1;
    }
    return 0;
}

static void
emit_notice(void) {
    const dsd_call_observation observation = dsd_call_observation_data(DSD_SYNC_DMR_BS_DATA_POS, 0, 222, 1201);
    assert(dsd_event_emit_data_notice(&opts, &state, 0, &observation, "PDU from 222;") == 0);
}

static int
test_notice_does_not_consume_call(void) {
    reset_fixture();
    seed_enriched_call();
    emit_notice();
    int failed = 0;
    failed |= expect_text("T10b notice GPS", history[0].Event_History_Items[1].gps_s, "");
    failed |= expect_text("T10b notice text", history[0].Event_History_Items[1].text_message, "");
    failed |= expect_text("T10b active GPS", history[0].Event_History_Items[0].gps_s, "GPS A");
    failed |= expect_text("T10b active text", history[0].Event_History_Items[0].text_message, "text A");
    return failed;
}

static void
stage_packet(void) {
    dsd_event_stage_text(&state, 0, "text B");
    dsd_event_stage_gps(&state, 0, "GPS B");
}

static int
expect_staging(const char* gps, const char* text) {
    int failed = expect_text("staged GPS", dsd_event_staged_gps(&state, 0), gps);
    return failed | expect_text("staged text", dsd_event_staged_text(&state, 0), text);
}

static int
expect_call_payload(void) {
    int failed = expect_text("active GPS", history[0].Event_History_Items[0].gps_s, "GPS A");
    return failed | expect_text("active text", history[0].Event_History_Items[0].text_message, "text A");
}

static int
expect_packet_notice(void) {
    int failed = expect_text("notice GPS", history[0].Event_History_Items[1].gps_s, "GPS B");
    return failed | expect_text("notice text", history[0].Event_History_Items[1].text_message, "text B");
}

static int
test_staged_notice(void) {
    reset_fixture();
    DSD_SNPRINTF(history[0].Event_History_Items[0].alias, sizeof history[0].Event_History_Items[0].alias, "%s",
                 "retained alias");
    Event_History before;
    DSD_MEMCPY(&before, &history[0].Event_History_Items[0], sizeof before);
    stage_packet();
    emit_notice();
    int failed = expect_packet_notice() | expect_staging("", "");
    // Verify the active row's payload and metadata remain unchanged without comparing padding.
    const Event_History* after = &history[0].Event_History_Items[0];
    assert(strcmp(before.alias, after->alias) == 0);
    assert(strcmp(before.gps_s, after->gps_s) == 0);
    assert(strcmp(before.text_message, after->text_message) == 0);
    assert(strcmp(before.event_string, after->event_string) == 0);
    assert(strcmp(before.internal_str, after->internal_str) == 0);
    assert(memcmp(before.pdu, after->pdu, sizeof before.pdu) == 0);
    assert(before.source_id == after->source_id);
    assert(before.target_id == after->target_id);
    assert(before.write == after->write);
    assert(before.severity == after->severity);
    assert(before.category == after->category);
    assert(before.crc_invalid == after->crc_invalid);
    return failed;
}

static int
test_staged_and_enriched(void) {
    reset_fixture();
    seed_enriched_call();
    stage_packet();
    emit_notice();
    return expect_packet_notice() | expect_staging("", "") | expect_call_payload();
}

static int
test_explicit_gps_preserves_staging(void) {
    reset_fixture();
    seed_enriched_call();
    stage_packet();
    const dsd_call_observation observation = dsd_call_observation_data(DSD_SYNC_DMR_BS_DATA_POS, 0, 333, 1201);
    assert(dsd_event_emit_data_notice_with_gps(&opts, &state, 0, &observation, "Explicit GPS;", "GPS C") == 0);
    int failed = expect_staging("GPS B", "text B") | expect_call_payload();
    failed |= expect_text("explicit notice GPS", history[0].Event_History_Items[1].gps_s, "GPS C");
    failed |= expect_text("explicit notice text", history[0].Event_History_Items[1].text_message, "");
    emit_notice();
    return failed | expect_packet_notice() | expect_staging("", "") | expect_call_payload();
}

static int
test_staging_guards_and_reset(void) {
    reset_fixture();
    stage_packet();
    dsd_event_stage_text(&state, 1, "slot 1");
    dsd_event_stage_gps(&state, 1, "GPS 1");
    dsd_event_history_reset(&state);
    int failed = expect_staging("", "");
    failed |= expect_text("reset slot-1 text", dsd_event_staged_text(&state, 1), "");
    failed |= expect_text("reset slot-1 GPS", dsd_event_staged_gps(&state, 1), "");
    stage_packet();
    dsd_event_stage_text(&state, 0, NULL);
    dsd_event_stage_gps(&state, 0, NULL);
    failed |= expect_staging("", "");
    stage_packet();
    dsd_event_stage_clear(&state, 0);
    failed |= expect_staging("", "");
    stage_packet();
    Event_History_I* saved = state.event_history_s;
    for (int i = 0; i < 3; ++i) {
        dsd_state* target = i == 0 ? NULL : &state;
        uint8_t slot = i == 2 ? 2U : 0U;
        state.event_history_s = i == 1 ? NULL : saved;
        dsd_event_stage_text(target, slot, "ignored");
        dsd_event_stage_text_append(target, slot, "ignored");
        dsd_event_stage_gps(target, slot, "ignored");
        dsd_event_stage_clear(target, slot);
        failed |= expect_text("invalid state/slot text", dsd_event_staged_text(target, slot), "");
        failed |= expect_text("invalid state/slot GPS", dsd_event_staged_gps(target, slot), "");
    }
    state.event_history_s = saved;
    return failed | expect_staging("GPS B", "text B");
}

static int
test_call_commit_keeps_packet_staged(void) {
    reset_fixture();
    seed_enriched_call();
    stage_packet();
    assert(dsd_call_state_end(&state, 0, 2.0) == 1);
    dsd_event_sync_slot(&opts, &state, 0);
    assert(history[0].Event_History_Items[1].source_id == 111);
    int failed = expect_text("T10f committed call GPS", history[0].Event_History_Items[1].gps_s, "GPS A");
    failed |= expect_text("T10f committed call text", history[0].Event_History_Items[1].text_message, "text A");
    failed |= expect_staging("GPS B", "text B");
    emit_notice();
    return failed | expect_packet_notice() | expect_staging("", "");
}

static int
test_append_isolation_and_counters(void) {
    reset_fixture();
    assert(dsd_call_state_ensure(&state) >= 0);
    uint64_t revision[2], push_seq[2], commit_rev[2];
    for (int slot = 0; slot < 2; ++slot) {
        revision[slot] = history[slot].revision;
        push_seq[slot] = history[slot].push_seq;
        commit_rev[slot] = history[slot].commit_rev;
    }
    dsd_event_stage_text(&state, 1, "A");
    dsd_event_stage_text_append(&state, 1, "B");
    dsd_event_stage_text_append(&state, 1, "C");
    dsd_event_stage_text_append(&state, 1, NULL);
    dsd_event_stage_gps(&state, 1, "GPS 1");
    int failed = expect_staging("", "");
    failed |= expect_text("T10g appended text", dsd_event_staged_text(&state, 1), "ABC");
    failed |= expect_text("T10g isolated GPS", dsd_event_staged_gps(&state, 1), "GPS 1");
    dsd_event_stage_clear(&state, 1);
    for (int slot = 0; slot < 2; ++slot) {
        assert(history[slot].revision == revision[slot]);
        assert(history[slot].push_seq == push_seq[slot]);
        assert(history[slot].commit_rev == commit_rev[slot]);
    }
    return failed;
}

static void
test_raw_pdu_not_consumed(void) {
    reset_fixture();
    DSD_MEMSET(history[0].Event_History_Items[0].pdu, 0xAB, sizeof history[0].Event_History_Items[0].pdu);
    emit_notice();
    for (size_t i = 0; i < sizeof history[0].Event_History_Items[0].pdu; ++i) {
        assert(history[0].Event_History_Items[1].pdu[i] == 0);
        assert(history[0].Event_History_Items[0].pdu[i] == 0xAB);
    }
}

int
main(void) {
    int failed = test_notice_does_not_consume_call();
    failed |= test_staged_notice();
    failed |= test_staged_and_enriched();
    failed |= test_explicit_gps_preserves_staging();
    failed |= test_staging_guards_and_reset();
    failed |= test_call_commit_keeps_packet_staged();
    failed |= test_append_isolation_and_counters();
    test_raw_pdu_not_consumed();
    dsd_state_ext_free_all(&state);
    return failed;
}
