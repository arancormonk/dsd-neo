// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */
/**
 * @file
 * @brief The published position ring and its cursor view (issue #800): an empty ring, partial drains, gaps after an
 * overrun at either hop, a stream restart read as a reset, a run's records kept after it ends, the clock sample and its
 * running flag (cleared by the final publish), copy counts, no allocation on either hop, and concurrent readers against
 * a publishing decoder.
 *
 * Every coordinate and ID here is made up.
 */
#include <assert.h>
#include <dsd-neo/app_control/position_view.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/position.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/exitflag.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "dsd-neo/core/call_state.h"
#include "position_ring.h"
#include "snapshot_internal.h"

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

/*============================================================================
 * Allocation counter: GNU ld --wrap, where it links (the CMake block sets DSD_TEST_POSITION_ALLOC_WRAP)
 *============================================================================*/

#ifdef DSD_TEST_POSITION_ALLOC_WRAP
static atomic_int g_count_allocs = 0;
static atomic_int g_allocs = 0;

// NOLINTNEXTLINE(bugprone-reserved-identifier,misc-use-internal-linkage)
void* __real_malloc(size_t size);
// NOLINTNEXTLINE(bugprone-reserved-identifier,misc-use-internal-linkage)
void* __real_calloc(size_t count, size_t size);
// NOLINTNEXTLINE(bugprone-reserved-identifier,misc-use-internal-linkage)
void* __real_realloc(void* ptr, size_t size);
// NOLINTNEXTLINE(bugprone-reserved-identifier,misc-use-internal-linkage)
void* __wrap_malloc(size_t size);
// NOLINTNEXTLINE(bugprone-reserved-identifier,misc-use-internal-linkage)
void* __wrap_calloc(size_t count, size_t size);
// NOLINTNEXTLINE(bugprone-reserved-identifier,misc-use-internal-linkage)
void* __wrap_realloc(void* ptr, size_t size);

void*
__wrap_malloc(size_t size) {
    if (atomic_load(&g_count_allocs)) {
        (void)atomic_fetch_add(&g_allocs, 1);
    }
    return __real_malloc(size);
}

void*
__wrap_calloc(size_t count, size_t size) {
    if (atomic_load(&g_count_allocs)) {
        (void)atomic_fetch_add(&g_allocs, 1);
    }
    return __real_calloc(count, size);
}

void*
__wrap_realloc(void* ptr, size_t size) {
    if (atomic_load(&g_count_allocs)) {
        (void)atomic_fetch_add(&g_allocs, 1);
    }
    return __real_realloc(ptr, size);
}

static void
allocs_begin(void) {
    atomic_store(&g_allocs, 0);
    atomic_store(&g_count_allocs, 1);
}

static int
allocs_end(void) {
    atomic_store(&g_count_allocs, 0);
    return atomic_load(&g_allocs);
}
#else
static void
allocs_begin(void) {}

static int
allocs_end(void) {
    return 0;
}
#endif

/*============================================================================
 * Helpers
 *============================================================================*/

static const uint64_t T0_MS = UINT64_C(1700000000000);
static const int32_t LAT_BASE = 100000000; /* 10.0 N, plus the report's number in 1e-7 degrees */

static dsd_state*
new_state(void) {
    dsd_state* st = (dsd_state*)calloc(1, sizeof(*st));
    assert(st != NULL);
    st->state_instance = dsd_state_rev_next();
    st->trunk_scan_avoids_ok = 1U;
    st->scan_avoids_complete = 1U;
    return st;
}

static void
free_state(dsd_state* st) {
    dsd_state_ext_free_all(st);
    dsd_state_trunk_lcn_free(st);
    dsd_state_trunk_scan_avoids_free(st);
    dsd_state_trunk_scan_entry_reasons_free(st);
    free(st);
}

/* Admit @p count reports from made-up radio 1234, each at its own latitude (so none is a repeat), numbered from
   @p first: report k lies at LAT_BASE + k. */
