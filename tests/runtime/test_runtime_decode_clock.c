// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/platform/platform.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/platform/timing.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                            \
            exit(1);                                                                                                   \
        }                                                                                                              \
    } while (0)

#define FLOOR_S 946684800LL
#define NS      1000000000ULL

static void
test_system(void) {
    dsd_decode_clock_use_system();
    CHECK(dsd_decode_clock_source() == DSD_DECODE_CLOCK_SYSTEM);

    uint64_t a = dsd_time_monotonic_ns();
    uint64_t d = dsd_decode_now_mono_ns();
    uint64_t b = dsd_time_monotonic_ns();
    CHECK(a <= d && d <= b);

    uint64_t ms0 = dsd_time_monotonic_ms();
    uint64_t ms = dsd_decode_now_mono_ms();
    uint64_t ms1 = dsd_time_monotonic_ms();
    CHECK(ms0 <= ms && ms <= ms1);

    double m0 = (double)dsd_time_monotonic_ns() / 1e9;
    double m = dsd_decode_now_mono_s();
    double m1 = (double)dsd_time_monotonic_ns() / 1e9;
    CHECK(m0 <= m && m <= m1);

    double w0 = (double)dsd_time_realtime_ns() / 1e9;
    double w = dsd_decode_now_realtime_s();
    double w1 = (double)dsd_time_realtime_ns() / 1e9;
    CHECK(w0 <= w && w <= w1);

    time_t t0 = time(NULL);
    time_t t = dsd_decode_time();
    time_t t1 = time(NULL);
    CHECK(t0 <= t && t <= t1);

    /* Real-time reads track the same platform clocks. */
    CHECK(fabs(dsd_realtime_now_s() - dsd_decode_now_realtime_s()) < 1.0);
    CHECK(fabs(dsd_realtime_mono_s() - dsd_decode_now_mono_s()) < 1.0);
    CHECK(llabs((long long)dsd_realtime_time() - (long long)dsd_decode_time()) <= 1);
    uint64_t r0 = dsd_realtime_mono_ns();
    uint64_t r1 = dsd_realtime_mono_ns();
    CHECK(r1 >= r0);
    CHECK(dsd_realtime_mono_ms() >= r0 / 1000000ULL);
}

/* The SYSTEM reads bracketed by the platform clocks: what a process that never left a replay reads. */
static void
check_system_reads_platform_clocks(void) {
    CHECK(dsd_decode_clock_source() == DSD_DECODE_CLOCK_SYSTEM);
    uint64_t a = dsd_time_monotonic_ns();
    uint64_t d = dsd_decode_now_mono_ns();
    uint64_t b = dsd_time_monotonic_ns();
    CHECK(a <= d && d <= b);
    uint64_t ms0 = dsd_time_monotonic_ms();
    uint64_t ms = dsd_decode_now_mono_ms();
    uint64_t ms1 = dsd_time_monotonic_ms();
    CHECK(ms0 <= ms && ms <= ms1);
}

/* Leaving TEST for SYSTEM carries no test value into SYSTEM: with no replay left before, SYSTEM reads exactly the
   platform clocks again. Run before any REPLAY in the process. */
static void
test_leaving_test_sets_no_offset(void) {
    dsd_decode_clock_use_system();
    check_system_reads_platform_clocks();
    dsd_decode_clock_use_test(42 * NS);
    CHECK(dsd_decode_now_mono_ns() == 42 * NS);
    dsd_decode_clock_use_system();
    check_system_reads_platform_clocks();
    dsd_decode_clock_use_test(UINT64_MAX / 2U);
    dsd_decode_clock_use_system();
    check_system_reads_platform_clocks();
}

/* Leaving REPLAY for SYSTEM keeps decode-mono time continuous: it goes on from the capture time the replay reached, at
   the platform clock's rate, while wall reads go back to real time. Leaving TEST afterwards does not move it, and a
   later replay's leave moves the origin to that replay's capture time. */
