// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief PulseAudio backend implementation for the audio abstraction layer.
 *
 * This file provides the PulseAudio implementation of the cross-platform
 * audio API defined in dsd-neo/platform/audio.h. It wraps the pa_simple API
 * for stream I/O and uses the mainloop API for device enumeration.
 */

#include <dsd-neo/core/string_utils.h>
#include <dsd-neo/platform/audio.h>
#include <dsd-neo/platform/audio_concealment.h>
#include <dsd-neo/platform/platform.h>
#include <dsd-neo/platform/threading.h>
#include <pulse/context.h>
#include <pulse/def.h>
#include <pulse/error.h>
#include <pulse/introspect.h>
#include <pulse/mainloop-api.h>
#include <pulse/mainloop.h>
#include <pulse/operation.h>
#include <pulse/sample.h>
#include <pulse/simple.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "dsd-neo/core/safe_api.h"

#if DSD_PLATFORM_POSIX

/*============================================================================
 * Internal Types
 *============================================================================*/

struct dsd_audio_stream {
    pa_simple* handle;
    int is_input;
    int channels;
    int sample_rate;

    /* Async output pump (playback streams only) */
    int use_async;
    int thread_started;
    dsd_thread_t thread;
    dsd_mutex_t mu;
    dsd_cond_t cv;
    int stop;
    int drain_requested;
    int drain_completed;
    int drain_failed;

    int16_t* ring;
    size_t ring_samples_capacity;
    size_t ring_samples_head;
    size_t ring_samples_tail;
    size_t ring_samples_count;

    int16_t* chunk;
    size_t chunk_frames;
    size_t chunk_samples;
    struct audio_conceal_state conceal;
    int conceal_inited;
    int conceal_has_good;

    uint64_t underruns;
    uint64_t drops;
};

/*============================================================================
 * Module State
 *============================================================================*/

static int s_initialized = 0;
static char s_last_error[512] = "";

/*============================================================================
 * Internal Helpers
 *============================================================================*/

static void
set_error(const char* msg) {
    if (msg) {
        DSD_STRNCPY(s_last_error, msg, sizeof(s_last_error) - 1);
        s_last_error[sizeof(s_last_error) - 1] = '\0';
    } else {
        s_last_error[0] = '\0';
    }
}

static void
set_error_pa(int pa_errno) {
    set_error(pa_strerror(pa_errno));
}

static int
validate_stream_params(const dsd_audio_params* params) {
    if (!params) {
        set_error("NULL parameters");
        return 0;
    }
    if (params->sample_rate <= 0) {
        set_error("Invalid sample rate");
        return 0;
    }
    if (params->channels <= 0 || params->channels > UINT8_MAX) {
        set_error("Invalid channel count");
        return 0;
    }
    return 1;
}

static int
size_mul_nonzero(size_t a, size_t b, size_t* result) {
    if (!result) {
        return 0;
    }
    *result = 0;
    if (a == 0 || b == 0 || a > SIZE_MAX / b) {
        return 0;
    }
    *result = a * b;
    return 1;
}

/*============================================================================
 * Async Output Pump Helpers
 *============================================================================*/

#define DSD_PULSE_OUTPUT_TLENGTH_MS 125
#define DSD_PULSE_OUTPUT_PREBUF_MS  60
#define DSD_PULSE_OUTPUT_RING_MS    1000
#define DSD_PULSE_OUTPUT_CHUNK_MS   20

static uint32_t
ms_to_bytes(int sample_rate, int channels, int ms) {
    if (sample_rate <= 0 || channels <= 0 || ms <= 0) {
        return 0;
    }
    uint64_t bytes_per_sec = (uint64_t)sample_rate * (uint64_t)channels * (uint64_t)sizeof(int16_t);
    uint64_t bytes = (bytes_per_sec * (uint64_t)ms) / 1000U;
    if (bytes == 0) {
        bytes = (uint64_t)channels * sizeof(int16_t);
    }
    if (bytes > UINT32_MAX) {
        bytes = UINT32_MAX;
    }
    return (uint32_t)bytes;
}

