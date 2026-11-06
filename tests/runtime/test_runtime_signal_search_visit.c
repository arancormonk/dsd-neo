// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The Signal search visit tag (issue #809): the engine names the visit on the air, the recorder reads it at a publish
 * on whichever thread closes the recording. A reader sees the tag clear or one whole (session, hit) pair, never half of
 * one pair and half of another.
 */

#include <assert.h>
#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/runtime/signal_search_hooks.h>
#include <stdint.h>
#include <stdio.h>

static void
test_set_get_clear(void) {
    uint64_t session = 7, hit = 7;
    assert(dsd_signal_search_visit_get(&session, &hit) == 0 && session == 0 && hit == 0);
    dsd_signal_search_visit_set(42, 3);
    session = hit = 0;
    assert(dsd_signal_search_visit_get(&session, &hit) == 1 && session == 42 && hit == 3);
    /* The full 64-bit range survives. */
    dsd_signal_search_visit_set(UINT64_MAX, UINT64_MAX - 1);
    assert(dsd_signal_search_visit_get(&session, &hit) == 1 && session == UINT64_MAX && hit == UINT64_MAX - 1);
    dsd_signal_search_visit_clear();
    assert(dsd_signal_search_visit_get(&session, &hit) == 0 && session == 0 && hit == 0);
    /* Clearing a clear tag is harmless. */
    dsd_signal_search_visit_clear();
    assert(dsd_signal_search_visit_get(&session, &hit) == 0 && session == 0 && hit == 0);
}

/* A pair with a zero half names no detection: it reads as clear, and it replaces a tag that was set. */
static void
test_zero_half_reads_clear(void) {
    uint64_t session = 0, hit = 0;
    dsd_signal_search_visit_set(42, 3);
    dsd_signal_search_visit_set(0, 5);
    assert(dsd_signal_search_visit_get(&session, &hit) == 0 && session == 0 && hit == 0);
    dsd_signal_search_visit_set(42, 3);
    dsd_signal_search_visit_set(9, 0);
    assert(dsd_signal_search_visit_get(&session, &hit) == 0 && session == 0 && hit == 0);
    dsd_signal_search_visit_set(0, 0);
    assert(dsd_signal_search_visit_get(&session, &hit) == 0 && session == 0 && hit == 0);
}

enum { WRITES = 200000, OVERLAPPED_READS = 1000 };

static atomic_int g_writer_done;
/* Reads made, capped once the writer may stop: the writer keeps writing until the reader has overlapped it, and has
   seen the tag both set and clear (on one CPU the reader can make all its overlapped reads inside one phase). */
static atomic_int g_reads;
static atomic_int g_seen_tagged;
static atomic_int g_seen_clear;
static int g_mixed;
static long g_reads_tagged;

/* The two pairs the writer alternates differ in both halves, so a torn read shows as a pair neither of them is. */
static DSD_THREAD_RETURN_TYPE
read_visits(void* arg) {
    (void)arg;
    while (!atomic_load(&g_writer_done)) {
        uint64_t session = 0, hit = 0;
        if (dsd_signal_search_visit_get(&session, &hit)) {
            ++g_reads_tagged;
            atomic_store(&g_seen_tagged, 1);
            if (!((session == 11 && hit == 101) || (session == 22 && hit == 202))) {
                g_mixed = 1;
            }
        } else {
            atomic_store(&g_seen_clear, 1);
            if (session != 0 || hit != 0) {
                g_mixed = 1;
            }
        }
        if (atomic_load(&g_reads) < OVERLAPPED_READS) {
            (void)atomic_fetch_add(&g_reads, 1);
        }
    }
    DSD_THREAD_RETURN;
}

/* The decoder sets and clears while another thread (the watchdog, a command) publishes: every read is whole. Run
   under TSan as well. */
static void
test_reader_never_sees_mixed_pair(void) {
    atomic_store(&g_writer_done, 0);
    atomic_store(&g_reads, 0);
    atomic_store(&g_seen_tagged, 0);
    atomic_store(&g_seen_clear, 0);
    dsd_thread_t reader;
    assert(dsd_thread_create(&reader, read_visits, NULL) == 0);
    for (int i = 0; i < WRITES || atomic_load(&g_reads) < OVERLAPPED_READS || !atomic_load(&g_seen_tagged)
                    || !atomic_load(&g_seen_clear);
         ++i) {
        switch (i % 3) {
            case 0: dsd_signal_search_visit_set(11, 101); break;
            case 1: dsd_signal_search_visit_set(22, 202); break;
            default: dsd_signal_search_visit_clear(); break;
        }
    }
    atomic_store(&g_writer_done, 1);
    assert(dsd_thread_join(reader) == 0);
    assert(!g_mixed && g_reads_tagged > 0);
    dsd_signal_search_visit_clear();
}

int
main(void) {
    test_set_get_clear();
    test_zero_half_reads_clear();
    test_reader_never_sees_mixed_pair();
    printf("RUNTIME_SIGNAL_SEARCH_VISIT: OK (%ld tagged reads)\n", g_reads_tagged);
    return 0;
}
