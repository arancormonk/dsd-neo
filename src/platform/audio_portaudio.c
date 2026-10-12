// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief PortAudio backend implementation for the audio abstraction layer.
 *
 * This file provides the PortAudio implementation of the cross-platform
 * audio API defined in dsd-neo/platform/audio.h. PortAudio supports
 * multiple backends including WASAPI (Windows), ALSA/PulseAudio (Linux),
 * and CoreAudio (macOS).
 */

#include <dsd-neo/platform/platform.h>

#ifdef DSD_USE_PORTAUDIO

#include <dsd-neo/core/string_utils.h>
#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/platform/audio.h>
#include <dsd-neo/platform/audio_concealment.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/platform/timing.h>

#include <portaudio.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define DSD_NEO_AUDIO_BACKEND_PORTAUDIO 1
#include "audio_error_internal.h"
#include "audio_output_recovery.h"
#include "audio_stream_internal.h"

/*============================================================================
 * Internal Types
 *============================================================================*/

/*============================================================================
 * Module State
 *============================================================================*/

static int s_initialized = 0;

/* PortAudio keeps its open streams on an unlocked global list, so no two threads may open, start, stop or close
   streams at once. With output device recovery (issue #659) the pumps do that too, so every such call takes this lock.
   Writes and reads do not: each stream has one writer or reader. 0 = uninitialized, 1 = in flight, 2 = ready. */
static dsd_mutex_t s_lifecycle_mu;
static atomic_int s_lifecycle_mu_state = 0;

static void
portaudio_lifecycle_lock(void) {
    if (atomic_load(&s_lifecycle_mu_state) != 2) {
        int expected = 0;
        if (atomic_compare_exchange_strong(&s_lifecycle_mu_state, &expected, 1)) {
            (void)dsd_mutex_init(&s_lifecycle_mu);
            atomic_store(&s_lifecycle_mu_state, 2);
        } else {
            while (atomic_load(&s_lifecycle_mu_state) != 2) {
                dsd_thread_yield();
            }
        }
    }
    dsd_mutex_lock(&s_lifecycle_mu);
}

static void
portaudio_lifecycle_unlock(void) {
    dsd_mutex_unlock(&s_lifecycle_mu);
}

/*============================================================================
 * Internal Helpers
 *============================================================================*/

static void
set_error_pa(PaError err) {
    set_error(Pa_GetErrorText(err));
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

#define DSD_PORTAUDIO_OUTPUT_RING_MS  1000
#define DSD_PORTAUDIO_OUTPUT_CHUNK_MS 20

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
    size_t frames = ms_to_frames(sample_rate, DSD_PORTAUDIO_OUTPUT_CHUNK_MS);
    if (frames < 1) {
        frames = 1;
    }
    return frames;
}