static size_t
ms_to_frames(int sample_rate, int ms) {
    if (sample_rate <= 0 || ms <= 0) {
        return 0;
    }
    uint64_t frames = ((uint64_t)sample_rate * (uint64_t)ms) / 1000U;
    if (frames == 0) {
        frames = 1;
    }
    if (frames > SIZE_MAX) {
        frames = SIZE_MAX;
    }
    return (size_t)frames;
}

static size_t
calc_chunk_frames(int sample_rate) {
    size_t frames = ms_to_frames(sample_rate, DSD_PULSE_OUTPUT_CHUNK_MS);
    if (frames < 1) {
        frames = 1;
    }
    return frames;
}

static void
ring_drop_oldest(dsd_audio_stream* stream, size_t drop_samples) {
    if (!stream || !stream->ring || stream->ring_samples_capacity == 0 || drop_samples == 0) {
        return;
    }
    if (drop_samples > stream->ring_samples_count) {
        drop_samples = stream->ring_samples_count;
    }
    stream->ring_samples_head = (stream->ring_samples_head + drop_samples) % stream->ring_samples_capacity;
    stream->ring_samples_count -= drop_samples;
}

static void
ring_write_samples(dsd_audio_stream* stream, const int16_t* src, size_t samples) {
    if (!stream || !stream->ring || !src || stream->ring_samples_capacity == 0 || samples == 0) {
        return;
    }
    /* assumes sufficient free capacity */
    size_t cap = stream->ring_samples_capacity;
    size_t tail = stream->ring_samples_tail;
    size_t first = cap - tail;
    if (samples <= first) {
        DSD_MEMCPY(&stream->ring[tail], src, samples * sizeof(int16_t));
    } else {
        DSD_MEMCPY(&stream->ring[tail], src, first * sizeof(int16_t));
        DSD_MEMCPY(&stream->ring[0], src + first, (samples - first) * sizeof(int16_t));
    }
    stream->ring_samples_tail = (tail + samples) % cap;
    stream->ring_samples_count += samples;
}

static size_t
ring_read_samples(dsd_audio_stream* stream, int16_t* dst, size_t samples) {
    if (!stream || !stream->ring || !dst || stream->ring_samples_capacity == 0 || samples == 0) {
        return 0;
    }
    if (samples > stream->ring_samples_count) {
        samples = stream->ring_samples_count;
    }
    size_t cap = stream->ring_samples_capacity;
    size_t head = stream->ring_samples_head;
    size_t first = cap - head;
    if (samples <= first) {
        DSD_MEMCPY(dst, &stream->ring[head], samples * sizeof(int16_t));
    } else {
        DSD_MEMCPY(dst, &stream->ring[head], first * sizeof(int16_t));
        DSD_MEMCPY(dst + first, &stream->ring[0], (samples - first) * sizeof(int16_t));
    }
    stream->ring_samples_head = (head + samples) % cap;
    stream->ring_samples_count -= samples;
    return samples;
}

static void
pulse_output_stop_on_error(dsd_audio_stream* stream, int pa_errno) {
    set_error_pa(pa_errno);
    dsd_mutex_lock(&stream->mu);
    stream->stop = 1;
    dsd_cond_broadcast(&stream->cv);
    dsd_mutex_unlock(&stream->mu);
}

static int
pulse_output_wait_for_work_locked(dsd_audio_stream* stream) {
    int synthesize_underrun = 0;

    /* Sleep until we have audio to push or a control event (drain/stop).
     * Once a good chunk exists, timed waits let the pump bridge total
     * starvation with a few attenuated repeats instead of waiting until the
     * Pulse server underruns abruptly. */
    while (!stream->stop && !stream->drain_requested && stream->ring_samples_count == 0) {
        if (stream->conceal_inited && stream->conceal_has_good) {
            int ret = dsd_cond_timedwait(&stream->cv, &stream->mu, DSD_PULSE_OUTPUT_CHUNK_MS);
            if (ret != 0 && !stream->stop && !stream->drain_requested && stream->ring_samples_count == 0) {
                synthesize_underrun = 1;
                break;
            }
        } else {
            (void)dsd_cond_wait(&stream->cv, &stream->mu);
        }
    }

    if (stream->stop) {
        return -1;
    }

    return synthesize_underrun;
}

