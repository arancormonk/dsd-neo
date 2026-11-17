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

#if defined(DSD_NEO_AUDIO_BACKEND_PULSE) || defined(DSD_NEO_AUDIO_BACKEND_PORTAUDIO)
#include "audio_output_recovery.h"
#endif

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

/* The frames @p keep_ns of input lasts at @p rate, rounded up and at most @p limit: what an input discard keeps of the
   newest input (dsd_audio_discard_input()). Past 10 s the count is held, which no discard's cap reaches. */
static inline uint64_t
audio_input_keep_frames(uint64_t keep_ns, int rate, uint64_t limit) {
    if (rate <= 0 || keep_ns == 0U) {
        return 0U;
    }
    const uint64_t max_ns = 10000000000ULL;
    const uint64_t ns = keep_ns < max_ns ? keep_ns : max_ns;
    const uint64_t frames = (ns * (uint64_t)rate + 999999999ULL) / 1000000000ULL;
    return frames < limit ? frames : limit;
}

/* The frames an input discard keeps (dsd_audio_discard_input()): those captured from the mark @p marked_ns until
   @p now_ns, both on the monotonic clock, rounded up and at most @p limit. @p now_ns is read after the backend counted
   what it holds, so the audio captured while the caller waited before that count is kept too. A mark after @p now_ns
   keeps nothing. */
static inline uint64_t
audio_input_keep_since(uint64_t marked_ns, uint64_t now_ns, int rate, uint64_t limit) {
    return audio_input_keep_frames(now_ns > marked_ns ? now_ns - marked_ns : 0U, rate, limit);
}

struct dsd_audio_stream {
    dsd_audio_backend_handle* handle;
    int is_input;
    int route_registered;
    int channels;
    int sample_rate;
    /* PulseAudio input streams only (NULL elsewhere): the frames an input discard kept because they were captured after
       its mark (dsd_audio_discard_input()), which dsd_audio_read() returns before the device's. pa_simple says nothing
       of what it holds, so a discard reads it all and keeps the newest frames. Allocated with the stream. In the part
       every backend's layout shares, so a stream sized without the backend's own fields still holds it. */
    struct pulse_input_carry* input_carry;

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

#if defined(DSD_NEO_AUDIO_BACKEND_PULSE) || defined(DSD_NEO_AUDIO_BACKEND_PORTAUDIO)
    /* Output device recovery (audio_output_recovery.h): what the stream was opened with, so a device whose write
     * failed can be reopened behind the same handle. `recovery` is read and written under `mu` in async mode and
     * belongs to the writing thread in sync mode. `handle` in async mode: PulseAudio's pump owns it after open and
     * closes and reopens it itself; PortAudio's pump only writes it while the device is up, and while it is down the
     * writer's thread, which opened it, closes and reopens it (the pump touches no handle until `recovery.down`
     * clears under `mu`). */
    char device_name[512];
    int has_device_name;
    char app_name[64];
    dsd_audio_output_recovery recovery;
#endif

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
    /* Monotonic deadline (ns) before which no further open is attempted, so a
     * wedged or mid-transition audio device is not reopened once per buffer. Zero
     * means "retry on the next call". Wall-clock rather than a frame count: with no
     * device open the write path returns immediately, so a caller that is not
     * rate-limited by a real device would spend a frame-denominated budget in
     * milliseconds. */
    uint64_t reopen_not_before_ns;
    /* Failed device writes since a whole buffer last went through; the first one
     * recovers immediately, repeats back off. */
    int recovery_streak;
    /* Consecutive device writes that timed out with nothing taken. A stall is not
     * a broken device, so it costs the buffer rather than the stream until there
     * have been enough of them to mean the device really has stopped. */
    int write_timeouts;
    /* Successful device reopens, i.e. route changes survived. */
    int reopen_count;
    /* Frames accepted by the device across the stream's whole life; unlike
     * AAudioStream_getFramesWritten this survives a reopen. */
    uint64_t device_frames;
    /* Samples dropped by the device-write path. Kept apart from `drops` because
     * only the single writer (pump thread, or the caller in sync mode) touches
     * it, without holding the ring mutex. */
    uint64_t device_drops;
    /* Playback cushion built once, before the pump first feeds the device: it
     * withholds output until the ring holds this many samples so the device
     * starts with runway. Deliberately never rebuilt after a gap -- on a
     * real-time feed the decoder can only refill it in wall-clock time, during
     * which an already-low device runs dry and one gap becomes a self-sustaining
     * stutter. Past the initial fill the device buffer is the cushion. */
    size_t prime_samples;
    int priming;
    /* Samples handed to the backend by the decoder. Compared against
     * `device_frames` this separates "the decoder produced nothing" from "the
     * backend lost what it was given". */
    uint64_t in_samples;
    /* Subset of `underruns` where the ring held some audio but less than a whole
     * chunk, i.e. a real fragment of speech was padded with concealment rather
     * than the stream simply being idle. This is the one that sounds broken. */
    uint64_t underruns_partial;
    /* Subset of `underruns` synthesized while the decoder was still actively
     * producing, i.e. concealment landed in the middle of a call instead of
     * after it. This is the one that sounds broken; idle-time concealment is
     * just silence and is expected. */
    uint64_t underruns_midspeech;
    /* Consecutive wholly-synthesized chunks, i.e. how long the pump has been
     * padding a device the decoder has stopped feeding. Bounds how far past the
     * end of a transmission concealment is worth writing; see
     * DSD_AAUDIO_CONCEAL_MAX_MS. Reset by any chunk carrying real audio. */
    int conceal_run;
    /* When the decoder last handed over audio, for the check above. */
    uint64_t last_write_ns;
#endif
};

#endif /* DSD_NEO_SRC_PLATFORM_AUDIO_STREAM_INTERNAL_H */