static void
admit(dsd_state* st, uint32_t first, uint32_t count) {
    for (uint32_t k = first; k < first + count; k++) {
        dsd_position_fix f;
        dsd_position_fix_init(&f);
        f.kind = DSD_POS_DMR_LRRP;
        f.protocol = DSD_SYNC_DMR_BS_DATA_POS;
        f.lat_e7 = LAT_BASE + (int32_t)k;
        f.lon_e7 = 200000000;
        f.source_id = 1234U;
        assert(dsd_position_admit(NULL, st, &f, 1, 1, NULL) == DSD_POSITION_ADMITTED_KEPT);
    }
}

static void
publish(const dsd_state* st) {
    dsd_app_telemetry_publish_snapshot(st);
}

static dsd_position_fix g_out[DSD_POSITION_FEED_CAPACITY];

/* Read everything after (stream, after) in pages of @p page; check order and the numbering admit() used (report k is
   seq k - @p first_report + 1 within its stream). Returns the records read; *cursor ends at the last read. */
static int
drain(uint64_t stream, uint64_t after, size_t page, uint32_t first_report, dsd_app_position_cursor* cursor) {
    int total = 0;
    uint64_t want = 0U;
    for (;;) {
        const int n = dsd_app_position_copy_since(stream, after, g_out, page, cursor);
        assert(n >= 0 && (size_t)n <= page);
        for (int i = 0; i < n; i++) {
            const uint64_t seq = g_out[i].seq;
            assert(want == 0U || seq == want);
            assert(g_out[i].stream_id == cursor->stream_id);
            assert(g_out[i].lat_e7 == LAT_BASE + (int32_t)(first_report + seq - 1U));
            want = seq + 1U;
        }
        total += n;
        stream = cursor->stream_id;
        after = cursor->last_seq;
        if (!cursor->more) {
            return total;
        }
        assert(n == (int)page);
    }
}

/*============================================================================
 * Cases
 *============================================================================*/

/* Before the first run's first publish: nothing, stream 0. */
static void
test_empty_before_first_publish(void) {
    dsd_app_position_cursor c;
    DSD_MEMSET(&c, 0x5A, sizeof c);
    assert(dsd_app_position_copy_since(0U, 0U, g_out, 8U, &c) == 0);
    assert(c.stream_id == 0U && c.last_seq == 0U && c.oldest_seq == 0U && c.newest_seq == 0U);
    assert(c.more == 0U && c.lost == 0U && c.reset == 0U && c.sample.stream_id == 0U && c.sample.running == 0U);
    assert(dsd_app_position_copy_since(0U, 0U, NULL, 0U, &c) == 0);
    assert(dsd_app_position_copy_since(0U, 0U, NULL, 1U, &c) == -1);
    assert(dsd_app_position_copy_since(0U, 0U, g_out, 1U, NULL) == -1);
}

/* A run whose ring is empty: its stream with nothing held, and a running sample on the decode clock. Then records,
   partial drains with too small a cap (more), and the copy counts of both hops. */
