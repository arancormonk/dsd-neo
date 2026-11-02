// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <assert.h>
#include <dsd-neo/app_control/notification_status.h>
#include <dsd-neo/app_control/snapshot.h>
#include <dsd-neo/app_control/telemetry_observers.h>
#include <dsd-neo/core/call_channel.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/input_level.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/received_identity.h>
#include <dsd-neo/core/source_alias.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/core/tetra_publication.h>
#include <dsd-neo/protocol/p25/p25_sm_watchdog.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/decode_mode.h>
#include <dsd-neo/runtime/scan_mode.h>
#include <dsd-neo/runtime/trunk_cc_candidates.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../src/app_control/snapshot_internal.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "p25_tick_guard_stub.h"

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
    /* The scan-list name store is a second heap allocation beside the LCN tail, so it
       needs the same explicit deep copy; a byte-range copy would publish nothing. */
    assert(strcmp(dsd_state_trunk_lcn_name_get(snap, 0U), "Dispatch") == 0);
    assert(strcmp(dsd_state_trunk_lcn_name_get(snap, 1U), "Fireground") == 0);
    assert(strcmp(snap->trunk_scan_active_id, "county-p25") == 0);
    assert(snap->trunk_scan_active_ordinal == 2U);
    assert(snap->trunk_scan_target_count == 5U);
    /* The scan-list avoid store is a third heap allocation with the same deep-copy need,
       and the hold/avoid scalars ride the publication range beside the target id. */
    assert(dsd_state_trunk_lcn_avoid_get(snap, 1U) == 1);
    assert(dsd_state_trunk_lcn_avoid_get(snap, 0U) == 0);
    assert(snap->lcn_scan_hold == 1U);
    assert(snap->lcn_avoid_count == 1U);
    /* The scan list's position records (issue #666) ride the range that carries the list, beside its cursor. */
    assert(snap->lcn_freq_roll == 2);
    assert(snap->lcn_on_air_row == 0 && snap->lcn_on_air_freq == 851012500L);
    assert(snap->lcn_back_retry_row == 1 && snap->lcn_back_retry_freq == 851025000L);
    assert(snap->trunk_scan_hold == 1U);
    assert(snap->trunk_scan_active_avoided == 1U);
    assert(snap->trunk_scan_avoided_count == 3U);
    /* The scan timing publication rides the same inline range. The renderer differences
       its absolute anchors against its own clock, so every field has to survive the copy. */
    assert(snap->scan_timing.reason == (uint8_t)DSD_SCAN_STAY_IDLE_DWELL);
    assert(snap->scan_timing.conventional == 1U);
    assert(fabs(snap->scan_timing.started_m - 120.5) < 1e-6);
    assert(fabs(snap->scan_timing.deadline_m - 123.5) < 1e-6);
    assert(snap->scan_timing.span_ms == 3000U);
    assert(snap->scan_timing.dwell_ms == 3000U);
    assert(snap->scan_timing.hold_ms == 2000U);
    assert(snap->scan_timing.hang_ms == 7000U);
    /* The received-tone publication (issue #522) rides the same range: every frontend reads
       the tone from its snapshot, never from the detector. */
    assert(snap->analog_rx.carrier_open == 1);
    assert(snap->analog_rx.tone_state == DSD_ANALOG_TONE_STATE_LOCKED);
    assert(snap->analog_rx.tone_kind == DSD_ANALOG_TONE_KIND_CTCSS);
    assert(snap->analog_rx.ctcss_tenths_hz == 1318);
    assert(snap->analog_rx.gate == DSD_ANALOG_TONE_GATE_OFF);
    assert(snap->analog_rx.generation == 41U);
    /* The decoder's two-slot TDMA verdict, which the Monitor reserves the second slot's strip by. */
    assert(snap->tdma_carrier == 1U);
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

static int g_observer_saw_guard = -1;

static void
observe_state(const dsd_state* state, void* user) {
    (void)state;
    (void)user;
    g_observer_saw_guard = p25_sm_tick_guard_held_by_caller();
}

/* The P25 watchdog's tick writes the decoder state (calls, tuned frequencies, site tables, control-channel candidates)
   under the tick guard on its own thread, so the publication reads the live state under it: taken for the publish and
   left after, or, when the decoder thread already holds it (a frame dispatch), used as held, never entered again. */
static void
test_publish_holds_the_tick_guard(const dsd_state* state) {
    static const dsd_app_telemetry_observer observer = {observe_state, NULL, NULL};
    assert(dsd_app_telemetry_observer_add(&observer) == 0);

    g_observer_saw_guard = -1;
    dsd_app_telemetry_publish_snapshot(state);
    assert(g_observer_saw_guard == 1);
    assert(!p25_sm_tick_guard_held_by_caller());
    assert(p25_sm_tick_guard_try_enter());

    g_observer_saw_guard = -1;
    dsd_app_telemetry_publish_snapshot(state);
    assert(g_observer_saw_guard == 1);
    assert(p25_sm_tick_guard_held_by_caller());
    assert(dsd_test_p25_tick_guard_reentries() == 0);
    p25_sm_tick_guard_leave();

    assert(dsd_app_telemetry_observer_remove(&observer) == 0);
}