static void
test_replay_leave_keeps_mono_continuous(void) {
    const int64_t anchor = 1788245497LL; /* 2026-09-01T06:51:37Z */
    dsd_decode_clock_use_replay(anchor);
    dsd_decode_clock_set_media_ns(5 * NS + 250000000ULL);
    const uint64_t before = dsd_decode_now_mono_ns();
    CHECK(before == (uint64_t)anchor * NS + 5 * NS + 250000000ULL);
    dsd_decode_clock_use_system();
    CHECK(dsd_decode_clock_source() == DSD_DECODE_CLOCK_SYSTEM);
    const uint64_t after = dsd_decode_now_mono_ns();
    CHECK(after >= before && after - before < NS);
    /* The ms and s reads follow the ns read. */
    const uint64_t ms = dsd_decode_now_mono_ms();
    const uint64_t ns_after_ms = dsd_decode_now_mono_ns();
    CHECK(ms * 1000000ULL >= after - 1000000ULL && ms <= ns_after_ms / 1000000ULL);
    CHECK(fabs(dsd_decode_now_mono_s() - (double)dsd_decode_now_mono_ns() / 1e9) < 1e-3);
    /* Wall reads are real time again. */
    CHECK(llabs((long long)dsd_decode_time() - (long long)time(NULL)) <= 1);
    CHECK(fabs(dsd_decode_now_realtime_s() - dsd_realtime_now_s()) < 1.0);
    /* Real-time reads never moved. */
    CHECK(dsd_realtime_mono_ns() < (uint64_t)anchor * NS);
    /* It runs at the platform clock's rate. */
    const uint64_t platform0 = dsd_time_monotonic_ns();
    const uint64_t decode0 = dsd_decode_now_mono_ns();
    dsd_sleep_ms(20U);
    const uint64_t decode1 = dsd_decode_now_mono_ns();
    const uint64_t platform1 = dsd_time_monotonic_ns();
    CHECK(decode1 - decode0 >= 20000000ULL && decode1 - decode0 <= platform1 - platform0);

    /* A TEST value does not carry into SYSTEM, and the replay's origin stays. */
    dsd_decode_clock_use_test(42 * NS);
    dsd_decode_clock_use_system();
    const uint64_t after_test = dsd_decode_now_mono_ns();
    CHECK(after_test >= decode1 && after_test - decode1 < NS);

    /* Leaving SYSTEM for SYSTEM changes nothing. */
    dsd_decode_clock_use_system();
    CHECK(dsd_decode_now_mono_ns() >= after_test);

    /* A later replay's leave goes on from that replay's capture time. */
    dsd_decode_clock_use_replay(FLOOR_S);
    dsd_decode_clock_set_media_ns(3 * NS);
    const uint64_t second = dsd_decode_now_mono_ns();
    dsd_decode_clock_use_system();
    const uint64_t second_after = dsd_decode_now_mono_ns();
    CHECK(second == (uint64_t)FLOOR_S * NS + 3 * NS);
    CHECK(second_after >= second && second_after - second < NS);
}

static void
test_replay(void) {
    int64_t anchor = 1700000000LL;
    dsd_decode_clock_use_replay(anchor);
    CHECK(dsd_decode_clock_source() == DSD_DECODE_CLOCK_REPLAY);

    CHECK(dsd_decode_now_mono_ns() == (uint64_t)anchor * NS);
    CHECK(dsd_decode_time() == (time_t)anchor);

    dsd_decode_clock_set_media_ns(2500000000ULL);
    CHECK(dsd_decode_now_mono_ns() == (uint64_t)anchor * NS + 2500000000ULL);
    CHECK(dsd_decode_time() == (time_t)(anchor + 2));
    CHECK(dsd_decode_now_mono_ms() == (uint64_t)anchor * 1000ULL + 2500ULL);
    CHECK(fabs(dsd_decode_now_mono_s() - ((double)anchor + 2.5)) < 1e-3);
    CHECK(fabs(dsd_decode_now_realtime_s() - ((double)anchor + 2.5)) < 1e-3);
    CHECK(fabs(dsd_decode_now_mono_s() - dsd_decode_now_realtime_s()) < 1e-6);

    /* Backwards media time is ignored. */
    dsd_decode_clock_set_media_ns(1000000000ULL);
    CHECK(dsd_decode_now_mono_ns() == (uint64_t)anchor * NS + 2500000000ULL);
    dsd_decode_clock_set_media_ns(2500000000ULL);
    CHECK(dsd_decode_now_mono_ns() == (uint64_t)anchor * NS + 2500000000ULL);
    dsd_decode_clock_set_media_ns(3000000000ULL);
    CHECK(dsd_decode_now_mono_ns() == (uint64_t)anchor * NS + 3000000000ULL);

    /* Real-time reads are unaffected by the replay source. */
    CHECK(fabs(dsd_realtime_now_s() - (double)time(NULL)) < 2.0);
    CHECK(llabs((long long)dsd_realtime_time() - (long long)time(NULL)) <= 1);

    /* Re-entering replay resets media time. */
    dsd_decode_clock_use_replay(anchor + 10);
    CHECK(dsd_decode_now_mono_ns() == (uint64_t)(anchor + 10) * NS);

    /* 2000-01-01 floor for a 1970 (or zero/negative) anchor. */
    dsd_decode_clock_use_replay(0);
    CHECK(dsd_decode_now_mono_ns() == (uint64_t)FLOOR_S * NS);
    CHECK(dsd_decode_time() == (time_t)FLOOR_S);
    dsd_decode_clock_use_replay(-5);
    CHECK(dsd_decode_time() == (time_t)FLOOR_S);
    dsd_decode_clock_use_replay(86400);
    dsd_decode_clock_set_media_ns(5 * NS);
    CHECK(dsd_decode_now_mono_ns() == (uint64_t)FLOOR_S * NS + 5 * NS);
    dsd_decode_clock_use_replay(FLOOR_S);
    CHECK(dsd_decode_time() == (time_t)FLOOR_S);

    dsd_decode_clock_use_system();
    CHECK(dsd_decode_clock_source() == DSD_DECODE_CLOCK_SYSTEM);
    CHECK(llabs((long long)dsd_decode_time() - (long long)time(NULL)) <= 1);
}

