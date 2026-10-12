// SPDX-License-Identifier: GPL-3.0-or-later
// Coverage fixtures intentionally use private-source inclusion, synthetic sentinels,
// invalid-value negative vectors, or wrapper symbols to exercise guarded behavior.
// NOLINTBEGIN(bugprone-suspicious-include)
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * PortAudio output device recovery (issue #659), with the real pump thread and real locks over a fake PortAudio:
 * - a device write that fails on the pump marks the device down, and the thread that opened the stream (never the
 *   pump) closes it and opens it again at its next write, after which the pump plays again and the stream reports one
 *   failure and one recovery;
 * - a drain while the device is down fails at once, and closing a stream whose device is down aborts it;
 * - streams opened and closed on two threads at once never overlap inside PortAudio, which keeps its streams on an
 *   unlocked global list.
 */

#define DSD_USE_PORTAUDIO 1
#include "../../src/platform/audio_portaudio.c"

#include <assert.h>
#include <portaudio.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/platform/audio.h"

/*============================================================================
 * Fake PortAudio
 *============================================================================*/

typedef struct {
    int open;
} fake_pa_stream;

static PaDeviceInfo g_device = {2, "fake output", 0, 0, 2, 0.01, 0.01, 0.1, 0.1, 48000.0};

static atomic_int g_in_lifecycle; /* threads inside an open/start/stop/abort/close right now */
static atomic_int g_lifecycle_overlap;
static atomic_int g_opens;
static atomic_int g_aborts;
static atomic_int g_closes;
static atomic_int g_lifecycle_off_owner; /* lifecycle calls made on a thread other than g_owner */
static pthread_t g_owner;
static atomic_int g_owner_set;
static atomic_int g_write_fail;
static atomic_int g_writes;
static atomic_int g_failed_writes;

/* A gate the pump's device write waits at while it is closed, so a test decides exactly which write fails, and can
   look at the stream while the pump is inside a write. */
static pthread_mutex_t g_gate_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_gate_cv = PTHREAD_COND_INITIALIZER;
static int g_gate_closed;
static int g_gate_waiters;

static void
gate_close(void) {
    assert(pthread_mutex_lock(&g_gate_mu) == 0);
    g_gate_closed = 1;
    assert(pthread_mutex_unlock(&g_gate_mu) == 0);
}

static void
gate_open(void) {
    assert(pthread_mutex_lock(&g_gate_mu) == 0);
    g_gate_closed = 0;
    assert(pthread_cond_broadcast(&g_gate_cv) == 0);
    assert(pthread_mutex_unlock(&g_gate_mu) == 0);
}

/* Wait up to 2 s for the pump to reach the closed gate. */
static int
gate_wait_for_pump(void) {
    for (int i = 0; i < 400; i++) {
        assert(pthread_mutex_lock(&g_gate_mu) == 0);
        const int waiting = g_gate_waiters;
        assert(pthread_mutex_unlock(&g_gate_mu) == 0);
        if (waiting > 0) {
            return 1;
        }
        usleep(5000);
    }
    return 0;
}

static void
gate_pass(void) {
    assert(pthread_mutex_lock(&g_gate_mu) == 0);
    if (g_gate_closed) {
        g_gate_waiters++;
        while (g_gate_closed) {
            assert(pthread_cond_wait(&g_gate_cv, &g_gate_mu) == 0);
        }
        g_gate_waiters--;
    }
    assert(pthread_mutex_unlock(&g_gate_mu) == 0);
}

static void
lifecycle_enter(void) {
    if (atomic_fetch_add(&g_in_lifecycle, 1) != 0) {
        atomic_store(&g_lifecycle_overlap, 1);
    }
    if (atomic_load(&g_owner_set) && !pthread_equal(pthread_self(), g_owner)) {
        atomic_fetch_add(&g_lifecycle_off_owner, 1);
    }
    /* Widen the window an unserialized caller would overlap in. */
    usleep(200);
}

static void
lifecycle_leave(void) {
    atomic_fetch_sub(&g_in_lifecycle, 1);
}

PaError
Pa_Initialize(void) {
    return paNoError;
}

PaError
Pa_Terminate(void) {
    return paNoError;
}

const char*
Pa_GetErrorText(PaError errorCode) {
    return errorCode == paUnanticipatedHostError ? "fake device lost" : "fake error";
}

const char*
Pa_GetVersionText(void) {
    return "fake";
}

PaDeviceIndex
Pa_GetDeviceCount(void) {
    return 1;
}

PaDeviceIndex
Pa_GetDefaultOutputDevice(void) {
    return 0;
}

PaDeviceIndex
Pa_GetDefaultInputDevice(void) {
    return paNoDevice;
}

const PaDeviceInfo*
Pa_GetDeviceInfo(PaDeviceIndex device) {
    return device == 0 ? &g_device : NULL;
}

const PaHostApiInfo*
Pa_GetHostApiInfo(PaHostApiIndex hostApi) {
    (void)hostApi;
    return NULL;
}

PaError
Pa_OpenStream(PaStream** stream, const PaStreamParameters* inputParameters, const PaStreamParameters* outputParameters,
              double sampleRate, unsigned long framesPerBuffer, PaStreamFlags streamFlags,
              PaStreamCallback* streamCallback, void* userData) {
    (void)inputParameters;
    (void)outputParameters;
    (void)sampleRate;
    (void)framesPerBuffer;
    (void)streamFlags;
    (void)streamCallback;
    (void)userData;
    lifecycle_enter();
    fake_pa_stream* s = (fake_pa_stream*)calloc(1, sizeof(*s));
    assert(s != NULL);
    s->open = 1;
    *stream = s;
    atomic_fetch_add(&g_opens, 1);
    lifecycle_leave();
    return paNoError;
}

PaError
Pa_StartStream(PaStream* stream) {
    (void)stream;
    lifecycle_enter();
    lifecycle_leave();
    return paNoError;
}

PaError
Pa_StopStream(PaStream* stream) {
    (void)stream;
    lifecycle_enter();
    lifecycle_leave();
    return paNoError;
}

PaError
Pa_AbortStream(PaStream* stream) {
    (void)stream;
    lifecycle_enter();
    atomic_fetch_add(&g_aborts, 1);
    lifecycle_leave();
    return paNoError;
}

PaError
Pa_CloseStream(PaStream* stream) {
    lifecycle_enter();
    fake_pa_stream* s = (fake_pa_stream*)stream;
    assert(s != NULL && s->open == 1);
    s->open = 0;
    free(s);
    atomic_fetch_add(&g_closes, 1);
    lifecycle_leave();
    return paNoError;
}

PaError
Pa_WriteStream(PaStream* stream, const void* buffer, unsigned long frames) {
    (void)buffer;
    (void)frames;
    const fake_pa_stream* s = (const fake_pa_stream*)stream;
    assert(s != NULL && s->open == 1);
    gate_pass();
    if (atomic_load(&g_write_fail)) {
        atomic_fetch_add(&g_failed_writes, 1);
        return paUnanticipatedHostError;
    }
    atomic_fetch_add(&g_writes, 1);
    /* A device takes a 20 ms chunk in about that long. */
    usleep(2000);
    return paNoError;
}

PaError
Pa_ReadStream(PaStream* stream, void* buffer, unsigned long frames) {
    (void)stream;
    (void)buffer;
    (void)frames;
    return paNoError;
}

/*============================================================================
 * Tests
 *============================================================================*/

static void
sleep_ms(int ms) {
    usleep((useconds_t)ms * 1000U);
}

static dsd_audio_output_health
health_of(dsd_audio_stream* stream) {
    dsd_audio_output_health h;
    assert(dsd_audio_output_health_get(stream, &h) == 0);
    return h;
}

/* Wait up to 2 s for @p pred on the stream's health. */
static int
wait_for_health(dsd_audio_stream* stream, int (*pred)(const dsd_audio_output_health*)) {
    for (int i = 0; i < 400; i++) {
        const dsd_audio_output_health h = health_of(stream);
        if (pred(&h)) {
            return 1;
        }
        sleep_ms(5);
    }
    return 0;
}

static int
is_down(const dsd_audio_output_health* h) {
    return h->down == 1;
}

static int
recovered_once(const dsd_audio_output_health* h) {
    return h->recoveries == 1U && h->down == 0;
}

/* Wait up to 2 s for the device to have taken a write. */
static int
wait_for_writes_above(int count) {
    for (int i = 0; i < 400; i++) {
        if (atomic_load(&g_writes) > count) {
            return 1;
        }
        sleep_ms(5);
    }
    return 0;
}

static dsd_audio_stream*
open_async_output(void) {
    dsd_audio_params params;
    DSD_MEMSET(&params, 0, sizeof(params));
    params.sample_rate = 8000;
    params.channels = 2;
    params.bits_per_sample = 16;
    params.async_output = 1;
    dsd_audio_stream* stream = dsd_audio_open_output(&params);
    assert(stream != NULL);
    assert(stream->use_async == 1);
    return stream;
}

static void
test_async_recovery_on_the_opening_thread(void) {
    int16_t block[160 * 2];
    DSD_MEMSET(block, 0, sizeof(block));
    g_owner = pthread_self();
    atomic_store(&g_owner_set, 1);
    atomic_store(&g_lifecycle_off_owner, 0);

    dsd_audio_stream* stream = open_async_output();
    const int opens_at_start = atomic_load(&g_opens);

    /* Playing. */
    assert(dsd_audio_write(stream, block, 160) == 160);
    assert(wait_for_writes_above(0));

    /* The device goes away at a write the test picks: the pump is held inside it, the failure is armed, and only then
       does the write return. The pump marks the device down and leaves it alone; nothing here writes meanwhile. */
    gate_close();
    assert(dsd_audio_write(stream, block, 160) == 160);
    assert(gate_wait_for_pump());
    atomic_store(&g_write_fail, 1);
    gate_open();
    assert(wait_for_health(stream, is_down));
    dsd_audio_output_health h = health_of(stream);
    assert(h.failures == 1U && h.recoveries == 0U);
    assert(strcmp(h.error, "fake device lost") == 0);
    assert(atomic_load(&g_opens) == opens_at_start);
    assert(atomic_load(&g_closes) == 0);

    /* A drain while it is down fails at once. */
    assert(dsd_audio_drain(stream) == -1);

    /* The next write reopens it, here: the first retry is due at once. The reopened device's first write is held: the
       episode is not over while it is, and is once the device took it. */
    atomic_store(&g_write_fail, 0);
    const int writes_before = atomic_load(&g_writes);
    gate_close();
    assert(dsd_audio_write(stream, block, 160) == 160);
    assert(atomic_load(&g_aborts) == 1 && atomic_load(&g_closes) == 1);
    assert(atomic_load(&g_opens) == opens_at_start + 1);
    assert(gate_wait_for_pump());
    h = health_of(stream);
    assert(h.down == 0 && h.failures == 1U && h.recoveries == 0U);
    gate_open();
    assert(wait_for_health(stream, recovered_once));
    assert(atomic_load(&g_writes) > writes_before);
    h = health_of(stream);
    assert(h.failures == 1U && h.recoveries == 1U);

    /* Failing again: the stream is closed while the device is down, which aborts it, here. */
    gate_close();
    assert(dsd_audio_write(stream, block, 160) == 160);
    assert(gate_wait_for_pump());
    atomic_store(&g_write_fail, 1);
    gate_open();
    assert(wait_for_health(stream, is_down));
    const int aborts_before = atomic_load(&g_aborts);
    dsd_audio_close(stream);
    assert(atomic_load(&g_aborts) == aborts_before + 1);
    atomic_store(&g_write_fail, 0);

    /* Every open and close above ran on this thread, none on the pump. */
    assert(atomic_load(&g_lifecycle_off_owner) == 0);
    assert(atomic_load(&g_lifecycle_overlap) == 0);
    atomic_store(&g_owner_set, 0);
}

static void*
open_close_loop(void* arg) {
    (void)arg;
    int16_t block[160 * 2];
    DSD_MEMSET(block, 0, sizeof(block));
    for (int i = 0; i < 20; i++) {
        dsd_audio_stream* stream = open_async_output();
        (void)dsd_audio_write(stream, block, 160);
        (void)dsd_audio_drain(stream);
        dsd_audio_close(stream);
    }
    return NULL;
}

static void
test_streams_on_two_threads_never_overlap_in_portaudio(void) {
    atomic_store(&g_lifecycle_overlap, 0);
    pthread_t a;
    pthread_t b;
    assert(pthread_create(&a, NULL, open_close_loop, NULL) == 0);
    assert(pthread_create(&b, NULL, open_close_loop, NULL) == 0);
    assert(pthread_join(a, NULL) == 0);
    assert(pthread_join(b, NULL) == 0);
    assert(atomic_load(&g_lifecycle_overlap) == 0);
}

int
main(void) {
    assert(dsd_audio_init() == 0);
    test_async_recovery_on_the_opening_thread();
    test_streams_on_two_threads_never_overlap_in_portaudio();
    dsd_audio_cleanup();
    printf("PLATFORM_AUDIO_PORTAUDIO_RECOVERY: OK\n");
    return 0;
}

// NOLINTEND(bugprone-suspicious-include)
