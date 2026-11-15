// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */
/**
 * @file
 * @brief The unit activity ring through both snapshot hops (decoder -> published -> consumer): copied only when its
 * revision moved and then only the rows revised since, a failed copy at either hop retried without a reader's cursor
 * moving, and a new decoder session read as a reset.
 */
#include <assert.h>
#include <dsd-neo/app_control/snapshot.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/unit_activity.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "../../src/app_control/snapshot_internal.h"
#include "dsd-neo/core/call_state.h"

/* The event history copy lives in dsd_events.c, which this test does not link; no state here has a history. */
int
dsd_event_state_copy_snapshot_incremental(dsd_state* dst, const dsd_state* src, Event_History_I event_history[2],
                                          const uint64_t source_revisions[2], int force_copy, uint8_t copied[2]) {
    (void)event_history;
    (void)source_revisions;
    (void)force_copy;
    (void)dsd_call_state_copy_to_state(dst, src);
    copied[0] = 0U;
    copied[1] = 0U;
    return 0;
}

int
dsd_event_state_copy_snapshot(dsd_state* dst, const dsd_state* src, Event_History_I event_history[2]) {
    uint8_t copied[2];
    return dsd_event_state_copy_snapshot_incremental(dst, src, event_history, NULL, 1, copied);
}

static const uint64_t T0_MS = UINT64_C(1700000000000);

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
    dsd_state_trunk_scan_entry_reasons_free(st);
    free(st);
}

/* A P25 group affiliation response for made-up RID @p rid joining talkgroup 4001 on WACN 0xBEE00 / SYS 0x123. */
static void
push_rid(dsd_state* st, uint32_t rid, int want) {
    dsd_unit_activity row;
    DSD_MEMSET(&row, 0, sizeof(row));
    row.protocol = DSD_UA_PROTO_P25P1;
    row.kind = DSD_UA_GROUP_AFFILIATION;
    row.role = DSD_UA_ROLE_RESPONSE;
    row.result = DSD_UA_RESULT_ACCEPTED;
    row.subject.id = rid;
    row.subject.subtype = DSD_UA_ID_RADIO;
    row.subject.valid = 1U;
    row.group = 4001U;
    row.group_valid = 1U;
    row.scope.kind = DSD_UA_SCOPE_P25;
    row.scope.valid = 1U;
    row.scope.a = 0xBEE00U;
    row.scope.b = 0x123U;
    assert(dsd_unit_activity_push(st, &row) == want);
}

/* A frontend reading the consumer snapshot: its cursor and what it has seen, in arrival order. */
typedef struct {
    uint64_t instance, rev;
    uint32_t rids[64];
    int count;
    uint8_t reset, lost, held;
} reader;

static int
reader_poll(reader* rd) {
    const dsd_state* snap = dsd_app_get_latest_snapshot();
    assert(snap != NULL);
    dsd_unit_activity rows[4];
    dsd_unit_activity_read_meta meta;
    int got = 0;
    rd->reset = rd->lost = 0U;
    do {
        const int n = dsd_unit_activity_read(snap, rd->instance, rd->rev, rows, 4, &meta);
        assert(n >= 0);
        rd->reset |= meta.reset;
        rd->lost |= meta.lost;
        rd->held = meta.held;
        if (meta.reset) {
            rd->count = 0;
        }
        for (int i = 0; i < n; i++) {
            assert(rd->count < 64);
            rd->rids[rd->count++] = rows[i].subject.id;
        }
        rd->instance = meta.instance;
        rd->rev = meta.last_rev;
        got += n;
    } while (meta.more);
    return got;
}

static void
publish(const dsd_state* st) {
    dsd_app_telemetry_publish_snapshot(st);
}

/* The published copy fails to allocate its ring: the consumer keeps nothing and the reader keeps its (empty) cursor,
 * even when the consumer's own retry runs; the next publication copies whole. */
static void
test_published_copy_failure_is_retried(dsd_state* st, reader* rd) {
    push_rid(st, 1U, 1);
    push_rid(st, 2U, 1);
    push_rid(st, 3U, 1);
    dsd_unit_activity_test_alloc_fail_after(0);
    publish(st);
    dsd_unit_activity_test_alloc_fail_after(-1);
    assert(reader_poll(rd) == 0 && rd->held == 0U && rd->instance == 0U && rd->rev == 0U);
    assert(reader_poll(rd) == 0 && rd->held == 0U); /* the consumer's retry: its source is not whole yet */

    const uint64_t copied = dsd_unit_activity_test_rows_copied();
    publish(st);
    assert(reader_poll(rd) == 3 && rd->reset == 1U && rd->lost == 0U);
    assert(rd->rids[0] == 1U && rd->rids[1] == 2U && rd->rids[2] == 3U);
    assert(dsd_unit_activity_test_rows_copied() - copied == 6U); /* three rows, each hop */
}