/* A watchdog tick in progress holds the guard: the publication does not wait for it (an RTL tune or a rigctl round trip
   would stall the decoder), but is put off, and the next publish carries the state as it is then. */
static void
test_publish_puts_off_while_a_tick_holds_the_guard(dsd_state* state) {
    state->trunk_cc_freq = 851000000L;
    dsd_app_telemetry_publish_snapshot(state);
    assert(dsd_app_telemetry_snapshot_pending() == 0);
    assert(dsd_app_get_latest_snapshot()->trunk_cc_freq == 851000000L);

    assert(dsd_test_p25_tick_guard_hold_elsewhere());
    state->trunk_cc_freq = 852000000L;
    dsd_app_telemetry_publish_snapshot(state);
    assert(dsd_app_telemetry_snapshot_pending() == 1);
    assert(dsd_app_get_latest_snapshot()->trunk_cc_freq == 851000000L);
    assert(dsd_test_p25_tick_guard_release_elsewhere()); /* still held: the publish returned without waiting */

    state->trunk_cc_freq = 853000000L;
    dsd_app_telemetry_publish_snapshot(state);
    assert(dsd_app_telemetry_snapshot_pending() == 0);
    assert(dsd_app_get_latest_snapshot()->trunk_cc_freq == 853000000L);
}

/* Issue #672: a decoder state as initState() leaves its avoid identity -- a fresh instance, whole stores -- without
   the rest of initState(), which this target does not link. */
static dsd_state*
new_session_state(void) {
    dsd_state* st = (dsd_state*)calloc(1, sizeof(*st));
    assert(st != NULL);
    st->state_instance = dsd_state_rev_next();
    st->lcn_list_rev = dsd_state_rev_next();
    st->lcn_avoid_rev = dsd_state_rev_next();
    st->trunk_scan_avoids_ok = 1U;
    st->scan_avoids_complete = 1U;
    return st;
}

static void
free_session_state(dsd_state* st) {
    dsd_state_ext_free_all(st);
    dsd_state_trunk_lcn_free(st);
    dsd_state_trunk_scan_avoids_free(st);
    free(st);
}

static long int
row_freq(int row) {
    return 851000000L + (12500L * row);
}

/* Rows 0..rows-1 carry row_freq(); the tail grows as needed. */
static void
seed_scan_rows(dsd_state* st, int rows) {
    assert(dsd_state_trunk_lcn_reserve(st, (size_t)rows) == 0);
    for (int row = 0; row < rows; row++) {
        *dsd_state_trunk_lcn_slot(st, row) = row_freq(row);
    }
    st->lcn_freq_count = rows;
}

static long int
snap_row_freq(const dsd_state* snap, int row) {
    return row < DSD_TRUNK_LCN_EMBEDDED ? snap->trunk_lcn_freq[row]
                                        : snap->trunk_lcn_freq_ext[row - DSD_TRUNK_LCN_EMBEDDED];
}

/* A row count that outgrows any tail either snapshot has grown so far (both double from the same start, and the
   consumer's is never more than one doubling behind the published one), so copying it must allocate. */
static int
rows_past_snapshot_tail(const dsd_state* snap) {
    return DSD_TRUNK_LCN_EMBEDDED + (int)(2U * snap->trunk_lcn_freq_ext_capacity) + 1;
}

/* The whole list as the live state has it: every row, the avoided rows (one past the embedded slots) and the name. */
static void
assert_scan_list_whole(const dsd_state* snap, const dsd_state* live, int far_row) {
    assert(snap->scan_avoids_complete == 1U);
    assert(snap->lcn_freq_count == live->lcn_freq_count);
    assert(snap->trunk_lcn_freq_ext != live->trunk_lcn_freq_ext);
    assert(snap_row_freq(snap, DSD_TRUNK_LCN_EMBEDDED) == row_freq(DSD_TRUNK_LCN_EMBEDDED));
    assert(snap_row_freq(snap, live->lcn_freq_count - 1) == row_freq(live->lcn_freq_count - 1));
    assert(dsd_state_trunk_lcn_avoid_get(snap, 3U) == 1);
    assert(dsd_state_trunk_lcn_avoid_get(snap, (size_t)far_row) == 1);
    assert(strcmp(dsd_state_trunk_lcn_name_get(snap, (size_t)far_row), "Far row") == 0);
    assert(snap->lcn_avoid_total == 2U && snap->lcn_avoid_rev == live->lcn_avoid_rev);
}

/* What a copy cut short at the embedded slots leaves: the first rows only, and scan_avoids_complete down, so the avoid
   count (still the decoder's, 2) is never read against flags the snapshot does not hold. */