static void
test_partial_drains_and_counts(dsd_state* st, dsd_app_position_cursor* rd) {
    assert(dsd_position_feed_start(st) == 0);
    const uint64_t stream = dsd_position_feed_stream_id(st);
    publish(st);
    dsd_app_position_cursor c;
    assert(dsd_app_position_copy_since(0U, 0U, g_out, 8U, &c) == 0);
    assert(c.stream_id == stream && c.newest_seq == 0U && c.oldest_seq == 0U && c.reset == 0U);
    assert(c.sample.stream_id == stream && c.sample.running == 1U);
    assert(c.sample.clock_source == (uint8_t)DSD_DECODE_CLOCK_TEST);
    assert(c.sample.decode_now_s == (int64_t)(T0_MS / 1000U));

    dsd_app_position_test_reset_copy_counts();
    admit(st, 1U, 10U);
    /* Not published yet: readers see nothing new. */
    assert(dsd_app_position_copy_since(stream, 0U, g_out, 8U, &c) == 0 && c.newest_seq == 0U);
    publish(st);
    dsd_app_position_copy_counts counts;
    dsd_app_position_test_get_copy_counts(&counts);
    assert(counts.published == 10U && counts.read == 0U);

    /* Pages of 3: 3, 3, 3, 1, each in order, more set until the last. */
    assert(dsd_app_position_copy_since(stream, 0U, g_out, 3U, &c) == 3);
    assert(c.more == 1U && c.last_seq == 3U && c.oldest_seq == 1U && c.newest_seq == 10U && c.lost == 0U);
    assert(g_out[0].seq == 1U && g_out[2].seq == 3U && g_out[2].lat_e7 == LAT_BASE + 3);
    assert(drain(stream, 3U, 3U, 1U, &c) == 7);
    assert(c.last_seq == 10U && c.more == 0U);
    /* Caught up: nothing, the cursor stays. */
    assert(dsd_app_position_copy_since(stream, 10U, g_out, 3U, &c) == 0 && c.last_seq == 10U && c.more == 0U);
    /* A cursor read alone (cap 0) says more remain and copies nothing. */
    assert(dsd_app_position_copy_since(stream, 4U, NULL, 0U, &c) == 0 && c.more == 1U && c.last_seq == 4U);
    dsd_app_position_test_get_copy_counts(&counts);
    assert(counts.published == 10U && counts.read == 10U);

    /* A publish with nothing new copies nothing at the publish hop. */
    publish(st);
    dsd_app_position_test_get_copy_counts(&counts);
    assert(counts.published == 10U);
    /* A second reader with its own cursor reads the same records. */
    assert(drain(0U, 0U, 64U, 1U, rd) == 10 && rd->last_seq == 10U && rd->reset == 0U);
}

/* Gaps: the decoder overruns its ring between two publishes (the publish hop copies only what it still holds), and a
   reader falls behind the published ring. Either way the cursor says lost and reads on from the oldest held. */
static void
test_gaps(dsd_state* st, dsd_app_position_cursor* rd) {
    const uint64_t stream = dsd_position_feed_stream_id(st);
    /* Reports 11..1040 (seqs 11..1040): the live ring keeps 17..1040. */
    admit(st, 11U, 1030U);
    dsd_app_position_test_reset_copy_counts();
    publish(st);
    dsd_app_position_copy_counts counts;
    dsd_app_position_test_get_copy_counts(&counts);
    assert(counts.published == (uint64_t)DSD_POSITION_FEED_CAPACITY);

    dsd_app_position_cursor c;
    /* The reader at 10 missed 11..16. */
    assert(dsd_app_position_copy_since(stream, rd->last_seq, g_out, 4U, &c) == 4);
    assert(c.lost == 1U && c.oldest_seq == 17U && c.newest_seq == 1040U && g_out[0].seq == 17U);
    assert(g_out[0].lat_e7 == LAT_BASE + 17);
    /* A reader exactly one before the oldest lost nothing. */
    assert(dsd_app_position_copy_since(stream, 16U, g_out, 4U, &c) == 4 && c.lost == 0U && g_out[0].seq == 17U);
    /* The whole ring, across its wrap, in order. */
    assert(drain(stream, 16U, (size_t)DSD_POSITION_FEED_CAPACITY, 1U, &c) == DSD_POSITION_FEED_CAPACITY);
    assert(c.last_seq == 1040U);
    *rd = c;

    /* A reader far behind the published ring: 1041..1100 published in two steps push the oldest to 77. */
    admit(st, 1041U, 30U);
    publish(st);
    admit(st, 1071U, 30U);
    publish(st);
    assert(dsd_app_position_copy_since(stream, 50U, g_out, 2U, &c) == 2);
    assert(c.lost == 1U && c.oldest_seq == 77U && c.newest_seq == 1100U && g_out[0].seq == 77U && g_out[1].seq == 78U);
    assert(drain(stream, rd->last_seq, 16U, 1U, rd) == 60 && rd->last_seq == 1100U && rd->lost == 0U);
}

