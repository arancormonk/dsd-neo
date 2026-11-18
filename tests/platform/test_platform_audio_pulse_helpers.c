// SPDX-License-Identifier: GPL-3.0-or-later
// Coverage fixtures intentionally use private-source inclusion, synthetic sentinels,
// invalid-value negative vectors, or wrapper symbols to exercise guarded behavior.
// NOLINTBEGIN(bugprone-suspicious-include)
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <assert.h>
#include <pulse/channelmap.h>
#include <pulse/context.h>
#include <pulse/def.h>
#include <pulse/error.h>
#include <pulse/introspect.h>
#include <pulse/mainloop-api.h>
#include <pulse/operation.h>
#include <pulse/sample.h>
#include <pulse/simple.h>
#include <pulse/stream.h>
#include <pulse/thread-mainloop.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <dsd-neo/platform/env.h>
#include <stdlib.h>
/* Issue #834: while set, every allocation the backend under test makes fails (the macro below renames its calls; the
   test's own go to the C library), so a discard is seen to lose no new audio when memory runs out. */
static int g_backend_alloc_fails;

static void*
backend_alloc(size_t bytes) {
    return g_backend_alloc_fails ? NULL : malloc(bytes);
}

#define malloc(bytes) backend_alloc(bytes)
#include "../../src/platform/audio_pulse.c"
#include "../../src/platform/audio_pulse_record.c"
#undef malloc
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/platform/audio.h"
#include "dsd-neo/platform/audio_concealment.h"
#include "dsd-neo/platform/threading.h"
#include "dsd-neo/platform/timing.h"

/* The backend reads its variables through the override store (#782); this test links no platform library, and no
   override is ever set here, so the process environment is the whole answer. */
size_t
dsd_env_copy(const char* name, char* buf, size_t cap) {
    const char* v = getenv(name);
    if (buf != NULL && cap > 0) {
        DSD_SNPRINTF(buf, cap, "%s", v ? v : "");
    }
    return v ? strlen(v) + 1U : 0U;
}

static int g_pa_simple_new_fail;
static int g_pa_simple_new_calls;
static uint64_t g_pa_simple_new_delay_ns; /* how long an open takes on the fake clock */
static char g_pa_simple_new_dev[64];
static char g_pa_simple_new_app[64];
static int g_pa_simple_error;
/* The input's record stream (audio_pulse_record.c) over a fake threaded mainloop, context and stream. Its open fails
   at the connect, with g_pa_simple_error, when g_pa_simple_new_fail; a peek fails with it when g_pa_record_peek_fail;
   the stream reads as failed when g_pa_record_failed. */
static int g_pa_record_peek_fail;
static int g_pa_record_failed;
static int g_pa_record_peeks;
static int g_pa_record_frees;
static int g_pa_lock_depth;
/* The capture the input reads (issue #834): mono frames numbered from g_pa_capture_next, g_pa_capture_held of them held
   by the client, handed over in fragments of at most g_pa_fragment_frames (frames handed over stay held until dropped).
   A wait on the mainloop for audio (a read that found none held) takes 2 ms on the fake clock and brings 96 frames,
   unless the source stopped delivering (g_pa_capture_stopped: suspended during a retune): then nothing would ever come,
   so the wait is counted as one without end and the stream fails, which ends the read. Off unless g_pa_capture_on. */
static int g_pa_capture_on;
static int16_t g_pa_capture_next;
static size_t g_pa_capture_held;
static size_t g_pa_fragment_frames;
static int g_pa_capture_stopped;
static int g_pa_waits;
static int g_pa_unbounded_waits;
/* At the g_pa_overrun_at_peek-th peek from then on (0: never), the decoder is preempted for g_pa_overrun_ns on the
   fake clock, and the server's record queue overruns meanwhile (issue #834): it throws its oldest frames away, so what
   the client holds is g_pa_overrun_held frames numbered from g_pa_overrun_next. */
static int g_pa_overrun_at_peek;
static uint64_t g_pa_overrun_ns;
static int16_t g_pa_overrun_next;
static size_t g_pa_overrun_held;
/* The next peek finds a hole in the stream of this many bytes (issue #834), once. */
static size_t g_pa_hole_bytes;
static int g_pa_simple_write_fail;
static int g_pa_simple_write_calls;
static int g_pa_simple_drain_fail;
static int g_pa_simple_drain_calls;

static void
reset_pa_simple_fakes(void) {
    g_pa_simple_new_fail = 0;
    g_pa_simple_new_calls = 0;
    g_pa_simple_new_delay_ns = 0;
    g_pa_simple_new_dev[0] = '\0';
    g_pa_simple_new_app[0] = '\0';
    g_pa_simple_error = 0;
    g_pa_record_peek_fail = 0;
    g_pa_record_failed = 0;
    g_pa_record_peeks = 0;
    g_pa_record_frees = 0;
    g_pa_lock_depth = 0;
    g_pa_capture_on = 0;
    g_pa_capture_next = 0;
    g_pa_capture_held = 0U;
    g_pa_fragment_frames = 100U;
    g_pa_capture_stopped = 0;
    g_pa_waits = 0;
    g_pa_unbounded_waits = 0;
    g_pa_overrun_at_peek = 0;
    g_pa_overrun_ns = 0U;
    g_pa_overrun_next = 0;
    g_pa_overrun_held = 0U;
    g_pa_hole_bytes = 0U;
    g_pa_simple_write_fail = 0;
    g_pa_simple_write_calls = 0;
    g_pa_simple_drain_fail = 0;
    g_pa_simple_drain_calls = 0;
}

/* The device recovery's clock (audio_output_recovery.h), set by the tests. */
static uint64_t g_now_ns = 1000000000ULL;
/* The next clock read is late (issue #834): the clock moves this far first, while the capture holds
   g_late_clock_frames more, once. */
static uint64_t g_late_clock_ns;
static size_t g_late_clock_frames;

uint64_t
dsd_time_monotonic_ns(void) {
    if (g_late_clock_ns > 0U) {
        g_now_ns += g_late_clock_ns;
        g_pa_capture_held += g_late_clock_frames;
        g_late_clock_ns = 0U;
        g_late_clock_frames = 0U;
    }
    return g_now_ns;
}

/* dsd_thread_create()'s SIGTSTP halves (threading_posix.c): no assertion here reads a thread's signal mask. */
void
dsd_thread_create_stop_hold(void) {}

int
dsd_thread_create_stop_release(int rc) {
    return rc;
}

int
dsd_mutex_init(dsd_mutex_t* mutex) {
    (void)mutex;
    return 0;
}

void
dsd_thread_yield(void) {
    /* Nothing to yield to: this test drives the pump body inline. */
}

int
dsd_mutex_destroy(dsd_mutex_t* mutex) {
    (void)mutex;
    return 0;
}

int
dsd_mutex_lock(dsd_mutex_t* mutex) {
    (void)mutex;
    return 0;
}