static int
pulse_output_handle_drain_locked(dsd_audio_stream* stream) {
    int drain_failed = 0;
    int err;

    /* Flush remaining queued audio without padding, then drain Pulse buffers. */
    while (!stream->stop && stream->ring_samples_count > 0) {
        size_t take = stream->ring_samples_count;
        if (take > stream->chunk_samples) {
            take = stream->chunk_samples;
        }
        (void)ring_read_samples(stream, stream->chunk, take);
        dsd_mutex_unlock(&stream->mu);

        err = 0;
        if (pa_simple_write(stream->handle, stream->chunk, take * sizeof(int16_t), &err) < 0) {
            pulse_output_stop_on_error(stream, err);
            return -1;
        }

        dsd_mutex_lock(&stream->mu);
    }

    if (!stream->stop) {
        dsd_mutex_unlock(&stream->mu);
        err = 0;
        if (pa_simple_drain(stream->handle, &err) < 0) {
            set_error_pa(err);
            drain_failed = 1;
        }
        dsd_mutex_lock(&stream->mu);
    }

    stream->drain_requested = 0;
    stream->drain_failed = drain_failed;
    stream->drain_completed = 1;
    dsd_cond_broadcast(&stream->cv);
    dsd_mutex_unlock(&stream->mu);
    return 0;
}

static void
pulse_output_prepare_chunk_locked(dsd_audio_stream* stream, int synthesize_underrun) {
    size_t take = stream->chunk_samples;
    if (synthesize_underrun) {
        take = 0;
    } else if (take > stream->ring_samples_count) {
        take = stream->ring_samples_count;
    }

    if (take > 0) {
        (void)ring_read_samples(stream, stream->chunk, take);
    }

    if (take < stream->chunk_samples) {
        stream->underruns++;
        if (stream->conceal_inited && stream->conceal_has_good) {
            size_t missing_frames = (stream->chunk_samples - take) / (size_t)stream->channels;
            size_t written = audio_conceal_on_underrun(&stream->conceal, stream->chunk + take, missing_frames);
            size_t written_samples = written * (size_t)stream->channels;
            if (written_samples < (stream->chunk_samples - take)) {
                DSD_MEMSET(stream->chunk + take + written_samples, 0,
                           (stream->chunk_samples - take - written_samples) * sizeof(int16_t));
            }
        } else {
            DSD_MEMSET(stream->chunk + take, 0, (stream->chunk_samples - take) * sizeof(int16_t));
        }
        return;
    }

    if (stream->conceal_inited) {
        audio_conceal_on_good_buffer(&stream->conceal, stream->chunk, stream->chunk_frames);
        stream->conceal_has_good = 1;
    }
}

static int
pulse_output_write_chunk(dsd_audio_stream* stream) {
    int err = 0;
    if (pa_simple_write(stream->handle, stream->chunk, stream->chunk_samples * sizeof(int16_t), &err) < 0) {
        pulse_output_stop_on_error(stream, err);
        return -1;
    }
    return 0;
}

static DSD_THREAD_RETURN_TYPE
pulse_output_pump_thread(void* arg) {
    dsd_audio_stream* stream = (dsd_audio_stream*)arg;
    if (!stream || !stream->handle) {
        DSD_THREAD_RETURN;
    }

    while (1) {
        dsd_mutex_lock(&stream->mu);
        int synthesize_underrun = pulse_output_wait_for_work_locked(stream);
        if (synthesize_underrun < 0) {
            dsd_mutex_unlock(&stream->mu);
            break;
        }

        if (stream->drain_requested) {
            if (pulse_output_handle_drain_locked(stream) < 0) {
                DSD_THREAD_RETURN;
            }
            continue;
        }

        pulse_output_prepare_chunk_locked(stream, synthesize_underrun);
        dsd_mutex_unlock(&stream->mu);
        if (pulse_output_write_chunk(stream) < 0) {
            break;
        }
    }

    DSD_THREAD_RETURN;
}

/*============================================================================
 * Device Enumeration Callbacks
 *============================================================================*/

typedef struct {
    dsd_audio_device* devices;
    int max_count;
    int current;
} enum_context;