/* A new run: the records of the run that ended stay until the new run's first publish, a publish with no live stream
   keeps them, and a reader of the old stream then reads the new stream from its start as a reset. */
static void
test_stream_restart(dsd_state* st, dsd_app_position_cursor* rd) {
    const uint64_t old_stream = dsd_position_feed_stream_id(st);
    admit(st, 1101U, 5U); /* kept by the old stream, never published */

    /* The decoder between runs publishes a state with no ring: the old stream's published records stay. */
    dsd_state* idle = new_state();
    publish(idle);
    free_state(idle);
    dsd_app_position_cursor c;
    assert(dsd_app_position_copy_since(old_stream, 1090U, g_out, 64U, &c) == 10);
    assert(c.stream_id == old_stream && c.reset == 0U && c.sample.stream_id == 0U && c.sample.running == 0U);

    /* The next run: a new stream, three records, then its first publish. */
    assert(dsd_position_feed_start(st) == 0);
    const uint64_t new_stream = dsd_position_feed_stream_id(st);
    assert(new_stream != 0U && new_stream != old_stream);
    admit(st, 1U, 3U);
    publish(st);
    assert(dsd_app_position_copy_since(old_stream, rd->last_seq, g_out, 64U, &c) == 3);
    assert(c.reset == 1U && c.lost == 0U && c.stream_id == new_stream && c.last_seq == 3U);
    assert(g_out[0].seq == 1U && g_out[0].stream_id == new_stream && g_out[0].lat_e7 == LAT_BASE + 1);
    assert(c.sample.stream_id == new_stream && c.sample.running == 1U);
    *rd = c;
    /* The new stream's reader is no reset. */
    assert(dsd_app_position_copy_since(new_stream, 3U, g_out, 64U, &c) == 0 && c.reset == 0U);
    /* A reader of the old stream after the new one overran its first records: reset and lost. */
    admit(st, 4U, (uint32_t)DSD_POSITION_FEED_CAPACITY);
    publish(st);
    assert(dsd_app_position_copy_since(old_stream, 1100U, g_out, 1U, &c) == 1);
    assert(c.reset == 1U && c.lost == 1U && g_out[0].seq == 4U && c.newest_seq == 3U + DSD_POSITION_FEED_CAPACITY);
    assert(drain(new_stream, rd->last_seq, 128U, 1U, rd) == DSD_POSITION_FEED_CAPACITY);
}

/* The sample: taken at each publish on the decode clock; the run's final publish (the engine sets the exit flag first)
   clears running, and a replay paused between publishes keeps its decode time. */
static void
test_sample_running(dsd_state* st) {
    const uint64_t stream = dsd_position_feed_stream_id(st);
    dsd_decode_clock_test_set_ns((T0_MS + 42000U) * UINT64_C(1000000));
    publish(st);
    dsd_app_position_cursor c;
    (void)dsd_app_position_copy_since(stream, 0U, NULL, 0U, &c);
    assert(c.sample.stream_id == stream && c.sample.running == 1U);
    assert(c.sample.decode_now_s == (int64_t)(T0_MS / 1000U) + 42);

    admit(st, 2000U, 1U); /* a late report the final publish carries */
    dsd_exitflag_store(1);
    publish(st);
    dsd_exitflag_store(0);
    assert(dsd_app_position_copy_since(stream, 0U, NULL, 0U, &c) == 0);
    assert(c.sample.stream_id == stream && c.sample.running == 0U && c.newest_seq == 4U + DSD_POSITION_FEED_CAPACITY);
    /* The run's state is freed: publishing with no ring keeps running clear and the records held. */
    dsd_state* idle = new_state();
    publish(idle);
    free_state(idle);
    assert(dsd_app_position_copy_since(stream, 0U, NULL, 0U, &c) == 0);
    assert(c.sample.running == 0U && c.stream_id == stream && c.newest_seq == 4U + DSD_POSITION_FEED_CAPACITY);
}