int
dsd_mutex_unlock(dsd_mutex_t* mutex) {
    (void)mutex;
    return 0;
}

int
dsd_cond_init(dsd_cond_t* cond) {
    (void)cond;
    return 0;
}

int
dsd_cond_destroy(dsd_cond_t* cond) {
    (void)cond;
    return 0;
}

int
dsd_cond_signal(dsd_cond_t* cond) {
    (void)cond;
    return 0;
}

int
dsd_cond_broadcast(dsd_cond_t* cond) {
    (void)cond;
    return 0;
}

int
dsd_cond_wait(dsd_cond_t* cond, dsd_mutex_t* mutex) {
    (void)cond;
    (void)mutex;
    return 0;
}

/* A wait with no one to wake it runs its whole timeout: the fake clock moves on by it. */
int
dsd_cond_timedwait(dsd_cond_t* cond, dsd_mutex_t* mutex, unsigned int timeout_ms) {
    (void)cond;
    (void)mutex;
    g_now_ns += (uint64_t)timeout_ms * 1000000ULL;
    return 0;
}

int
audio_conceal_init(struct audio_conceal_state* cs, size_t buffer_frames, int channels) {
    (void)cs;
    return (buffer_frames > 0U && channels > 0) ? 0 : -1;
}

void
audio_conceal_destroy(struct audio_conceal_state* cs) {
    (void)cs;
}

void
audio_conceal_on_good_buffer(struct audio_conceal_state* cs, const int16_t* buf, size_t frames) {
    (void)cs;
    (void)buf;
    (void)frames;
}

size_t
audio_conceal_on_underrun(struct audio_conceal_state* cs, int16_t* out, size_t frames) {
    const size_t channels = (cs != NULL && cs->channels > 0) ? (size_t)cs->channels : 1U;

    if (out == NULL) {
        return 0;
    }
    DSD_MEMSET(out, 0, frames * channels * sizeof(*out));
    return frames;
}

pa_simple*
pa_simple_new(const char* server, const char* name, pa_stream_direction_t dir, const char* dev, const char* stream_name,
              const pa_sample_spec* ss, const pa_channel_map* map, const pa_buffer_attr* attr, int* error) {
    (void)server;
    (void)dir;
    (void)stream_name;
    (void)ss;
    (void)map;
    (void)attr;
    g_pa_simple_new_calls++;
    g_now_ns += g_pa_simple_new_delay_ns;
    DSD_SNPRINTF(g_pa_simple_new_dev, sizeof(g_pa_simple_new_dev), "%s", dev ? dev : "");
    DSD_SNPRINTF(g_pa_simple_new_app, sizeof(g_pa_simple_new_app), "%s", name ? name : "");
    if (g_pa_simple_new_fail) {
        if (error != NULL) {
            *error = g_pa_simple_error;
        }
        return NULL;
    }
    if (error != NULL) {
        *error = 0;
    }
    return (pa_simple*)0x1;
}

void
pa_simple_free(pa_simple* s) {
    (void)s;
}

/* ---- The fake record stream (audio_pulse_record.c) ---- */

static char g_fake_mainloop;
static char g_fake_context;
static char g_fake_stream;
static pa_mainloop_api g_fake_api;
static int16_t g_fake_fragment[4096];
static size_t g_fake_fragment_out; /* frames handed over by the last peek and not dropped yet */
static size_t g_fake_hole_out;     /* bytes of a hole handed over and not dropped yet */

pa_threaded_mainloop*
pa_threaded_mainloop_new(void) {
    return (pa_threaded_mainloop*)&g_fake_mainloop;
}

void
pa_threaded_mainloop_free(pa_threaded_mainloop* m) {
    (void)m;
    g_pa_record_frees++;
}

int
pa_threaded_mainloop_start(pa_threaded_mainloop* m) {
    (void)m;
    return 0;
}

void
pa_threaded_mainloop_stop(pa_threaded_mainloop* m) {
    (void)m;
}

void
pa_threaded_mainloop_lock(pa_threaded_mainloop* m) {
    (void)m;
    g_pa_lock_depth++;
}

void
pa_threaded_mainloop_unlock(pa_threaded_mainloop* m) {
    (void)m;
    assert(g_pa_lock_depth > 0);
    g_pa_lock_depth--;
}

/* A read that found no audio held waits here for the source. */
void
pa_threaded_mainloop_wait(pa_threaded_mainloop* m) {
    (void)m;
    assert(g_pa_lock_depth == 1);
    g_pa_waits++;
    if (g_pa_capture_stopped) {
        g_pa_unbounded_waits++;
        g_pa_record_failed = 1;
        return;
    }
    g_now_ns += 2000000ULL;
    g_pa_capture_held += 96U;
}

void
pa_threaded_mainloop_signal(pa_threaded_mainloop* m, int wait_for_accept) {
    (void)m;
    (void)wait_for_accept;
}

pa_mainloop_api*
pa_threaded_mainloop_get_api(pa_threaded_mainloop* m) {
    (void)m;
    return &g_fake_api;
}

pa_context*
pa_context_new(pa_mainloop_api* mainloop, const char* name) {
    (void)mainloop;
    DSD_SNPRINTF(g_pa_simple_new_app, sizeof(g_pa_simple_new_app), "%s", name ? name : "");
    return (pa_context*)&g_fake_context;
}

void
pa_context_set_state_callback(pa_context* c, pa_context_notify_cb_t cb, void* userdata) {
    (void)c;
    (void)cb;
    (void)userdata;
}

int
pa_context_connect(pa_context* c, const char* server, pa_context_flags_t flags, const pa_spawn_api* api) {
    (void)c;
    (void)server;
    (void)flags;
    (void)api;
    g_pa_simple_new_calls++;
    return g_pa_simple_new_fail ? -1 : 0;
}

pa_context_state_t
pa_context_get_state(const pa_context* c) {
    (void)c;
    return PA_CONTEXT_READY;
}

int
pa_context_errno(const pa_context* c) {
    (void)c;
    return g_pa_simple_error;
}

void
pa_context_disconnect(pa_context* c) {
    (void)c;
}

void
pa_context_unref(pa_context* c) {
    (void)c;
}

pa_stream*
pa_stream_new(pa_context* c, const char* name, const pa_sample_spec* ss, const pa_channel_map* map) {
    (void)c;
    (void)name;
    (void)ss;
    (void)map;
    return (pa_stream*)&g_fake_stream;
}

void
pa_stream_set_state_callback(pa_stream* s, pa_stream_notify_cb_t cb, void* userdata) {
    (void)s;
    (void)cb;
    (void)userdata;
}

void
pa_stream_set_read_callback(pa_stream* p, pa_stream_request_cb_t cb, void* userdata) {
    (void)p;
    (void)cb;
    (void)userdata;
}

