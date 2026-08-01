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
#elif defined(DSD_NEO_AUDIO_BACKEND_AAUDIO)
#include <aaudio/AAudio.h>
typedef AAudioStream dsd_audio_backend_handle;
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

#ifdef DSD_NEO_AUDIO_BACKEND_AAUDIO
    /* AAudio grants a rate/channel layout that need not match the requested one
     * (an 8 kHz open is refused outright on some devices), so the backend keeps
     * the device format alongside the logical one and converts on the way out. */
    int device_sample_rate;
    int device_channels;
    int needs_convert;
    uint32_t resample_step_q16;  /* source frames advanced per device frame */
    uint32_t resample_phase_q16; /* fractional source position carried between writes */
    int16_t resample_prev[2];    /* last source frame, for interpolation across writes */
    int resample_prev_valid;
    int16_t* convert_buf;
    size_t convert_buf_samples;
    /* Frames still to be dropped before another open is attempted, so a wedged
     * or mid-transition audio device is not reopened once per buffer. */
    size_t reopen_debt_frames;
    /* Failed device writes since audio last flowed; the first one recovers
     * immediately, repeats back off. */
    int recovery_streak;
    /* Successful device reopens, i.e. route changes survived. */
    int reopen_count;
    /* Frames accepted by the device across the stream's whole life; unlike
     * AAudioStream_getFramesWritten this survives a reopen. */
    uint64_t device_frames;
    /* Samples dropped by the device-write path. Kept apart from `drops` because
     * only the single writer (pump thread, or the caller in sync mode) touches
     * it, without holding the ring mutex. */
    uint64_t device_drops;
#endif
};

#endif /* DSD_NEO_SRC_PLATFORM_AUDIO_STREAM_INTERNAL_H */
