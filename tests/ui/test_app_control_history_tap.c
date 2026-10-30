// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Issue #708: the History tap (dsd_app_history_tap_copy()), the second reader of the published event history. Its
 * cursor copies on the first read, on a commit and on a new ring with the same commit count, never on a staged-row
 * render alone; its options fields follow the options snapshot; and it neither consumes nor writes the single
 * consumer's buffers. A publisher racing two taps and the consumer (run under the tsan preset too) never shows a tap a
 * torn ring, and every tap ends on the last commit.
 */

#include <assert.h>
#include <dsd-neo/app_control/history_tap.h>
#include <dsd-neo/app_control/snapshot.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/platform/threading.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "../../src/app_control/snapshot_internal.h"
#include "dsd-neo/core/safe_api.h"

/* The event layer's copy, as the snapshot tests stub it: a slot whose revision moved is copied whole. */
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

/* A commit: every row of the slot carries @p marker, so a copy torn between two commits shows two markers. */
static void
commit(Event_History_I* ring, uint32_t marker) {
    for (size_t i = 0; i < DSD_EVENT_HISTORY_LEN; i++) {
        ring->Event_History_Items[i].source_id = marker;
    }
    ring->revision++;
    ring->push_seq++;
    ring->commit_rev++;
}

/* A staged-row render: revision moves, commit_rev does not. */
static void
render_staged(Event_History_I* ring) {
    ring->Event_History_Items[0].target_id++;
    ring->revision++;
}

static void
assert_ring_is(const Event_History_I* ring, uint32_t marker) {
    for (size_t i = 0; i < DSD_EVENT_HISTORY_LEN; i++) {
        assert(ring->Event_History_Items[i].source_id == marker);
    }
}

static void
assert_consumer_copies(uint64_t slot0, uint64_t slot1) {
    dsd_app_snapshot_event_history_copy_counts counts;
    dsd_app_snapshot_test_get_event_history_copy_counts(&counts);
    assert(counts.published_to_consumer[0] == slot0);
    assert(counts.published_to_consumer[1] == slot1);
}

static void
publish_opts(dsd_opts* opts, int scanner_mode, int trunk_scan_enabled) {
    opts->scanner_mode = scanner_mode;
    opts->trunk_scan_enabled = trunk_scan_enabled;
    dsd_app_telemetry_publish_opts_snapshot(opts);
}

/* Nothing published yet: nothing to copy, and options read as unpublished. Runs first. */
static void
test_before_any_publish(Event_History_I* out) {
    dsd_app_history_tap_cursor cursor;
    DSD_MEMSET(&cursor, 0, sizeof(cursor));
    dsd_app_history_tap_meta meta;
    DSD_MEMSET(&meta, 0x5A, sizeof(meta));
    out->Event_History_Items[1].source_id = 0xDEADU;
    assert(dsd_app_history_tap_copy(0, out, &cursor, &meta) == 0);
    assert(out->Event_History_Items[1].source_id == 0xDEADU);
    assert(cursor.read == 0);
    assert(meta.opts_published == 0);

    assert(dsd_app_history_tap_copy(-1, out, &cursor, &meta) == -1);
    assert(dsd_app_history_tap_copy(2, out, &cursor, &meta) == -1);
    assert(dsd_app_history_tap_copy(0, NULL, &cursor, &meta) == -1);
    assert(dsd_app_history_tap_copy(0, out, NULL, &meta) == -1);
    assert(dsd_app_history_tap_copy(0, out, &cursor, NULL) == 0);
}