int
pa_stream_connect_record(pa_stream* s, const char* dev, const pa_buffer_attr* attr, pa_stream_flags_t flags) {
    (void)s;
    (void)attr;
    (void)flags;
    DSD_SNPRINTF(g_pa_simple_new_dev, sizeof(g_pa_simple_new_dev), "%s", dev ? dev : "");
    g_fake_fragment_out = 0U;
    g_fake_hole_out = 0U;
    return 0;
}

pa_stream_state_t
pa_stream_get_state(const pa_stream* p) {
    (void)p;
    return g_pa_record_failed ? PA_STREAM_FAILED : PA_STREAM_READY;
}

/* From the next peek on, the backend's allocations fail (g_backend_alloc_fails). */
static int g_pa_peek_runs_out_of_memory;

/* The next fragment the client holds, numbered frames, or none (no data and no length) when it holds none. */
int
pa_stream_peek(pa_stream* p, const void** data, size_t* nbytes) {
    (void)p;
    assert(g_pa_lock_depth == 1 && g_fake_fragment_out == 0U && g_fake_hole_out == 0U);
    g_pa_record_peeks++;
    *data = NULL;
    *nbytes = 0U;
    if (g_pa_record_peek_fail) {
        return -1;
    }
    if (g_pa_hole_bytes > 0U) {
        *nbytes = g_pa_hole_bytes;
        g_fake_hole_out = g_pa_hole_bytes;
        g_pa_hole_bytes = 0U;
        return 0;
    }
    if (g_pa_peek_runs_out_of_memory) {
        g_backend_alloc_fails = 1; /* from the discard's first read of the client on */
    }
    const int overrun_now = g_pa_overrun_at_peek == 1;
    if (g_pa_overrun_at_peek > 0) {
        g_pa_overrun_at_peek--;
    }
    if (overrun_now) {
        g_now_ns += g_pa_overrun_ns;
        g_pa_capture_next = g_pa_overrun_next;
        g_pa_capture_held = g_pa_overrun_held;
    }
    size_t frames = g_pa_capture_on ? g_pa_capture_held : 0U;
    if (frames > g_pa_fragment_frames) {
        frames = g_pa_fragment_frames;
    }
    if (frames > sizeof g_fake_fragment / sizeof g_fake_fragment[0]) {
        frames = sizeof g_fake_fragment / sizeof g_fake_fragment[0];
    }
    for (size_t i = 0; i < frames; i++) {
        g_fake_fragment[i] = (int16_t)(g_pa_capture_next + (int16_t)i);
    }
    g_fake_fragment_out = frames;
    *data = frames > 0U ? g_fake_fragment : NULL;
    *nbytes = frames * sizeof(int16_t);
    return 0;
}

int
pa_stream_drop(pa_stream* p) {
    (void)p;
    assert(g_pa_lock_depth == 1 && (g_fake_fragment_out > 0U || g_fake_hole_out > 0U));
    g_pa_capture_next = (int16_t)(g_pa_capture_next + (int16_t)g_fake_fragment_out);
    g_pa_capture_held -= g_fake_fragment_out;
    g_fake_fragment_out = 0U;
    g_fake_hole_out = 0U;
    return 0;
}

void
pa_stream_unref(pa_stream* s) {
    (void)s;
}

int
pa_simple_drain(pa_simple* s, int* error) {
    (void)s;
    g_pa_simple_drain_calls++;
    if (g_pa_simple_drain_fail) {
        if (error != NULL) {
            *error = g_pa_simple_error;
        }
        return -1;
    }
    if (error != NULL) {
        *error = 0;
    }
    return 0;
}

int
pa_simple_write(pa_simple* s, const void* data, size_t bytes, int* error) {
    (void)s;
    (void)data;
    (void)bytes;
    g_pa_simple_write_calls++;
    if (g_pa_simple_write_fail) {
        if (error != NULL) {
            *error = g_pa_simple_error;
        }
        return -1;
    }
    if (error != NULL) {
        *error = 0;
    }
    return 0;
}

static void
test_pulse_size_helpers(void) {
    size_t result = 99;

    assert(ms_to_bytes(0, 1, 20) == 0);
    assert(ms_to_bytes(48000, 0, 20) == 0);
    assert(ms_to_bytes(48000, 1, 0) == 0);
    assert(ms_to_bytes(1, 1, 1) == sizeof(int16_t));
    assert(ms_to_bytes(48000, 2, 125) == 24000U);
    assert(ms_to_bytes(INT32_MAX, UINT8_MAX, INT32_MAX) == UINT32_MAX);

    assert(ms_to_frames(0, 20) == 0);
    assert(ms_to_frames(48000, 0) == 0);
    assert(ms_to_frames(1, 1) == 1);
    assert(ms_to_frames(48000, 20) == 960);
    assert(calc_chunk_frames(0) == 1);
    assert(calc_chunk_frames(48000) == 960);

    assert(size_mul_nonzero(3, 4, &result) == 1);
    assert(result == 12);
    assert(size_mul_nonzero(0, 4, &result) == 0);
    assert(result == 0);
    assert(size_mul_nonzero(SIZE_MAX, 2, &result) == 0);
    assert(result == 0);
    assert(size_mul_nonzero(1, 1, NULL) == 0);
}

static void
test_pulse_ring_wrap_read_and_drop(void) {
    dsd_audio_stream stream;
    int16_t ring[5] = {0};
    int16_t first_write[3] = {1, 2, 3};
    int16_t second_write[4] = {4, 5, 6, 7};
    int16_t third_write[4] = {10, 11, 12, 13};
    int16_t fourth_write[2] = {20, 21};
    int16_t out[6] = {0};

    DSD_MEMSET(&stream, 0, sizeof(stream));
    stream.ring = ring;
    stream.ring_samples_capacity = 5;

    ring_write_samples(NULL, first_write, 3);
    ring_write_samples(&stream, NULL, 3);
    ring_write_samples(&stream, first_write, 0);
    assert(stream.ring_samples_count == 0);

    ring_write_samples(&stream, first_write, 3);
    assert(stream.ring_samples_tail == 3);
    assert(stream.ring_samples_count == 3);
    assert(ring[0] == 1 && ring[1] == 2 && ring[2] == 3);

    assert(ring_read_samples(&stream, out, 2) == 2);
    assert(out[0] == 1 && out[1] == 2);
    assert(stream.ring_samples_head == 2);
    assert(stream.ring_samples_count == 1);

    ring_write_samples(&stream, second_write, 4);
    assert(stream.ring_samples_tail == 2);
    assert(stream.ring_samples_count == 5);
    DSD_MEMSET(out, 0, sizeof(out));
    assert(ring_read_samples(&stream, out, 5) == 5);
    assert(out[0] == 3 && out[1] == 4 && out[2] == 5 && out[3] == 6 && out[4] == 7);
    assert(stream.ring_samples_head == 2);
    assert(stream.ring_samples_count == 0);

    ring_write_samples(&stream, third_write, 4);
    ring_drop_oldest(&stream, 2);
    assert(stream.ring_samples_head == 4);
    assert(stream.ring_samples_count == 2);
    DSD_MEMSET(out, 0, sizeof(out));
    assert(ring_read_samples(&stream, out, 4) == 2);
    assert(out[0] == 12 && out[1] == 13);

    ring_write_samples(&stream, fourth_write, 2);
    ring_drop_oldest(&stream, 99);
    assert(stream.ring_samples_count == 0);
    assert(ring_read_samples(&stream, out, 1) == 0);
}