/* An anchor past 18e9 s (about the year 2540) is held there, so anchor_ns stays inside 64 bits; the floor and the
   ceiling are the header's. */
static void
test_replay_anchor_clamp(void) {
    CHECK(DSD_DECODE_CLOCK_ANCHOR_FLOOR_S == FLOOR_S);
    CHECK(DSD_DECODE_CLOCK_ANCHOR_MAX_S == 18000000000LL);
    /* The ceiling in ns fits, with room for media time on top. */
    CHECK((uint64_t)DSD_DECODE_CLOCK_ANCHOR_MAX_S <= UINT64_MAX / NS);

    dsd_decode_clock_use_replay(DSD_DECODE_CLOCK_ANCHOR_MAX_S);
    CHECK(dsd_decode_time() == (time_t)DSD_DECODE_CLOCK_ANCHOR_MAX_S);
    CHECK(dsd_decode_now_mono_ns() == (uint64_t)DSD_DECODE_CLOCK_ANCHOR_MAX_S * NS);

    dsd_decode_clock_use_replay(DSD_DECODE_CLOCK_ANCHOR_MAX_S + 1);
    CHECK(dsd_decode_time() == (time_t)DSD_DECODE_CLOCK_ANCHOR_MAX_S);
    CHECK(dsd_decode_now_mono_ns() == (uint64_t)DSD_DECODE_CLOCK_ANCHOR_MAX_S * NS);

    dsd_decode_clock_use_replay(INT64_MAX);
    CHECK(dsd_decode_now_mono_ns() == (uint64_t)DSD_DECODE_CLOCK_ANCHOR_MAX_S * NS);
    dsd_decode_clock_set_media_ns(3 * NS);
    CHECK(dsd_decode_now_mono_ns() == (uint64_t)DSD_DECODE_CLOCK_ANCHOR_MAX_S * NS + 3 * NS);
    CHECK(dsd_decode_time() == (time_t)(DSD_DECODE_CLOCK_ANCHOR_MAX_S + 3));

    /* Just below the ceiling is taken as it is. */
    dsd_decode_clock_use_replay(DSD_DECODE_CLOCK_ANCHOR_MAX_S - 1);
    CHECK(dsd_decode_time() == (time_t)(DSD_DECODE_CLOCK_ANCHOR_MAX_S - 1));

    dsd_decode_clock_use_system();
}

/* A replay batch's samples sit evenly across its span: sample i of n at start + i * duration / n, rounded down, exact
   where the 64-bit product would overflow. */
static void
test_batch_media_time(void) {
    /* 512 samples over 21.333333 ms (a 1024-sample chunk at 48 kHz, say): 41666.666 ns apart. */
    const uint64_t start = 5000000000ULL;
    const uint64_t dur = 21333333ULL;
    CHECK(dsd_decode_clock_batch_media_ns(start, dur, 512U, 0U) == start);
    CHECK(dsd_decode_clock_batch_media_ns(start, dur, 512U, 1U) == start + 41666ULL);
    CHECK(dsd_decode_clock_batch_media_ns(start, dur, 512U, 3U) == start + 124999ULL);
    CHECK(dsd_decode_clock_batch_media_ns(start, dur, 512U, 511U) == start + (dur * 511ULL) / 512ULL);
    CHECK(dsd_decode_clock_batch_media_ns(start, dur, 512U, 512U) == start + dur);
    /* Past the batch is held at its end; no count is the span's start. */
    CHECK(dsd_decode_clock_batch_media_ns(start, dur, 512U, 9999U) == start + dur);
    CHECK(dsd_decode_clock_batch_media_ns(start, dur, 0U, 7U) == start);
    /* Monotone across the batch. */
    uint64_t prev = 0U;
    for (uint32_t i = 0U; i <= 512U; i++) {
        const uint64_t t = dsd_decode_clock_batch_media_ns(start, dur, 512U, i);
        CHECK(t >= prev);
        prev = t;
    }
    /* index * duration would overflow 64 bits here (4e9 * 1e10), the split division does not: exactly half way. */
    CHECK(dsd_decode_clock_batch_media_ns(0U, 10000000000ULL, 4000000000U, 2000000000U) == 5000000000ULL);
    CHECK(dsd_decode_clock_batch_media_ns(0U, UINT64_MAX, UINT32_MAX, UINT32_MAX - 1U)
          == UINT64_MAX - UINT64_MAX / UINT32_MAX);
    /* The sum saturates. */
    CHECK(dsd_decode_clock_batch_media_ns(UINT64_MAX - 5U, 100U, 10U, 10U) == UINT64_MAX);
}