static void
pa_enum_state_cb(pa_context* c, void* userdata) {
    int* ready = userdata;
    pa_context_state_t state = pa_context_get_state(c);

    switch (state) {
        case PA_CONTEXT_READY: *ready = 1; break;
        case PA_CONTEXT_FAILED:
        case PA_CONTEXT_TERMINATED: *ready = 2; break;
        default: break;
    }
}

static void
pa_sink_enum_cb(pa_context* c, const pa_sink_info* info, int eol, void* userdata) {
    enum_context* ctx = userdata;
    (void)c;

    if (eol > 0 || !info) {
        return;
    }

    if (ctx->current >= ctx->max_count) {
        return;
    }

    dsd_audio_device* dev = &ctx->devices[ctx->current];
    dev->index = (int)info->index;
    DSD_STRNCPY(dev->name, info->name, sizeof(dev->name) - 1);
    dev->name[sizeof(dev->name) - 1] = '\0';
    if (info->description) {
        DSD_STRNCPY(dev->description, info->description, sizeof(dev->description) - 1);
        dev->description[sizeof(dev->description) - 1] = '\0';
    } else {
        dev->description[0] = '\0';
    }
    dev->is_input = 0;
    dev->is_output = 1;
    dev->initialized = 1;
    ctx->current++;
}

static void
pa_source_enum_cb(pa_context* c, const pa_source_info* info, int eol, void* userdata) {
    enum_context* ctx = userdata;
    (void)c;

    if (eol > 0 || !info) {
        return;
    }

    if (ctx->current >= ctx->max_count) {
        return;
    }

    dsd_audio_device* dev = &ctx->devices[ctx->current];
    dev->index = (int)info->index;
    DSD_STRNCPY(dev->name, info->name, sizeof(dev->name) - 1);
    dev->name[sizeof(dev->name) - 1] = '\0';
    if (info->description) {
        DSD_STRNCPY(dev->description, info->description, sizeof(dev->description) - 1);
        dev->description[sizeof(dev->description) - 1] = '\0';
    } else {
        dev->description[0] = '\0';
    }
    dev->is_input = 1;
    dev->is_output = 0;
    dev->initialized = 1;
    ctx->current++;
}

static void
pulse_enum_reset_devices(dsd_audio_device* devices, int max_count) {
    if (devices) {
        DSD_MEMSET(devices, 0, (size_t)max_count * sizeof(dsd_audio_device));
    }
}

static int
pulse_enum_connect(pa_mainloop** ml_out, pa_context** ctx_out, int* ready_out) {
    pa_mainloop* ml = pa_mainloop_new();
    if (!ml) {
        set_error("Failed to create PulseAudio mainloop");
        return -1;
    }

    pa_mainloop_api* mlapi = pa_mainloop_get_api(ml);
    pa_context* ctx = pa_context_new(mlapi, "dsd-neo-enum");
    if (!ctx) {
        set_error("Failed to create PulseAudio context");
        pa_mainloop_free(ml);
        return -1;
    }

    if (pa_context_connect(ctx, NULL, PA_CONTEXT_NOFLAGS, NULL) < 0) {
        set_error_pa(pa_context_errno(ctx));
        pa_context_unref(ctx);
        pa_mainloop_free(ml);
        return -1;
    }

    *ready_out = 0;
    pa_context_set_state_callback(ctx, pa_enum_state_cb, ready_out);
    *ml_out = ml;
    *ctx_out = ctx;
    return 0;
}

static int
pulse_enum_start_query(pa_context* ctx, pa_operation** op, int state, enum_context* out_ctx, enum_context* in_ctx) {
    *op = NULL;
    if (state == 0) {
        if (out_ctx->devices) {
            *op = pa_context_get_sink_info_list(ctx, pa_sink_enum_cb, out_ctx);
        }
        return 0;
    }
    if (state == 1) {
        if (in_ctx->devices) {
            *op = pa_context_get_source_info_list(ctx, pa_source_enum_cb, in_ctx);
        }
        return 0;
    }
    set_error("Internal error: unexpected enumeration state");
    return -1;
}