static void
assert_scan_list_cut_short(const dsd_state* snap, int far_row) {
    assert(snap->scan_avoids_complete == 0U);
    assert(snap->lcn_freq_count == DSD_TRUNK_LCN_EMBEDDED);
    assert(dsd_state_trunk_lcn_avoid_get(snap, 3U) == 1);
    assert(dsd_state_trunk_lcn_avoid_get(snap, (size_t)far_row) == 0);
    assert(snap->lcn_avoid_total == 2U);
}

/* Issue #672 B2: an LCN list past the 26 embedded rows reaches the frontends whole, and a reserve failure at either
   hop marks the snapshot incomplete until a later copy at unchanged revisions succeeds. */
static void
test_scan_list_completeness_across_hops(void) {
    dsd_state* st = new_session_state();
    seed_scan_rows(st, 40);
    int far_row = 35;
    assert(dsd_state_trunk_lcn_avoid_set(st, 3U, 1) == 0);
    assert(dsd_state_trunk_lcn_avoid_set(st, (size_t)far_row, 1) == 0);
    assert(dsd_state_trunk_lcn_name_set(st, (size_t)far_row, "Far row") == 0);
    dsd_app_telemetry_publish_snapshot(st);
    const dsd_state* snap = dsd_app_get_latest_snapshot();
    assert_scan_list_whole(snap, st, far_row);

    /* Hop 1 (decoder -> published) fails to grow its tail: the published copy is cut short, and the consumer copy of
       it says so even though its own copy allocated nothing it lacked. */
    int rows = rows_past_snapshot_tail(snap);
    seed_scan_rows(st, rows);
    far_row = rows - 1;
    assert(dsd_state_trunk_lcn_avoid_set(st, 35U, 0) == 0);
    assert(dsd_state_trunk_lcn_avoid_set(st, (size_t)far_row, 1) == 0);
    assert(dsd_state_trunk_lcn_name_set(st, (size_t)far_row, "Far row") == 0);
    const uint64_t list_rev = st->lcn_list_rev;
    const uint64_t avoid_rev = st->lcn_avoid_rev;
    dsd_state_trunk_lcn_test_alloc_fail_after(0);
    dsd_app_telemetry_publish_snapshot(st);
    dsd_state_trunk_lcn_test_alloc_fail_after(-1);
    snap = dsd_app_get_latest_snapshot();
    assert_scan_list_cut_short(snap, far_row);
    /* Re-reading with nothing new published copies the same short source again: still incomplete. */
    snap = dsd_app_get_latest_snapshot();
    assert_scan_list_cut_short(snap, far_row);

    /* The next publication, at the same revisions, copies whole at both hops. */
    dsd_app_telemetry_publish_snapshot(st);
    snap = dsd_app_get_latest_snapshot();
    assert(st->lcn_list_rev == list_rev && st->lcn_avoid_rev == avoid_rev);
    assert_scan_list_whole(snap, st, far_row);

    /* Hop 2 (published -> consumer) fails to grow its tail: incomplete, and the next read retries without waiting for
       a new publication. */
    rows = rows_past_snapshot_tail(snap);
    seed_scan_rows(st, rows);
    assert(dsd_state_trunk_lcn_avoid_set(st, (size_t)far_row, 0) == 0);
    far_row = rows - 1;
    assert(dsd_state_trunk_lcn_avoid_set(st, (size_t)far_row, 1) == 0);
    assert(dsd_state_trunk_lcn_name_set(st, (size_t)far_row, "Far row") == 0);
    dsd_app_telemetry_publish_snapshot(st);
    dsd_state_trunk_lcn_test_alloc_fail_after(0);
    snap = dsd_app_get_latest_snapshot();
    dsd_state_trunk_lcn_test_alloc_fail_after(-1);
    assert_scan_list_cut_short(snap, far_row);
    snap = dsd_app_get_latest_snapshot();
    assert_scan_list_whole(snap, st, far_row);
    free_session_state(st);
}

static void
fill_trunk_scan_entries(dsd_trunk_scan_avoid_entry* entries, size_t count, const char* label) {
    DSD_MEMSET(entries, 0, count * sizeof(entries[0]));
    for (size_t i = 0; i < count; i++) {
        entries[i].ordinal = (uint32_t)(i + 1U);
        entries[i].frequency_hz = 851000000U + (12500U * (uint64_t)i);
        DSD_SNPRINTF(entries[i].id, sizeof entries[i].id, "target-%zu", i);
        DSD_SNPRINTF(entries[i].label, sizeof entries[i].label, "%s", label);
    }
}

/* Publish a new trunk-scan avoid list the way the coordinator does: the entries, then a new revision. Sized past what
   the snapshots hold, so copying it must allocate. */
