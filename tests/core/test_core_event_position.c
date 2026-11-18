// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */

/*
 * History rows carry the admitted position (issue #800): the position writers (active call by epoch, the captured
 * closing call, the open call, the staged and the explicit data notice) set the row's GPS text and position in one step
 * under the typed-source rule; push, staging, the snapshot copy and the reacquisition merge keep them together, and the
 * newest rev wins whatever the arrival order or the wall clock says. All coordinates and IDs are made up.
 */

#include <assert.h>
#include <dsd-neo/core/access_code.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/crc_provenance.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/position.h>
#include <dsd-neo/core/received_identity.h>
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
__wrap_beeper(dsd_opts* opts, dsd_state* state, int lr, int id, int len) {
    (void)opts;
    (void)state;
    (void)lr;
    (void)id;
    (void)len;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif

static dsd_opts g_opts;
static dsd_state g_state;
static Event_History_I history[2];
static double g_observed_m = 1.0;

static void
reset_fixture(void) {
    dsd_state_ext_free_all(&g_state);
    DSD_MEMSET(&g_opts, 0, sizeof g_opts);
    DSD_MEMSET(&g_state, 0, sizeof g_state);
    DSD_MEMSET(history, 0, sizeof history);
    g_state.event_history_s = history;
    init_event_history(&history[0], 0, DSD_EVENT_HISTORY_LEN);
    init_event_history(&history[1], 0, DSD_EVENT_HISTORY_LEN);
    g_observed_m = 1.0;
}

static uint64_t
observe(int protocol, uint64_t source_id, const char* source_text, dsd_call_boundary boundary) {
    dsd_call_observation observation = {
        .protocol = protocol,
        .slot = 0U,
        .kind = DSD_CALL_KIND_GROUP_VOICE,
        .ota_target_id = 1201U,
        .policy_target_id = 1201U,
        .ota_source_id = source_id,
        .observed_m = g_observed_m,
    };
    if (source_text != NULL) {
        DSD_SNPRINTF(observation.source_text, sizeof observation.source_text, "%s", source_text);
    }
    g_observed_m += 0.1;
    assert(dsd_call_state_observe(&g_state, &observation, boundary) == 1);
    dsd_event_sync_slot(&g_opts, &g_state, 0U);
    dsd_call_snapshot call;
    assert(dsd_call_state_get(&g_state, 0U, &call) == 1);
    return call.epoch;
}

static void
end_call(dsd_call_end_reason reason) {
    assert(dsd_call_state_end_ex(&g_state, 0U, g_observed_m, reason) == 1);
    g_observed_m += 0.1;
    dsd_event_sync_slot(&g_opts, &g_state, 0U);
}

/* An admitted report from made-up radio @p source_id at the given made-up coordinates. */
static dsd_position_fix
admitted(int protocol, uint32_t source_id, int32_t lat_e7, int32_t lon_e7) {
    dsd_position_fix f;
    dsd_position_fix_init(&f);
    f.kind = DSD_SYNC_IS_DSTAR(protocol) ? DSD_POS_DSTAR_DPRS : DSD_POS_DMR_EMBEDDED;
    f.protocol = (uint8_t)protocol;
    f.lat_e7 = lat_e7;
    f.lon_e7 = lon_e7;
    f.err_m = 10U;
    f.source_id = source_id;
    assert(dsd_position_admit(&g_opts, &g_state, &f, 1, 1, NULL) != DSD_POSITION_REFUSED);
    assert(f.rev != 0U);
    return f;
}

static const Event_History*
row(int index) {
    return &history[0].Event_History_Items[index];
}

static void
expect_row_fix(const Event_History* item, const dsd_position_fix* f, const char* text) {
    assert(strcmp(item->gps_s, text) == 0);
    assert(item->pos.valid == 1U);
    assert(item->pos.rev == f->rev && item->pos.proc_instance == f->proc_instance);
    assert(item->pos.lat_e7 == f->lat_e7 && item->pos.lon_e7 == f->lon_e7 && item->pos.err_m == f->err_m);
    assert(item->pos.flags == f->flags && item->pos.kind == f->kind && item->pos.rx_time_s == f->rx_time_s);
    assert(item->pos.scope.kind == f->sender_scope.kind && item->pos.scope.a == f->sender_scope.a);
}

/* The active call's row takes text and position together; both survive the commit push and the snapshot copy. */
static void
test_active_call_push_and_snapshot(void) {
    reset_fixture();
    const uint64_t epoch = observe(DSD_SYNC_DMR_BS_VOICE_POS, 111U, NULL, DSD_CALL_BOUNDARY_BEGIN);
    const dsd_position_fix f = admitted(DSD_SYNC_DMR_BS_VOICE_POS, 111U, 225000000, 450000000);
    assert(dsd_event_enrich_position(NULL, &g_state, 0U, epoch, &f, "GPS 22.5 45.0") == 1);
    expect_row_fix(row(0), &f, "GPS 22.5 45.0");
    end_call(DSD_CALL_END_TERMINATOR);
    assert(row(1)->source_id == 111U);
    expect_row_fix(row(1), &f, "GPS 22.5 45.0");
    assert(row(0)->pos.valid == 0U && row(0)->gps_s[0] == '\0');

    static Event_History_I snap[2];
    assert(dsd_event_history_copy_snapshot(&g_state, snap) == 1);
    expect_row_fix(&snap[0].Event_History_Items[1], &f, "GPS 22.5 45.0");

    /* An ended call takes no position from the active-call writer; a report that admission never stamped is no
       input. */
    assert(dsd_event_enrich_position(NULL, &g_state, 0U, epoch, &f, "late") == 0);
    dsd_position_fix raw = f;
    raw.rev = 0U;
    assert(dsd_event_enrich_position(NULL, &g_state, 0U, epoch, &raw, "raw") == -1);
    assert(dsd_event_enrich_position(NULL, &g_state, 0U, epoch, &f, NULL) == -1);
    expect_row_fix(row(1), &f, "GPS 22.5 45.0");
}

/* The typed-source rule: protocol family, numeric source, and anonymous reports only on calls naming no source. */
static void
test_typed_source_rule(void) {
    reset_fixture();
    const uint64_t epoch = observe(DSD_SYNC_DMR_BS_VOICE_POS, 111U, NULL, DSD_CALL_BOUNDARY_BEGIN);
    const dsd_position_fix other = admitted(DSD_SYNC_DMR_BS_VOICE_POS, 222U, 225000000, 450000000);
    assert(dsd_event_enrich_position(NULL, &g_state, 0U, epoch, &other, "other radio") == 0);
    dsd_position_fix anon;
    dsd_position_fix_init(&anon);
    anon.kind = DSD_POS_DMR_EMBEDDED;
    anon.protocol = DSD_SYNC_DMR_BS_VOICE_POS;
    anon.source_id = 111U;
    anon.lat_e7 = 100000000;
    anon.lon_e7 = 200000000;
    assert(dsd_position_admit(&g_opts, &g_state, &anon, 1, 0, NULL) != DSD_POSITION_REFUSED);
    assert((anon.flags & DSD_POS_FLAG_SOURCE_UNVERIFIED) != 0U && anon.source_id == 0U);
    assert(dsd_event_enrich_position(NULL, &g_state, 0U, epoch, &anon, "unverified") == 0);
    const dsd_position_fix p25 = admitted(DSD_SYNC_P25P1_POS, 111U, 225000000, 450000000);
    assert(dsd_event_enrich_position(NULL, &g_state, 0U, epoch, &p25, "another protocol") == 0);
    assert(row(0)->pos.valid == 0U && row(0)->gps_s[0] == '\0');
    /* DMR base and mobile bursts are one family. */
    const dsd_position_fix ms = admitted(DSD_SYNC_DMR_MS_VOICE, 111U, 225000000, 450000000);
    assert(dsd_event_enrich_position(NULL, &g_state, 0U, epoch, &ms, "same radio") == 1);

    /* A call that names no source takes an anonymous report. */
    reset_fixture();
    const uint64_t nameless = observe(DSD_SYNC_DMR_BS_VOICE_POS, 0U, NULL, DSD_CALL_BOUNDARY_BEGIN);
    anon.rev = 0U;
    anon.source_id = 111U;
    anon.flags = 0U;
    assert(dsd_position_admit(&g_opts, &g_state, &anon, 1, 0, NULL) != DSD_POSITION_REFUSED);
    assert(dsd_event_enrich_position(NULL, &g_state, 0U, nameless, &anon, "anonymous") == 1);
    expect_row_fix(row(0), &anon, "anonymous");
}

/* The newest rev wins in either arrival order, within one second and across a backward wall clock step. */
static void
test_newest_rev_wins(void) {
    reset_fixture();
    const uint64_t epoch = observe(DSD_SYNC_DMR_BS_VOICE_POS, 111U, NULL, DSD_CALL_BOUNDARY_BEGIN);
    const dsd_position_fix older = admitted(DSD_SYNC_DMR_BS_VOICE_POS, 111U, 225000000, 450000000);
    dsd_position_fix newer = admitted(DSD_SYNC_DMR_BS_VOICE_POS, 111U, 100000000, 200000000);
    assert(newer.rev > older.rev);
    newer.rx_time_s = older.rx_time_s - 3600; /* the wall clock stepped back an hour between the two */
    assert(dsd_event_enrich_position(NULL, &g_state, 0U, epoch, &newer, "newer") == 1);
    assert(dsd_event_enrich_position(NULL, &g_state, 0U, epoch, &older, "older") == 0);
    expect_row_fix(row(0), &newer, "newer");
    assert(dsd_event_enrich_position(NULL, &g_state, 0U, epoch, &newer, "newer again") == 0);

    reset_fixture();
    const uint64_t epoch2 = observe(DSD_SYNC_DMR_BS_VOICE_POS, 111U, NULL, DSD_CALL_BOUNDARY_BEGIN);
    dsd_position_fix first = admitted(DSD_SYNC_DMR_BS_VOICE_POS, 111U, 225000000, 450000000);
    dsd_position_fix second = admitted(DSD_SYNC_DMR_BS_VOICE_POS, 111U, 100000000, 200000000);
    second.rx_time_s = first.rx_time_s; /* one second */
    assert(dsd_event_enrich_position(NULL, &g_state, 0U, epoch2, &first, "first") == 1);
    assert(dsd_event_enrich_position(NULL, &g_state, 0U, epoch2, &second, "second") == 1);
    expect_row_fix(row(0), &second, "second");
}

/* One radio number in two systems is two radios: a row heard in one system takes no report heard in another. The row's
 * reception identity is taken while its call is active on the carrier it began on, pinned, and committed with it. */
static void
test_same_number_two_scopes(void) {
    reset_fixture();
    dsd_rxid_note_p25_network(&g_state, 0xA1234U, 0x111U);
    const uint64_t epoch = observe(DSD_SYNC_P25P1_POS, 111U, NULL, DSD_CALL_BOUNDARY_BEGIN);
    assert(row(0)->rx_scope.resolved == 1U && row(0)->rx_scope.scope.kind == DSD_POS_SCOPE_P25);
    assert(row(0)->rx_scope.scope.a == 0xA1234U && row(0)->rx_scope.scope.b == 0x111U);
    const dsd_position_fix here = admitted(DSD_SYNC_P25P1_POS, 111U, 225000000, 450000000);
    assert(here.reception_scope.kind == DSD_POS_SCOPE_P25);
    dsd_rxid_reset(&g_state);
    dsd_rxid_note_p25_network(&g_state, 0xA1234U, 0x222U);
    const dsd_position_fix there = admitted(DSD_SYNC_P25P1_POS, 111U, 100000000, 200000000);
    assert(there.rev > here.rev && there.reception_scope.b != here.reception_scope.b);
    assert(dsd_event_enrich_position(NULL, &g_state, 0U, epoch, &there, "there") == 0);
    assert(dsd_event_enrich_position(NULL, &g_state, 0U, epoch, &here, "here") == 1);
    expect_row_fix(row(0), &here, "here");
    /* Pinned: another carrier's record does not move it, and the commit keeps it. */
    dsd_event_sync_slot(&g_opts, &g_state, 0U);
    assert(row(0)->rx_scope.scope.b == 0x111U);
    end_call(DSD_CALL_END_TERMINATOR);
    assert(row(1)->source_id == 111U && row(1)->rx_scope.resolved == 1U && row(1)->rx_scope.scope.b == 0x111U);

    /* A row that knows no reception scope takes a report from the reception its call began on (as every GPS
       enrichment did before rows knew one), and none from another: an unknown scope is no wildcard. */
    reset_fixture();
    const uint64_t unknown = observe(DSD_SYNC_P25P1_POS, 111U, NULL, DSD_CALL_BOUNDARY_BEGIN);
    assert(row(0)->rx_scope.scope.kind == DSD_POS_SCOPE_NONE);
    const dsd_position_fix same = admitted(DSD_SYNC_P25P1_POS, 111U, 225000000, 450000000);
    assert(dsd_event_enrich_position(NULL, &g_state, 0U, unknown, &same, "same carrier") == 1);
    dsd_rxid_reset(&g_state);
    dsd_rxid_note_p25_network(&g_state, 0xA1234U, 0x222U);
    const dsd_position_fix elsewhere = admitted(DSD_SYNC_P25P1_POS, 111U, 100000000, 200000000);
    assert(dsd_event_enrich_position(NULL, &g_state, 0U, unknown, &elsewhere, "elsewhere") == 0);
    expect_row_fix(row(0), &same, "same carrier");
}

/* A conventional P25 channel: the call keys up while the record holds only the NAC; the network arrives mid-call
 * without a new record epoch. The row's reception identity follows (NAC + tuned frequency, then WACN/SYS), and a report
 * heard after the network was learned still enriches the call. */
static void
test_reception_scope_refines_mid_call(void) {
    reset_fixture();
    g_opts.audio_in_type = AUDIO_IN_RTL;
    g_opts.rtlsdr_center_freq = 450000000U;
    DSD_MEMSET(&g_state.rx_id, 0, sizeof(g_state.rx_id));
    g_state.rx_id.family = DSD_RXID_FAMILY_P25;
    g_state.rx_id.valid = DSD_RXID_ACCESS;
    g_state.rx_id.access_kind = DSD_ACCESS_CODE_NAC;
    g_state.rx_id.access = 0x293U;
    const uint64_t epoch = observe(DSD_SYNC_P25P1_POS, 111U, NULL, DSD_CALL_BOUNDARY_BEGIN);
    assert(row(0)->rx_scope.scope.kind == DSD_POS_SCOPE_P25_NAC && row(0)->rx_scope.identity_freq_hz == 450000000);
    const uint32_t record_epoch = g_state.rx_id.epoch;
    dsd_rxid_note_p25_network(&g_state, 0xA1234U, 0x111U);
    assert(g_state.rx_id.epoch == record_epoch);
    const dsd_position_fix f = admitted(DSD_SYNC_P25P1_POS, 111U, 225000000, 450000000);
    assert(f.reception_scope.kind == DSD_POS_SCOPE_P25);
    assert(dsd_event_enrich_position(NULL, &g_state, 0U, epoch, &f, "GPS") == 1);
    dsd_event_sync_slot(&g_opts, &g_state, 0U);
    assert(row(0)->rx_scope.scope.kind == DSD_POS_SCOPE_P25 && row(0)->rx_scope.scope.b == 0x111U);
    assert(row(0)->rx_scope.identity_freq_hz == 0);
    end_call(DSD_CALL_END_TERMINATOR);
    assert(row(1)->rx_scope.scope.kind == DSD_POS_SCOPE_P25);
}

/* A P25 call heard first by its NAC loses sync and learns its WACN and SYSID after reacquisition: the merged row keeps
 * the stronger identity the reacquired segment read (a NAC-only row would refuse every report heard under the
 * network's identity in a later reception). */
static void
test_reacquired_segment_strengthens_reception_scope(void) {
    reset_fixture();
    g_opts.audio_in_type = AUDIO_IN_RTL;
    g_opts.rtlsdr_center_freq = 450000000U;
    DSD_MEMSET(&g_state.rx_id, 0, sizeof(g_state.rx_id));
    g_state.rx_id.family = DSD_RXID_FAMILY_P25;
    g_state.rx_id.valid = DSD_RXID_ACCESS;
    g_state.rx_id.access_kind = DSD_ACCESS_CODE_NAC;
    g_state.rx_id.access = 0x293U;
    const uint64_t first = observe(DSD_SYNC_P25P1_POS, 111U, NULL, DSD_CALL_BOUNDARY_BEGIN);
    end_call(DSD_CALL_END_SYNC_LOSS);
    assert(row(1)->rx_scope.resolved == 1U && row(1)->rx_scope.scope.kind == DSD_POS_SCOPE_P25_NAC);
    dsd_rxid_note_p25_network(&g_state, 0xA1234U, 0x111U);
    assert(observe(DSD_SYNC_P25P1_POS, 111U, NULL, DSD_CALL_BOUNDARY_BEGIN) != first);
    assert(row(0)->rx_scope.scope.kind == DSD_POS_SCOPE_P25);
    end_call(DSD_CALL_END_SYNC_LOSS);
    assert(row(1)->source_id == 111U && row(2)->source_id == 0U); /* one row */
    assert(row(1)->rx_scope.resolved == 1U && row(1)->rx_scope.scope.kind == DSD_POS_SCOPE_P25);
    assert(row(1)->rx_scope.scope.a == 0xA1234U && row(1)->rx_scope.scope.b == 0x111U);
    assert(row(1)->rx_scope.identity_freq_hz == 0);
}

/* A call that ended before any render takes the record as it ended, with the frequency the row is given (the call's
 * own), not whatever the staged row held. */
static void
test_reception_scope_of_a_call_ended_unseen(void) {
    reset_fixture();
    DSD_MEMSET(&g_state.rx_id, 0, sizeof(g_state.rx_id));
    g_state.rx_id.family = DSD_RXID_FAMILY_P25;
    g_state.rx_id.valid = DSD_RXID_ACCESS;
    g_state.rx_id.access_kind = DSD_ACCESS_CODE_NAC;
    g_state.rx_id.access = 0x293U;
    history[0].Event_History_Items[0].freq_hz = 123456789; /* a stale staged row */
    const dsd_call_observation observation = {
        .protocol = DSD_SYNC_P25P1_POS,
        .slot = 0U,
        .kind = DSD_CALL_KIND_GROUP_VOICE,
        .ota_target_id = 1201U,
        .policy_target_id = 1201U,
        .ota_source_id = 111U,
        .frequency_hz = 450000000,
        .observed_m = 1.0,
    };
    assert(dsd_call_state_observe(&g_state, &observation, DSD_CALL_BOUNDARY_BEGIN) == 1);
    assert(dsd_call_state_end_ex(&g_state, 0U, 1.1, DSD_CALL_END_TERMINATOR) == 1);
    dsd_event_sync_slot(&g_opts, &g_state, 0U);
    assert(row(1)->source_id == 111U && row(1)->freq_hz == 450000000);
    assert(row(1)->rx_scope.resolved == 1U && row(1)->rx_scope.scope.kind == DSD_POS_SCOPE_P25_NAC);
    assert(row(1)->rx_scope.identity_freq_hz == 450000000);
}

/* A position the row declines still leaves the transmission's CRC provenance on the call, as every enrichment does. */
static void
test_declined_position_keeps_crc_marking(void) {
    reset_fixture();
    const uint64_t epoch = observe(DSD_SYNC_DMR_BS_VOICE_POS, 111U, NULL, DSD_CALL_BOUNDARY_BEGIN);
    const dsd_position_fix older = admitted(DSD_SYNC_DMR_BS_VOICE_POS, 111U, 225000000, 450000000);
    const dsd_position_fix newer = admitted(DSD_SYNC_DMR_BS_VOICE_POS, 111U, 100000000, 200000000);
    assert(dsd_event_enrich_position(NULL, &g_state, 0U, epoch, &newer, "newer") == 1);
    g_state.event_crc_invalid[0] = (uint8_t)DSD_CRC_RAS_UNVERIFIED;
    assert(dsd_event_enrich_position(NULL, &g_state, 0U, epoch, &older, "older") == 0);
    g_state.event_crc_invalid[0] = 0U;
    dsd_call_snapshot call;
    assert(dsd_call_state_get(&g_state, 0U, &call) == 1);
    assert((call.crc_invalid & DSD_CRC_RAS_UNVERIFIED) != 0U);
    assert((row(0)->crc_invalid & DSD_CRC_RAS_UNVERIFIED) != 0U);
    expect_row_fix(row(0), &newer, "newer");

    /* On a committed row (the closing call's), a declined position that adds a marking is a committed-row change. */
    reset_fixture();
    (void)observe(DSD_SYNC_P25P1_POS, 111U, NULL, DSD_CALL_BOUNDARY_BEGIN);
    dsd_event_closing_call closing;
    assert(dsd_event_capture_closing_call(&g_state, 0U, &closing) == 1);
    end_call(DSD_CALL_END_TERMINATOR);
    const dsd_position_fix first = admitted(DSD_SYNC_P25P1_POS, 111U, 225000000, 450000000);
    const dsd_position_fix second = admitted(DSD_SYNC_P25P1_POS, 111U, 100000000, 200000000);
    assert(dsd_event_enrich_position_closing(NULL, &g_state, &closing, &second, "second") == 1);
    const uint64_t commit_rev = history[0].commit_rev;
    g_state.event_crc_invalid[0] = (uint8_t)DSD_CRC_RAS_UNVERIFIED;
    assert(dsd_event_enrich_position_closing(NULL, &g_state, &closing, &first, "first") == 0);
    assert((row(1)->crc_invalid & DSD_CRC_RAS_UNVERIFIED) != 0U);
    assert(history[0].commit_rev == commit_rev + 1U);
    /* Nothing new to mark: no committed-row change. */
    assert(dsd_event_enrich_position_closing(NULL, &g_state, &closing, &first, "first") == 0);
    assert(history[0].commit_rev == commit_rev + 1U);
    g_state.event_crc_invalid[0] = 0U;
    expect_row_fix(row(1), &second, "second");
}

/* The sender scope is only the track key: a TETRA radio of a foreign network, heard here, enriches its own call. */
static void
test_foreign_sender_enriches_its_call(void) {
    reset_fixture();
    DSD_MEMSET(&g_state.rx_id, 0, sizeof(g_state.rx_id));
    g_state.rx_id.family = DSD_RXID_FAMILY_TETRA;
    g_state.rx_id.valid = DSD_RXID_TETRA_NET;
    g_state.rx_id.tetra_mcc = 901U;
    g_state.rx_id.tetra_mnc = 1U;
    const uint64_t epoch = observe(DSD_SYNC_TETRA_NDB_POS, 4001U, NULL, DSD_CALL_BOUNDARY_BEGIN);
    assert(row(0)->rx_scope.scope.kind == DSD_POS_SCOPE_TETRA && row(0)->rx_scope.scope.a == 901U);
    dsd_position_fix f;
    dsd_position_fix_init(&f);
    f.kind = DSD_POS_TETRA_LIP;
    f.protocol = DSD_SYNC_TETRA_NDB_POS;
    f.lat_e7 = 225000000;
    f.lon_e7 = 450000000;
    f.source_id = 4001U;
    f.sender_scope.kind = (uint8_t)DSD_POS_SCOPE_TETRA;
    f.sender_scope.a = 902U;
    f.sender_scope.b = 7U;
    assert(dsd_position_admit(&g_opts, &g_state, &f, 1, 1, NULL) != DSD_POSITION_REFUSED);
    assert(f.sender_scope.a == 902U && f.reception_scope.a == 901U);
    assert(dsd_event_enrich_position(NULL, &g_state, 0U, epoch, &f, "LIP 22.5 45.0") == 1);
    expect_row_fix(row(0), &f, "LIP 22.5 45.0");
    assert(row(0)->pos.scope.a == 902U && row(0)->rx_scope.scope.a == 901U);
}

/* Callsign sources: the open-call writer (D-STAR) checks the report's callsign against the call's. */
static void
test_callsign_sources(void) {
    reset_fixture();
    (void)observe(DSD_SYNC_DSTAR_VOICE_POS, 0U, "N0CALL  /ABCD", DSD_CALL_BOUNDARY_BEGIN);
    dsd_position_fix f;
    dsd_position_fix_init(&f);
    f.kind = DSD_POS_DSTAR_DPRS;
    f.protocol = DSD_SYNC_DSTAR_VOICE_POS;
    f.lat_e7 = 225000000;
    f.lon_e7 = 450000000;
    DSD_SNPRINTF(f.source_text, sizeof f.source_text, "%s", "N0CALL  /ABCD");
    dsd_position_fix other = f;
    DSD_SNPRINTF(other.source_text, sizeof other.source_text, "%s", "N1CALL  /ABCD");
    dsd_position_fix prefix = f;
    DSD_SNPRINTF(prefix.source_text, sizeof prefix.source_text, "%s", "N0CALL");
    assert(dsd_position_admit(&g_opts, &g_state, &f, 1, 1, NULL) != DSD_POSITION_REFUSED);
    assert(dsd_position_admit(&g_opts, &g_state, &other, 1, 1, NULL) != DSD_POSITION_REFUSED);
    assert(dsd_position_admit(&g_opts, &g_state, &prefix, 1, 1, NULL) != DSD_POSITION_REFUSED);
    assert(f.sender_scope.kind == DSD_POS_SCOPE_NONE);
    assert(dsd_event_set_open_call_position(&g_state, 0U, &other, "other caller") == 0);
    assert(dsd_event_set_open_call_position(&g_state, 0U, &prefix, "a shorter callsign") == 0);
    assert(dsd_event_set_open_call_position(&g_state, 0U, &f, "DPRS 22.5 45.0") == 1);
    expect_row_fix(row(0), &f, "DPRS 22.5 45.0");
    /* A callsign the report cut at 15 bytes matches the call's longer one by prefix; padding is not a cut. */
    reset_fixture();
    (void)observe(DSD_SYNC_DSTAR_VOICE_POS, 0U, "ABCDEFGHIJKLMNOPQ", DSD_CALL_BOUNDARY_BEGIN);
    dsd_position_fix cut = f;
    cut.rev = 0U;
    DSD_SNPRINTF(cut.source_text, sizeof cut.source_text, "%s", "ABCDEFGHIJKLMNO");
    assert(dsd_position_admit(&g_opts, &g_state, &cut, 1, 1, NULL) != DSD_POSITION_REFUSED);
    assert(dsd_event_set_open_call_position(&g_state, 0U, &cut, "cut") == 1);
    reset_fixture();
    (void)observe(DSD_SYNC_DSTAR_VOICE_POS, 0U, "ABCDEFGHIJKLMN XYZ", DSD_CALL_BOUNDARY_BEGIN);
    dsd_position_fix padded = f;
    padded.rev = 0U;
    DSD_SNPRINTF(padded.source_text, sizeof padded.source_text, "%s", "ABCDEFGHIJKLMN ");
    assert(dsd_position_admit(&g_opts, &g_state, &padded, 1, 1, NULL) != DSD_POSITION_REFUSED);
    assert(strlen(padded.source_text) == 15U);
    assert(dsd_event_set_open_call_position(&g_state, 0U, &padded, "padded") == 0);
    reset_fixture();
    (void)observe(DSD_SYNC_DSTAR_VOICE_POS, 0U, "N0CALL  /ABCD", DSD_CALL_BOUNDARY_BEGIN);
    assert(dsd_event_set_open_call_position(&g_state, 0U, &f, "DPRS 22.5 45.0") == 1);

    /* The text-only open-call GPS drops the position with the text it replaces. */
    assert(dsd_event_set_open_call_detail(&g_state, 0U, DSD_EVENT_DETAIL_GPS, "text only") == 1);
    assert(row(0)->pos.valid == 0U && strcmp(row(0)->gps_s, "text only") == 0);
}

/* A text message after a GPS enrichment leaves the position; a text-only GPS write drops it with the old text. */
static void
test_text_only_after_gps(void) {
    reset_fixture();
    const uint64_t epoch = observe(DSD_SYNC_DMR_BS_VOICE_POS, 111U, NULL, DSD_CALL_BOUNDARY_BEGIN);
    dsd_rxid_note_dmr_t3_syscode(&g_state, 0x0A3FU);
    const dsd_position_fix f = admitted(DSD_SYNC_DMR_BS_VOICE_POS, 111U, 225000000, 450000000);
    assert(dsd_event_enrich_position(NULL, &g_state, 0U, epoch, &f, "GPS A") == 1);
    assert(dsd_event_enrich_text(&g_state, 0U, epoch, "hello") == 1);
    expect_row_fix(row(0), &f, "GPS A");
    assert(strcmp(row(0)->text_message, "hello") == 0);
    assert(dsd_event_enrich_gps(&g_state, 0U, epoch, "GPS text only") == 1);
    assert(strcmp(row(0)->gps_s, "GPS text only") == 0);
    assert(row(0)->pos.valid == 0U && row(0)->pos.rev == 0U && row(0)->pos.lat_e7 == 0);
    /* The sender identity stays with the row. */
    assert(row(0)->pos.scope.kind == f.sender_scope.kind && row(0)->pos.scope.a == f.sender_scope.a);
}

/* A sync-loss interrupted call merges into one row; its GPS text and position come from the same side. */
static void
merge_case(int newer_first, int second_text_only) {
    reset_fixture();
    const dsd_position_fix older = admitted(DSD_SYNC_DMR_BS_VOICE_POS, 111U, 225000000, 450000000);
    const dsd_position_fix newer = admitted(DSD_SYNC_DMR_BS_VOICE_POS, 111U, 100000000, 200000000);
    const dsd_position_fix* first = newer_first ? &newer : &older;
    const dsd_position_fix* second = newer_first ? &older : &newer;
    const uint64_t epoch1 = observe(DSD_SYNC_DMR_BS_VOICE_POS, 111U, NULL, DSD_CALL_BOUNDARY_BEGIN);
    assert(dsd_event_enrich_position(NULL, &g_state, 0U, epoch1, first, newer_first ? "newer" : "older") == 1);
    end_call(DSD_CALL_END_SYNC_LOSS);
    const uint64_t epoch2 = observe(DSD_SYNC_DMR_BS_VOICE_POS, 111U, NULL, DSD_CALL_BOUNDARY_BEGIN);
    assert(epoch2 != epoch1);
    if (second_text_only) {
        assert(dsd_event_enrich_gps(&g_state, 0U, epoch2, "text only") == 1);
    } else {
        assert(dsd_event_enrich_position(NULL, &g_state, 0U, epoch2, second, newer_first ? "older" : "newer") == 1);
    }
    end_call(DSD_CALL_END_SYNC_LOSS);
    assert(row(1)->source_id == 111U && row(2)->source_id == 0U); /* one row */
    if (second_text_only || newer_first) {
        expect_row_fix(row(1), first, newer_first ? "newer" : "older");
    } else {
        expect_row_fix(row(1), &newer, "newer");
    }
}

static void
test_merge_keeps_text_and_position_together(void) {
    merge_case(0, 0);
    merge_case(1, 0);
    merge_case(1, 1);
}

/* The active call's source fills in (no new epoch), and its row is rendered again. */
static void
fill_source(int protocol, uint64_t source_id, const char* source_text) {
    dsd_call_observation observation = {
        .protocol = protocol,
        .slot = 0U,
        .kind = DSD_CALL_KIND_GROUP_VOICE,
        .ota_target_id = 1201U,
        .policy_target_id = 1201U,
        .ota_source_id = source_id,
        .observed_m = g_observed_m,
    };
    if (source_text != NULL) {
        DSD_SNPRINTF(observation.source_text, sizeof observation.source_text, "%s", source_text);
    }
    g_observed_m += 0.1;
    assert(dsd_call_state_observe(&g_state, &observation, DSD_CALL_BOUNDARY_CONTINUE) == 0);
    dsd_event_sync_slot(&g_opts, &g_state, 0U);
}

/* An anonymous report admitted while the call named no source. */
static dsd_position_fix
anonymous_on_nameless(int protocol, uint64_t epoch, const char* text) {
    dsd_position_fix anon;
    dsd_position_fix_init(&anon);
    anon.kind = DSD_POS_P25_HARRIS;
    anon.protocol = (uint8_t)protocol;
    anon.source_id = 3001U;
    anon.lat_e7 = 100000000;
    anon.lon_e7 = 200000000;
    assert(dsd_position_admit(&g_opts, &g_state, &anon, 1, 0, NULL) != DSD_POSITION_REFUSED);
    assert((anon.flags & DSD_POS_FLAG_SOURCE_UNVERIFIED) != 0U);
    assert(dsd_event_enrich_position(NULL, &g_state, 0U, epoch, &anon, text) == 1);
    assert(row(0)->pos.valid == 1U && row(0)->pos.anonymous == 1U);
    return anon;
}

/* An anonymous report stays on a row only while the row names no source (spec 2.14): once the call's source fills in
 * within the same epoch (a voice-user LC after the report), or a reacquired segment gives the row its source, the
 * report and its GPS text go, so no named caller is shown with a report that never named it. A report that named the
 * source stays when the row learns its name. */
static void
test_anonymous_position_leaves_a_named_row(void) {
    reset_fixture();
    const uint64_t epoch = observe(DSD_SYNC_P25P1_POS, 0U, NULL, DSD_CALL_BOUNDARY_BEGIN);
    (void)anonymous_on_nameless(DSD_SYNC_P25P1_POS, epoch, "anonymous fix");
    fill_source(DSD_SYNC_P25P1_POS, 3001U, NULL);
    assert(row(0)->source_id == 3001U);
    assert(row(0)->pos.valid == 0U && row(0)->gps_s[0] == '\0');
    end_call(DSD_CALL_END_TERMINATOR);
    assert(row(1)->source_id == 3001U && row(1)->pos.valid == 0U && row(1)->gps_s[0] == '\0');

    /* A reacquired segment fills the committed row's source. */
    reset_fixture();
    const uint64_t first = observe(DSD_SYNC_P25P1_POS, 0U, NULL, DSD_CALL_BOUNDARY_BEGIN);
    (void)anonymous_on_nameless(DSD_SYNC_P25P1_POS, first, "anonymous fix");
    end_call(DSD_CALL_END_SYNC_LOSS);
    assert(row(1)->pos.valid == 1U);
    assert(observe(DSD_SYNC_P25P1_POS, 3001U, NULL, DSD_CALL_BOUNDARY_BEGIN) != first);
    end_call(DSD_CALL_END_SYNC_LOSS);
    assert(row(1)->source_id == 3001U && row(2)->source_id == 0U && row(2)->pos.valid == 0U); /* one row */
    assert(row(1)->pos.valid == 0U && row(1)->gps_s[0] == '\0');

    /* A callsign call: the D-STAR row that later shows the header's callsign drops the anonymous record too. */
    reset_fixture();
    const uint64_t dstar = observe(DSD_SYNC_DSTAR_HD_POS, 0U, NULL, DSD_CALL_BOUNDARY_BEGIN);
    dsd_position_fix dprs;
    dsd_position_fix_init(&dprs);
    dprs.kind = DSD_POS_DSTAR_DPRS;
    dprs.protocol = DSD_SYNC_DSTAR_HD_POS;
    dprs.lat_e7 = 100000000;
    dprs.lon_e7 = 200000000;
    assert(dsd_position_admit(&g_opts, &g_state, &dprs, 1, 0, NULL) != DSD_POSITION_REFUSED);
    assert(dsd_event_enrich_position(NULL, &g_state, 0U, dstar, &dprs, "DPRS anonymous") == 1);
    fill_source(DSD_SYNC_DSTAR_HD_POS, 0U, "N0CALL");
    assert(row(0)->pos.valid == 0U && row(0)->gps_s[0] == '\0');

    /* A report that named the call's radio is not anonymous: it stays whatever the row learns. */
    reset_fixture();
    const uint64_t named = observe(DSD_SYNC_P25P1_POS, 3001U, NULL, DSD_CALL_BOUNDARY_BEGIN);
    assert(named != 0U);
    const dsd_position_fix own = admitted(DSD_SYNC_P25P1_POS, 3001U, 225000000, 450000000);
    assert(dsd_event_enrich_position(NULL, &g_state, 0U, named, &own, "own fix") == 1);
    assert(row(0)->pos.anonymous == 0U);
    dsd_event_sync_slot(&g_opts, &g_state, 0U);
    expect_row_fix(row(0), &own, "own fix");
    end_call(DSD_CALL_END_TERMINATOR);
    expect_row_fix(row(1), &own, "own fix");
}

/* The P25 terminator: the call it ends is captured first, and that capture alone may enrich the ended call. */
static void
test_closing_call(void) {
    reset_fixture();
    const uint64_t epoch = observe(DSD_SYNC_P25P1_POS, 111U, NULL, DSD_CALL_BOUNDARY_BEGIN);
    dsd_event_closing_call closing;
    assert(dsd_event_capture_closing_call(&g_state, 0U, &closing) == 1);
    assert(closing.valid == 1U && closing.epoch == epoch && closing.source_id == 111U);
    end_call(DSD_CALL_END_TERMINATOR);
    const dsd_position_fix f = admitted(DSD_SYNC_P25P1_POS, 111U, 225000000, 450000000);
    assert(dsd_event_enrich_position(NULL, &g_state, 0U, epoch, &f, "after the end") == 0);
    assert(dsd_event_enrich_position_closing(NULL, &g_state, &closing, &f, "terminator GPS") == 1);
    assert(row(1)->source_id == 111U);
    expect_row_fix(row(1), &f, "terminator GPS");
    /* Another radio's report is not the closing call's. */
    const dsd_position_fix other = admitted(DSD_SYNC_P25P1_POS, 222U, 100000000, 200000000);
    assert(dsd_event_enrich_position_closing(NULL, &g_state, &closing, &other, "other") == 0);
    /* Once the next call began, the capture enriches nothing. */
    (void)observe(DSD_SYNC_P25P1_POS, 333U, NULL, DSD_CALL_BOUNDARY_BEGIN);
    const dsd_position_fix late = admitted(DSD_SYNC_P25P1_POS, 111U, 100000000, 200000000);
    assert(dsd_event_enrich_position_closing(NULL, &g_state, &closing, &late, "late") == 0);
    expect_row_fix(row(1), &f, "terminator GPS");
    dsd_event_closing_call none;
    reset_fixture();
    assert(dsd_event_capture_closing_call(&g_state, 0U, &none) == 0 && none.valid == 0U);
    assert(dsd_event_enrich_position_closing(NULL, &g_state, &none, &f, "nothing") == 0);
}

/* Staged and explicit data notices: the report goes in with its text only for the notice's own source, and every
 * notice stamps its sender identity and its site. */
static void
test_data_notices(void) {
    reset_fixture();
    dsd_rxid_note_p25_network(&g_state, 0xA1234U, 0x111U);
    dsd_rxid_note_p25_site(&g_state, 0x111U, 2U, 3U);
    const dsd_position_fix f = admitted(DSD_SYNC_P25P1_POS, 222U, 225000000, 450000000);
    assert(dsd_event_stage_position(&g_state, 0U, &f, "LRRP 22.5 45.0") == 1);
    assert(dsd_event_staged_position(&g_state, 0U)->rev == f.rev);
    const dsd_call_observation mine = dsd_call_observation_data(DSD_SYNC_P25P1_POS, 0U, 222U, 1201U);
    assert(dsd_event_emit_data_notice(&g_opts, &g_state, 0U, &mine, "LRRP from 222;") == 0);
    expect_row_fix(row(1), &f, "LRRP 22.5 45.0");
    assert(row(1)->site_id.family == DSD_RXID_FAMILY_P25 && row(1)->site_id.p25_site == 3U);
    assert(dsd_event_staged_position(&g_state, 0U)->rev == 0U && dsd_event_staged_gps(&g_state, 0U)[0] == '\0');

    /* Another source's notice takes neither the text nor the position. */
    assert(dsd_event_stage_position(&g_state, 0U, &f, "LRRP again") == 1);
    const dsd_call_observation theirs = dsd_call_observation_data(DSD_SYNC_P25P1_POS, 0U, 333U, 1201U);
    assert(dsd_event_emit_data_notice(&g_opts, &g_state, 0U, &theirs, "PDU from 333;") == 0);
    assert(row(1)->source_id == 333U && row(1)->pos.valid == 0U && row(1)->gps_s[0] == '\0');
    /* A notice without a position still stamps the reception's scope. */
    assert(row(1)->pos.scope.kind == DSD_POS_SCOPE_P25 && row(1)->pos.scope.a == 0xA1234U
           && row(1)->pos.scope.b == 0x111U);

    assert(row(1)->rx_scope.resolved == 1U && row(1)->rx_scope.scope.b == 0x111U);

    /* A report whose identity admission cleared makes the notice anonymous and keeps the position. */
    dsd_position_fix unverified;
    dsd_position_fix_init(&unverified);
    unverified.kind = DSD_POS_DMR_LRRP;
    unverified.protocol = DSD_SYNC_P25P1_POS;
    unverified.source_id = 333U;
    unverified.target_id = 1201U;
    unverified.lat_e7 = 100000000;
    unverified.lon_e7 = 200000000;
    assert(dsd_position_admit(&g_opts, &g_state, &unverified, 1, 0, NULL) != DSD_POSITION_REFUSED);
    assert(dsd_event_stage_position(&g_state, 0U, &unverified, "LRRP unverified") == 1);
    dsd_call_observation named = dsd_call_observation_data(DSD_SYNC_P25P1_POS, 0U, 333U, 1201U);
    DSD_SNPRINTF(named.source_text, sizeof named.source_text, "%s", "UNIT 333");
    assert(dsd_event_emit_data_notice(&g_opts, &g_state, 0U, &named, "PDU;") == 0);
    expect_row_fix(row(1), &unverified, "LRRP unverified");
    assert(row(1)->source_id == 0U && row(1)->target_id == 0U && row(1)->src_str[0] == '\0'
           && row(1)->tgt_str[0] == '\0');

    /* Playing back files, a notice records no reception identity (as call rows and its site). */
    g_opts.playfiles = 1;
    assert(dsd_event_emit_data_notice(&g_opts, &g_state, 0U, &theirs, "playback;") == 0);
    assert(row(1)->rx_scope.resolved == 0U && row(1)->site_id.family == 0U);
    g_opts.playfiles = 0;

    /* A text-only stage clears a staged position. */
    assert(dsd_event_stage_position(&g_state, 0U, &f, "LRRP B") == 1);
    dsd_event_stage_gps(&g_state, 0U, "text only");
    assert(dsd_event_staged_position(&g_state, 0U)->rev == 0U);
    assert(dsd_event_emit_data_notice(&g_opts, &g_state, 0U, &mine, "LRRP text;") == 0);
    assert(strcmp(row(1)->gps_s, "text only") == 0 && row(1)->pos.valid == 0U);

    /* The explicit notice leaves the staging alone. */
    assert(dsd_event_stage_position(&g_state, 0U, &f, "staged") == 1);
    assert(dsd_event_emit_data_notice_with_position(&g_opts, &g_state, 0U, &mine, DSD_EVENT_CATEGORY_DATA, "explicit;",
                                                    &f, "explicit GPS")
           == 0);
    expect_row_fix(row(1), &f, "explicit GPS");
    assert(dsd_event_staged_position(&g_state, 0U)->rev == f.rev);
    dsd_position_fix raw = f;
    raw.rev = 0U;
    assert(dsd_event_emit_data_notice_with_position(&g_opts, &g_state, 0U, &mine, DSD_EVENT_CATEGORY_DATA, "raw;", &raw,
                                                    "raw")
           == -1);
    assert(dsd_event_stage_position(&g_state, 0U, &raw, "raw") == -1);
    assert(dsd_event_stage_position(&g_state, 2U, &f, "slot") == -1);
}

int
main(void) {
    test_active_call_push_and_snapshot();
    test_typed_source_rule();
    test_newest_rev_wins();
    test_same_number_two_scopes();
    test_foreign_sender_enriches_its_call();
    test_reception_scope_refines_mid_call();
    test_reacquired_segment_strengthens_reception_scope();
    test_reception_scope_of_a_call_ended_unseen();
    test_declined_position_keeps_crc_marking();
    test_callsign_sources();
    test_text_only_after_gps();
    test_merge_keeps_text_and_position_together();
    test_anonymous_position_leaves_a_named_row();
    test_closing_call();
    test_data_notices();
    dsd_state_ext_free_all(&g_state);
    printf("event position tests passed\n");
    return 0;
}
