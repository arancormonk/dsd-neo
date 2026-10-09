// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The audible-audio stamp (issue #574): off until armed, one word holding the stamp in real-time monotonic
 * milliseconds, read whole with its age, cleared by reset, and coherent while one thread notes, another reads and a
 * third resets.
 *
 * The stamp is process-wide and arming is for good, so the unarmed checks run first.
 */

#include <dsd-neo/core/audio_activity.h>
#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/platform/threading.h>
#include <stdint.h>
#include <stdio.h>
#include "core/audio/audio_activity_internal.h"
#include "dsd-neo/core/safe_api.h"

static uint64_t g_now_ms = 0U;
static int g_clock_reads = 0;

static uint64_t
test_clock(void) {
    g_clock_reads++;
    return g_now_ms;
}

static int
expect_u64(const char* label, uint64_t got, uint64_t want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %llu want %llu\n", label, (unsigned long long)got, (unsigned long long)want);
        return 1;
    }
    return 0;
}

static int
expect_int(const char* label, long long got, long long want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %lld want %lld\n", label, got, want);
        return 1;
    }
    return 0;
}

/* One read, checked against the stamp and age it should give. */
static int
expect_read(const char* label, uint64_t want_stamp, int32_t want_age) {
    uint64_t stamp = 12345U;
    int32_t age = 12345;
    dsd_audio_activity_read(&stamp, &age);
    char tag[128];
    DSD_SNPRINTF(tag, sizeof(tag), "%s: stamp", label);
    int rc = expect_u64(tag, stamp, want_stamp);
    DSD_SNPRINTF(tag, sizeof(tag), "%s: age", label);
    rc |= expect_int(tag, age, want_age);
    return rc;
}

static int
test_unarmed_note_does_nothing(void) {
    int rc = 0;
    g_now_ms = 5000U;
    g_clock_reads = 0;
    rc |= expect_int("starts unarmed", dsd_audio_activity_armed(), 0);
    dsd_audio_activity_note();
    dsd_audio_activity_note();
    rc |= expect_int("unarmed note reads no clock", g_clock_reads, 0);
    rc |= expect_read("unarmed note stores nothing", 0U, -1);
    rc |= expect_int("an empty stamp reads no clock", g_clock_reads, 0);
    return rc;
}

static int
test_armed_note_stamps_and_ages(void) {
    int rc = 0;
    dsd_audio_activity_arm();
    rc |= expect_int("armed", dsd_audio_activity_armed(), 1);
    dsd_audio_activity_arm();
    rc |= expect_int("arming again keeps it armed", dsd_audio_activity_armed(), 1);
    rc |= expect_read("arming stamps nothing", 0U, -1);

    g_now_ms = 1000U;
    g_clock_reads = 0;
    dsd_audio_activity_note();
    rc |= expect_int("armed note reads the clock once", g_clock_reads, 1);
    rc |= expect_read("read at the note", 1000U, 0);
    g_now_ms = 1250U;
    rc |= expect_read("read 250 ms later", 1000U, 250);

    /* The stamp's value is its identity: a note in a later millisecond is a new stamp, one in the same is not. */
    g_now_ms = 1251U;
    dsd_audio_activity_note();
    rc |= expect_read("a note at a new millisecond is a new stamp", 1251U, 0);
    dsd_audio_activity_note();
    rc |= expect_read("a note in the same millisecond keeps the stamp", 1251U, 0);

    /* A clock reading 0 still stamps: 0 stands for none. */
    g_now_ms = 0U;
    dsd_audio_activity_note();
    rc |= expect_read("a note at clock 0 stamps 1", 1U, 0);

    /* A clock behind the stamp (another thread's note between the load and the clock read) ages it 0. */
    g_now_ms = 5000U;
    dsd_audio_activity_note();
    g_now_ms = 4990U;
    rc |= expect_read("age clamps at 0", 5000U, 0);
    return rc;
}

static int
test_stamp_expires_after_a_minute(void) {
    int rc = 0;
    g_now_ms = 10000U;
    dsd_audio_activity_note();
    g_now_ms = 10000U + DSD_AUDIO_ACTIVITY_MAX_AGE_MS;
    rc |= expect_read("a stamp exactly a minute old still ages", 10000U, DSD_AUDIO_ACTIVITY_MAX_AGE_MS);
    g_now_ms = 10001U + DSD_AUDIO_ACTIVITY_MAX_AGE_MS;
    rc |= expect_read("a stamp over a minute old reads age -1, the stamp kept", 10000U, -1);
    g_now_ms = 10000U + 3600000U;
    rc |= expect_read("an hour-old stamp reads age -1", 10000U, -1);
    return rc;
}