/* The cursor rules: the first read copies whatever the ring holds, then only a commit or a new ring does. */
static void
test_cursor(dsd_state* state, Event_History_I* out) {
    Event_History_I* rings = state->event_history_s;
    rings[0].instance = 0x1111U;
    rings[1].instance = 0x1111U;
    dsd_app_telemetry_publish_snapshot(state);

    dsd_app_history_tap_cursor cursor;
    DSD_MEMSET(&cursor, 0, sizeof(cursor));
    out->Event_History_Items[1].source_id = 0xDEADU;
    /* commit_rev 0 and nothing read: still a copy. */
    assert(dsd_app_history_tap_copy(0, out, &cursor, NULL) == 1);
    assert(out->Event_History_Items[1].source_id == 0U);
    assert(cursor.read == 1);
    assert(cursor.instance == 0x1111U);
    assert(cursor.commit_rev == 0U);

    out->Event_History_Items[1].source_id = 0xDEADU;
    assert(dsd_app_history_tap_copy(0, out, &cursor, NULL) == 0);
    assert(out->Event_History_Items[1].source_id == 0xDEADU);

    render_staged(&rings[0]);
    dsd_app_telemetry_publish_snapshot(state);
    assert(dsd_app_history_tap_copy(0, out, &cursor, NULL) == 0);
    assert(out->Event_History_Items[1].source_id == 0xDEADU);

    commit(&rings[0], 7U);
    dsd_app_telemetry_publish_snapshot(state);
    assert(dsd_app_history_tap_copy(0, out, &cursor, NULL) == 1);
    assert_ring_is(out, 7U);
    assert(out->commit_rev == 1U);
    assert(cursor.commit_rev == 1U);
    assert(dsd_app_history_tap_copy(0, out, &cursor, NULL) == 0);

    /* Slot 2 has its own cursor: its first read copies, and slot 1's commits do not move it. */
    dsd_app_history_tap_cursor cursor1;
    DSD_MEMSET(&cursor1, 0, sizeof(cursor1));
    assert(dsd_app_history_tap_copy(1, out, &cursor1, NULL) == 1);
    assert_ring_is(out, 0U);
    commit(&rings[0], 8U);
    dsd_app_telemetry_publish_snapshot(state);
    assert(dsd_app_history_tap_copy(1, out, &cursor1, NULL) == 0);
    assert(dsd_app_history_tap_copy(0, out, &cursor, NULL) == 1);
    assert_ring_is(out, 8U);

    /* A new ring (another state, as a replay in a fresh state) read at the very commit count the old one stopped at. */
    Event_History_I* fresh = (Event_History_I*)calloc(2, sizeof(Event_History_I));
    assert(fresh != NULL);
    fresh[0] = rings[0];
    fresh[1] = rings[1];
    fresh[0].instance = 0x2222U;
    fresh[1].instance = 0x2222U;
    for (size_t i = 0; i < DSD_EVENT_HISTORY_LEN; i++) {
        fresh[0].Event_History_Items[i].source_id = 9U;
    }
    state->event_history_s = fresh;
    dsd_app_telemetry_publish_snapshot(state);
    assert(fresh[0].commit_rev == cursor.commit_rev);
    assert(dsd_app_history_tap_copy(0, out, &cursor, NULL) == 1);
    assert_ring_is(out, 9U);
    assert(cursor.instance == 0x2222U);
    rings[0] = fresh[0];
    rings[1] = fresh[1];
    state->event_history_s = rings;
    free(fresh);
    dsd_app_telemetry_publish_snapshot(state);
}

/* The tap leaves the frontend's single consumer alone: it does not consume a change the consumer has not read, does not
   write the consumer's buffer, and the consumer reading first does not consume the tap's change either. */
