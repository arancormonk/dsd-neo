// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The Signal search avoid lists (issue #828): the store writes the permanent list, the engine the session list, and
 * every reader takes a list whole with the revision it belongs to. A revision moves only when its list changes.
 */

#include <assert.h>
#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/runtime/signal_search.h>
#include <dsd-neo/runtime/signal_search_avoids.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

static uint32_t out[DSD_SEARCH_MAX_AVOIDS + 8];

/* Both lists start empty at revision 0. */
static void
test_starts_empty(void) {
    uint64_t revision = 7, session = 7;
    assert(dsd_signal_search_avoids_permanent_revision() == 0);
    assert(dsd_signal_search_avoids_session_revision() == 0);
    assert(dsd_signal_search_avoids_permanent_snapshot(out, DSD_SEARCH_MAX_AVOIDS, &revision) == 0 && revision == 0);
    assert(dsd_signal_search_avoids_session_get(&session, out, DSD_SEARCH_MAX_AVOIDS) == 0 && session == 0);
    /* Setting what is already there is no change. */
    dsd_signal_search_avoids_permanent_set(NULL, 0);
    dsd_signal_search_avoids_session_set(0, NULL, 0);
    assert(dsd_signal_search_avoids_permanent_revision() == 0);
    assert(dsd_signal_search_avoids_session_revision() == 0);
}

/* 0 and repeats are dropped, order is kept, and only the first DSD_SEARCH_MAX_AVOIDS that remain fit. */
static void
test_permanent_normalised(void) {
    const uint32_t list[] = {463550000, 0, 451000000, 463550000, 451000000, 162550000};
    dsd_signal_search_avoids_permanent_set(list, sizeof(list) / sizeof(list[0]));
    uint64_t revision = 0;
    assert(dsd_signal_search_avoids_permanent_snapshot(out, DSD_SEARCH_MAX_AVOIDS, &revision) == 3);
    assert(out[0] == 463550000 && out[1] == 451000000 && out[2] == 162550000);
    assert(revision == 1 && dsd_signal_search_avoids_permanent_revision() == 1);
    /* The same content again, in another spelling: no new revision. */
    const uint32_t again[] = {463550000, 463550000, 451000000, 0, 162550000};
    dsd_signal_search_avoids_permanent_set(again, sizeof(again) / sizeof(again[0]));
    assert(dsd_signal_search_avoids_permanent_revision() == 1);
    /* Another order is another list. */
    const uint32_t reordered[] = {451000000, 463550000, 162550000};
    dsd_signal_search_avoids_permanent_set(reordered, 3);
    assert(dsd_signal_search_avoids_permanent_revision() == 2);
    /* A short buffer takes the head of the list; no buffer takes nothing, and still gives the revision. */
    assert(dsd_signal_search_avoids_permanent_snapshot(out, 2, &revision) == 2 && revision == 2);
    assert(out[0] == 451000000 && out[1] == 463550000);
    assert(dsd_signal_search_avoids_permanent_snapshot(NULL, 4, &revision) == 0 && revision == 2);
    assert(dsd_signal_search_avoids_permanent_snapshot(out, 4, NULL) == 3);

    /* More than fit: the first DSD_SEARCH_MAX_AVOIDS distinct ones, a repeat and a 0 not counting against the room. */
    static uint32_t many[DSD_SEARCH_MAX_AVOIDS + 8];
    many[0] = 0;
    many[1] = 100000000;
    many[2] = 100000000;
    for (size_t i = 3; i < sizeof(many) / sizeof(many[0]); ++i) {
        many[i] = 100000000 + (uint32_t)(i - 2) * 12500U;
    }
    dsd_signal_search_avoids_permanent_set(many, sizeof(many) / sizeof(many[0]));
    assert(dsd_signal_search_avoids_permanent_snapshot(out, sizeof(out) / sizeof(out[0]), &revision)
           == DSD_SEARCH_MAX_AVOIDS);
    assert(revision == 3 && out[0] == 100000000 && out[1] == 100012500);
    assert(out[DSD_SEARCH_MAX_AVOIDS - 1] == 100000000 + (uint32_t)(DSD_SEARCH_MAX_AVOIDS - 1) * 12500U);
    dsd_signal_search_avoids_permanent_set(NULL, 0);
    assert(dsd_signal_search_avoids_permanent_revision() == 4);
    assert(dsd_signal_search_avoids_permanent_snapshot(out, 4, &revision) == 0 && revision == 4);
}

/* The session list belongs to one session; session 0 clears it whatever list comes with it. A new session with the same
   frequencies is a change. */
