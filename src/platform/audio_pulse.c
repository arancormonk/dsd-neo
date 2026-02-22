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

#include <dsd-neo/platform/platform.h>
#include <pulse/context.h>
#include <pulse/def.h>
#include <pulse/error.h>
#include <pulse/introspect.h>
#include <pulse/mainloop-api.h>
#include <pulse/mainloop.h>
#include <pulse/operation.h>
#include <pulse/sample.h>
#include <stdint.h>

#if DSD_PLATFORM_POSIX

#include <dsd-neo/platform/audio.h>
#include <dsd-neo/platform/threading.h>
#include <pulse/simple.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
        strncpy(s_last_error, msg, sizeof(s_last_error) - 1);
        s_last_error[sizeof(s_last_error) - 1] = '\0';
    } else {
        s_last_error[0] = '\0';
    }
}

static void
set_error_pa(int pa_errno) {
    set_error(pa_strerror(pa_errno));
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
        memcpy(&stream->ring[tail], src, samples * sizeof(int16_t));
    } else {
        memcpy(&stream->ring[tail], src, first * sizeof(int16_t));
        memcpy(&stream->ring[0], src + first, (samples - first) * sizeof(int16_t));
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
        memcpy(dst, &stream->ring[head], samples * sizeof(int16_t));
    } else {
        memcpy(dst, &stream->ring[head], first * sizeof(int16_t));
        memcpy(dst + first, &stream->ring[0], (samples - first) * sizeof(int16_t));
    }
    stream->ring_samples_head = (head + samples) % cap;
    stream->ring_samples_count -= samples;
    return samples;
}