static void
test_independent_of_the_consumer(dsd_state* state, Event_History_I* out) {
    Event_History_I* rings = state->event_history_s;
    dsd_app_history_tap_cursor cursor;
    DSD_MEMSET(&cursor, 0, sizeof(cursor));
    (void)dsd_app_history_tap_copy(0, out, &cursor, NULL);

    const dsd_state* consumer = dsd_app_get_latest_snapshot();
    assert(consumer != NULL);
    const uint32_t consumer_marker = consumer->event_history_s[0].Event_History_Items[1].source_id;
    dsd_app_snapshot_test_reset_event_history_copy_counts();

    commit(&rings[0], 20U);
    dsd_app_telemetry_publish_snapshot(state);
    assert(dsd_app_history_tap_copy(0, out, &cursor, NULL) == 1);
    assert_ring_is(out, 20U);
    assert(consumer->event_history_s[0].Event_History_Items[1].source_id == consumer_marker);
    assert_consumer_copies(0U, 0U);
    consumer = dsd_app_get_latest_snapshot();
    assert_consumer_copies(1U, 0U);
    assert_ring_is(&consumer->event_history_s[0], 20U);

    commit(&rings[0], 21U);
    dsd_app_telemetry_publish_snapshot(state);
    consumer = dsd_app_get_latest_snapshot();
    assert_consumer_copies(2U, 0U);
    assert_ring_is(&consumer->event_history_s[0], 21U);
    assert(dsd_app_history_tap_copy(0, out, &cursor, NULL) == 1);
    assert_ring_is(out, 21U);
    assert_consumer_copies(2U, 0U);
}

/* The meta follows the published options, on a copy and without one, and leaves the consumer's copy alone. Which rows
   carry a saved system's identity is each row's own stamp (issue #671), so the meta carries no scan flags. */
static void
test_meta(dsd_state* state, dsd_opts* opts, Event_History_I* out) {
    dsd_app_history_tap_cursor cursor;
    DSD_MEMSET(&cursor, 0, sizeof(cursor));
    dsd_app_history_tap_meta meta;

    publish_opts(opts, 0, 0);
    const dsd_opts* consumer = dsd_app_get_latest_opts_snapshot();
    assert(consumer != NULL);
    assert(dsd_app_history_tap_copy(0, out, &cursor, &meta) == 1);
    assert(meta.opts_published == 1);

    publish_opts(opts, 1, 0);
    DSD_MEMSET(&meta, 0, sizeof(meta));
    assert(dsd_app_history_tap_copy(0, out, &cursor, &meta) == 0);
    assert(meta.opts_published == 1);
    assert(consumer->scanner_mode == 0);

    publish_opts(opts, 0, 1);
    commit(&state->event_history_s[0], 30U);
    dsd_app_telemetry_publish_snapshot(state);
    assert(dsd_app_history_tap_copy(0, out, &cursor, &meta) == 1);
    assert(meta.opts_published == 1);
    assert(consumer->trunk_scan_enabled == 0);
    consumer = dsd_app_get_latest_opts_snapshot();
    assert(consumer->trunk_scan_enabled == 1);
}

/* --- A publisher racing two taps and the single consumer. --- */

enum { RACE_COMMITS = 400 };

typedef struct {
    dsd_state* state;
    dsd_opts* opts;
    atomic_int done;
} race_publisher;

typedef struct {
    race_publisher* publisher;
    Event_History_I* out;
    uint64_t copies;
    uint64_t last_commit_rev[2];
    int torn;
    int backwards;
} race_tap;

static DSD_THREAD_RETURN_TYPE
race_publish(void* arg) {
    race_publisher* p = (race_publisher*)arg;
    for (uint32_t i = 1; i <= RACE_COMMITS; i++) {
        Event_History_I* rings = p->state->event_history_s;
        commit(&rings[i & 1U], (uint32_t)(rings[i & 1U].commit_rev + 1U));
        if ((i % 3U) == 0U) {
            render_staged(&rings[0]);
        }
        dsd_app_telemetry_publish_snapshot(p->state);
        if ((i % 50U) == 0U) {
            publish_opts(p->opts, (int)((i / 50U) & 1U), 0);
        }
    }
    atomic_store(&p->done, 1);
    DSD_THREAD_RETURN;
}