static size_t
publish_trunk_scan_list(dsd_state* st, const dsd_state* snap, const char* label) {
    const size_t held = snap != NULL ? snap->trunk_scan_avoid_capacity : 0U;
    assert(held <= 4096U);
    const size_t count = (2U * held) + 1U;
    dsd_trunk_scan_avoid_entry* entries = (dsd_trunk_scan_avoid_entry*)calloc(count, sizeof(*entries));
    assert(entries != NULL);
    fill_trunk_scan_entries(entries, count, label);
    assert(dsd_state_trunk_scan_avoids_set(st, entries, count) == 0);
    st->trunk_scan_avoid_rev = dsd_state_rev_next();
    free(entries);
    return count;
}

static void
assert_trunk_scan_list(const dsd_state* snap, const dsd_state* live, size_t count, const char* label) {
    assert(snap->scan_avoids_complete == 1U);
    assert(snap->trunk_scan_avoid_count == count);
    assert(snap->trunk_scan_avoids != NULL && snap->trunk_scan_avoids != live->trunk_scan_avoids);
    assert(snap->trunk_scan_avoid_rev == live->trunk_scan_avoid_rev);
    assert(snap->trunk_scan_avoids[count - 1U].ordinal == (uint32_t)count);
    assert(strcmp(snap->trunk_scan_avoids[0].label, label) == 0);
    assert(strcmp(snap->trunk_scan_avoids[count - 1U].label, label) == 0);
}

/* Issue #672 §3.2: the published trunk-scan avoid list is deep-copied at both hops and copied again only when its
   (instance, revision) moved -- or while the copy held is not whole (spec S1: a hop-1 failure, a hop-2 copy of the
   incomplete source, hop 1 recovering at the same revision, then hop 2 copying again). */
static void
test_trunk_scan_avoids_gated_copy(void) {
    dsd_state* st = new_session_state();
    size_t count = publish_trunk_scan_list(st, NULL, "North");
    dsd_app_telemetry_publish_snapshot(st);
    const dsd_state* snap = dsd_app_get_latest_snapshot();
    assert_trunk_scan_list(snap, st, count, "North");

    /* A change the coordinator has not stamped is not copied: the gate keys on the revision alone. */
    DSD_SNPRINTF(st->trunk_scan_avoids[0].label, sizeof st->trunk_scan_avoids[0].label, "%s", "unstamped");
    dsd_app_telemetry_publish_snapshot(st);
    snap = dsd_app_get_latest_snapshot();
    assert(strcmp(snap->trunk_scan_avoids[0].label, "North") == 0);
    st->trunk_scan_avoid_rev = dsd_state_rev_next();
    dsd_app_telemetry_publish_snapshot(st);
    snap = dsd_app_get_latest_snapshot();
    assert(strcmp(snap->trunk_scan_avoids[0].label, "unstamped") == 0);

    /* S1. Hop 1 cannot grow its copy: the published list is empty and not whole, and hop 2 copies that. */
    count = publish_trunk_scan_list(st, snap, "South");
    const uint64_t rev = st->trunk_scan_avoid_rev;
    dsd_state_trunk_lcn_test_alloc_fail_after(0);
    dsd_app_telemetry_publish_snapshot(st);
    dsd_state_trunk_lcn_test_alloc_fail_after(-1);
    snap = dsd_app_get_latest_snapshot();
    assert(snap->scan_avoids_complete == 0U && snap->trunk_scan_avoid_count == 0U);
    assert(snap->trunk_scan_avoid_rev == rev);
    /* Hop 1 recovers at the same revision; hop 2, holding that revision but not whole, copies again. */
    dsd_app_telemetry_publish_snapshot(st);
    snap = dsd_app_get_latest_snapshot();
    assert(st->trunk_scan_avoid_rev == rev);
    assert_trunk_scan_list(snap, st, count, "South");

    /* Hop 2 cannot grow its copy: incomplete, then the next read copies again at the same revision with no new
       publication. */
    count = publish_trunk_scan_list(st, snap, "East");
    dsd_app_telemetry_publish_snapshot(st);
    dsd_state_trunk_lcn_test_alloc_fail_after(0);
    snap = dsd_app_get_latest_snapshot();
    dsd_state_trunk_lcn_test_alloc_fail_after(-1);
    assert(snap->scan_avoids_complete == 0U && snap->trunk_scan_avoid_count == 0U);
    snap = dsd_app_get_latest_snapshot();
    assert_trunk_scan_list(snap, st, count, "East");

    /* A restarted decoder (new instance) starts its own revisions over, so an equal revision is not the same list. */
    dsd_state* next = new_session_state();
    dsd_trunk_scan_avoid_entry entry;
    fill_trunk_scan_entries(&entry, 1U, "Restarted");
    assert(dsd_state_trunk_scan_avoids_set(next, &entry, 1U) == 0);
    next->trunk_scan_avoid_rev = st->trunk_scan_avoid_rev;
    dsd_app_telemetry_publish_snapshot(next);
    snap = dsd_app_get_latest_snapshot();
    assert_trunk_scan_list(snap, next, 1U, "Restarted");
    free_session_state(next);
    free_session_state(st);
}