static DSD_THREAD_RETURN_TYPE
pulse_output_pump_thread(void* arg) {
    dsd_audio_stream* stream = (dsd_audio_stream*)arg;
    if (!stream || !stream->handle) {
        DSD_THREAD_RETURN;
    }

    int err = 0;

    while (1) {
        dsd_mutex_lock(&stream->mu);

        /* Sleep until we have audio to push or a control event (drain/stop). */
        while (!stream->stop && !stream->drain_requested && stream->ring_samples_count == 0) {
            (void)dsd_cond_wait(&stream->cv, &stream->mu);
        }

        if (stream->stop) {
            dsd_mutex_unlock(&stream->mu);
            break;
        }

        if (stream->drain_requested) {
            int drain_failed = 0;
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
                    set_error_pa(err);
                    dsd_mutex_lock(&stream->mu);
                    stream->stop = 1;
                    dsd_cond_broadcast(&stream->cv);
                    dsd_mutex_unlock(&stream->mu);
                    DSD_THREAD_RETURN;
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
            continue;
        }

        /* Normal: write fixed chunks; only pad the tail of a non-empty read. */
        size_t take = stream->chunk_samples;
        if (take > stream->ring_samples_count) {
            take = stream->ring_samples_count;
        }
        if (take > 0) {
            (void)ring_read_samples(stream, stream->chunk, take);
        }
        if (take < stream->chunk_samples) {
            memset(stream->chunk + take, 0, (stream->chunk_samples - take) * sizeof(int16_t));
        }

        dsd_mutex_unlock(&stream->mu);

        err = 0;
        if (pa_simple_write(stream->handle, stream->chunk, stream->chunk_samples * sizeof(int16_t), &err) < 0) {
            set_error_pa(err);
            dsd_mutex_lock(&stream->mu);
            stream->stop = 1;
            dsd_cond_broadcast(&stream->cv);
            dsd_mutex_unlock(&stream->mu);
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
    strncpy(dev->name, info->name, sizeof(dev->name) - 1);
    dev->name[sizeof(dev->name) - 1] = '\0';
    if (info->description) {
        strncpy(dev->description, info->description, sizeof(dev->description) - 1);
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
    strncpy(dev->name, info->name, sizeof(dev->name) - 1);
    dev->name[sizeof(dev->name) - 1] = '\0';
    if (info->description) {
        strncpy(dev->description, info->description, sizeof(dev->description) - 1);
        dev->description[sizeof(dev->description) - 1] = '\0';
    } else {
        dev->description[0] = '\0';
    }
    dev->is_input = 1;
    dev->is_output = 0;
    dev->initialized = 1;
    ctx->current++;
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
    pa_mainloop_api* mlapi = NULL;
    pa_context* ctx = NULL;
    pa_operation* op = NULL;
    int ready = 0;
    int state = 0;
    int ret = -1;

    if (inputs) {
        memset(inputs, 0, (size_t)max_count * sizeof(dsd_audio_device));
    }
    if (outputs) {
        memset(outputs, 0, (size_t)max_count * sizeof(dsd_audio_device));
    }

    ml = pa_mainloop_new();
    if (!ml) {
        set_error("Failed to create PulseAudio mainloop");
        return -1;
    }

    mlapi = pa_mainloop_get_api(ml);
    ctx = pa_context_new(mlapi, "dsd-neo-enum");
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

    pa_context_set_state_callback(ctx, pa_enum_state_cb, &ready);

    enum_context out_ctx = {outputs, max_count, 0};
    enum_context in_ctx = {inputs, max_count, 0};

    /* State machine for async enumeration */
    for (;;) {
        if (ready == 0) {
            pa_mainloop_iterate(ml, 1, NULL);
            continue;
        }

        if (ready == 2) {
            set_error("PulseAudio connection failed");
            goto cleanup;
        }

        switch (state) {
            case 0: /* Enumerate sinks (outputs) */
                if (outputs) {
                    op = pa_context_get_sink_info_list(ctx, pa_sink_enum_cb, &out_ctx);
                }
                state++;
                break;

            case 1: /* Wait for sink enumeration */
                if (!op || pa_operation_get_state(op) == PA_OPERATION_DONE) {
                    if (op) {
                        pa_operation_unref(op);
                        op = NULL;
                    }
                    /* Enumerate sources (inputs) */
                    if (inputs) {
                        op = pa_context_get_source_info_list(ctx, pa_source_enum_cb, &in_ctx);
                    }
                    state++;
                }
                break;

            case 2: /* Wait for source enumeration */
                if (!op || pa_operation_get_state(op) == PA_OPERATION_DONE) {
                    if (op) {
                        pa_operation_unref(op);
                        op = NULL;
                    }
                    ret = 0;
                    goto cleanup;
                }
                break;
            default: set_error("Internal error: unexpected enumeration state"); goto cleanup;
        }

        pa_mainloop_iterate(ml, 1, NULL);
    }

cleanup:
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
        fprintf(stderr, "Error: Failed to enumerate audio devices: %s\n", dsd_audio_get_error());
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
    if (!params) {
        set_error("NULL parameters");
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
    memset(&attr, 0, sizeof(attr));
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

dsd_audio_stream*
dsd_audio_open_output(const dsd_audio_params* params) {
    if (!params) {
        set_error("NULL parameters");
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

    /* Playback buffering: keep ~125ms in the server to tolerate jitter. */
    pa_buffer_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.maxlength = (uint32_t)-1;
    attr.fragsize = (uint32_t)-1;
    attr.tlength = ms_to_bytes(params->sample_rate, params->channels, DSD_PULSE_OUTPUT_TLENGTH_MS);
    attr.prebuf = ms_to_bytes(params->sample_rate, params->channels, DSD_PULSE_OUTPUT_PREBUF_MS);
    attr.minreq = ms_to_bytes(params->sample_rate, params->channels, DSD_PULSE_OUTPUT_CHUNK_MS);
    if (attr.minreq > attr.tlength) {
        attr.minreq = attr.tlength;
    }
    if (attr.prebuf > attr.tlength) {
        attr.prebuf = attr.tlength;
    }

    stream->handle = pa_simple_new(NULL, app, PA_STREAM_PLAYBACK, dev, "Audio Output", &ss, NULL, &attr, &err);

    if (!stream->handle) {
        set_error_pa(err);
        free(stream);
        return NULL;
    }

    stream->is_input = 0;
    stream->channels = params->channels;
    stream->sample_rate = params->sample_rate;

    /* Async output pump: decouple decode thread from Pulse writes. */
    stream->use_async = 1;
    stream->thread_started = 0;
    stream->stop = 0;
    stream->drain_requested = 0;
    stream->drain_completed = 0;
    stream->drain_failed = 0;
    stream->underruns = 0;
    stream->drops = 0;

    int async_sync_inited = 0;
    if (dsd_mutex_init(&stream->mu) == 0) {
        if (dsd_cond_init(&stream->cv) == 0) {
            async_sync_inited = 1;
        } else {
            (void)dsd_mutex_destroy(&stream->mu);
        }
    }
    if (!async_sync_inited) {
        stream->use_async = 0;
    }

    if (stream->use_async) {
        stream->chunk_frames = calc_chunk_frames(stream->sample_rate);
        stream->chunk_samples = stream->chunk_frames * (size_t)stream->channels;
        stream->chunk = calloc(stream->chunk_samples, sizeof(int16_t));

        size_t ring_frames = ms_to_frames(stream->sample_rate, DSD_PULSE_OUTPUT_RING_MS);
        if (ring_frames < stream->chunk_frames * 8U) {
            ring_frames = stream->chunk_frames * 8U;
        }
        stream->ring_samples_capacity = ring_frames * (size_t)stream->channels;
        stream->ring = calloc(stream->ring_samples_capacity, sizeof(int16_t));
        stream->ring_samples_head = 0;
        stream->ring_samples_tail = 0;
        stream->ring_samples_count = 0;

        if (!stream->chunk || !stream->ring || stream->ring_samples_capacity == 0 || stream->chunk_samples == 0) {
            stream->use_async = 0;
        }
    }

    if (stream->use_async) {
        if (dsd_thread_create(&stream->thread, (dsd_thread_fn)pulse_output_pump_thread, stream) != 0) {
            stream->use_async = 0;
        } else {
            stream->thread_started = 1;
        }
    }

    if (!stream->use_async) {
        if (stream->thread_started) {
            (void)dsd_thread_join(stream->thread);
            stream->thread_started = 0;
        }
        if (stream->chunk) {
            free(stream->chunk);
            stream->chunk = NULL;
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
            fprintf(stderr, "PulseAudio output stats: rate=%d ch=%d underruns=%llu drops=%llu\n", stream->sample_rate,
                    stream->channels, (unsigned long long)stream->underruns, (unsigned long long)stream->drops);
        }

        (void)dsd_cond_destroy(&stream->cv);
        (void)dsd_mutex_destroy(&stream->mu);

        if (stream->chunk) {
            free(stream->chunk);
            stream->chunk = NULL;
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