static void
race_check(race_tap* tap, int slot) {
    const uint32_t marker = (uint32_t)tap->out->commit_rev;
    for (size_t i = 0; i < DSD_EVENT_HISTORY_LEN; i++) {
        if (tap->out->Event_History_Items[i].source_id != marker) {
            tap->torn = 1;
        }
    }
    if (tap->out->commit_rev < tap->last_commit_rev[slot]) {
        tap->backwards = 1;
    }
    tap->last_commit_rev[slot] = tap->out->commit_rev;
    tap->copies++;
}

static DSD_THREAD_RETURN_TYPE
race_read(void* arg) {
    race_tap* tap = (race_tap*)arg;
    dsd_app_history_tap_cursor cursors[2];
    DSD_MEMSET(cursors, 0, sizeof(cursors));
    dsd_app_history_tap_meta meta;
    for (;;) {
        const int done = atomic_load(&tap->publisher->done);
        for (int slot = 0; slot < 2; slot++) {
            if (dsd_app_history_tap_copy(slot, tap->out, &cursors[slot], &meta) == 1) {
                race_check(tap, slot);
            }
        }
        if (done) {
            break;
        }
    }
    DSD_THREAD_RETURN;
}

static void
test_race(dsd_state* state, dsd_opts* opts) {
    Event_History_I* rings = state->event_history_s;
    DSD_MEMSET(rings, 0, 2U * sizeof(Event_History_I));
    rings[0].instance = 0x3333U;
    rings[1].instance = 0x3333U;
    dsd_app_telemetry_publish_snapshot(state);

    race_publisher publisher;
    publisher.state = state;
    publisher.opts = opts;
    atomic_store(&publisher.done, 0);
    race_tap taps[2];
    dsd_thread_t tap_threads[2];
    for (int t = 0; t < 2; t++) {
        DSD_MEMSET(&taps[t], 0, sizeof(taps[t]));
        taps[t].publisher = &publisher;
        taps[t].out = (Event_History_I*)calloc(1, sizeof(Event_History_I));
        assert(taps[t].out != NULL);
        assert(dsd_thread_create(&tap_threads[t], race_read, &taps[t]) == 0);
    }
    dsd_thread_t publisher_thread;
    assert(dsd_thread_create(&publisher_thread, race_publish, &publisher) == 0);
    /* The frontend's own polling, at the same time. */
    while (!atomic_load(&publisher.done)) {
        const dsd_state* consumer = dsd_app_get_latest_snapshot();
        assert(consumer != NULL);
        (void)dsd_app_get_latest_opts_snapshot();
    }
    assert(dsd_thread_join(publisher_thread) == 0);
    for (int t = 0; t < 2; t++) {
        assert(dsd_thread_join(tap_threads[t]) == 0);
        assert(taps[t].torn == 0);
        assert(taps[t].backwards == 0);
        assert(taps[t].copies > 0U);
        /* Each tap's last read came after the last publish: it ends on the last commit of both slots. */
        assert(taps[t].last_commit_rev[0] == RACE_COMMITS / 2U);
        assert(taps[t].last_commit_rev[1] == RACE_COMMITS / 2U);
        free(taps[t].out);
    }
    const dsd_state* consumer = dsd_app_get_latest_snapshot();
    assert(consumer->event_history_s[0].commit_rev == RACE_COMMITS / 2U);
    assert(consumer->event_history_s[1].commit_rev == RACE_COMMITS / 2U);
}

int
main(void) {
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    Event_History_I* rings = (Event_History_I*)calloc(2, sizeof(Event_History_I));
    Event_History_I* out = (Event_History_I*)calloc(1, sizeof(Event_History_I));
    assert(state != NULL && opts != NULL && rings != NULL && out != NULL);
    state->event_history_s = rings;

    test_before_any_publish(out);
    test_cursor(state, out);
    test_independent_of_the_consumer(state, out);
    test_meta(state, opts, out);
    test_race(state, opts);

    state->event_history_s = NULL;
    free(out);
    free(rings);
    free(opts);
    free(state);
    printf("APP_CONTROL_HISTORY_TAP: OK\n");
    return 0;
}