static int
pulse_enum_op_done(pa_operation* op) {
    if (!op) {
        return 1;
    }
    return pa_operation_get_state(op) == PA_OPERATION_DONE;
}

static int
pulse_enum_run_loop(pa_mainloop* ml, pa_context* ctx, enum_context* out_ctx, enum_context* in_ctx, const int* ready) {
    pa_operation* op = NULL;
    int state = 0;

    for (;;) {
        if (*ready == 0) {
            pa_mainloop_iterate(ml, 1, NULL);
            continue;
        }

        if (*ready == 2) {
            set_error("PulseAudio connection failed");
            break;
        }

        if (state == 0) {
            if (pulse_enum_start_query(ctx, &op, 0, out_ctx, in_ctx) < 0) {
                break;
            }
            state = 1;
        } else if (state == 1) {
            if (!pulse_enum_op_done(op)) {
                pa_mainloop_iterate(ml, 1, NULL);
                continue;
            }
            if (op) {
                pa_operation_unref(op);
            }
            if (pulse_enum_start_query(ctx, &op, 1, out_ctx, in_ctx) < 0) {
                op = NULL;
                break;
            }
            state = 2;
        } else if (state == 2) {
            if (pulse_enum_op_done(op)) {
                if (op) {
                    pa_operation_unref(op);
                }
                return 0;
            }
        } else {
            set_error("Internal error: unexpected enumeration state");
            break;
        }

        pa_mainloop_iterate(ml, 1, NULL);
    }

    if (op) {
        pa_operation_unref(op);
    }
    return -1;
}

/*============================================================================
 * Public API Implementation
 *============================================================================*/

int
dsd_audio_init(void) {
    if (s_initialized) {
        return 0;
    }
    /* PulseAudio doesn't require explicit initialization for pa_simple */
    s_initialized = 1;
    set_error(NULL);
    return 0;
}

void
dsd_audio_cleanup(void) {
    s_initialized = 0;
}

int
dsd_audio_enumerate_devices(dsd_audio_device* inputs, dsd_audio_device* outputs, int max_count) {
    pa_mainloop* ml = NULL;
    pa_context* ctx = NULL;
    int ready = 0;
    int ret = 0;

    enum_context out_ctx = {outputs, max_count, 0};
    enum_context in_ctx = {inputs, max_count, 0};

    pulse_enum_reset_devices(inputs, max_count);
    pulse_enum_reset_devices(outputs, max_count);

    if (pulse_enum_connect(&ml, &ctx, &ready) < 0) {
        return -1;
    }

    ret = pulse_enum_run_loop(ml, ctx, &out_ctx, &in_ctx, &ready);
    pa_context_disconnect(ctx);
    pa_context_unref(ctx);
    pa_mainloop_free(ml);
    return ret;
}

int
dsd_audio_list_devices(void) {
    dsd_audio_device inputs[16];
    dsd_audio_device outputs[16];

    if (dsd_audio_enumerate_devices(inputs, outputs, 16) < 0) {
        DSD_FPRINTF(stderr, "Error: Failed to enumerate audio devices: %s\n", dsd_audio_get_error());
        return -1;
    }

    printf("\n");

    /* Print outputs (sinks) */
    for (int i = 0; i < 16; i++) {
        if (!outputs[i].initialized) {
            break;
        }
        printf("=======[ Output Device #%d ]=======\n", i + 1);
        printf("Description: %s\n", outputs[i].description);
        printf("Name: %s\n", outputs[i].name);
        printf("Index: %d\n", outputs[i].index);
        printf("\n");
    }

    /* Print inputs (sources) */
    for (int i = 0; i < 16; i++) {
        if (!inputs[i].initialized) {
            break;
        }
        printf("=======[ Input Device #%d ]=======\n", i + 1);
        printf("Description: %s\n", inputs[i].description);
        printf("Name: %s\n", inputs[i].name);
        printf("Index: %d\n", inputs[i].index);
        printf("\n");
    }

    return 0;
}