static void
test_pulse_output_attr_and_async_buffers(void) {
    dsd_audio_params params;
    dsd_audio_stream stream;
    pa_buffer_attr attr;

    DSD_MEMSET(&params, 0, sizeof(params));
    params.sample_rate = 48000;
    params.channels = 2;
    pulse_output_init_attr(&params, &attr);
    assert(attr.tlength == 24000U);
    assert(attr.prebuf == 11520U);
    assert(attr.minreq == 3840U);

    DSD_MEMSET(&params, 0, sizeof(params));
    params.sample_rate = 1;
    params.channels = 1;
    pulse_output_init_attr(&params, &attr);
    assert(attr.tlength == sizeof(int16_t));
    assert(attr.prebuf == sizeof(int16_t));
    assert(attr.minreq == sizeof(int16_t));

    DSD_MEMSET(&stream, 0, sizeof(stream));
    stream.sample_rate = 8000;
    stream.channels = 1;
    pulse_output_init_async_state(&stream, 1);
    assert(stream.use_async == 1);
    assert(stream.stop == 0);
    assert(stream.drain_requested == 0);
    assert(stream.conceal_has_good == 0);

    assert(pulse_output_init_async_sync(&stream) == 1);
    assert(pulse_output_prepare_async_buffers(&stream) == 1);
    assert(stream.chunk_frames == 160U);
    assert(stream.chunk_samples == 160U);
    assert(stream.ring_samples_capacity >= 1280U);
    assert(stream.chunk != NULL);
    assert(stream.ring != NULL);

    pulse_output_disable_async(&stream, 1);
    assert(stream.use_async == 0);
    assert(stream.chunk == NULL);
    assert(stream.ring == NULL);
    assert(stream.ring_samples_capacity == 0);

    DSD_MEMSET(&stream, 0, sizeof(stream));
    pulse_output_init_async_state(&stream, 0);
    assert(stream.use_async == 0);
}