static void
test_session_list(void) {
    const uint64_t before = dsd_signal_search_avoids_session_revision();
    const uint32_t list[] = {100025000, 0, 100025000, 100012500};
    dsd_signal_search_avoids_session_set(55, list, 4);
    uint64_t session = 0;
    assert(dsd_signal_search_avoids_session_get(&session, out, DSD_SEARCH_MAX_AVOIDS) == 2 && session == 55);
    assert(out[0] == 100025000 && out[1] == 100012500);
    assert(dsd_signal_search_avoids_session_revision() == before + 1);
    dsd_signal_search_avoids_session_set(55, list, 4);
    assert(dsd_signal_search_avoids_session_revision() == before + 1);
    dsd_signal_search_avoids_session_set(56, list, 4);
    assert(dsd_signal_search_avoids_session_revision() == before + 2);
    assert(dsd_signal_search_avoids_session_get(&session, out, 1) == 1 && session == 56 && out[0] == 100025000);
    dsd_signal_search_avoids_session_set(0, list, 4);
    assert(dsd_signal_search_avoids_session_revision() == before + 3);
    assert(dsd_signal_search_avoids_session_get(&session, out, DSD_SEARCH_MAX_AVOIDS) == 0 && session == 0);
    dsd_signal_search_avoids_session_set(0, NULL, 0);
    assert(dsd_signal_search_avoids_session_revision() == before + 3);
    /* The two lists are apart: neither write moves the other's revision. */
    const uint64_t permanent = dsd_signal_search_avoids_permanent_revision();
    dsd_signal_search_avoids_session_set(57, list, 1);
    assert(dsd_signal_search_avoids_permanent_revision() == permanent);
    const uint64_t at = dsd_signal_search_avoids_session_revision();
    dsd_signal_search_avoids_permanent_set(list, 1);
    assert(dsd_signal_search_avoids_session_revision() == at);
    dsd_signal_search_avoids_session_set(0, NULL, 0);
    dsd_signal_search_avoids_permanent_set(NULL, 0);
}

enum { WRITES = 20000, OVERLAPPED_READS = 1000, LIST_LEN = 64 };

static atomic_int g_writer_done;
static atomic_int g_reads;
static int g_torn;
static long g_snapshots;

/* The list written at revision r holds LIST_LEN frequencies all derived from one value, so a snapshot mixing two
   writes, or carrying another write's revision, shows. Every write changes the content, so the revisions are 1, 2, ...
   from the base and value (r - base) is what each one wrote. */
static uint64_t g_base;

static uint32_t
list_value(uint64_t value, size_t i) {
    return (uint32_t)(100000000U + (uint32_t)(value % 1000U) * 100000U + (uint32_t)i * 12500U);
}

static DSD_THREAD_RETURN_TYPE
read_snapshots(void* arg) {
    (void)arg;
    uint32_t got[DSD_SEARCH_MAX_AVOIDS];
    while (!atomic_load(&g_writer_done)) {
        uint64_t revision = 0;
        const size_t n = dsd_signal_search_avoids_permanent_snapshot(got, DSD_SEARCH_MAX_AVOIDS, &revision);
        if (revision > g_base) {
            const uint64_t value = revision - g_base;
            if (n != LIST_LEN) {
                g_torn = 1;
            }
            for (size_t i = 0; i < n; ++i) {
                if (got[i] != list_value(value, i)) {
                    g_torn = 1;
                }
            }
            ++g_snapshots;
        }
        if (atomic_load(&g_reads) < OVERLAPPED_READS) {
            (void)atomic_fetch_add(&g_reads, 1);
        }
    }
    DSD_THREAD_RETURN;
}

/* The store writes while the decoder snapshots: every snapshot is one whole list with its own revision. Run under
   TSan as well. */
static void
test_snapshot_whole_under_writer(void) {
    g_base = dsd_signal_search_avoids_permanent_revision();
    atomic_store(&g_writer_done, 0);
    atomic_store(&g_reads, 0);
    dsd_thread_t reader;
    assert(dsd_thread_create(&reader, read_snapshots, NULL) == 0);
    uint32_t list[LIST_LEN];
    uint64_t value = 1;
    for (; value <= WRITES || atomic_load(&g_reads) < OVERLAPPED_READS; ++value) {
        for (size_t i = 0; i < LIST_LEN; ++i) {
            list[i] = list_value(value, i);
        }
        dsd_signal_search_avoids_permanent_set(list, LIST_LEN);
    }
    atomic_store(&g_writer_done, 1);
    assert(dsd_thread_join(reader) == 0);
    assert(!g_torn && g_snapshots > 0);
    assert(dsd_signal_search_avoids_permanent_revision() == g_base + value - 1);
    dsd_signal_search_avoids_permanent_set(NULL, 0);
}

int
main(void) {
    test_starts_empty();
    test_permanent_normalised();
    test_session_list();
    test_snapshot_whole_under_writer();
    printf("RUNTIME_SIGNAL_SEARCH_AVOIDS: OK (%ld snapshots)\n", g_snapshots);
    return 0;
}