dsd_audio_stream*
dsd_audio_open_input(const dsd_audio_params* params) {
    if (!validate_stream_params(params)) {
        return NULL;
    }

    dsd_audio_stream* stream = calloc(1, sizeof(dsd_audio_stream));
    if (!stream) {
        set_error("Out of memory");
        return NULL;
    }

    pa_sample_spec ss;
    ss.format = PA_SAMPLE_S16NE;
    ss.channels = (uint8_t)params->channels;
    ss.rate = (uint32_t)params->sample_rate;

    /* Buffer attributes for low latency input */
    pa_buffer_attr attr;
    DSD_MEMSET(&attr, 0, sizeof(attr));
    attr.maxlength = (uint32_t)-1;
    attr.tlength = (uint32_t)-1;
    attr.prebuf = (uint32_t)-1;
    attr.minreq = (uint32_t)-1;
    /* fragsize tuned for ~100ms latency at 48kHz stereo */
    attr.fragsize = (uint32_t)(params->sample_rate * params->channels * 2 / 10);

    int err = 0;
    const char* dev = (params->device && params->device[0]) ? params->device : NULL;
    const char* app = (params->app_name && params->app_name[0]) ? params->app_name : "DSD-neo";

    stream->handle = pa_simple_new(NULL, app, PA_STREAM_RECORD, dev, "Audio Input", &ss, NULL, &attr, &err);

    if (!stream->handle) {
        set_error_pa(err);
        free(stream);
        return NULL;
    }

    stream->is_input = 1;
    stream->channels = params->channels;
    stream->sample_rate = params->sample_rate;

    return stream;
}

static void
pulse_output_init_attr(const dsd_audio_params* params, pa_buffer_attr* attr) {
    /* Playback buffering: keep ~125ms in the server to tolerate jitter. */
    DSD_MEMSET(attr, 0, sizeof(*attr));
    attr->maxlength = (uint32_t)-1;
    attr->fragsize = (uint32_t)-1;
    attr->tlength = ms_to_bytes(params->sample_rate, params->channels, DSD_PULSE_OUTPUT_TLENGTH_MS);
    attr->prebuf = ms_to_bytes(params->sample_rate, params->channels, DSD_PULSE_OUTPUT_PREBUF_MS);
    attr->minreq = ms_to_bytes(params->sample_rate, params->channels, DSD_PULSE_OUTPUT_CHUNK_MS);
    if (attr->minreq > attr->tlength) {
        attr->minreq = attr->tlength;
    }
    if (attr->prebuf > attr->tlength) {
        attr->prebuf = attr->tlength;
    }
}

static void
pulse_output_init_async_state(dsd_audio_stream* stream) {
    /* Async output pump: decouple decode thread from Pulse writes. */
    stream->use_async = 1;
    stream->thread_started = 0;
    stream->stop = 0;
    stream->drain_requested = 0;
    stream->drain_completed = 0;
    stream->drain_failed = 0;
    stream->underruns = 0;
    stream->drops = 0;
    stream->conceal_has_good = 0;
}

static int
pulse_output_init_async_sync(dsd_audio_stream* stream) {
    if (dsd_mutex_init(&stream->mu) != 0) {
        return 0;
    }
    if (dsd_cond_init(&stream->cv) != 0) {
        (void)dsd_mutex_destroy(&stream->mu);
        return 0;
    }
    return 1;
}

static int
pulse_output_prepare_async_buffers(dsd_audio_stream* stream) {
    const size_t channel_count = (size_t)stream->channels;
    size_t min_ring_frames = 0;
    size_t ring_frames = 0;

    stream->chunk_frames = calc_chunk_frames(stream->sample_rate);
    if (!size_mul_nonzero(stream->chunk_frames, channel_count, &stream->chunk_samples)) {
        return 0;
    }

    stream->chunk = calloc(stream->chunk_samples, sizeof(int16_t));
    if (audio_conceal_init(&stream->conceal, stream->chunk_frames, stream->channels) == 0) {
        stream->conceal_inited = 1;
    }

    if (!size_mul_nonzero(stream->chunk_frames, 8U, &min_ring_frames)) {
        return 0;
    }
    ring_frames = ms_to_frames(stream->sample_rate, DSD_PULSE_OUTPUT_RING_MS);
    if (ring_frames < min_ring_frames) {
        ring_frames = min_ring_frames;
    }
    if (!size_mul_nonzero(ring_frames, channel_count, &stream->ring_samples_capacity)) {
        return 0;
    }

    stream->ring = calloc(stream->ring_samples_capacity, sizeof(int16_t));
    stream->ring_samples_head = 0;
    stream->ring_samples_tail = 0;
    stream->ring_samples_count = 0;
    return stream->chunk && stream->ring;
}

