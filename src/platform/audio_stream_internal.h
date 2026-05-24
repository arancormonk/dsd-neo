// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_SRC_PLATFORM_AUDIO_STREAM_INTERNAL_H
#define DSD_NEO_SRC_PLATFORM_AUDIO_STREAM_INTERNAL_H

#include <dsd-neo/platform/audio_concealment.h>
#include <dsd-neo/platform/threading.h>

#include <stddef.h>
#include <stdint.h>

#if defined(DSD_NEO_AUDIO_BACKEND_PULSE)
#include <pulse/simple.h>
typedef pa_simple dsd_audio_backend_handle;
#elif defined(DSD_NEO_AUDIO_BACKEND_PORTAUDIO)
#include <portaudio.h>
typedef PaStream dsd_audio_backend_handle;
#else
typedef void dsd_audio_backend_handle;
#endif

struct dsd_audio_stream {
    dsd_audio_backend_handle* handle;
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

#endif /* DSD_NEO_SRC_PLATFORM_AUDIO_STREAM_INTERNAL_H */