static void
test_pulse_async_write_policy(void) {
    dsd_audio_stream stream;
    int16_t samples[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    int16_t ring[4] = {0};

    DSD_MEMSET(&stream, 0, sizeof(stream));
    stream.handle = (pa_simple*)0x1;
    stream.is_input = 0;
    stream.use_async = 1;
    stream.channels = 1;
    stream.ring = ring;
    stream.ring_samples_capacity = 4;
    assert(dsd_mutex_init(&stream.mu) == 0);
    assert(dsd_cond_init(&stream.cv) == 0);

    assert(dsd_audio_write(&stream, samples, 0) == 0);
    assert(stream.ring_samples_count == 0);

    assert(dsd_audio_write(&stream, samples, 3) == 3);
    assert(stream.ring_samples_count == 3);
    assert(stream.ring_samples_head == 0);
    assert(stream.ring_samples_tail == 3);
    assert(ring[0] == 1 && ring[1] == 2 && ring[2] == 3);

    assert(dsd_audio_write(&stream, samples + 3, 3) == 3);
    assert(stream.drops == 2U);
    assert(stream.ring_samples_count == 4);
    assert(stream.ring_samples_head == 2);

    int16_t out[4] = {0};
    assert(ring_read_samples(&stream, out, 4) == 4);
    assert(out[0] == 3 && out[1] == 4 && out[2] == 5 && out[3] == 6);

    assert(dsd_audio_write(&stream, samples, 8) == 8);
    assert(stream.ring_samples_count == 4);
    DSD_MEMSET(out, 0, sizeof(out));
    assert(ring_read_samples(&stream, out, 4) == 4);
    assert(out[0] == 5 && out[1] == 6 && out[2] == 7 && out[3] == 8);

    stream.drain_requested = 1;
    assert(dsd_audio_write(&stream, samples, 2) == 2);
    assert(stream.drops == 4U);
    assert(stream.ring_samples_count == 0);

    stream.drain_requested = 0;
    stream.ring = NULL;
    assert(dsd_audio_write(&stream, samples, 1) == -1);

    stream.ring = ring;
    stream.stop = 1;
    assert(dsd_audio_write(&stream, samples, 1) == -1);

    stream.stop = 0;
    stream.is_input = 1;
    assert(dsd_audio_write(&stream, samples, 1) == -1);
    assert(strcmp(dsd_audio_get_error(), "Cannot write to input stream") == 0);

    assert(dsd_cond_destroy(&stream.cv) == 0);
    assert(dsd_mutex_destroy(&stream.mu) == 0);
}

static dsd_audio_params
pulse_test_valid_params(void) {
    dsd_audio_params params;
    DSD_MEMSET(&params, 0, sizeof(params));
    params.sample_rate = 48000;
    params.channels = 1;
    params.bits_per_sample = 16;
    params.app_name = "dsd-neo-pulse-helper-test";
    return params;
}

static void
test_pulse_sync_output_open_uses_blocking_write(void) {
    dsd_audio_params params = pulse_test_valid_params();
    int16_t samples[4] = {1, 2, 3, 4};

    params.async_output = 0;
    reset_pa_simple_fakes();

    dsd_audio_stream* stream = dsd_audio_open_output(&params);
    assert(stream != NULL);
    assert(stream->use_async == 0);

    assert(dsd_audio_write(stream, samples, 4) == 4);
    assert(g_pa_simple_write_calls == 1);

    assert(dsd_audio_drain(stream) == 0);
    assert(g_pa_simple_drain_calls == 1);

    dsd_audio_close(stream);
}

static void
test_pulse_simple_error_paths(void) {
    dsd_audio_params params = pulse_test_valid_params();
    dsd_audio_stream stream;
    int16_t samples[4] = {1, 2, 3, 4};

    reset_pa_simple_fakes();
    g_pa_simple_new_fail = 1;
    g_pa_simple_error = PA_ERR_CONNECTIONREFUSED;
    assert(dsd_audio_open_input(&params) == NULL);
    assert(strcmp(dsd_audio_get_error(), pa_strerror(PA_ERR_CONNECTIONREFUSED)) == 0);

    reset_pa_simple_fakes();
    g_pa_simple_new_fail = 1;
    g_pa_simple_error = PA_ERR_ACCESS;
    assert(dsd_audio_open_output(&params) == NULL);
    assert(strcmp(dsd_audio_get_error(), pa_strerror(PA_ERR_ACCESS)) == 0);

    /* The input's record stream: a failed peek fails the read with the context's error, and so does a stream that
       failed while the read waited for audio. A discard fails the same way, and an input closes its record stream. */
    reset_pa_simple_fakes();
    params.device = "alsa_input.test-source";
    dsd_audio_stream* in = dsd_audio_open_input(&params);
    assert(in != NULL && in->record != NULL && in->handle == NULL);
    assert(strcmp(g_pa_simple_new_dev, "alsa_input.test-source") == 0);
    g_pa_record_peek_fail = 1;
    g_pa_simple_error = PA_ERR_IO;
    assert(dsd_audio_read(in, samples, 4) == -1);
    assert(g_pa_record_peeks == 1);
    assert(strcmp(dsd_audio_get_error(), pa_strerror(PA_ERR_IO)) == 0);
    assert(dsd_audio_discard_input(in, g_now_ns) == -1);
    assert(strcmp(dsd_audio_get_error(), pa_strerror(PA_ERR_IO)) == 0);
    g_pa_record_peek_fail = 0;
    g_pa_capture_on = 1;
    g_pa_capture_stopped = 1;
    g_pa_simple_error = PA_ERR_KILLED;
    assert(dsd_audio_read(in, samples, 4) == -1);
    assert(g_pa_waits == 1 && strcmp(dsd_audio_get_error(), pa_strerror(PA_ERR_KILLED)) == 0);
    assert(g_pa_lock_depth == 0);
    dsd_audio_close(in);
    assert(g_pa_record_frees == 1);
    params.device = NULL;

    /* A hole in the stream is skipped. */
    reset_pa_simple_fakes();
    in = dsd_audio_open_input(&params);
    assert(in != NULL);
    g_pa_capture_on = 1;
    g_pa_capture_held = 10U;
    g_pa_capture_next = 40;
    g_pa_hole_bytes = 6U;
    assert(dsd_audio_read(in, samples, 4) == 4 && samples[0] == 40 && samples[3] == 43);
    dsd_audio_close(in);

    reset_pa_simple_fakes();
    DSD_MEMSET(&stream, 0, sizeof(stream));
    stream.handle = (pa_simple*)0x1;
    stream.is_input = 0;
    stream.use_async = 0;
    stream.channels = 1;
    g_pa_simple_write_fail = 1;
    g_pa_simple_error = PA_ERR_INTERNAL;
    assert(dsd_audio_write(&stream, samples, 4) == -1);
    assert(g_pa_simple_write_calls == 1);
    assert(strcmp(dsd_audio_get_error(), pa_strerror(PA_ERR_INTERNAL)) == 0);
    /* The failed device is closed and owed a reopen (issue #659). */
    assert(stream.handle == NULL);
    assert(stream.recovery.down == 1);

    reset_pa_simple_fakes();
    DSD_MEMSET(&stream, 0, sizeof(stream));
    stream.handle = (pa_simple*)0x1;
    stream.is_input = 0;
    stream.use_async = 0;
    g_pa_simple_drain_fail = 1;
    g_pa_simple_error = PA_ERR_TIMEOUT;
    assert(dsd_audio_drain(&stream) == -1);
    assert(g_pa_simple_drain_calls == 1);
    assert(strcmp(dsd_audio_get_error(), pa_strerror(PA_ERR_TIMEOUT)) == 0);
}

static void
test_pulse_async_error_paths(void) {
    dsd_audio_stream stream;
    int16_t chunk[4] = {0};
    int16_t ring[4] = {10, 11, 12, 13};

    reset_pa_simple_fakes();
    DSD_MEMSET(&stream, 0, sizeof(stream));
    stream.handle = (pa_simple*)0x1;
    stream.is_input = 0;
    stream.use_async = 1;
    stream.channels = 1;
    stream.chunk = chunk;
    stream.chunk_samples = 4;
    assert(dsd_mutex_init(&stream.mu) == 0);
    assert(dsd_cond_init(&stream.cv) == 0);
    g_pa_simple_write_fail = 1;
    g_pa_simple_error = PA_ERR_CONNECTIONTERMINATED;
    assert(pulse_output_write_chunk(&stream) == -1);
    assert(g_pa_simple_write_calls == 1);
    /* A failed device write no longer stops the stream for good (issue #659): the device is closed and owed a reopen. */
    assert(stream.stop == 0);
    assert(stream.handle == NULL);
    assert(stream.recovery.down == 1);
    assert(stream.recovery.failures == 1U);
    assert(strcmp(stream.recovery.error, pa_strerror(PA_ERR_CONNECTIONTERMINATED)) == 0);
    assert(strcmp(dsd_audio_get_error(), pa_strerror(PA_ERR_CONNECTIONTERMINATED)) == 0);

    reset_pa_simple_fakes();
    DSD_MEMSET(&stream, 0, sizeof(stream));
    stream.handle = (pa_simple*)0x1;
    stream.is_input = 0;
    stream.use_async = 1;
    stream.channels = 1;
    stream.drain_requested = 1;
    g_pa_simple_drain_fail = 1;
    g_pa_simple_error = PA_ERR_TIMEOUT;
    assert(pulse_output_handle_drain_locked(&stream) == 0);
    assert(g_pa_simple_drain_calls == 1);
    assert(stream.drain_requested == 0);
    assert(stream.drain_completed == 1);
    assert(stream.drain_failed == 1);
    assert(strcmp(dsd_audio_get_error(), pa_strerror(PA_ERR_TIMEOUT)) == 0);

    reset_pa_simple_fakes();
    DSD_MEMSET(&stream, 0, sizeof(stream));
    stream.handle = (pa_simple*)0x1;
    stream.is_input = 0;
    stream.use_async = 1;
    stream.channels = 1;
    stream.ring = ring;
    stream.ring_samples_capacity = 4;
    stream.ring_samples_count = 2;
    stream.chunk = chunk;
    stream.chunk_samples = 2;
    stream.drain_requested = 1;
    g_pa_simple_write_fail = 1;
    g_pa_simple_error = PA_ERR_IO;
    assert(pulse_output_handle_drain_locked(&stream) == 0);
    assert(g_pa_simple_write_calls == 1);
    /* The drain ends, failed, and the device is down; the stream itself goes on. */
    assert(stream.stop == 0);
    assert(stream.drain_requested == 0);
    assert(stream.drain_completed == 1);
    assert(stream.drain_failed == 1);
    assert(stream.recovery.down == 1);
    assert(stream.ring_samples_count == 0U);
    assert(strcmp(dsd_audio_get_error(), pa_strerror(PA_ERR_IO)) == 0);

    assert(dsd_cond_destroy(&stream.cv) == 0);
    assert(dsd_mutex_destroy(&stream.mu) == 0);
}

/* Issue #659: a device whose write fails is reopened behind the same stream, from the pump, with a growing wait while
   it cannot be; writes fail meanwhile, and a failure counts as recovered only once the reopened device takes a write. */
static void
test_pulse_async_device_recovery(void) {
    dsd_audio_params params = pulse_test_valid_params();
    params.async_output = 0;
    params.device = "alsa_output.test-sink";
    params.app_name = "dsd-neo-test";
    int16_t samples[4] = {1, 2, 3, 4};

    reset_pa_simple_fakes();
    /* Opened blocking, then given the async ring with no pump thread: the test runs the pump's passes itself. */
    dsd_audio_stream* stream = dsd_audio_open_output(&params);
    assert(stream != NULL);
    assert(g_pa_simple_new_calls == 1);
    stream->use_async = 1;
    assert(pulse_output_init_async_sync(stream) == 1);
    assert(pulse_output_prepare_async_buffers(stream) == 1);

    dsd_audio_output_health health;
    assert(dsd_audio_output_health_get(stream, &health) == 0);
    assert(health.failures == 0U && health.recoveries == 0U && health.down == 0);

    /* The pump's write fails: the device closes and the queue is dropped. */
    assert(dsd_audio_write(stream, samples, 4) == 4);
    g_pa_simple_write_fail = 1;
    g_pa_simple_error = PA_ERR_CONNECTIONTERMINATED;
    assert(pulse_output_pump_step(stream) == 0);
    assert(stream->handle == NULL);
    assert(stream->ring_samples_count == 0U);
    assert(dsd_audio_output_health_get(stream, &health) == 0);
    assert(health.failures == 1U && health.recoveries == 0U && health.down == 1);
    assert(strcmp(health.error, pa_strerror(PA_ERR_CONNECTIONTERMINATED)) == 0);

    /* Writes fail while it is down, and queue nothing; a drain fails at once rather than wait on a pump that may be
       busy reopening. */
    assert(dsd_audio_write(stream, samples, 4) == -1);
    assert(stream->ring_samples_count == 0U);
    assert(dsd_audio_drain(stream) == -1);
    assert(stream->drain_requested == 0);

    /* The first reopen is tried at once; the server is still away, so the next waits 250 ms. */
    g_pa_simple_new_fail = 1;
    g_pa_simple_error = PA_ERR_CONNECTIONREFUSED;
    const uint64_t t0 = g_now_ns;
    assert(pulse_output_pump_step(stream) == 0);
    assert(g_pa_simple_new_calls == 2);
    assert(g_now_ns == t0);
    assert(stream->recovery.retry_at_ns == t0 + 250000000ULL);
    assert(pulse_output_pump_step(stream) == 0);
    assert(g_pa_simple_new_calls == 3);
    assert(g_now_ns >= t0 + 250000000ULL);
    /* ...then 500 ms. */
    const uint64_t t1 = g_now_ns;
    assert(stream->recovery.retry_at_ns == t1 + 500000000ULL);
    assert(dsd_audio_output_health_get(stream, &health) == 0);
    assert(health.failures == 1U && health.down == 1);
    assert(strcmp(health.error, pa_strerror(PA_ERR_CONNECTIONREFUSED)) == 0);

    /* The server is back: the reopen uses the stream's own device and name, and writes queue again. */
    g_pa_simple_new_fail = 0;
    g_pa_simple_write_fail = 0;
    assert(pulse_output_pump_step(stream) == 0);
    assert(g_pa_simple_new_calls == 4);
    assert(strcmp(g_pa_simple_new_dev, "alsa_output.test-sink") == 0);
    assert(strcmp(g_pa_simple_new_app, "dsd-neo-test") == 0);
    assert(stream->handle != NULL);
    assert(dsd_audio_output_health_get(stream, &health) == 0);
    assert(health.down == 0);
    /* Reopened is not recovered: only a write the device takes is. */
    assert(health.recoveries == 0U);
    assert(dsd_audio_write(stream, samples, 4) == 4);
    assert(pulse_output_pump_step(stream) == 0);
    assert(dsd_audio_output_health_get(stream, &health) == 0);
    assert(health.failures == 1U && health.recoveries == 1U && health.down == 0);

    /* A device that fails again before it has stayed healthy is not retried at once. */
    g_pa_simple_write_fail = 1;
    assert(dsd_audio_write(stream, samples, 4) == 4);
    assert(pulse_output_pump_step(stream) == 0);
    assert(stream->recovery.down == 1);
    assert(stream->recovery.retry_at_ns > g_now_ns);
    assert(dsd_audio_output_health_get(stream, &health) == 0);
    assert(health.failures == 2U && health.recoveries == 1U);

    /* A drain while the device is down ends at once, failed. */
    stream->drain_requested = 1;
    dsd_mutex_lock(&stream->mu);
    assert(pulse_output_wait_for_reopen_locked(stream) == 0);
    dsd_mutex_unlock(&stream->mu);
    assert(stream->drain_completed == 1 && stream->drain_failed == 1 && stream->drain_requested == 0);

    /* Stop ends the wait for a reopen. */
    stream->recovery.retry_at_ns = g_now_ns + 1000000000ULL;
    stream->stop = 1;
    assert(pulse_output_pump_step(stream) == -1);
    stream->stop = 0;
    dsd_audio_close(stream);
}

/* Issue #659: blocking output reopens the device on the writer's thread, on the next write once that is due. */
static void
test_pulse_sync_device_recovery(void) {
    dsd_audio_params params = pulse_test_valid_params();
    params.async_output = 0;
    int16_t samples[4] = {1, 2, 3, 4};

    reset_pa_simple_fakes();
    dsd_audio_stream* stream = dsd_audio_open_output(&params);
    assert(stream != NULL);
    assert(stream->use_async == 0);

    g_pa_simple_write_fail = 1;
    g_pa_simple_error = PA_ERR_CONNECTIONTERMINATED;
    assert(dsd_audio_write(stream, samples, 4) == -1);
    assert(stream->handle == NULL);
    dsd_audio_output_health health;
    assert(dsd_audio_output_health_get(stream, &health) == 0);
    assert(health.failures == 1U && health.down == 1);
    assert(dsd_audio_drain(stream) == -1);

    /* The first reopen comes with the next write. */
    g_pa_simple_write_fail = 0;
    assert(dsd_audio_write(stream, samples, 4) == 4);
    assert(g_pa_simple_new_calls == 2);
    assert(dsd_audio_output_health_get(stream, &health) == 0);
    assert(health.failures == 1U && health.recoveries == 1U && health.down == 0);

    /* Failing again at once: the reopen waits 250 ms, and writes fail without trying it until then. */
    g_pa_simple_write_fail = 1;
    assert(dsd_audio_write(stream, samples, 4) == -1);
    g_pa_simple_write_fail = 0;
    assert(dsd_audio_write(stream, samples, 4) == -1);
    assert(g_pa_simple_new_calls == 2);
    g_now_ns += 250000000ULL;
    assert(dsd_audio_write(stream, samples, 4) == 4);
    assert(g_pa_simple_new_calls == 3);

    /* After 2 s of writes the device counts as healthy: the next failure is retried at once again. */
    g_now_ns += 2000000000ULL;
    assert(dsd_audio_write(stream, samples, 4) == 4);
    assert(stream->recovery.streak == 0U);
    g_pa_simple_write_fail = 1;
    assert(dsd_audio_write(stream, samples, 4) == -1);
    g_pa_simple_write_fail = 0;
    assert(dsd_audio_write(stream, samples, 4) == 4);
    assert(g_pa_simple_new_calls == 4);
    assert(dsd_audio_output_health_get(stream, &health) == 0);
    assert(health.failures == 3U && health.recoveries == 3U);

    /* An open that blocks longer than the wait (a server that does not answer) is timed from when it gave up: the
       next write does not try again at once. */
    g_pa_simple_write_fail = 1;
    assert(dsd_audio_write(stream, samples, 4) == -1);
    assert(stream->handle == NULL);
    g_pa_simple_write_fail = 0;
    g_now_ns += 5000000000ULL; /* past any wait */
    g_pa_simple_new_fail = 1;
    g_pa_simple_new_delay_ns = 5000000000ULL;
    const int opens_before = g_pa_simple_new_calls;
    assert(dsd_audio_write(stream, samples, 4) == -1);
    assert(g_pa_simple_new_calls == opens_before + 1);
    assert(stream->recovery.retry_at_ns > g_now_ns);
    assert(dsd_audio_write(stream, samples, 4) == -1);
    assert(g_pa_simple_new_calls == opens_before + 1);
    g_pa_simple_new_fail = 0;
    g_pa_simple_new_delay_ns = 0;

    assert(dsd_audio_output_health_get(NULL, &health) == -1);
    dsd_audio_close(stream);
}

static void
test_pulse_enum_callbacks_and_guards(void) {
    dsd_audio_device devices[2];
    enum_context ctx = {devices, 1, 0};
    pa_sink_info sink;
    pa_source_info source;
    pa_operation* op = (pa_operation*)0x1;

    DSD_MEMSET(devices, 0x5A, sizeof(devices));
    pulse_enum_reset_devices(devices, 2);
    assert(devices[0].initialized == 0);
    assert(devices[1].initialized == 0);
    pulse_enum_reset_devices(NULL, 2);

    DSD_MEMSET(&sink, 0, sizeof(sink));
    sink.index = 7;
    sink.name = "sink-name";
    sink.description = NULL;
    pa_sink_enum_cb(NULL, &sink, 0, &ctx);
    assert(ctx.current == 1);
    assert(devices[0].index == 7);
    assert(strcmp(devices[0].name, "sink-name") == 0);
    assert(devices[0].description[0] == '\0');
    assert(devices[0].is_output == 1);
    assert(devices[0].is_input == 0);
    assert(devices[0].initialized == 1);
    pa_sink_enum_cb(NULL, &sink, 0, &ctx);
    assert(ctx.current == 1);
    pa_sink_enum_cb(NULL, &sink, 1, &ctx);
    assert(ctx.current == 1);

    DSD_MEMSET(devices, 0, sizeof(devices));
    ctx.current = 0;
    DSD_MEMSET(&source, 0, sizeof(source));
    source.index = 9;
    source.name = "source-name";
    source.description = "source description";
    pa_source_enum_cb(NULL, &source, 0, &ctx);
    assert(ctx.current == 1);
    assert(devices[0].index == 9);
    assert(strcmp(devices[0].name, "source-name") == 0);
    assert(strcmp(devices[0].description, "source description") == 0);
    assert(devices[0].is_input == 1);
    assert(devices[0].is_output == 0);
    assert(devices[0].initialized == 1);

    assert(pulse_enum_op_done(NULL) == 1);
    op = (pa_operation*)0x1;
    enum_context no_outputs = {NULL, 1, 0};
    enum_context no_inputs = {NULL, 1, 0};
    assert(pulse_enum_start_query(NULL, &op, 0, &no_outputs, &ctx) == 0);
    assert(op == NULL);
    op = (pa_operation*)0x1;
    assert(pulse_enum_start_query(NULL, &op, 1, &ctx, &no_inputs) == 0);
    assert(op == NULL);
    assert(pulse_enum_start_query(NULL, &op, 2, &ctx, &ctx) == -1);
    assert(op == NULL);
    assert(strcmp(dsd_audio_get_error(), "Internal error: unexpected enumeration state") == 0);
}

/* A mono 48 kHz input on the fake record stream, its capture on. */
static dsd_audio_stream*
open_capture_input(void) {
    reset_pa_simple_fakes();
    dsd_audio_params params;
    DSD_MEMSET(&params, 0, sizeof(params));
    params.sample_rate = 48000;
    params.channels = 1;
    params.bits_per_sample = 16;
    params.app_name = "dsd-neo-test";
    dsd_audio_stream* in = dsd_audio_open_input(&params);
    assert(in != NULL);
    g_pa_capture_on = 1;
    return in;
}

/* Issue #834: an input discard reads all the client holds, never waiting for more, and keeps the newest of it for the
   reads that follow: what was captured since the mark, the time before the discard included (a mark 1 ms before it:
   48 frames at 48 kHz). What a discard kept and no read took is the oldest of what the next one drops. */
static void
test_pulse_input_discard_keeps_the_newest(void) {
    dsd_audio_stream* in = open_capture_input();
    /* Held: frames 0..299, in fragments of 100. */
    g_pa_capture_held = 300U;
    assert(dsd_audio_discard_input(in, g_now_ns - 1000000U) == 252 && g_pa_waits == 0);
    int16_t frames[50] = {0};
    assert(dsd_audio_read(in, frames, 1) == 1 && frames[0] == 252);
    /* The carry (253..299) goes first; the read then waits for the source, which brings 300.. */
    assert(dsd_audio_read(in, frames, 50) == 50 && g_pa_waits == 1);
    for (int i = 0; i < 50; i++) {
        assert(frames[i] == (int16_t)(253 + i));
    }
    /* Held now: 303..395. A discard of a mark now keeps nothing and takes it all. */
    assert(dsd_audio_discard_input(in, g_now_ns) == 93);
    assert(dsd_audio_read(in, frames, 1) == 1 && frames[0] == 396 && g_pa_waits == 2);
    /* Held: 397..491. A mark 1 ms ago keeps 48: 47 go, and the next frame read is 444. */
    assert(dsd_audio_discard_input(in, g_now_ns - 1000000U) == 47);
    assert(dsd_audio_read(in, frames, 1) == 1 && frames[0] == 444);
    /* The carry (445..491) is the oldest: with 200 more held (492..691), a mark 1 ms ago drops 199 of the 247. */
    g_pa_capture_held += 200U;
    assert(dsd_audio_discard_input(in, g_now_ns - 1000000U) == 199);
    assert(dsd_audio_read(in, frames, 1) == 1 && frames[0] == 644);
    assert(g_pa_lock_depth == 0);
    dsd_audio_close(in);
    reset_pa_simple_fakes();
}

/* Issue #834: an input that holds nothing, its drop run at once (no time since the mark): the discard drops nothing
   and waits for nothing, and the next read waits for the source as every read does. */
static void
test_pulse_input_discard_of_an_empty_input(void) {
    dsd_audio_stream* in = open_capture_input();
    g_pa_capture_next = 101;
    assert(dsd_audio_discard_input(in, g_now_ns) == 0 && g_pa_waits == 0);
    int16_t frame = 0;
    assert(dsd_audio_read(in, &frame, 1) == 1 && frame == 101 && g_pa_waits == 1);
    dsd_audio_close(in);
    reset_pa_simple_fakes();
}

/* Issue #834: the decoder was descheduled after the retune's mark, before the discard began (its first clock read is
   late by 100 ms): frames 0..239 were held at the mark and 240..5039 (100 ms at 48 kHz) captured meanwhile. Exactly
   the old frames go: the next frame read is 240. */
static void
test_pulse_input_discard_keeps_what_came_before_it_began(void) {
    dsd_audio_stream* in = open_capture_input();
    g_pa_capture_held = 240U;
    g_late_clock_ns = 100000000ULL;
    g_late_clock_frames = 4800U;
    const int dropped = dsd_audio_discard_input(in, g_now_ns);
    int16_t frame = -1;
    assert(dsd_audio_read(in, &frame, 1) == 1);
    if (dropped != 240 || frame != 240) {
        DSD_FPRINTF(stderr, "a late discard dropped %d, next %d (expected 240 and 240)\n", dropped, frame);
    }
    assert(dropped == 240 && frame == 240 && g_pa_waits == 0);
    dsd_audio_close(in);
    reset_pa_simple_fakes();
}

/* Issue #834: memory runs out during a discard. Once it read the client (the late discard above: 240 old frames, 4800
   captured since the mark), the newest frames are kept with no allocation after the read: the next frame read is
   240. With no memory for the ring that keeps them, the discard reads nothing, and the next frame read is the oldest
   held. */
static void
test_pulse_input_discard_without_memory_loses_no_new_audio(void) {
    dsd_audio_stream* in = open_capture_input();
    g_pa_capture_held = 240U;
    g_late_clock_ns = 100000000ULL;
    g_late_clock_frames = 4800U;
    g_pa_peek_runs_out_of_memory = 1;
    const int dropped = dsd_audio_discard_input(in, g_now_ns);
    g_pa_peek_runs_out_of_memory = 0;
    g_backend_alloc_fails = 0;
    int16_t frame = -1;
    assert(dsd_audio_read(in, &frame, 1) == 1);
    if (dropped != 240 || frame != 240) {
        DSD_FPRINTF(stderr, "a discard out of memory dropped %d, next %d (expected 240 and 240)\n", dropped, frame);
    }
    assert(dropped == 240 && frame == 240);
    dsd_audio_close(in);
    reset_pa_simple_fakes();

    in = open_capture_input();
    g_pa_capture_held = 300U;
    g_backend_alloc_fails = 1;
    const int none = dsd_audio_discard_input(in, g_now_ns);
    g_backend_alloc_fails = 0;
    frame = -1;
    assert(dsd_audio_read(in, &frame, 1) == 1);
    if (none != -1 || frame != 0) {
        DSD_FPRINTF(stderr, "a discard with no ring returned %d, next %d (expected -1 and 0)\n", none, frame);
    }
    assert(none == -1 && frame == 0);
    dsd_audio_close(in);
    reset_pa_simple_fakes();
}

/* Issue #834: the server's record queue overruns during the discard. The decoder was preempted for 100 ms before the
   discard's third peek, and the server threw its oldest frames away meanwhile: frames 0..299 were held at the mark,
   the first two fragments took 0..199, and what the client holds from there on is 4800 frames numbered from 20000. The
   discard keeps the frames captured since the mark, counted by time: 100 ms, all the overrun left, so only the old
   200 go, and every frame the overrun left is read, in order. */
static void
test_pulse_input_discard_keeps_what_an_overrun_left(void) {
    dsd_audio_stream* in = open_capture_input();
    g_pa_capture_held = 300U;
    g_pa_overrun_at_peek = 3;
    g_pa_overrun_ns = 100000000ULL;
    g_pa_overrun_next = 20000;
    g_pa_overrun_held = 4800U;
    const int dropped = dsd_audio_discard_input(in, g_now_ns);
    int16_t frames[100] = {0};
    assert(dsd_audio_read(in, frames, 100) == 100);
    if (dropped != 200 || frames[0] != 20000) {
        DSD_FPRINTF(stderr, "an overrun's discard dropped %d, then read %d (expected 200 and 20000)\n", dropped,
                    frames[0]);
    }
    assert(dropped == 200 && g_pa_waits == 0);
    for (int i = 0; i < 100; i++) {
        assert(frames[i] == (int16_t)(20000 + i));
    }
    dsd_audio_close(in);
    reset_pa_simple_fakes();
}

/* Issue #834: the source stops delivering during a retune (suspended): the discard reads only what is held, never
   waits for more, and returns. 1000 frames held at a mark 5 ms ago: 760 go, the newest 240 are kept and read first. */
static void
test_pulse_input_discard_never_waits_on_a_stopped_source(void) {
    dsd_audio_stream* in = open_capture_input();
    g_pa_capture_held = 1000U;
    g_pa_capture_stopped = 1;
    const int dropped = dsd_audio_discard_input(in, g_now_ns - 5000000ULL);
    if (g_pa_unbounded_waits != 0 || dropped != 760) {
        DSD_FPRINTF(stderr,
                    "a stopped source's discard made %d reads that wait without end, dropped %d (expected 0, 760)\n",
                    g_pa_unbounded_waits, dropped);
    }
    assert(g_pa_unbounded_waits == 0 && g_pa_waits == 0 && dropped == 760);
    int16_t frame = -1;
    assert(dsd_audio_read(in, &frame, 1) == 1 && frame == 760);
    dsd_audio_close(in);
    reset_pa_simple_fakes();
}

int
main(void) {
    test_pulse_size_helpers();
    test_pulse_ring_wrap_read_and_drop();
    test_pulse_output_attr_and_async_buffers();
    test_pulse_async_write_policy();
    test_pulse_sync_output_open_uses_blocking_write();
    test_pulse_simple_error_paths();
    test_pulse_async_error_paths();
    test_pulse_async_device_recovery();
    test_pulse_sync_device_recovery();
    test_pulse_enum_callbacks_and_guards();
    test_pulse_input_discard_of_an_empty_input();
    test_pulse_input_discard_keeps_the_newest();
    test_pulse_input_discard_keeps_what_came_before_it_began();
    test_pulse_input_discard_keeps_what_an_overrun_left();
    test_pulse_input_discard_without_memory_loses_no_new_audio();
    test_pulse_input_discard_never_waits_on_a_stopped_source();
    return 0;
}

// NOLINTEND(bugprone-suspicious-include)