static int
pulse_output_start_async_thread(dsd_audio_stream* stream) {
    if (dsd_thread_create(&stream->thread, (dsd_thread_fn)pulse_output_pump_thread, stream) != 0) {
        return 0;
    }
    stream->thread_started = 1;
    return 1;
}

static void
pulse_output_disable_async(dsd_audio_stream* stream, int async_sync_inited) {
    stream->use_async = 0;

    if (stream->thread_started) {
        (void)dsd_thread_join(stream->thread);
        stream->thread_started = 0;
    }
    if (stream->chunk) {
        free(stream->chunk);
        stream->chunk = NULL;
    }
    if (stream->conceal_inited) {
        audio_conceal_destroy(&stream->conceal);
        stream->conceal_inited = 0;
    }
    if (stream->ring) {
        free(stream->ring);
        stream->ring = NULL;
    }
    stream->ring_samples_capacity = 0;
    if (async_sync_inited) {
        (void)dsd_cond_destroy(&stream->cv);
        (void)dsd_mutex_destroy(&stream->mu);
    }
}

dsd_audio_stream*
dsd_audio_open_output(const dsd_audio_params* params) {
    if (!validate_stream_params(params)) {
        return NULL;
    }

    dsd_audio_stream* stream = calloc(1, sizeof(dsd_audio_stream));
    if (!stream) {
        set_error("Out of memory");
        return NULL;
    }

    pa_sample_spec ss;
    ss.format = PA_SAMPLE_S16NE;
    ss.channels = (uint8_t)params->channels;
    ss.rate = (uint32_t)params->sample_rate;

    int err = 0;
    const char* dev = (params->device && params->device[0]) ? params->device : NULL;
    const char* app = (params->app_name && params->app_name[0]) ? params->app_name : "DSD-neo";

    pa_buffer_attr attr;
    pulse_output_init_attr(params, &attr);

    stream->handle = pa_simple_new(NULL, app, PA_STREAM_PLAYBACK, dev, "Audio Output", &ss, NULL, &attr, &err);

    if (!stream->handle) {
        set_error_pa(err);
        free(stream);
        return NULL;
    }

    stream->is_input = 0;
    stream->channels = params->channels;
    stream->sample_rate = params->sample_rate;
    pulse_output_init_async_state(stream);

    if (stream->use_async) {
        int async_sync_inited = pulse_output_init_async_sync(stream);
        if (!async_sync_inited || !pulse_output_prepare_async_buffers(stream)
            || !pulse_output_start_async_thread(stream)) {
            pulse_output_disable_async(stream, async_sync_inited);
        }
    }

    return stream;
}

int
dsd_audio_read(dsd_audio_stream* stream, int16_t* buffer, size_t frames) {
    if (!stream || !stream->handle || !buffer) {
        set_error("Invalid arguments");
        return -1;
    }

    if (!stream->is_input) {
        set_error("Cannot read from output stream");
        return -1;
    }

    size_t bytes = frames * (size_t)stream->channels * sizeof(int16_t);
    int err = 0;

    if (pa_simple_read(stream->handle, buffer, bytes, &err) < 0) {
        set_error_pa(err);
        return -1;
    }

    return (int)frames;
}