/* Neither hop allocates: a publish carrying a burst of records allocates exactly what one carrying none does (the
   other stores the publish copies are unchanged), and a read allocates nothing. Counted where GNU ld --wrap links. */
static void
test_no_allocation(dsd_state* st) {
    const uint64_t stream = dsd_position_feed_stream_id(st);
    publish(st);
    allocs_begin();
    publish(st);
    const int without = allocs_end();
    admit(st, 3000U, 500U);
    allocs_begin();
    publish(st);
    const int with = allocs_end();
    assert(with == without);
    dsd_app_position_cursor c;
    allocs_begin();
    const int n = dsd_app_position_copy_since(stream, 0U, g_out, (size_t)DSD_POSITION_FEED_CAPACITY, &c);
    assert(allocs_end() == 0);
    assert(n == DSD_POSITION_FEED_CAPACITY);
#ifdef DSD_TEST_POSITION_ALLOC_WRAP
    /* The counter counts: a plain allocation is seen. The pointer goes through a volatile object, because an optimizer
       may delete a malloc/free pair whose result is never used (clang does at -O2), and then nothing is counted. */
    allocs_begin();
    void* volatile p = malloc(16);
    assert(allocs_end() == 1);
    free(p);
#endif
}

/*============================================================================
 * Concurrent readers
 *============================================================================*/

enum { READERS = 3, ROUNDS = 400, PER_ROUND = 7 };

typedef struct {
    uint64_t stream;
    uint64_t want_last;
    atomic_int* stop;
    int failures;
    uint64_t read;
    dsd_position_fix page[5];
} reader_ctx;

/* Follow the published ring in small pages: every record in seq order and whole (its latitude is its report's), a gap
   only where the cursor says lost, until the decoder has stopped and the last record is read. */
static DSD_THREAD_RETURN_TYPE
reader_main(void* arg) {
    reader_ctx* ctx = (reader_ctx*)arg;
    dsd_position_fix* page = ctx->page;
    uint64_t stream = ctx->stream;
    uint64_t after = 0U;
    for (;;) {
        dsd_app_position_cursor c;
        const int n = dsd_app_position_copy_since(stream, after, page, 5U, &c);
        if (n < 0 || c.stream_id != ctx->stream || c.reset) {
            ctx->failures++;
            break;
        }
        for (int i = 0; i < n; i++) {
            const int contiguous = page[i].seq == after + 1U || (i == 0 && c.lost && page[i].seq > after + 1U);
            if (!contiguous || page[i].lat_e7 != LAT_BASE + (int32_t)(5000U + page[i].seq - 1U)) {
                ctx->failures++;
            }
            after = page[i].seq;
        }
        ctx->read += (uint64_t)n;
        stream = c.stream_id;
        if (after == ctx->want_last) {
            break;
        }
        if (n == 0) {
            if (atomic_load(ctx->stop) && after == c.newest_seq) {
                break;
            }
            dsd_thread_yield();
        }
    }
    DSD_THREAD_RETURN;
}

/* Readers on their own threads while the decoder thread admits and publishes: each reads every record in order (or a
   gap the cursor names) without disturbing the others. */
static void
test_concurrent_readers(void) {
    dsd_state* st = new_state();
    assert(dsd_position_feed_start(st) == 0);
    const uint64_t stream = dsd_position_feed_stream_id(st);
    publish(st);

    atomic_int stop = 0;
    reader_ctx ctx[READERS];
    dsd_thread_t threads[READERS];
    for (int r = 0; r < READERS; r++) {
        DSD_MEMSET(&ctx[r], 0, sizeof ctx[r]);
        ctx[r].stream = stream;
        ctx[r].want_last = (uint64_t)ROUNDS * PER_ROUND;
        ctx[r].stop = &stop;
        assert(dsd_thread_create(&threads[r], reader_main, &ctx[r]) == 0);
    }
    for (uint32_t round = 0; round < ROUNDS; round++) {
        admit(st, 5000U + round * PER_ROUND, PER_ROUND);
        publish(st);
    }
    atomic_store(&stop, 1);
    for (int r = 0; r < READERS; r++) {
        assert(dsd_thread_join(threads[r]) == 0);
        assert(ctx[r].failures == 0 && ctx[r].read > 0U);
    }
    free_state(st);
}

