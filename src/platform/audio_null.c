// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Null backend implementation for the audio abstraction layer.
 *
 * Selected with -DDSD_AUDIO_BACKEND=none. It lets the tree build and link with
 * no audio library present: device-audio output becomes a no-op sink instead of
 * a configure failure, and device-audio input yields silence.
 *
 * Output deliberately does not pace itself. Realtime sources pace on the input
 * side, and file/IQ replay is intentionally faster than realtime. Input does
 * pace, because an instantly-returning silent microphone busy-spins the caller.
 */

#include <dsd-neo/core/string_utils.h>
#include <dsd-neo/platform/audio.h>
#include <dsd-neo/platform/timing.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "dsd-neo/core/safe_api.h"
#define DSD_NEO_AUDIO_BACKEND_NULL 1
#include "audio_error_internal.h"
#include "audio_stream_internal.h"

/*============================================================================
 * Module State
 *============================================================================*/

static int s_initialized = 0;

/*============================================================================
 * Internal Helpers
 *============================================================================*/

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

static dsd_audio_stream*
null_open(const dsd_audio_params* params, int is_input) {
    if (!validate_stream_params(params)) {
        return NULL;
    }

    dsd_audio_stream* stream = calloc(1, sizeof(dsd_audio_stream));
    if (!stream) {
        set_error("Out of memory");
        return NULL;
    }

    stream->handle = NULL;
    stream->is_input = is_input;
    stream->channels = params->channels;
    stream->sample_rate = params->sample_rate;
    set_error(NULL);
    return stream;
}

static void
null_fill_device(dsd_audio_device* dev, int is_input) {
    if (!dev) {
        return;
    }
    dev->index = 0;
    DSD_STRNCPY(dev->name, "null", sizeof(dev->name) - 1);
    dev->name[sizeof(dev->name) - 1] = '\0';
    DSD_STRNCPY(dev->description, is_input ? "Null audio input" : "Null audio output", sizeof(dev->description) - 1);
    dev->description[sizeof(dev->description) - 1] = '\0';
    dev->is_input = is_input;
    dev->is_output = !is_input;
    dev->initialized = 1;
}

/*============================================================================
 * Public API Implementation
 *============================================================================*/

int
dsd_audio_init(void) {
    if (s_initialized) {
        return 0;
    }
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
    if (max_count <= 0) {
        set_error("Invalid device array size");
        return -1;
    }

    if (inputs) {
        DSD_MEMSET(inputs, 0, (size_t)max_count * sizeof(dsd_audio_device));
        null_fill_device(&inputs[0], 1);
    }
    if (outputs) {
        DSD_MEMSET(outputs, 0, (size_t)max_count * sizeof(dsd_audio_device));
        null_fill_device(&outputs[0], 0);
    }

    set_error(NULL);
    return 0;
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
    printf("=======[ Output Device #1 ]=======\n");
    printf("Description: %s\n", outputs[0].description);
    printf("Name: %s\n", outputs[0].name);
    printf("Index: %d\n", outputs[0].index);
    printf("\n");
    printf("=======[ Input Device #1 ]=======\n");
    printf("Description: %s\n", inputs[0].description);
    printf("Name: %s\n", inputs[0].name);
    printf("Index: %d\n", inputs[0].index);
    printf("\n");

    return 0;
}

dsd_audio_stream*
dsd_audio_open_input(const dsd_audio_params* params) {
    return null_open(params, 1);
}

dsd_audio_stream*
dsd_audio_open_output(const dsd_audio_params* params) {
    return null_open(params, 0);
}

int
// cppcheck-suppress constParameterPointer -- signature fixed by dsd-neo/platform/audio.h
dsd_audio_read(dsd_audio_stream* stream, int16_t* buffer, size_t frames) {
    if (!stream || !buffer) {
        set_error("Invalid arguments");
        return -1;
    }

    if (!stream->is_input) {
        set_error("Cannot read from output stream");
        return -1;
    }

    if (frames == 0) {
        return 0;
    }

    /* Pace to the requested rate: an instant silent read busy-spins the caller. */
    uint64_t us = ((uint64_t)frames * 1000000U) / (uint64_t)stream->sample_rate;
    if (us > 0) {
        dsd_sleep_us(us);
    }

    DSD_MEMSET(buffer, 0, frames * (size_t)stream->channels * sizeof(int16_t));
    return (int)frames;
}

int
// cppcheck-suppress constParameterPointer -- signature fixed by dsd-neo/platform/audio.h
dsd_audio_write(dsd_audio_stream* stream, const int16_t* buffer, size_t frames) {
    if (!stream || !buffer) {
        set_error("Invalid arguments");
        return -1;
    }

    if (stream->is_input) {
        set_error("Cannot write to input stream");
        return -1;
    }

    /* Discard immediately; no pacing (see file header). */
    return (int)frames;
}

void
dsd_audio_close(dsd_audio_stream* stream) {
    free(stream);
}

int
// cppcheck-suppress constParameterPointer -- signature fixed by dsd-neo/platform/audio.h
dsd_audio_drain(dsd_audio_stream* stream) {
    if (!stream) {
        return -1;
    }
    return 0;
}

const char*
dsd_audio_get_error(void) {
    return audio_error_get();
}

const char*
dsd_audio_backend_name(void) {
    return "null";
}