static void
test_injection(void) {
    dsd_decode_clock_use_test(42 * NS + 500000000ULL);
    CHECK(dsd_decode_clock_source() == DSD_DECODE_CLOCK_TEST);
    CHECK(dsd_decode_now_mono_ns() == 42 * NS + 500000000ULL);
    CHECK(dsd_decode_now_mono_ms() == 42500ULL);
    CHECK(dsd_decode_time() == 42);
    CHECK(fabs(dsd_decode_now_mono_s() - 42.5) < 1e-9);
    CHECK(fabs(dsd_decode_now_realtime_s() - 42.5) < 1e-9);

    dsd_decode_clock_test_set_ns(7 * NS);
    CHECK(dsd_decode_now_mono_ns() == 7 * NS);
    CHECK(dsd_decode_time() == 7);
    CHECK(fabs(dsd_decode_now_realtime_s() - 7.0) < 1e-9);

    /* The test value may move backwards, and media time does not affect it. */
    dsd_decode_clock_set_media_ns(99 * NS);
    CHECK(dsd_decode_now_mono_ns() == 7 * NS);

    /* Real-time reads are not injected. */
    CHECK(fabs(dsd_realtime_now_s() - (double)time(NULL)) < 2.0);

    dsd_decode_clock_use_system();
}

#define CONC_ANCHOR  1700000000LL
#define CONC_STEPS   200000ULL
#define CONC_READERS 3

static atomic_int s_writer_done = 0;
static atomic_int s_reader_bad = 0;

static DSD_THREAD_RETURN_TYPE
#if DSD_PLATFORM_WIN_NATIVE
    __stdcall
#endif
    writer_thread(void* arg) {
    (void)arg;
    for (uint64_t i = 1; i <= CONC_STEPS; i++) {
        dsd_decode_clock_set_media_ns(i * 1000ULL);
    }
    atomic_store(&s_writer_done, 1);
    DSD_THREAD_RETURN;
}

static DSD_THREAD_RETURN_TYPE
#if DSD_PLATFORM_WIN_NATIVE
    __stdcall
#endif
    reader_thread(void* arg) {
    (void)arg;
    uint64_t prev = 0;
    uint64_t lo = (uint64_t)CONC_ANCHOR * NS;
    uint64_t hi = lo + CONC_STEPS * 1000ULL;
    int done;
    do {
        done = atomic_load(&s_writer_done);
        uint64_t v = dsd_decode_now_mono_ns();
        if (v < lo || v > hi || v < prev) {
            atomic_store(&s_reader_bad, 1);
        }
        prev = v;
        time_t t = dsd_decode_time();
        if (t != (time_t)CONC_ANCHOR) {
            atomic_store(&s_reader_bad, 1);
        }
    } while (!done);
    DSD_THREAD_RETURN;
}

static void
test_concurrent(void) {
    dsd_decode_clock_use_replay(CONC_ANCHOR);
    dsd_thread_t w;
    dsd_thread_t r[CONC_READERS];
    for (int i = 0; i < CONC_READERS; i++) {
        CHECK(dsd_thread_create(&r[i], reader_thread, NULL) == 0);
    }
    CHECK(dsd_thread_create(&w, writer_thread, NULL) == 0);
    (void)dsd_thread_join(w);
    for (int i = 0; i < CONC_READERS; i++) {
        (void)dsd_thread_join(r[i]);
    }
    CHECK(atomic_load(&s_reader_bad) == 0);
    CHECK(dsd_decode_now_mono_ns() == (uint64_t)CONC_ANCHOR * NS + CONC_STEPS * 1000ULL);
    dsd_decode_clock_use_system();
}

int
main(void) {
    test_system();
    test_leaving_test_sets_no_offset();
    test_replay_leave_keeps_mono_continuous();
    test_replay();
    test_replay_anchor_clamp();
    test_batch_media_time();
    test_injection();
    test_concurrent();
    return 0;
}