static void
assert_chan_map_holds(const dsd_state* snap, uint16_t channel, long int freq) {
    assert(snap->trunk_chan_map_used_count == 1U);
    assert(snap->trunk_chan_map_used[0] == channel);
    assert(snap->trunk_chan_map[channel] == freq);
}

/* Issue #672 B3: initState() restarts trunk_chan_map_seq, so two sessions can publish different maps at the same
   sequence and count. The copy keys on the state instance too, at both hops -- including a consumer that missed the
   new session's earlier publications and so compares only against the old session's map. */
static void
test_chan_map_gate_keys_on_the_session(void) {
    dsd_state* first = new_session_state();
    dsd_state_set_trunk_chan_freq(first, 0x10U, 851000000L);
    dsd_app_telemetry_publish_snapshot(first);
    assert_chan_map_holds(dsd_app_get_latest_snapshot(), 0x10U, 851000000L);

    dsd_state* second = new_session_state();
    dsd_state_set_trunk_chan_freq(second, 0x20U, 852000000L);
    assert(second->trunk_chan_map_seq == first->trunk_chan_map_seq
           && second->trunk_chan_map_used_count == first->trunk_chan_map_used_count);
    dsd_app_telemetry_publish_snapshot(second);
    const dsd_state* snap = dsd_app_get_latest_snapshot();
    assert_chan_map_holds(snap, 0x20U, 852000000L);
    assert(snap->trunk_chan_map[0x10] == 0);

    /* The consumer holds the second session's map at sequence 2; a third session publishes sequence 1 (missed) and
       then sequence 2, so the published copy moved twice but the consumer sees the same sequence and count. */
    dsd_state_set_trunk_chan_freq(second, 0x20U, 852500000L);
    dsd_app_telemetry_publish_snapshot(second);
    assert_chan_map_holds(dsd_app_get_latest_snapshot(), 0x20U, 852500000L);
    dsd_state* third = new_session_state();
    dsd_state_set_trunk_chan_freq(third, 0x30U, 853000000L);
    dsd_app_telemetry_publish_snapshot(third);
    dsd_state_set_trunk_chan_freq(third, 0x30U, 853500000L);
    dsd_app_telemetry_publish_snapshot(third);
    assert(third->trunk_chan_map_seq == second->trunk_chan_map_seq);
    snap = dsd_app_get_latest_snapshot();
    assert_chan_map_holds(snap, 0x30U, 853500000L);
    assert(snap->trunk_chan_map[0x20] == 0);
    free_session_state(third);
    free_session_state(second);
    free_session_state(first);
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
    dsd_state_set_trunk_chan_freq(state, 0x1234U, 769768750L);

    dsd_call_observation observation = {0};
    observation.protocol = 35;
    observation.slot = 0U;
    observation.kind = DSD_CALL_KIND_GROUP_VOICE;
    observation.ota_target_id = 321U;
    observation.policy_target_id = 321U;
    observation.ota_source_id = 654U;
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
    assert(dsd_recent_activity_publish(state, 0U, &observation, "Active Ch: 1234 TG: 321; ", 1000U) == 1);

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
    state->lcn_freq_count = 2;
    state->trunk_lcn_freq[0] = 851012500L;
    state->trunk_lcn_freq[1] = 851025000L;
    state->lcn_freq_roll = 2;
    state->lcn_on_air_row = 0;
    state->lcn_on_air_freq = 851012500L;
    state->lcn_back_retry_row = 1;
    state->lcn_back_retry_freq = 851025000L;
    assert(dsd_state_trunk_lcn_name_set(state, 0U, "Dispatch") == 0);
    assert(dsd_state_trunk_lcn_name_set(state, 1U, "Fireground") == 0);
    DSD_SNPRINTF(state->trunk_scan_active_id, sizeof state->trunk_scan_active_id, "%s", "county-p25");
    state->trunk_scan_active_ordinal = 2U;
    state->trunk_scan_target_count = 5U;
    assert(dsd_state_trunk_lcn_avoid_set(state, 1U, 1) == 0);
    state->lcn_scan_hold = 1U;
    state->trunk_scan_hold = 1U;
    state->trunk_scan_active_avoided = 1U;
    state->trunk_scan_avoided_count = 3U;
    state->scan_timing.reason = (uint8_t)DSD_SCAN_STAY_IDLE_DWELL;
    state->scan_timing.conventional = 1U;
    state->scan_timing.started_m = 120.5;
    state->scan_timing.deadline_m = 123.5;
    state->scan_timing.span_ms = 3000U;
    state->scan_timing.dwell_ms = 3000U;
    state->scan_timing.hold_ms = 2000U;
    state->scan_timing.hang_ms = 7000U;
    state->analog_rx.carrier_open = 1;
    state->analog_rx.tone_state = DSD_ANALOG_TONE_STATE_LOCKED;
    state->analog_rx.tone_kind = DSD_ANALOG_TONE_KIND_CTCSS;
    state->analog_rx.ctcss_tenths_hz = 1318;
    state->analog_rx.generation = 41U;
    state->tdma_carrier = 1U;

    assert(dsd_trunk_cc_candidates_add(state, 851006250L, 1, DSD_TRUNK_CC_CANDIDATE_CURRENT_SITE) == 1);
    assert(dsd_trunk_cc_candidates_add(state, 852006250L, 1, DSD_TRUNK_CC_CANDIDATE_CURRENT_SITE) == 1);
    dsd_trunk_cc_candidates* cc = dsd_trunk_cc_candidates_get(state);
    assert(cc != NULL);
    cc->idx = 1;
    cc->cool_until[1] = 12.5;
    cc->used = 3U;

    history[0].Event_History_Items[1].source_id = 123U;
    history[1].Event_History_Items[1].source_id = 456U;
    /* A committed row's site and the numbering of its channel (issue #799) ride the snapshot with the row. */
    dsd_received_identity row_site;
    DSD_MEMSET(&row_site, 0, sizeof row_site);
    row_site.family = (uint8_t)DSD_RXID_FAMILY_DMR;
    row_site.valid = DSD_RXID_DMR_T3_SYSCODE;
    row_site.dmr_trunk = (uint8_t)DSD_RXID_DMR_TRUNK_TIER3;
    row_site.dmr_t3_syscode = 0x0A3FU;
    DSD_MEMCPY(&history[0].Event_History_Items[1].site_id, &row_site, sizeof row_site);
    history[0].Event_History_Items[1].channel = 12U;
    history[0].Event_History_Items[1].call_channel_kind = (uint8_t)DSD_CALL_CHANNEL_LPCN;
    DSD_SNPRINTF(history[0].Event_History_Items[1].src_str, sizeof history[0].Event_History_Items[1].src_str, "%s",
                 "RADIO-123");
    /* The staged row's rdio-scanner export verdict and the talkgroup it was judged on (issue #773 D7) ride the
       snapshot with the row. */
    history[0].Event_History_Items[0].policy_target_id = 123U;
    history[0].Event_History_Items[0].stream_blocked = 1U;
    history[1].Event_History_Items[0].policy_target_id = 0U;
    history[1].Event_History_Items[0].stream_blocked = 0U;
    mark_history(&history[0]);
    mark_history(&history[1]);

    dsd_app_snapshot_test_reset_event_history_copy_counts();
    /* The TETRA publication reaches the frontends with the snapshot, not with the live state. */
    state->tetra_pub.mode = DSD_TETRA_MODE_TMO_DOWNLINK;
    state->tetra_pub.net_known = 1U;
    state->tetra_pub.mcc = 262U;
    state->tetra_pub.traffic_slots = 0x0AU;
    state->tetra_pub.codec_frames = 18U;
    state->tetra_pub.ksg_mask = 0x0003U;
    state->tetra_pub.la_known = 1U;
    state->tetra_pub.la = 4097U;
    state->tetra_pub.rx_carrier_known = 1U;
    state->tetra_pub.rx_carrier = 654U;
    state->tetra_pub.key_kind = DSD_TETRA_KEY_TEA1_REDUCED;
    state->tetra_pub.key_tea = 1U;
    state->tetra_pub.aie = DSD_TETRA_AIE_AVAILABLE;
    state->tetra_pub.sec_known = 1U;
    state->tetra_pub.sec_sckn = 3U;
    state->tetra_pub.dmo_sckn = 5U;
    /* The air keys the publication only names the kind of never ride the snapshot themselves. */
    state->tetra_air_keys[0].loaded = 1U;
    state->tetra_air_keys[0].key[0] = 0xA5U;
    /* So does the identity received on the carrier, which every frontend's identity line reads. */
    dsd_rxid_note_p25_network(state, 0xBEE00U, 0x348U);
    dsd_rxid_note_p25_site(state, 0x348U, 3U, 21U);
    dsd_rxid_note_cc_freq(state, DSD_RXID_FAMILY_P25, 851012500U);
    const uint32_t rxid_epoch = state->rx_id.epoch;
    dsd_app_telemetry_publish_snapshot(state);
    dsd_rxid_reset(state);
    dsd_rxid_note_dmr_rest_lsn(state, 3U);
    state->tetra_pub.mode = DSD_TETRA_MODE_DMO_DIRECT;
    state->tetra_pub.mcc = 1U;
    state->tetra_pub.codec_frames = 99U;
    state->tetra_pub.ksg_mask = 0U;
    state->tetra_pub.rx_carrier = 1U;
    state->tetra_pub.key_kind = DSD_TETRA_KEY_NONE;
    state->tetra_pub.sec_sckn = 9U;
    observation.ota_source_id = 999U;
    observation.observed_m = 2.0;
    assert(dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_CONTINUE) == 1);
    crypto.classification = DSD_CALL_CRYPTO_ENCRYPTED;
    crypto.audio_permitted = 0U;
    crypto.observed_m = 2.1;
    assert(dsd_call_state_update_crypto(state, 0U, &crypto) == 1);
    assert(dsd_recent_activity_publish(state, 0U, &observation, "mutated TG: 999; ", 2000U) == 1);
    history[0].Event_History_Items[1].source_id = 999U;
    history[0].Event_History_Items[1].site_id.dmr_t3_syscode = 0x0123U;
    history[0].Event_History_Items[1].call_channel_kind = (uint8_t)DSD_CALL_CHANNEL_LSN;
    DSD_SNPRINTF(history[0].Event_History_Items[1].src_str, sizeof history[0].Event_History_Items[1].src_str, "%s",
                 "MUTATED");
    cc->count = 1;
    cc->candidates[0] = 999999999L;
    cc->added = 99U;
    assert(dsd_state_trunk_lcn_name_set(state, 0U, "MUTATED") == 0);
    assert(dsd_state_trunk_lcn_avoid_set(state, 0U, 1) == 0);
    state->lcn_on_air_row = 1;
    state->lcn_on_air_freq = 999L;
    state->lcn_back_retry_row = -1;
    state->lcn_back_retry_freq = 0L;

    const dsd_state* snap = dsd_app_get_latest_snapshot();
    assert(snap->tetra_pub.mode == DSD_TETRA_MODE_TMO_DOWNLINK && snap->tetra_pub.net_known == 1U);
    assert(snap->tetra_pub.mcc == 262U && snap->tetra_pub.traffic_slots == 0x0AU
           && snap->tetra_pub.codec_frames == 18U);
    assert(snap->tetra_pub.ksg_mask == 0x0003U && snap->tetra_pub.la_known == 1U && snap->tetra_pub.la == 4097U);
    assert(snap->tetra_pub.rx_carrier_known == 1U && snap->tetra_pub.rx_carrier == 654U);
    assert(snap->tetra_pub.key_kind == DSD_TETRA_KEY_TEA1_REDUCED && snap->tetra_pub.key_tea == 1U);
    assert(snap->tetra_pub.aie == DSD_TETRA_AIE_AVAILABLE && snap->tetra_pub.sec_known == 1U
           && snap->tetra_pub.sec_sckn == 3U && snap->tetra_pub.dmo_sckn == 5U);
    assert(snap->tetra_air_keys[0].loaded == 0U && snap->tetra_air_keys[0].key[0] == 0U);
    assert(snap->rx_id.family == DSD_RXID_FAMILY_P25 && snap->rx_id.epoch == rxid_epoch);
    assert(snap->rx_id.p25_wacn == 0xBEE00U && snap->rx_id.p25_sysid == 0x348U && snap->rx_id.p25_rfss == 3U
           && snap->rx_id.p25_site == 21U && snap->rx_id.cc_freq_hz == 851012500U);
    assert((snap->rx_id.valid & (DSD_RXID_P25_WACN | DSD_RXID_P25_SITE | DSD_RXID_CC_FREQ))
           == (DSD_RXID_P25_WACN | DSD_RXID_P25_SITE | DSD_RXID_CC_FREQ));
    assert_slot_tail(snap, 123U, 456U);
    assert(strcmp(snap->event_history_s[0].Event_History_Items[1].src_str, "RADIO-123") == 0);
    assert(memcmp(&snap->event_history_s[0].Event_History_Items[1].site_id, &row_site, sizeof row_site) == 0);
    assert(snap->event_history_s[0].Event_History_Items[1].channel == 12U
           && snap->event_history_s[0].Event_History_Items[1].call_channel_kind == (uint8_t)DSD_CALL_CHANNEL_LPCN);
    assert(snap->event_history_s[0].Event_History_Items[0].policy_target_id == 123U);
    assert(snap->event_history_s[0].Event_History_Items[0].stream_blocked == 1U);
    assert(snap->event_history_s[1].Event_History_Items[0].policy_target_id == 0U);
    assert(snap->event_history_s[1].Event_History_Items[0].stream_blocked == 0U);
    /* A shared pointer would let the next reserve() on either side realloc the other's
       buffer, and would republish the live mutation above as if it had been snapshotted. */
    assert(snap->trunk_lcn_name != NULL);
    assert(snap->trunk_lcn_name != state->trunk_lcn_name);
    assert(snap->trunk_lcn_avoid != NULL);
    assert(snap->trunk_lcn_avoid != state->trunk_lcn_avoid);
    assert_render_fields(snap);
    assert_cc_candidates(snap);
    dsd_call_snapshot call;
    assert(dsd_call_state_get(snap, 0U, &call) == 1);
    assert(call.ota_source_id == 654U);
    assert(call.crypto == DSD_CALL_CRYPTO_DECRYPTABLE);
    assert(call.algid == 0x84U);
    dsd_recent_activity_snapshot recent;
    assert(dsd_recent_activity_copy_snapshot(snap, &recent) == 1);
    assert(strcmp(recent.entries[0].notice, "Active Ch: 1234 TG: 321; ") == 0);
    assert_history_copy_counts(1U, 1U, 1U, 1U);

    // Repeated publications with unchanged revisions do not copy either history slot.
    dsd_app_snapshot_test_reset_event_history_copy_counts();
    dsd_app_telemetry_publish_snapshot(state);
    assert(dsd_app_get_latest_snapshot()->tetra_pub.mode == DSD_TETRA_MODE_DMO_DIRECT);
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

    for (int alias_version = 0; alias_version < 2; ++alias_version) {
        dsd_source_alias_store* aliases = dsd_source_alias_store_create();
        assert(aliases);
        dsd_source_alias_entry alias = {.id_start = 7777, .id_end = 7777};
        DSD_SNPRINTF(alias.name, sizeof(alias.name), "Unit %d", alias_version);
        assert(dsd_source_alias_store_append(aliases, &alias) == 0);
        dsd_source_alias_install(state, aliases);
        dsd_app_telemetry_publish_snapshot(state);
        snap = dsd_app_get_latest_snapshot();
        char alias_name[50];
        assert(dsd_source_label_lookup(snap, 7777, NULL, 0, alias_name, sizeof(alias_name)));
        assert(strcmp(alias_name, alias.name) == 0);
    }
    assert(dsd_source_alias_clear(state) == 0);
    dsd_app_telemetry_publish_snapshot(state);
    snap = dsd_app_get_latest_snapshot();
    assert(!dsd_source_alias_loaded(snap));
    char alias_name[50];
    assert(dsd_source_label_lookup(snap, 7777, NULL, 0, alias_name, sizeof(alias_name)));
    assert(strcmp(alias_name, "POLICY-ONLY") == 0);

    /* A cleared channel map must not leave the previous map's names in the snapshot. */
    dsd_state_trunk_lcn_name_free(state);
    dsd_state_trunk_lcn_avoid_free(state);
    dsd_app_telemetry_publish_snapshot(state);
    snap = dsd_app_get_latest_snapshot();
    assert(strcmp(dsd_state_trunk_lcn_name_get(snap, 0U), "") == 0);
    assert(strcmp(dsd_state_trunk_lcn_name_get(snap, 1U), "") == 0);
    assert(snap->trunk_lcn_avoid == NULL);
    assert(dsd_state_trunk_lcn_avoid_get(snap, 1U) == 0);

    /* This publisher also feeds the notification record the Android foreground service
       reads with no Qt in the picture. The fan-out sits at the tail of
       dsd_app_telemetry_publish_snapshot() and had no coverage anywhere: deleting the call
       left the whole suite green while the notification went permanently blank. The
       publishers stay dormant until a reader has asked once, so arm them first. */
    dsd_app_notification_status notification;
    (void)dsd_app_notification_get(&notification);
    state->synctype = DSD_SYNC_P25P2_POS;
    state->trunk_cc_freq = 851006250L;
    dsd_app_telemetry_publish_snapshot(state);
    assert(dsd_app_notification_get(&notification) == 1);
    assert(strcmp(notification.protocol, "P25p2") == 0);
    assert(notification.cc_freq_hz == 851006250);

    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    assert(opts);
    assert(dsd_apply_decode_mode_preset(DSDCFG_MODE_NXDN48, DSD_DECODE_PRESET_PROFILE_CLI, opts, state) == 0);
    assert(dsd_scan_mode_enter(opts, state, DSD_SCAN_MODE_P25) == 0);
    dsd_app_telemetry_publish_snapshot(state);
    snap = dsd_app_get_latest_snapshot();
    assert(dsd_scan_mode_active(snap) == DSD_SCAN_MODE_P25);
    assert(snap->state_ext[DSD_STATE_EXT_RUNTIME_SCAN_MODE] != state->state_ext[DSD_STATE_EXT_RUNTIME_SCAN_MODE]);
    dsd_scan_settings settings;
    dsd_app_snapshot_configured_mode(opts, snap, &settings);
    assert(settings.frame_nxdn48 == 1 && settings.frame_p25p1 == 0);
    dsd_scan_mode_leave(opts, state);
    assert(dsd_scan_mode_active(snap) == DSD_SCAN_MODE_P25);
    dsd_app_telemetry_publish_snapshot(state);
    snap = dsd_app_get_latest_snapshot();
    assert(dsd_scan_mode_active(snap) == DSD_SCAN_MODE_INHERIT);
    free(opts);

    test_publish_holds_the_tick_guard(state);
    test_publish_puts_off_while_a_tick_holds_the_guard(state);
    test_scan_list_completeness_across_hops();
    test_trunk_scan_avoids_gated_copy();
    test_chan_map_gate_keys_on_the_session();

    puts("UI_SNAPSHOT_EVENT_HISTORY: OK");
    dsd_state_ext_free_all(state);
    dsd_state_trunk_lcn_free(state);
    free(replacement);
    free(history);
    free(state);
    return 0;
}