int
dsd_audio_write(dsd_audio_stream* stream, const int16_t* buffer, size_t frames) {
    if (!stream || !stream->handle || !buffer) {
        set_error("Invalid arguments");
        return -1;
    }

    if (stream->is_input) {
        set_error("Cannot write to input stream");
        return -1;
    }

    if (!stream->use_async) {
        size_t bytes = frames * (size_t)stream->channels * sizeof(int16_t);
        int err = 0;

        if (pa_simple_write(stream->handle, buffer, bytes, &err) < 0) {
            set_error_pa(err);
            return -1;
        }

        return (int)frames;
    }

    size_t samples = frames * (size_t)stream->channels;
    if (samples == 0) {
        return 0;
    }

    dsd_mutex_lock(&stream->mu);

    if (stream->stop) {
        dsd_mutex_unlock(&stream->mu);
        return -1;
    }

    /* During drain, ignore new writes to guarantee completion. */
    if (stream->drain_requested) {
        stream->drops += samples;
        dsd_mutex_unlock(&stream->mu);
        return (int)frames;
    }

    if (!stream->ring || stream->ring_samples_capacity == 0) {
        dsd_mutex_unlock(&stream->mu);
        return -1;
    }

    if (samples >= stream->ring_samples_capacity) {
        /* Keep only the newest window. */
        const int16_t* src = buffer + (samples - stream->ring_samples_capacity);
        stream->drops += stream->ring_samples_count;
        stream->ring_samples_head = 0;
        stream->ring_samples_tail = 0;
        stream->ring_samples_count = 0;
        samples = stream->ring_samples_capacity;
        ring_write_samples(stream, src, samples);
    } else {
        size_t free_samples = stream->ring_samples_capacity - stream->ring_samples_count;
        if (free_samples < samples) {
            size_t drop = samples - free_samples;
            ring_drop_oldest(stream, drop);
            stream->drops += drop;
        }
        ring_write_samples(stream, buffer, samples);
    }

    dsd_cond_signal(&stream->cv);
    dsd_mutex_unlock(&stream->mu);

    return (int)frames;
}

void
dsd_audio_close(dsd_audio_stream* stream) {
    if (!stream) {
        return;
    }

    if (!stream->is_input && stream->use_async) {
        dsd_mutex_lock(&stream->mu);
        stream->stop = 1;
        dsd_cond_broadcast(&stream->cv);
        dsd_mutex_unlock(&stream->mu);

        if (stream->thread_started) {
            (void)dsd_thread_join(stream->thread);
            stream->thread_started = 0;
        }

        const char* stats_env = getenv("DSD_NEO_AUDIO_STATS");
        if (stats_env && stats_env[0] != '\0' && (stream->underruns || stream->drops)) {
            DSD_FPRINTF(stderr, "PulseAudio output stats: rate=%d ch=%d underruns=%llu drops=%llu\n",
                        stream->sample_rate, stream->channels, (unsigned long long)stream->underruns,
                        (unsigned long long)stream->drops);
        }

        (void)dsd_cond_destroy(&stream->cv);
        (void)dsd_mutex_destroy(&stream->mu);

        if (stream->chunk) {
            free(stream->chunk);
            stream->chunk = NULL;
        }
        if (stream->conceal_inited) {
            audio_conceal_destroy(&stream->conceal);
            stream->conceal_inited = 0;
        }
        if (stream->ring) {
            free(stream->ring);
            stream->ring = NULL;
        }
    }

    if (stream->handle) {
        pa_simple_free(stream->handle);
    }

    free(stream);
}

int
dsd_audio_drain(dsd_audio_stream* stream) {
    if (!stream || !stream->handle) {
        return -1;
    }

    if (stream->is_input) {
        /* Drain doesn't apply to input streams */
        return 0;
    }

    if (!stream->use_async) {
        int err = 0;
        if (pa_simple_drain(stream->handle, &err) < 0) {
            set_error_pa(err);
            return -1;
        }
        return 0;
    }

    dsd_mutex_lock(&stream->mu);
    stream->drain_failed = 0;
    stream->drain_completed = 0;
    stream->drain_requested = 1;
    dsd_cond_broadcast(&stream->cv);
    while (!stream->drain_completed && !stream->stop) {
        (void)dsd_cond_wait(&stream->cv, &stream->mu);
    }
    int stopped = stream->stop;
    int drain_failed = stream->drain_failed;
    dsd_mutex_unlock(&stream->mu);

    if (stopped || drain_failed) {
        return -1;
    }

    return 0;
}

const char*
dsd_audio_get_error(void) {
    return s_last_error;
}

const char*
dsd_audio_backend_name(void) {
    return "pulse";
}

#endif /* DSD_PLATFORM_POSIX */