/* Only a moved revision copies, and then only the rows revised since: the reader goes on in revision order. */
static void
test_hops_copy_only_changed_rows(dsd_state* st, reader* rd) {
    uint64_t copied = dsd_unit_activity_test_rows_copied();
    publish(st);
    assert(reader_poll(rd) == 0 && rd->held == 1U);
    assert(dsd_unit_activity_test_rows_copied() == copied);

    dsd_decode_clock_test_set_ns((T0_MS + 300U) * UINT64_C(1000000));
    push_rid(st, 1U, 0); /* a repeat of the first row merges */
    push_rid(st, 4U, 1);
    publish(st);
    assert(reader_poll(rd) == 2 && rd->reset == 0U && rd->lost == 0U);
    assert(rd->count == 5 && rd->rids[3] == 1U && rd->rids[4] == 4U); /* row 1 again: an update of seq 1 */
    assert(dsd_unit_activity_test_rows_copied() - copied == 4U);

    /* Several publications before one read: the consumer copies once, the rows of all of them. */
    copied = dsd_unit_activity_test_rows_copied();
    push_rid(st, 5U, 1);
    publish(st);
    push_rid(st, 6U, 1);
    publish(st);
    assert(reader_poll(rd) == 2 && rd->rids[5] == 5U && rd->rids[6] == 6U);
    assert(dsd_unit_activity_test_rows_copied() - copied == 4U); /* 1 + 1 published, 2 consumed */
}

/* The published copy loses its ring (a source without one) and then fails to allocate one while the consumer still
 * holds rows: the consumer keeps them, as its source is not whole, and the reader reads nothing new; the next
 * publication brings the row the failed one missed. */
static void
test_published_failure_keeps_consumer_copy(dsd_state* st, reader* rd) {
    dsd_state* empty = new_session_state();
    publish(empty);
    free_session_state(empty);
    const uint64_t instance = rd->instance;
    const uint64_t rev = rd->rev;

    push_rid(st, 7U, 1);
    dsd_unit_activity_test_alloc_fail_after(0);
    publish(st);
    dsd_unit_activity_test_alloc_fail_after(-1);
    assert(reader_poll(rd) == 0 && rd->held == 1U && rd->instance == instance && rd->rev == rev);
    publish(st);
    assert(reader_poll(rd) == 1 && rd->reset == 0U && rd->lost == 0U && rd->rids[rd->count - 1] == 7U);
}

/* The consumer's copy fails: the reader reads nothing new and keeps its cursor; the next read retries without a new
 * publication and the reader goes on from its cursor. */
static void
test_consumer_copy_failure_is_retried(dsd_state* st, reader* rd) {
    /* Drop the copies' rings first (a source without a ring takes them away), so the next copy allocates. */
    dsd_state* empty = new_session_state();
    publish(empty);
    assert(reader_poll(rd) == 0 && rd->held == 0U);
    free_session_state(empty);
    const uint64_t instance = rd->instance;
    const uint64_t rev = rd->rev;

    push_rid(st, 8U, 1);
    dsd_unit_activity_test_alloc_fail_after(1); /* the published copy allocates, the consumer's fails */
    publish(st);
    assert(reader_poll(rd) == 0 && rd->held == 0U && rd->instance == instance && rd->rev == rev);
    dsd_unit_activity_test_alloc_fail_after(-1);
    assert(reader_poll(rd) == 1 && rd->reset == 0U && rd->lost == 0U && rd->rids[rd->count - 1] == 8U);
}

/* A new decoder session's ring is another instance: copied whole, and read as a reset from its start. */
static void
test_new_session_resets(reader* rd) {
    dsd_state* next = new_session_state();
    push_rid(next, 40U, 1);
    push_rid(next, 41U, 1);
    const uint64_t old_instance = rd->instance;
    publish(next);
    assert(reader_poll(rd) == 2 && rd->reset == 1U && rd->instance != old_instance);
    assert(rd->count == 2 && rd->rids[0] == 40U && rd->rids[1] == 41U);
    free_session_state(next);
}

int
main(void) {
    dsd_decode_clock_use_test(T0_MS * UINT64_C(1000000));
    dsd_state* st = new_session_state();
    reader rd = {0};
    test_published_copy_failure_is_retried(st, &rd);
    test_hops_copy_only_changed_rows(st, &rd);
    test_published_failure_keeps_consumer_copy(st, &rd);
    test_consumer_copy_failure_is_retried(st, &rd);
    test_new_session_resets(&rd);
    free_session_state(st);
    dsd_decode_clock_use_system();
    printf("APP_CONTROL_SNAPSHOT_UNIT_ACTIVITY: OK\n");
    return 0;
}
