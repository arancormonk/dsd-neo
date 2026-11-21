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
#include <stdlib.h>
/* Issue #834: while set, every allocation the backend under test makes fails (the macro below renames its calls; the
   test's own go to the C library), so a discard is seen to lose no new audio when memory runs out. */
static int g_backend_alloc_fails;

static void*
backend_alloc(size_t bytes) {
    return g_backend_alloc_fails ? NULL : malloc(bytes);
}

#define malloc(bytes) backend_alloc(bytes)
#include "../../src/platform/audio_portaudio.c"
#undef malloc

#include <assert.h>
#include <portaudio.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/platform/atomic_compat.h"
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

/* The input the discard test opens (paNoDevice: none), the frames its stream reports available (a negative value is
   the error the availability call returns), and the frames read from it. */
static PaDeviceIndex g_default_input = paNoDevice;
static signed long g_read_available;
static unsigned long g_frames_read;
/* The clock the input discard tests drive (issue #834): the discard reads it after its count, through
   portaudio_discard_input_timed(), so the frames it keeps never depend on how fast the host runs the test. */
static uint64_t g_discard_now_ns = 1000000000000ULL;
/* The next availability query is late (issue #834): the decoder was descheduled this long first, on the test's clock,
   while the device captured g_available_late_frames more, once. g_available_real_delay_ms adds a real pause to the next
   query too, once, which the test's clock does not see. */
static uint64_t g_available_late_ns;
static signed long g_available_late_frames;
static unsigned int g_available_real_delay_ms;
/* Right after the next availability query answers, the device holds only this many frames (-1: no change), once: what
   it reported is no longer all there, and a blocking read of it would wait for new capture. A read that asks for more
   than the device holds is counted. */
static signed long g_available_shrinks_to = -1;
static int g_reads_past_available;
/* Input frames numbered from g_read_next (mono), when g_read_numbered. At the g_overrun_at_read-th read from then on
   (0: never) the input buffer has overrun first (issue #834): PortAudio threw its oldest frames away, what it holds is
   g_overrun_available frames numbered from g_overrun_next, and the read reports paInputOverflowed. */
static int g_read_numbered;
static int16_t g_read_next;
static int g_overrun_at_read;
static int16_t g_overrun_next;
static signed long g_overrun_available;
/* From the overrun on, the backend's allocations fail (g_backend_alloc_fails). */
static int g_overrun_runs_out_of_memory;
/* How far the test's clock moves at each read: a driver's slow reads. */
static uint64_t g_read_advances_ns;

PaDeviceIndex
Pa_GetDefaultInputDevice(void) {
    return g_default_input;
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
    PaError result = paNoError;
    g_discard_now_ns += g_read_advances_ns;
    if (g_overrun_at_read > 0) {
        g_overrun_at_read--;
        if (g_overrun_at_read == 0) {
            g_read_available = g_overrun_available;
            g_read_next = g_overrun_next;
            result = paInputOverflowed;
            g_backend_alloc_fails = g_overrun_runs_out_of_memory;
        }
    }
    if (g_read_numbered && buffer != NULL) {
        int16_t* out = (int16_t*)buffer;
        for (unsigned long i = 0; i < frames; i++) {
            out[i] = g_read_next++;
        }
    }
    if ((signed long)frames > g_read_available) {
        g_reads_past_available++;
    }
    g_frames_read += frames;
    if (g_read_available > 0) {
        g_read_available -= (signed long)frames < g_read_available ? (signed long)frames : g_read_available;
    }
    return result;
}

/* Availability queries made, for a case that bounds them. */
static unsigned long g_available_calls = 0U;

signed long
Pa_GetStreamReadAvailable(PaStream* stream) {
    (void)stream;
    g_available_calls++;
    if (g_available_real_delay_ms > 0U) {
        usleep((useconds_t)g_available_real_delay_ms * 1000U);
        g_available_real_delay_ms = 0U;
    }
    if (g_available_late_ns > 0U) {
        g_discard_now_ns += g_available_late_ns;
        g_read_available += g_available_late_frames;
        g_available_late_ns = 0U;
        g_available_late_frames = 0;
    }
    const signed long answer = g_read_available;
    if (g_available_shrinks_to >= 0) {
        g_read_available = g_available_shrinks_to;
        g_available_shrinks_to = -1;
    }
    return answer;
}

static uint64_t
discard_clock(void) {
    return g_discard_now_ns;
}