static void
portaudio_destroy_concealment(dsd_audio_stream* stream) {
    if (!stream) {
        return;
    }
    if (stream->conceal_inited) {
        audio_conceal_destroy(&stream->conceal);
        stream->conceal_inited = 0;
    }
    stream->conceal_has_good = 0;
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

static int
portaudio_write_frames(PaStream* handle, const int16_t* samples, size_t frames) {
    if (!handle || !samples || frames == 0) {
        return 0;
    }
    PaError err = Pa_WriteStream(handle, samples, (unsigned long)frames);
    if (err != paNoError && err != paOutputUnderflowed) {
        set_error_pa(err);
        return -1;
    }
    return 0;
}

static int portaudio_output_open_handle(dsd_audio_stream* stream);

/* Close a device whose write failed. Abort, not stop: a stop waits for buffers a dead device will never play. */
static void
portaudio_close_handle(PaStream* handle) {
    if (handle) {
        portaudio_lifecycle_lock();
        (void)Pa_AbortStream(handle);
        (void)Pa_CloseStream(handle);
        portaudio_lifecycle_unlock();
    }
}

/* The pump's device write failed: drop what was queued for it and owe a reopen (audio_output_recovery.h). The pump
   leaves the failed device alone from now on; the thread that opened it closes and reopens it (dsd_audio_write()),
   since PortAudio's host APIs want a stream closed where it was made (WASAPI's COM objects). Called without the
   lock. */
static void
portaudio_output_device_failed(dsd_audio_stream* stream) {
    const char* msg = audio_error_get();
    dsd_mutex_lock(&stream->mu);
    audio_recovery_note_device_failed(&stream->recovery, dsd_time_monotonic_ns(), msg);
    stream->drops += stream->ring_samples_count;
    stream->ring_samples_head = 0;
    stream->ring_samples_tail = 0;
    stream->ring_samples_count = 0;
    stream->conceal_has_good = 0;
    dsd_cond_broadcast(&stream->cv);
    dsd_mutex_unlock(&stream->mu);
}

static void
portaudio_complete_drain_locked(dsd_audio_stream* stream, int drain_failed) {
    stream->drain_requested = 0;
    stream->drain_failed = drain_failed;
    stream->drain_completed = 1;
    dsd_cond_broadcast(&stream->cv);
}

/* While the device is down the pump only waits for the writer's reopen, answering a drain at once. 0 once the device
   is back, -1 on stop. */
static int
portaudio_wait_while_down_locked(dsd_audio_stream* stream) {
    while (!stream->stop && stream->recovery.down) {
        if (stream->drain_requested) {
            portaudio_complete_drain_locked(stream, 1);
            continue;
        }
        (void)dsd_cond_wait(&stream->cv, &stream->mu);
    }
    return stream->stop ? -1 : 0;
}

static void
portaudio_output_note_write_ok(dsd_audio_stream* stream) {
    const uint64_t now = dsd_time_monotonic_ns();
    dsd_mutex_lock(&stream->mu);
    audio_recovery_note_write_ok(&stream->recovery, now);
    dsd_mutex_unlock(&stream->mu);
}

static void
portaudio_wait_for_work_locked(dsd_audio_stream* stream, int* synthesize_underrun) {
    if (!stream || !synthesize_underrun) {
        return;
    }
    *synthesize_underrun = 0;
    while (!stream->stop && !stream->drain_requested && stream->ring_samples_count == 0) {
        if (stream->conceal_inited && stream->conceal_has_good) {
            int ret = dsd_cond_timedwait(&stream->cv, &stream->mu, DSD_PORTAUDIO_OUTPUT_CHUNK_MS);
            if (ret != 0 && !stream->stop && !stream->drain_requested && stream->ring_samples_count == 0) {
                *synthesize_underrun = 1;
                break;
            }
        } else {
            (void)dsd_cond_wait(&stream->cv, &stream->mu);
        }
    }
}

static int
portaudio_flush_drain_locked(dsd_audio_stream* stream) {
    if (!stream) {
        return -1;
    }
    while (!stream->stop && stream->ring_samples_count > 0) {
        size_t take = stream->ring_samples_count;
        if (take > stream->chunk_samples) {
            take = stream->chunk_samples;
        }
        (void)ring_read_samples(stream, stream->chunk, take);
        size_t frames = take / (size_t)stream->channels;
        dsd_mutex_unlock(&stream->mu);
        if (portaudio_write_frames(stream->handle, stream->chunk, frames) != 0) {
            portaudio_output_device_failed(stream);
            dsd_mutex_lock(&stream->mu);
            return -1;
        }
        portaudio_output_note_write_ok(stream);
        dsd_mutex_lock(&stream->mu);
    }
    return 0;
}

static int
portaudio_stop_and_restart_stream(dsd_audio_stream* stream) {
    if (!stream || !stream->handle) {
        return -1;
    }
    portaudio_lifecycle_lock();
    PaError err = Pa_StopStream(stream->handle);
    if (err == paNoError) {
        err = Pa_StartStream(stream->handle);
    }
    portaudio_lifecycle_unlock();
    if (err != paNoError) {
        set_error_pa(err);
        return -1;
    }
    return 0;
}

static int
portaudio_handle_drain_request_locked(dsd_audio_stream* stream) {
    if (!stream) {
        return -1;
    }

    int drain_failed = 0;
    if (portaudio_flush_drain_locked(stream) != 0) {
        /* The device failed while flushing: it is down now, and the drain is over. */
        portaudio_complete_drain_locked(stream, 1);
        return 0;
    }

    if (!stream->stop) {
        dsd_mutex_unlock(&stream->mu);
        if (portaudio_stop_and_restart_stream(stream) != 0) {
            drain_failed = 1;
        }
        dsd_mutex_lock(&stream->mu);
    }

    portaudio_complete_drain_locked(stream, drain_failed);
    return 0;
}

static void
portaudio_prepare_chunk_locked(dsd_audio_stream* stream, int synthesize_underrun) {
    if (!stream) {
        return;
    }

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
    } else if (stream->conceal_inited) {
        audio_conceal_on_good_buffer(&stream->conceal, stream->chunk, stream->chunk_frames);
        stream->conceal_has_good = 1;
    }
}