static int
test_reset_clears_and_out_pointers_are_optional(void) {
    int rc = 0;
    g_now_ms = 20000U;
    dsd_audio_activity_note();
    uint64_t stamp = 0U;
    dsd_audio_activity_read(&stamp, NULL);
    rc |= expect_u64("stamp alone", stamp, 20000U);
    int32_t age = -7;
    g_now_ms = 20040U;
    dsd_audio_activity_read(NULL, &age);
    rc |= expect_int("age alone", age, 40);
    dsd_audio_activity_read(NULL, NULL);

    dsd_audio_activity_reset();
    rc |= expect_read("reset clears the stamp", 0U, -1);
    rc |= expect_int("reset leaves it armed", dsd_audio_activity_armed(), 1);
    g_now_ms = 20100U;
    dsd_audio_activity_note();
    rc |= expect_read("a note after reset stamps again", 20100U, 0);
    dsd_audio_activity_reset();
    return rc;
}

/* The decoder thread notes while a frontend reads and resets: every read sees one stamp whole, its age taken from it.
   Run under the tsan-debug preset too. */
typedef struct {
    dsd_atomic_u64 stop;
    dsd_atomic_u64 reads;
    int bad_reads;
} race_ctx;

static DSD_THREAD_RETURN_TYPE
race_writer(void* arg) {
    race_ctx* ctx = (race_ctx*)arg;
    while (dsd_atomic_u64_load_acquire(&ctx->stop) == 0U) {
        dsd_audio_activity_note();
    }
    DSD_THREAD_RETURN;
}

static DSD_THREAD_RETURN_TYPE
race_resetter(void* arg) {
    race_ctx* ctx = (race_ctx*)arg;
    while (dsd_atomic_u64_load_acquire(&ctx->stop) == 0U) {
        dsd_audio_activity_reset();
    }
    DSD_THREAD_RETURN;
}

static DSD_THREAD_RETURN_TYPE
race_reader(void* arg) {
    race_ctx* ctx = (race_ctx*)arg;
    for (int i = 0; i < 200000; i++) {
        uint64_t stamp = 0U;
        int32_t age = 0;
        dsd_audio_activity_read(&stamp, &age);
        const int coherent = stamp == 0U ? age == -1 : (age >= 0 && age <= DSD_AUDIO_ACTIVITY_MAX_AGE_MS);
        if (!coherent) {
            ctx->bad_reads++;
        }
        (void)dsd_atomic_u64_fetch_add_relaxed(&ctx->reads, 1U);
    }
    dsd_atomic_u64_store_release(&ctx->stop, 1U);
    DSD_THREAD_RETURN;
}

static int
test_concurrent_note_read_reset(void) {
    static race_ctx ctx;
    dsd_atomic_u64_init(&ctx.stop, 0U);
    dsd_atomic_u64_init(&ctx.reads, 0U);
    ctx.bad_reads = 0;
    dsd_audio_activity_set_clock_for_test(NULL);
    dsd_audio_activity_arm();

    dsd_thread_t writer;
    dsd_thread_t resetter;
    dsd_thread_t reader;
    const int writer_ok = dsd_thread_create(&writer, race_writer, &ctx) == 0;
    const int resetter_ok = dsd_thread_create(&resetter, race_resetter, &ctx) == 0;
    const int reader_ok = dsd_thread_create(&reader, race_reader, &ctx) == 0;
    if (!reader_ok) {
        dsd_atomic_u64_store_release(&ctx.stop, 1U);
    }
    int rc = 0;
    if (reader_ok) {
        rc |= expect_int("reader joined", dsd_thread_join(reader), 0);
    }
    if (writer_ok) {
        rc |= expect_int("writer joined", dsd_thread_join(writer), 0);
    }
    if (resetter_ok) {
        rc |= expect_int("resetter joined", dsd_thread_join(resetter), 0);
    }
    rc |= expect_int("threads started", writer_ok && resetter_ok && reader_ok, 1);
    rc |= expect_u64("reads made", dsd_atomic_u64_load_acquire(&ctx.reads), 200000U);
    rc |= expect_int("every read coherent", ctx.bad_reads, 0);

    /* The real clock afterwards: a note stamps a nonzero time that reads young. */
    dsd_audio_activity_note();
    uint64_t stamp = 0U;
    int32_t age = -1;
    dsd_audio_activity_read(&stamp, &age);
    rc |= expect_int("real clock stamps", stamp != 0U && age >= 0 && age <= 1000, 1);
    dsd_audio_activity_reset();
    return rc;
}

int
main(void) {
    dsd_audio_activity_set_clock_for_test(test_clock);
    int rc = 0;
    rc |= test_unarmed_note_does_nothing();
    rc |= test_armed_note_stamps_and_ages();
    rc |= test_stamp_expires_after_a_minute();
    rc |= test_reset_clears_and_out_pointers_are_optional();
    rc |= test_concurrent_note_read_reset();
    if (rc == 0) {
        DSD_FPRINTF(stdout, "CORE_AUDIO_ACTIVITY: OK\n");
    }
    return rc;
}