/*============================================================================
 * The publish hop's cost, alone and under contending readers
 *============================================================================*/

enum { TIMING_BLOCKS = 4, TIMING_ROUNDS = 100 };

/* A reader that only contends for the snapshot lock: reads everything new in small pages until stopped. */
static DSD_THREAD_RETURN_TYPE
contender_main(void* arg) {
    reader_ctx* ctx = (reader_ctx*)arg;
    uint64_t stream = 0U;
    uint64_t after = 0U;
    while (!atomic_load(ctx->stop)) {
        dsd_app_position_cursor c;
        const int n = dsd_app_position_copy_since(stream, after, ctx->page, 5U, &c);
        if (n < 0) {
            ctx->failures++;
            break;
        }
        stream = c.stream_id;
        after = c.last_seq;
        if (n == 0) {
            dsd_thread_yield();
        }
    }
    DSD_THREAD_RETURN;
}

/* One block of publishes of PER_ROUND new records each; the summed publish time in ns. */
static uint64_t
timed_block(dsd_state* st, uint32_t* next_report) {
    uint64_t ns = 0U;
    for (int round = 0; round < TIMING_ROUNDS; round++) {
        admit(st, *next_report, PER_ROUND);
        *next_report += PER_ROUND;
        const uint64_t t0 = dsd_realtime_mono_ns();
        publish(st);
        ns += dsd_realtime_mono_ns() - t0;
    }
    return ns;
}

/* Informational: after a warm-up block, quiet and contended blocks alternate on one stream, so neither side carries a
   cold-cache or ordering bias. The whole publish path (history + API + positions) is measured by the PR's performance
   run (spec 4.9). */
static void
measure_publish_hop(void) {
    dsd_state* st = new_state();
    assert(dsd_position_feed_start(st) == 0);
    publish(st);
    uint32_t next_report = 20000U;
    (void)timed_block(st, &next_report); /* warm-up */
    uint64_t quiet_ns = 0U;
    uint64_t contended_ns = 0U;
    for (int block = 0; block < TIMING_BLOCKS; block++) {
        quiet_ns += timed_block(st, &next_report);
        atomic_int stop = 0;
        reader_ctx ctx[READERS];
        dsd_thread_t threads[READERS];
        for (int r = 0; r < READERS; r++) {
            DSD_MEMSET(&ctx[r], 0, sizeof ctx[r]);
            ctx[r].stop = &stop;
            assert(dsd_thread_create(&threads[r], contender_main, &ctx[r]) == 0);
        }
        contended_ns += timed_block(st, &next_report);
        atomic_store(&stop, 1);
        for (int r = 0; r < READERS; r++) {
            assert(dsd_thread_join(threads[r]) == 0);
            assert(ctx[r].failures == 0);
        }
    }
    const double publishes = (double)TIMING_BLOCKS * TIMING_ROUNDS;
    printf("position publish hop, %d records per publish: %.1f us alone, %.1f us under %d readers\n", PER_ROUND,
           (double)quiet_ns / publishes / 1000.0, (double)contended_ns / publishes / 1000.0, READERS);
    free_state(st);
}

int
main(void) {
    dsd_decode_clock_use_test(T0_MS * UINT64_C(1000000));
    test_empty_before_first_publish();
    dsd_state* st = new_state();
    dsd_app_position_cursor rd;
    DSD_MEMSET(&rd, 0, sizeof rd);
    test_partial_drains_and_counts(st, &rd);
    test_gaps(st, &rd);
    test_stream_restart(st, &rd);
    test_sample_running(st);
    test_no_allocation(st);
    free_state(st);
    test_concurrent_readers();
    measure_publish_hop();
    dsd_decode_clock_use_system();
    printf("APP_CONTROL_POSITION_VIEW: OK\n");
    return 0;
}