static int
portaudio_write_chunk(dsd_audio_stream* stream) {
    if (!stream) {
        return -1;
    }
    if (portaudio_write_frames(stream->handle, stream->chunk, stream->chunk_frames) == 0) {
        portaudio_output_note_write_ok(stream);
        return 0;
    }
    portaudio_output_device_failed(stream);
    return -1;
}

/* One pass of the pump: wait out a device that is down, else feed the device one chunk or serve a drain. Returns -1
   when the stream stops. */
static int
portaudio_output_pump_step(dsd_audio_stream* stream) {
    dsd_mutex_lock(&stream->mu);
    if (stream->recovery.down) {
        const int back = portaudio_wait_while_down_locked(stream);
        dsd_mutex_unlock(&stream->mu);
        return back;
    }

    int synthesize_underrun = 0;
    portaudio_wait_for_work_locked(stream, &synthesize_underrun);

    if (stream->stop) {
        dsd_mutex_unlock(&stream->mu);
        return -1;
    }

    if (stream->drain_requested) {
        (void)portaudio_handle_drain_request_locked(stream);
        dsd_mutex_unlock(&stream->mu);
        return 0;
    }

    portaudio_prepare_chunk_locked(stream, synthesize_underrun);
    dsd_mutex_unlock(&stream->mu);
    (void)portaudio_write_chunk(stream);
    return 0;
}

static DSD_THREAD_RETURN_TYPE
portaudio_output_pump_thread(void* arg) {
    dsd_audio_stream* stream = (dsd_audio_stream*)arg;
    if (!stream || !stream->handle) {
        DSD_THREAD_RETURN;
    }
    while (portaudio_output_pump_step(stream) == 0) {}

    DSD_THREAD_RETURN;
}

static int
device_supports_direction(const PaDeviceInfo* info, int is_input) {
    if (!info) {
        return 0;
    }
    if (is_input) {
        return info->maxInputChannels > 0;
    }
    return info->maxOutputChannels > 0;
}

static PaDeviceIndex
find_device_match(const char* name, int is_input, int count, int partial_match) {
    if (!name || count <= 0) {
        return paNoDevice;
    }

    for (int i = 0; i < count; i++) {
        const PaDeviceInfo* info = Pa_GetDeviceInfo(i);
        if (!device_supports_direction(info, is_input)) {
            continue;
        }

        if (!partial_match) {
            if (strcmp(info->name, name) == 0) {
                return i;
            }
            continue;
        }

        if (strstr(info->name, name) != NULL) {
            return i;
        }
    }

    return paNoDevice;
}

/**
 * @brief Find device index by name.
 *
 * @param name Device name to search for (NULL for default).
 * @param is_input Non-zero for input devices, zero for output.
 * @return Device index or paNoDevice if not found.
 */
static PaDeviceIndex
find_device_by_name(const char* name, int is_input) {
    if (!name || !name[0]) {
        return is_input ? Pa_GetDefaultInputDevice() : Pa_GetDefaultOutputDevice();
    }

    int count = Pa_GetDeviceCount();
    if (count < 0) {
        return paNoDevice;
    }

    PaDeviceIndex match = find_device_match(name, is_input, count, 0);
    if (match != paNoDevice) {
        return match;
    }

    return find_device_match(name, is_input, count, 1);
}

/*============================================================================
 * Public API Implementation
 *============================================================================*/

