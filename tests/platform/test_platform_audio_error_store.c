// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Concurrency contract of the audio backends' shared last-error store.
 *
 * The async output pump reports device write, drain and reopen failures from its own
 * thread while the decoder thread can call dsd_audio_get_error() at any moment. Before
 * the store was introduced each backend kept a bare `static char[512]` written with an
 * unguarded strncpy, so a reader could be handed a half-overwritten message; under
 * ThreadSanitizer it is a straight data race.
 *
 * This drives the store the way those two threads do — many concurrent writers against
 * a reader — and asserts every read yields one whole message. It is built into the
 * tsan-debug preset like every other test, which is where the race itself is caught;
 * here the assertions cover the tearing.
 */

#include <assert.h>
#include <dsd-neo/platform/threading.h>
#include <stdio.h>
#include <string.h>

#include "../../src/platform/audio_error_internal.h"
#include "dsd-neo/core/safe_api.h"

enum { WRITER_COUNT = 4, ITERATIONS = 20000 };

/* Deliberately different lengths: a torn copy of a long message over a short one
 * leaves a recognisable tail, which a same-length set would hide. */
static const char* const k_messages[WRITER_COUNT] = {
    "x",
    "AAUDIO_ERROR_DISCONNECTED",
    "AAudio granted an unusable rate/channel combination",
    "AAudio output drain timed out",
};

static int
is_known_message(const char* text) {
    if (text[0] == '\0') {
        return 1; /* the cleared state */
    }
    for (int i = 0; i < WRITER_COUNT; i++) {
        if (strcmp(text, k_messages[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

/* Writers take their message by index: the payload is a pointer to the caller's
 * slot, so nothing has to cast const away to cross the thread entry point. */
static int g_writer_index[WRITER_COUNT];

static DSD_THREAD_RETURN_TYPE
writer_thread(void* arg) {
    const char* message = k_messages[*(const int*)arg];
    for (int i = 0; i < ITERATIONS; i++) {
        set_error(message);
        /* Exercise the clear path too; it takes the same lock. */
        if ((i & 0xFF) == 0) {
            set_error(NULL);
        }
    }
    DSD_THREAD_RETURN;
}

static DSD_THREAD_RETURN_TYPE
reader_thread(void* arg) {
    int* torn = (int*)arg;
    for (int i = 0; i < ITERATIONS; i++) {
        const char* text = audio_error_get();
        if (!is_known_message(text)) {
            *torn = 1;
            DSD_FPRINTF(stderr, "torn read: '%s'\n", text);
            break;
        }
    }
    DSD_THREAD_RETURN;
}

static void
test_set_and_get_roundtrip(void) {
    set_error("first");
    assert(strcmp(audio_error_get(), "first") == 0);

    set_error(NULL);
    assert(audio_error_get()[0] == '\0');

    /* Over-long messages are truncated, never overrun. */
    char oversized[DSD_AUDIO_ERROR_CAPACITY * 2];
    DSD_MEMSET(oversized, 'a', sizeof oversized - 1U);
    oversized[sizeof oversized - 1U] = '\0';
    set_error(oversized);
    const char* got = audio_error_get();
    assert(strlen(got) == (size_t)DSD_AUDIO_ERROR_CAPACITY - 1U);

    set_error(NULL);
}

/**
 * @brief The returned pointer stays this thread's until it calls again.
 *
 * This is what makes a concurrent set_error() safe: readers never share a buffer
 * with a writer, so nothing can rewrite the message underneath the caller.
 */
static void
test_reader_owns_its_copy(void) {
    set_error("stable");
    const char* held = audio_error_get();
    assert(strcmp(held, "stable") == 0);

    set_error("replaced");
    /* Not re-read: the earlier pointer must still show what it was handed. */
    assert(strcmp(held, "stable") == 0);
    assert(strcmp(audio_error_get(), "replaced") == 0);

    set_error(NULL);
}

static void
test_concurrent_writers_never_tear_a_read(void) {
    dsd_thread_t writers[WRITER_COUNT];
    dsd_thread_t reader;
    int torn = 0;

    assert(dsd_thread_create(&reader, reader_thread, &torn) == 0);
    for (int i = 0; i < WRITER_COUNT; i++) {
        g_writer_index[i] = i;
        assert(dsd_thread_create(&writers[i], writer_thread, &g_writer_index[i]) == 0);
    }

    for (int i = 0; i < WRITER_COUNT; i++) {
        assert(dsd_thread_join(writers[i]) == 0);
    }
    assert(dsd_thread_join(reader) == 0);
    assert(torn == 0);

    set_error(NULL);
}

int
main(void) {
    test_set_and_get_roundtrip();
    test_reader_owns_its_copy();
    test_concurrent_writers_never_tear_a_read();
    printf("PLATFORM_AUDIO_ERROR_STORE: OK\n");
    return 0;
}