/* An input discard on the test's clock. */
static int
discard_at(dsd_audio_stream* in, uint64_t marked_ns) {
    return portaudio_discard_input_timed(in, marked_ns, discard_clock);
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

/* An input discard on the test's clock, with @p available frames reported and nothing captured meanwhile: it drops
   what was held less the frames captured since the mark @p marked_ns (rounded up), and reads exactly what it drops. */
static int
discard_counted(dsd_audio_stream* in, uint64_t marked_ns, signed long available) {
    const unsigned long read_before = g_frames_read;
    const int dropped = discard_at(in, marked_ns);
    const uint64_t expected =
        (uint64_t)available - audio_input_keep_since(marked_ns, g_discard_now_ns, 48000, (uint64_t)available);
    if (dropped < 0 || (uint64_t)dropped != expected) {
        DSD_FPRINTF(stderr, "a discard of %ld frames dropped %d (expected %llu)\n", available, dropped,
                    (unsigned long long)expected);
    }
    assert(dropped >= 0 && (uint64_t)dropped == expected);
    assert(g_frames_read - read_before == (unsigned long)dropped);
    return dropped;
}

/* Issue #834: an input discard reads exactly the frames the stream reports available, in chunks, and no more, less
   the newest frames it keeps (the audio captured since the mark: 5 ms at 48 kHz is 240 frames, and a mark longer ago
   than what is held keeps it all); an availability error is an error. */
static void
test_input_discard_reads_what_is_available(void) {
    g_default_input = 0;
    const dsd_audio_params params = {48000, 1, 16, NULL, "test", 0};
    dsd_audio_stream* in = dsd_audio_open_input(&params);
    assert(in != NULL);
    g_read_available = 600;
    g_frames_read = 0U;
    assert(discard_counted(in, g_discard_now_ns, 600) == 600);
    g_read_available = 0;
    assert(discard_counted(in, g_discard_now_ns, 0) == 0);
    g_read_available = 600;
    assert(discard_counted(in, g_discard_now_ns - 5000000U, 600) == 360);
    g_read_available = 600;
    assert(discard_counted(in, g_discard_now_ns - 1000000000U, 600) == 0);
    assert(g_read_available == 600);
    const unsigned long read_before = g_frames_read;
    g_read_available = (signed long)paUnanticipatedHostError;
    assert(discard_at(in, g_discard_now_ns) < 0);
    assert(g_frames_read == read_before);
    g_read_available = 0;
    dsd_audio_close(in);
    g_default_input = paNoDevice;
}

/* Issue #834: the decoder was descheduled after the retune's mark, before the discard sampled the stream (the query
   is late by 100 ms): 240 frames were held at the mark, and 4800 more (100 ms at 48 kHz) captured meanwhile. Exactly
   the 240 go. */
static void
test_input_discard_keeps_what_was_captured_before_its_snapshot(void) {
    g_default_input = 0;
    const dsd_audio_params params = {48000, 1, 16, NULL, "test", 0};
    dsd_audio_stream* in = dsd_audio_open_input(&params);
    assert(in != NULL);
    g_read_available = 240;
    g_frames_read = 0U;
    g_available_late_ns = 100000000U;
    g_available_late_frames = 4800;
    const int dropped = discard_at(in, g_discard_now_ns);
    if (dropped != 240 || g_read_available != 4800) {
        DSD_FPRINTF(stderr, "a late snapshot's discard dropped %d frames, %ld left (expected 240, 4800 kept)\n",
                    dropped, g_read_available);
    }
    assert(dropped == 240 && g_read_available == 4800);
    g_read_available = 0;
    dsd_audio_close(in);
    g_default_input = paNoDevice;
}

/* Issue #834: the input buffer overruns during the discard (a 100 ms buffer turning over while the decoder was
   descheduled): 48000 frames were available at the count, and at the discard's second read PortAudio has thrown its
   oldest frames away, so what it holds from there on (4800 frames, numbered from 20000) was captured after the mark.
   The discard drops the first read's 256 frames only, keeps the second read's chunk for the next read, and leaves the
   rest in the device. Every expectation is on the test's clock; @p real_delay_ms pauses each discard's first
   availability query for real as well, which changes nothing. */
static void
input_discard_stops_at_an_overrun(unsigned int real_delay_ms) {
    g_default_input = 0;
    const dsd_audio_params params = {48000, 1, 16, NULL, "test", 0};
    dsd_audio_stream* in = dsd_audio_open_input(&params);
    assert(in != NULL);
    g_available_real_delay_ms = real_delay_ms;
    g_read_available = 48000;
    g_read_numbered = 1;
    g_read_next = 0;
    g_overrun_at_read = 2;
    g_overrun_next = 20000;
    g_overrun_available = 4800;
    const int dropped = discard_at(in, g_discard_now_ns);
    int16_t next[4] = {0};
    const int got = dsd_audio_read(in, next, 4);
    if (dropped != 256 || got != 4 || next[0] != 20000) {
        DSD_FPRINTF(stderr, "an overrun's discard dropped %d frames, next %d (expected 256, then 20000)\n", dropped,
                    (int)next[0]);
    }
    assert(dropped == 256 && got == 4);
    for (int i = 0; i < 4; i++) {
        assert(next[i] == (int16_t)(20000 + i));
    }
    /* The kept chunk goes first, then the device: one read across both. */
    int16_t across[300] = {0};
    assert(dsd_audio_read(in, across, 300) == 300);
    for (int i = 0; i < 300; i++) {
        assert(across[i] == (int16_t)(20004 + i));
    }
    /* An overrun at the first read drops nothing and keeps that chunk (frames numbered from 1000 on); a later discard
       of a mark long ago keeps it all, and one of a mark now drops the kept chunk no read took, as the oldest the stream
       holds, then all the device holds (24000 frames). */
    g_read_available = 1000;
    g_overrun_at_read = 1;
    g_overrun_next = 1000;
    g_overrun_available = 1000;
    g_available_real_delay_ms = real_delay_ms;
    assert(discard_at(in, g_discard_now_ns) == 0);
    assert(g_overrun_at_read == 0);
    g_read_available = 24000;
    g_available_real_delay_ms = real_delay_ms;
    assert(discard_at(in, g_discard_now_ns - 1000000000U) == 0);
    int16_t one = 0;
    g_available_real_delay_ms = real_delay_ms;
    const int later = discard_at(in, g_discard_now_ns);
    assert(dsd_audio_read(in, &one, 1) == 1);
    if (later != 256 + 24000 || one != (int16_t)(1000 + later)) {
        DSD_FPRINTF(stderr, "a discard after an overrun's chunk dropped %d, next %d (expected %d, then %d)\n", later,
                    (int)one, 256 + 24000, (int)(int16_t)(1000 + 256 + 24000));
    }
    assert(later == 256 + 24000 && one == (int16_t)(1000 + later));
    g_read_numbered = 0;
    g_read_available = 0;
    g_available_real_delay_ms = 0U;
    dsd_audio_close(in);
    g_default_input = paNoDevice;
}

/* The same, with no pause, and with a 25 ms pause in each discard's count, which a real clock would take for captured
   audio (more than 1000 frames at 48 kHz), keeping everything and leaving the overrun unread. */
static void
test_input_discard_stops_at_an_overrun(void) {
    input_discard_stops_at_an_overrun(0U);
    input_discard_stops_at_an_overrun(25U);
}

/* Issue #834: memory runs out once the device overran: the overrun's chunk, which may be the new channel's, is still
   kept, in the buffer the discard reserved before it read anything. And with no memory for that buffer the discard
   reads nothing at all: every frame stays in the device. */
static void
test_input_discard_without_memory_loses_no_new_audio(void) {
    g_default_input = 0;
    const dsd_audio_params params = {48000, 1, 16, NULL, "test", 0};
    dsd_audio_stream* in = dsd_audio_open_input(&params);
    assert(in != NULL);
    g_read_available = 48000;
    g_read_numbered = 1;
    g_read_next = 0;
    g_overrun_at_read = 2;
    g_overrun_next = 20000;
    g_overrun_available = 4800;
    g_overrun_runs_out_of_memory = 1;
    const int dropped = discard_at(in, g_discard_now_ns);
    g_backend_alloc_fails = 0;
    g_overrun_runs_out_of_memory = 0;
    int16_t next = 0;
    const int got = dsd_audio_read(in, &next, 1);
    if (dropped != 256 || got != 1 || next != 20000) {
        DSD_FPRINTF(stderr,
                    "an overrun's discard out of memory dropped %d frames, next %d (expected 256, then 20000)\n",
                    dropped, (int)next);
    }
    assert(dropped == 256 && got == 1 && next == 20000);
    g_read_available = 1000;
    g_read_next = 5000;
    g_backend_alloc_fails = 1;
    assert(discard_at(in, g_discard_now_ns) == -1);
    g_backend_alloc_fails = 0;
    assert(g_read_available == 1000);
    g_read_numbered = 0;
    g_read_available = 0;
    dsd_audio_close(in);
    g_default_input = paNoDevice;
}

/* Issue #834: the device holds fewer frames than it reported at the count (it lost some since): the discard never
   asks a read for more than the stream holds just before it, since a blocking read of frames not there waits for new
   capture, without end on a source that stopped. 1000 counted, 400 still held: those 400 go, no read waits. */
static void
test_input_discard_never_reads_past_what_is_held(void) {
    g_default_input = 0;
    const dsd_audio_params params = {48000, 1, 16, NULL, "test", 0};
    dsd_audio_stream* in = dsd_audio_open_input(&params);
    assert(in != NULL);
    g_read_available = 1000;
    g_available_shrinks_to = 400;
    g_reads_past_available = 0;
    const int dropped = discard_at(in, g_discard_now_ns);
    if (g_reads_past_available != 0 || dropped != 400) {
        DSD_FPRINTF(stderr, "a discard made %d reads past what the device held, dropped %d (expected 0, 400)\n",
                    g_reads_past_available, dropped);
    }
    assert(g_reads_past_available == 0 && dropped == 400 && g_read_available == 0);
    g_available_shrinks_to = -1;
    dsd_audio_close(in);
    g_default_input = paNoDevice;
}

/* Issue #834: what one discard drops is bounded as documented (docs/cli.md) and as the other sound-card inputs bound it:
   at most 2 s of frames at the stream's rate, however much the driver holds, and no read begun 250 ms or more after
   the discard's first, however slow the driver's reads. What the bounds leave stays in the device. */
static void
test_input_discard_is_bounded(void) {
    g_default_input = 0;
    const dsd_audio_params params = {48000, 1, 16, NULL, "test", 0};
    dsd_audio_stream* in = dsd_audio_open_input(&params);
    assert(in != NULL);
    g_read_available = 200000;
    int dropped = discard_at(in, g_discard_now_ns);
    if (dropped != 96000 || g_read_available != 104000) {
        DSD_FPRINTF(stderr, "a discard of 200000 frames dropped %d, %ld left (expected 96000, 104000)\n", dropped,
                    g_read_available);
    }
    assert(dropped == 96000 && g_read_available == 104000);
    /* Each read takes 100 ms: reads begin at 0, 100 and 200 ms, and none at 300 ms. */
    g_read_available = 10000;
    g_read_advances_ns = 100000000U;
    dropped = discard_at(in, g_discard_now_ns);
    g_read_advances_ns = 0U;
    if (dropped != 3 * 256 || g_read_available != 10000 - 3 * 256) {
        DSD_FPRINTF(stderr, "a discard of slow reads dropped %d, %ld left (expected %d, %d)\n", dropped,
                    g_read_available, 3 * 256, 10000 - 3 * 256);
    }
    assert(dropped == 3 * 256 && g_read_available == 10000 - 3 * 256);
    g_read_available = 0;
    dsd_audio_close(in);
    g_default_input = paNoDevice;
}

/* The hunt's held reads take one frame at a time: a count the stream reported covers the frames after it, so a run of
   buffered frames costs one availability query, not one each, and never reads past what was counted. A blocking read
   or a discard takes frames the count included, so the next held read counts again. */
static void
test_held_reads_count_availability_once_per_run(void) {
    g_default_input = 0;
    const dsd_audio_params params = {48000, 1, 16, NULL, "test", 0};
    dsd_audio_stream* in = dsd_audio_open_input(&params);
    assert(in != NULL);
    int16_t frame = 0;
    g_read_available = 100;
    g_reads_past_available = 0;
    g_available_calls = 0U;
    for (int i = 0; i < 100; i++) {
        assert(dsd_audio_read_held(in, &frame, 1U) == 1);
    }
    assert(g_available_calls == 1U);
    assert(dsd_audio_read_held(in, &frame, 1U) == 0); /* the counted run is used up: counted again, none held */
    assert(g_available_calls == 2U);
    g_read_available = 10;
    assert(dsd_audio_read_held(in, &frame, 1U) == 1);
    assert(g_available_calls == 3U);
    assert(dsd_audio_read(in, &frame, 5U) == 5); /* takes frames the count included */
    assert(dsd_audio_read_held(in, &frame, 1U) == 1);
    assert(g_available_calls == 4U);
    assert(discard_at(in, g_discard_now_ns - 1000000000U) == 0); /* keeps all, but counts afresh */
    assert(dsd_audio_read_held(in, &frame, 1U) == 1);
    assert(g_available_calls == 6U);
    assert(g_reads_past_available == 0);
    g_read_available = 0;
    dsd_audio_close(in);
    g_default_input = paNoDevice;
}

int
main(void) {
    assert(dsd_audio_init() == 0);
    test_async_recovery_on_the_opening_thread();
    test_streams_on_two_threads_never_overlap_in_portaudio();
    test_input_discard_reads_what_is_available();
    test_input_discard_keeps_what_was_captured_before_its_snapshot();
    test_input_discard_stops_at_an_overrun();
    test_input_discard_without_memory_loses_no_new_audio();
    test_input_discard_never_reads_past_what_is_held();
    test_input_discard_is_bounded();
    test_held_reads_count_availability_once_per_run();
    dsd_audio_cleanup();
    printf("PLATFORM_AUDIO_PORTAUDIO_RECOVERY: OK\n");
    return 0;
}

// NOLINTEND(bugprone-suspicious-include)