int
dsd_audio_init(void) {
    if (s_initialized) {
        return 0;
    }

    portaudio_lifecycle_lock();
    PaError err = Pa_Initialize();
    portaudio_lifecycle_unlock();
    if (err != paNoError) {
        set_error_pa(err);
        return -1;
    }

    s_initialized = 1;
    set_error(NULL);
    return 0;
}

void
dsd_audio_cleanup(void) {
    if (s_initialized) {
        portaudio_lifecycle_lock();
        Pa_Terminate();
        portaudio_lifecycle_unlock();
        s_initialized = 0;
    }
}

int
dsd_audio_enumerate_devices(dsd_audio_device* inputs, dsd_audio_device* outputs, int max_count) {
    if (!s_initialized) {
        if (dsd_audio_init() != 0) {
            return -1;
        }
    }

    if (inputs) {
        DSD_MEMSET(inputs, 0, (size_t)max_count * sizeof(dsd_audio_device));
    }
    if (outputs) {
        DSD_MEMSET(outputs, 0, (size_t)max_count * sizeof(dsd_audio_device));
    }

    int num_devices = Pa_GetDeviceCount();
    if (num_devices < 0) {
        set_error_pa(num_devices);
        return -1;
    }

    int in_idx = 0;
    int out_idx = 0;

    for (int i = 0; i < num_devices; i++) {
        const PaDeviceInfo* info = Pa_GetDeviceInfo(i);
        if (!info) {
            continue;
        }

        /* Add to input list */
        if (info->maxInputChannels > 0 && inputs && in_idx < max_count) {
            dsd_audio_device* dev = &inputs[in_idx];
            dev->index = i;
            DSD_STRNCPY(dev->name, info->name, sizeof(dev->name) - 1);
            dev->name[sizeof(dev->name) - 1] = '\0';
            DSD_STRNCPY(dev->description, info->name, sizeof(dev->description) - 1);
            dev->description[sizeof(dev->description) - 1] = '\0';
            dev->is_input = 1;
            dev->is_output = 0;
            dev->initialized = 1;
            in_idx++;
        }

        /* Add to output list */
        if (info->maxOutputChannels > 0 && outputs && out_idx < max_count) {
            dsd_audio_device* dev = &outputs[out_idx];
            dev->index = i;
            DSD_STRNCPY(dev->name, info->name, sizeof(dev->name) - 1);
            dev->name[sizeof(dev->name) - 1] = '\0';
            DSD_STRNCPY(dev->description, info->name, sizeof(dev->description) - 1);
            dev->description[sizeof(dev->description) - 1] = '\0';
            dev->is_input = 0;
            dev->is_output = 1;
            dev->initialized = 1;
            out_idx++;
        }
    }

    return 0;
}

