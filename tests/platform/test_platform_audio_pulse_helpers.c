// SPDX-License-Identifier: GPL-3.0-or-later
// Coverage fixtures intentionally use private-source inclusion, synthetic sentinels,
// invalid-value negative vectors, or wrapper symbols to exercise guarded behavior.
// NOLINTBEGIN(bugprone-suspicious-include)
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <assert.h>
#include <pulse/channelmap.h>
#include <pulse/def.h>
#include <pulse/error.h>
#include <pulse/introspect.h>
#include <pulse/operation.h>
#include <pulse/sample.h>
#include <pulse/simple.h>
#include <stdint.h>
#include <string.h>

#include "../../src/platform/audio_pulse.c"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/platform/audio.h"
#include "dsd-neo/platform/audio_concealment.h"
#include "dsd-neo/platform/threading.h"

static int g_pa_simple_new_fail;
static int g_pa_simple_error;
static int g_pa_simple_read_fail;
static int g_pa_simple_read_calls;
static int g_pa_simple_write_fail;
static int g_pa_simple_write_calls;
static int g_pa_simple_drain_fail;
static int g_pa_simple_drain_calls;

static void
reset_pa_simple_fakes(void) {
    g_pa_simple_new_fail = 0;
    g_pa_simple_error = 0;
    g_pa_simple_read_fail = 0;
    g_pa_simple_read_calls = 0;
    g_pa_simple_write_fail = 0;
    g_pa_simple_write_calls = 0;
    g_pa_simple_drain_fail = 0;
    g_pa_simple_drain_calls = 0;
}

int
dsd_mutex_init(dsd_mutex_t* mutex) {
    (void)mutex;
    return 0;
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

int
dsd_cond_timedwait(dsd_cond_t* cond, dsd_mutex_t* mutex, unsigned int timeout_ms) {
    (void)cond;
    (void)mutex;
    (void)timeout_ms;
    return 0;
}

int
dsd_thread_join(dsd_thread_t thread) {
    (void)thread;
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
    (void)name;
    (void)dir;
    (void)dev;
    (void)stream_name;
    (void)ss;
    (void)map;
    (void)attr;
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

int
pa_simple_read(pa_simple* s, void* data, size_t bytes, int* error) {
    (void)s;
    g_pa_simple_read_calls++;
    if (g_pa_simple_read_fail) {
        if (error != NULL) {
            *error = g_pa_simple_error;
        }
        return -1;
    }
    if (data != NULL) {
        DSD_MEMSET(data, 0, bytes);
    }
    if (error != NULL) {
        *error = 0;
    }
    return 0;
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
    pulse_output_init_async_state(&stream);
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

    reset_pa_simple_fakes();
    DSD_MEMSET(&stream, 0, sizeof(stream));
    stream.handle = (pa_simple*)0x1;
    stream.is_input = 1;
    stream.channels = 1;
    g_pa_simple_read_fail = 1;
    g_pa_simple_error = PA_ERR_IO;
    assert(dsd_audio_read(&stream, samples, 4) == -1);
    assert(g_pa_simple_read_calls == 1);
    assert(strcmp(dsd_audio_get_error(), pa_strerror(PA_ERR_IO)) == 0);

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
    assert(stream.stop == 1);
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
    assert(pulse_output_handle_drain_locked(&stream) == -1);
    assert(g_pa_simple_write_calls == 1);
    assert(stream.stop == 1);
    assert(strcmp(dsd_audio_get_error(), pa_strerror(PA_ERR_IO)) == 0);

    assert(dsd_cond_destroy(&stream.cv) == 0);
    assert(dsd_mutex_destroy(&stream.mu) == 0);
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

int
main(void) {
    test_pulse_size_helpers();
    test_pulse_ring_wrap_read_and_drop();
    test_pulse_output_attr_and_async_buffers();
    test_pulse_async_write_policy();
    test_pulse_simple_error_paths();
    test_pulse_async_error_paths();
    test_pulse_enum_callbacks_and_guards();
    return 0;
}

// NOLINTEND(bugprone-suspicious-include)