int
dsd_audio_list_devices(void) {
    if (!s_initialized) {
        if (dsd_audio_init() != 0) {
            DSD_FPRINTF(stderr, "Error: Failed to initialize PortAudio: %s\n", dsd_audio_get_error());
            return -1;
        }
    }

    int num_devices = Pa_GetDeviceCount();
    if (num_devices < 0) {
        DSD_FPRINTF(stderr, "Error: Failed to enumerate devices: %s\n", Pa_GetErrorText(num_devices));
        return -1;
    }

    PaDeviceIndex default_in = Pa_GetDefaultInputDevice();
    PaDeviceIndex default_out = Pa_GetDefaultOutputDevice();

    printf("\nPortAudio version: %s\n\n", Pa_GetVersionText());

    /* List output devices */
    int out_count = 0;
    for (int i = 0; i < num_devices; i++) {
        const PaDeviceInfo* info = Pa_GetDeviceInfo(i);
        if (!info || info->maxOutputChannels <= 0) {
            continue;
        }

        out_count++;
        printf("=======[ Output Device #%d ]=======%s\n", out_count, (i == default_out) ? " [DEFAULT]" : "");
        printf("Name: %s\n", info->name);
        printf("Index: %d\n", i);
        printf("Max Channels: %d\n", info->maxOutputChannels);
        printf("Default Sample Rate: %.0f Hz\n", info->defaultSampleRate);

        const PaHostApiInfo* host = Pa_GetHostApiInfo(info->hostApi);
        if (host) {
            printf("Host API: %s\n", host->name);
        }
        printf("\n");
    }

    /* List input devices */
    int in_count = 0;
    for (int i = 0; i < num_devices; i++) {
        const PaDeviceInfo* info = Pa_GetDeviceInfo(i);
        if (!info || info->maxInputChannels <= 0) {
            continue;
        }

        in_count++;
        printf("=======[ Input Device #%d ]=======%s\n", in_count, (i == default_in) ? " [DEFAULT]" : "");
        printf("Name: %s\n", info->name);
        printf("Index: %d\n", i);
        printf("Max Channels: %d\n", info->maxInputChannels);
        printf("Default Sample Rate: %.0f Hz\n", info->defaultSampleRate);

        const PaHostApiInfo* host = Pa_GetHostApiInfo(info->hostApi);
        if (host) {
            printf("Host API: %s\n", host->name);
        }
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

    if (!s_initialized) {
        if (dsd_audio_init() != 0) {
            return NULL;
        }
    }

    PaDeviceIndex dev_idx = find_device_by_name(params->device, 1);
    if (dev_idx == paNoDevice) {
        set_error("No suitable input device found");
        return NULL;
    }

    const PaDeviceInfo* dev_info = Pa_GetDeviceInfo(dev_idx);
    if (!dev_info) {
        set_error("Failed to get device info");
        return NULL;
    }

    dsd_audio_stream* stream = calloc(1, sizeof(dsd_audio_stream));
    if (!stream) {
        set_error("Out of memory");
        return NULL;
    }

    PaStreamParameters input_params;
    input_params.device = dev_idx;
    input_params.channelCount = params->channels;
    input_params.sampleFormat = paInt16;
    input_params.suggestedLatency = dev_info->defaultLowInputLatency;
    input_params.hostApiSpecificStreamInfo = NULL;

    portaudio_lifecycle_lock();
    PaError err = Pa_OpenStream(&stream->handle, &input_params, NULL, (double)params->sample_rate,
                                paFramesPerBufferUnspecified, paNoFlag, NULL, /* No callback - blocking I/O */
                                NULL);
    PaError start_err = paNoError;
    if (err == paNoError) {
        start_err = Pa_StartStream(stream->handle);
        if (start_err != paNoError) {
            Pa_CloseStream(stream->handle);
        }
    }
    portaudio_lifecycle_unlock();

    if (err != paNoError || start_err != paNoError) {
        set_error_pa(err != paNoError ? err : start_err);
        free(stream);
        return NULL;
    }

    stream->is_input = 1;
    stream->channels = params->channels;
    stream->sample_rate = params->sample_rate;

    return stream;
}

static int
portaudio_open_output_stream(dsd_audio_stream* stream, const dsd_audio_params* params, PaDeviceIndex dev_idx,
                             const PaDeviceInfo* dev_info) {
    if (!stream || !params || !dev_info) {
        return -1;
    }

    PaStreamParameters output_params;
    output_params.device = dev_idx;
    output_params.channelCount = params->channels;
    output_params.sampleFormat = paInt16;
    output_params.suggestedLatency = dev_info->defaultLowOutputLatency;
    output_params.hostApiSpecificStreamInfo = NULL;

    PaStream* handle = NULL;
    portaudio_lifecycle_lock();
    PaError err = Pa_OpenStream(&handle, NULL, &output_params, (double)params->sample_rate,
                                paFramesPerBufferUnspecified, paNoFlag, NULL, /* No callback - blocking I/O */
                                NULL);
    PaError start_err = paNoError;
    if (err == paNoError) {
        start_err = Pa_StartStream(handle);
        if (start_err != paNoError) {
            Pa_CloseStream(handle);
            handle = NULL;
        }
    }
    portaudio_lifecycle_unlock();

    if (err != paNoError || start_err != paNoError) {
        set_error_pa(err != paNoError ? err : start_err);
        return -1;
    }
    stream->handle = handle;
    return 0;
}

/* Keep the device name a reopen finds the device by (NULL or empty: the default device). */
static int
portaudio_output_remember_device(dsd_audio_stream* stream, const char* device) {
    if (!device || !device[0]) {
        return 0;
    }
    if (strlen(device) >= sizeof(stream->device_name)) {
        set_error("Output device name too long");
        return -1;
    }
    DSD_STRNCPY(stream->device_name, device, sizeof(stream->device_name) - 1);
    stream->device_name[sizeof(stream->device_name) - 1] = '\0';
    stream->has_device_name = 1;
    return 0;
}

/* Reopen the playback device with what the stream was opened with. The device list PortAudio read at
   initialization is not refreshed, so this finds the same device (or the default) again. */
static int
portaudio_output_open_handle(dsd_audio_stream* stream) {
    PaDeviceIndex dev_idx = find_device_by_name(stream->has_device_name ? stream->device_name : NULL, 0);
    if (dev_idx == paNoDevice) {
        set_error("No suitable output device found");
        return -1;
    }
    const PaDeviceInfo* dev_info = Pa_GetDeviceInfo(dev_idx);
    if (!dev_info) {
        set_error("Failed to get device info");
        return -1;
    }
    dsd_audio_params params;
    DSD_MEMSET(&params, 0, sizeof(params));
    params.sample_rate = stream->sample_rate;
    params.channels = stream->channels;
    return portaudio_open_output_stream(stream, &params, dev_idx, dev_info);
}

static void
portaudio_init_output_stream_state(dsd_audio_stream* stream, const dsd_audio_params* params) {
    if (!stream || !params) {
        return;
    }

    stream->is_input = 0;
    stream->channels = params->channels;
    stream->sample_rate = params->sample_rate;
    stream->use_async = params->async_output ? 1 : 0;
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
portaudio_init_async_sync(dsd_audio_stream* stream) {
    if (!stream) {
        return 0;
    }
    if (dsd_mutex_init(&stream->mu) != 0) {
        return 0;
    }
    if (dsd_cond_init(&stream->cv) != 0) {
        (void)dsd_mutex_destroy(&stream->mu);
        return 0;
    }
    return 1;
}

static void
portaudio_setup_async_buffers(dsd_audio_stream* stream) {
    if (!stream || !stream->use_async) {
        return;
    }

    const size_t channel_count = (size_t)stream->channels;
    size_t min_ring_frames = 0;
    stream->chunk_frames = calc_chunk_frames(stream->sample_rate);
    stream->use_async = size_mul_nonzero(stream->chunk_frames, channel_count, &stream->chunk_samples);
    if (stream->use_async) {
        stream->chunk = (int16_t*)calloc(stream->chunk_samples, sizeof(int16_t));
        if (audio_conceal_init(&stream->conceal, stream->chunk_frames, stream->channels) == 0) {
            stream->conceal_inited = 1;
        }
    }

    if (stream->use_async) {
        stream->use_async = size_mul_nonzero(stream->chunk_frames, 8U, &min_ring_frames);
    }
    if (stream->use_async) {
        size_t ring_frames = ms_to_frames(stream->sample_rate, DSD_PORTAUDIO_OUTPUT_RING_MS);
        if (ring_frames < min_ring_frames) {
            ring_frames = min_ring_frames;
        }
        stream->use_async = size_mul_nonzero(ring_frames, channel_count, &stream->ring_samples_capacity);
    }
    if (stream->use_async) {
        stream->ring = (int16_t*)calloc(stream->ring_samples_capacity, sizeof(int16_t));
        stream->ring_samples_head = 0;
        stream->ring_samples_tail = 0;
        stream->ring_samples_count = 0;
    }

    if (!stream->chunk || !stream->ring) {
        stream->use_async = 0;
    }
}

static void
portaudio_start_async_thread(dsd_audio_stream* stream) {
    if (!stream || !stream->use_async) {
        return;
    }

    if (dsd_thread_create(&stream->thread, portaudio_output_pump_thread, stream) != 0) {
        stream->use_async = 0;
    } else {
        stream->thread_started = 1;
    }
}

static void
portaudio_cleanup_async_state(dsd_audio_stream* stream, int async_sync_inited) {
    if (!stream) {
        return;
    }
    if (stream->thread_started) {
        (void)dsd_thread_join(stream->thread);
        stream->thread_started = 0;
    }
    if (stream->chunk) {
        free(stream->chunk);
        stream->chunk = NULL;
    }
    portaudio_destroy_concealment(stream);
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
    if (!params) {
        set_error("NULL parameters");
        return NULL;
    }

    if (!s_initialized && dsd_audio_init() != 0) {
        return NULL;
    }

    PaDeviceIndex dev_idx = find_device_by_name(params->device, 0);
    if (dev_idx == paNoDevice) {
        set_error("No suitable output device found");
        return NULL;
    }

    const PaDeviceInfo* dev_info = Pa_GetDeviceInfo(dev_idx);
    if (!dev_info) {
        set_error("Failed to get device info");
        return NULL;
    }

    dsd_audio_stream* stream = calloc(1, sizeof(dsd_audio_stream));
    if (!stream) {
        set_error("Out of memory");
        return NULL;
    }
    if (portaudio_output_remember_device(stream, params->device) != 0) {
        free(stream);
        return NULL;
    }

    if (portaudio_open_output_stream(stream, params, dev_idx, dev_info) != 0) {
        free(stream);
        return NULL;
    }

    portaudio_init_output_stream_state(stream, params);

    int async_sync_inited = 0;
    if (stream->use_async) {
        async_sync_inited = portaudio_init_async_sync(stream);
        if (!async_sync_inited) {
            stream->use_async = 0;
        }
    }

    if (stream->use_async) {
        portaudio_setup_async_buffers(stream);
    }

    if (stream->use_async) {
        portaudio_start_async_thread(stream);
    }

    if (!stream->use_async) {
        portaudio_cleanup_async_state(stream, async_sync_inited);
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

    PaError err = Pa_ReadStream(stream->handle, buffer, (unsigned long)frames);
    if (err != paNoError && err != paInputOverflowed) {
        set_error_pa(err);
        return -1;
    }

    return (int)frames;
}

/* An async stream's device is down: once a reopen is due, close the failed device and open it again, on the writer's
   thread, which opened it (the pump only waits meanwhile and touches no handle). Called and returns with the lock held;
   drops it around the device calls. 0 when the device is back. */
static int
portaudio_output_async_reopen_locked(dsd_audio_stream* stream) {
    if (!audio_recovery_reopen_due(&stream->recovery, dsd_time_monotonic_ns())) {
        set_error(stream->recovery.error);
        return -1;
    }
    PaStream* dead = stream->handle;
    stream->handle = NULL;
    dsd_mutex_unlock(&stream->mu);
    portaudio_close_handle(dead);
    const int rc = portaudio_output_open_handle(stream);
    const char* msg = rc == 0 ? NULL : audio_error_get();
    dsd_mutex_lock(&stream->mu);
    if (rc != 0) {
        /* Timed after the attempt: an open that blocks past the wait must not make the next one due at once. */
        audio_recovery_note_reopen_failed(&stream->recovery, dsd_time_monotonic_ns(), msg);
        return -1;
    }
    audio_recovery_note_reopened(&stream->recovery);
    dsd_cond_broadcast(&stream->cv);
    return 0;
}

/* Blocking output: the caller's thread owns the device. A failed write closes it, and the next write that finds a
   reopen due reopens it (audio_output_recovery.h); until then writes fail at once. */
static int
portaudio_output_sync_write(dsd_audio_stream* stream, const int16_t* buffer, size_t frames) {
    if (!stream->handle) {
        if (!audio_recovery_reopen_due(&stream->recovery, dsd_time_monotonic_ns())) {
            set_error(stream->recovery.error);
            return -1;
        }
        if (portaudio_output_open_handle(stream) != 0) {
            /* Timed after the attempt: an open that blocks past the wait must not make the next one due at once. */
            audio_recovery_note_reopen_failed(&stream->recovery, dsd_time_monotonic_ns(), audio_error_get());
            return -1;
        }
        audio_recovery_note_reopened(&stream->recovery);
    }
    if (portaudio_write_frames(stream->handle, buffer, frames) != 0) {
        const char* msg = audio_error_get();
        portaudio_close_handle(stream->handle);
        stream->handle = NULL;
        audio_recovery_note_device_failed(&stream->recovery, dsd_time_monotonic_ns(), msg);
        return -1;
    }
    audio_recovery_note_write_ok(&stream->recovery, dsd_time_monotonic_ns());
    return (int)frames;
}

int
dsd_audio_write(dsd_audio_stream* stream, const int16_t* buffer, size_t frames) {
    /* A NULL device handle is legal on an output stream: it is down and owed a reopen. */
    if (!stream || !buffer) {
        set_error("Invalid arguments");
        return -1;
    }

    if (stream->is_input) {
        set_error("Cannot write to input stream");
        return -1;
    }

    if (!stream->use_async) {
        return portaudio_output_sync_write(stream, buffer, frames);
    }

    size_t samples = 0;
    if (!size_mul_nonzero(frames, (size_t)stream->channels, &samples)) {
        return 0;
    }

    dsd_mutex_lock(&stream->mu);

    if (stream->stop) {
        dsd_mutex_unlock(&stream->mu);
        return -1;
    }

    /* The device is down: reopen it here, on the thread that opened it, once that is due; until then nothing is queued,
       since it would not play before the reopen and stale audio after it is worse than a gap. */
    if (stream->recovery.down && portaudio_output_async_reopen_locked(stream) != 0) {
        stream->drops += samples;
        dsd_mutex_unlock(&stream->mu);
        return -1;
    }

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
            DSD_FPRINTF(stderr, "PortAudio output stats: rate=%d ch=%d underruns=%llu drops=%llu\n",
                        stream->sample_rate, stream->channels, (unsigned long long)stream->underruns,
                        (unsigned long long)stream->drops);
        }

        (void)dsd_cond_destroy(&stream->cv);
        (void)dsd_mutex_destroy(&stream->mu);

        if (stream->chunk) {
            free(stream->chunk);
            stream->chunk = NULL;
        }
        portaudio_destroy_concealment(stream);
        if (stream->ring) {
            free(stream->ring);
            stream->ring = NULL;
        }
    }

    if (stream->handle && stream->recovery.down) {
        /* A failed device would never play out what a stop waits for. */
        portaudio_close_handle(stream->handle);
    } else if (stream->handle) {
        portaudio_lifecycle_lock();
        Pa_StopStream(stream->handle);
        Pa_CloseStream(stream->handle);
        portaudio_lifecycle_unlock();
    }

    free(stream);
}

int
dsd_audio_drain(dsd_audio_stream* stream) {
    if (!stream) {
        return -1;
    }

    if (stream->is_input) {
        /* Drain doesn't apply to input streams */
        return 0;
    }

    if (stream->use_async) {
        dsd_mutex_lock(&stream->mu);
        if (stream->recovery.down) {
            /* Nothing is queued for a device that is down, and the pump will not wait on it. */
            dsd_mutex_unlock(&stream->mu);
            return -1;
        }
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

    if (!stream->handle) {
        return -1;
    }
    /* Stop, then restart the stream for continued use. */
    return portaudio_stop_and_restart_stream(stream);
}

int
dsd_audio_output_health_get(dsd_audio_stream* stream, dsd_audio_output_health* out) {
    if (!out) {
        return -1;
    }
    DSD_MEMSET(out, 0, sizeof(*out));
    if (!stream || stream->is_input) {
        return -1;
    }
    if (stream->use_async) {
        dsd_mutex_lock(&stream->mu);
    }
    out->failures = stream->recovery.failures;
    out->recoveries = stream->recovery.recoveries;
    out->down = stream->recovery.down;
    DSD_STRNCPY(out->error, stream->recovery.error, sizeof(out->error) - 1);
    out->error[sizeof(out->error) - 1] = '\0';
    if (stream->use_async) {
        dsd_mutex_unlock(&stream->mu);
    }
    return 0;
}

const char*
dsd_audio_get_error(void) {
    return audio_error_get();
}

const char*
dsd_audio_backend_name(void) {
    return "portaudio";
}

#endif /* DSD_USE_PORTAUDIO */

int
dsd_audio_output_device_id(void) {
    return -1;
}
