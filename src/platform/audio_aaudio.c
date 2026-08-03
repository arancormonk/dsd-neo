// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief AAudio backend implementation for the audio abstraction layer.
 *
 * Selected with -DDSD_AUDIO_BACKEND=aaudio, which is only valid for Android
 * builds. Output uses blocking AAudioStream_write so the semantics match the
 * PulseAudio backend, optionally fronted by an async pump thread that shares the
 * ring/concealment design (the pump itself is per-backend code).
 *
 * Two Android realities are absorbed here instead of being pushed onto callers:
 * decoded voice is 8 kHz and older devices refuse an 8 kHz open, so a failed
 * open is retried at the device's own rate and the backend resamples; and a
 * route change (headphones, Bluetooth) disconnects the stream, which is
 * recovered by reopening it in place.
 *
 * Device selection is deliberately ignored: AAudio routing is OS-managed, so
 * dsd_audio_params::device has no effect here.
 */

#include <aaudio/AAudio.h>
#include <dsd-neo/core/string_utils.h>
#include <dsd-neo/platform/audio.h>
#include <dsd-neo/platform/audio_concealment.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/platform/timing.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "dsd-neo/core/safe_api.h"
#define DSD_NEO_AUDIO_BACKEND_AAUDIO 1
#include "audio_error_internal.h"
#include "audio_stream_internal.h"

/*============================================================================
 * Tunables
 *============================================================================*/

#define DSD_AAUDIO_OUTPUT_RING_MS       1000
#define DSD_AAUDIO_OUTPUT_CHUNK_MS      20
#define DSD_AAUDIO_BUFFER_BURSTS        3
/* Device-side buffer the pump keeps ahead of the mixer. Three bursts is the
 * latency-first default and is far too tight for a decoder: the pump wakes on a
 * 20 ms cadence from an ordinary thread, so any scheduling jitter past the buffer
 * depth is an audible xrun. Several chunk periods of slack cost latency nobody
 * hears on a scanner and remove the stutter. */
#define DSD_AAUDIO_BUFFER_TARGET_MS     160
/* Cushion the pump builds once, before it first feeds the device. Decoder output
 * is bursty -- a voice frame arrives, then nothing until the next one -- so
 * starting playback on the very first chunk leaves the device with no runway.
 * One cushion's worth of added latency is inaudible on a scanner. It is
 * deliberately not rebuilt after a gap; see aaudio_output_wait_for_work_locked. */
#define DSD_AAUDIO_OUTPUT_PRIME_MS      100
/* A tail shorter than the cushion still has to play: after this long with data
 * waiting but not enough of it, give up on filling the cushion and flush. */
#define DSD_AAUDIO_PRIME_FLUSH_MS       200
/* How little the device may still hold before an empty ring counts as a real
 * starvation. Above this the device has enough runway to wait for real audio;
 * below it, concealment is what keeps the mixer from hard-underrunning. Must
 * stay well under DSD_AAUDIO_BUFFER_TARGET_MS or the pump conceals constantly. */
#define DSD_AAUDIO_CONCEAL_LOW_WATER_MS 80
/* How recently the decoder must have produced audio for concealment to count as
 * landing inside a call rather than after it. Three chunk periods. */
#define DSD_AAUDIO_MIDSPEECH_MS         60
#define DSD_AAUDIO_BUFFER_CAPACITY_MS   500
#define DSD_AAUDIO_IO_TIMEOUT_NS        500000000LL /* 500 ms */
#define DSD_AAUDIO_DRAIN_POLL_MS        5
#define DSD_AAUDIO_DRAIN_TIMEOUT_MS     2000
#define DSD_AAUDIO_MAX_CONV_CHANNELS    2
#define DSD_AAUDIO_REOPEN_BACKOFF_MS    500

/*============================================================================
 * Module State
 *============================================================================*/

static int s_initialized = 0;

/*============================================================================
 * Internal Helpers
 *============================================================================*/

static void
set_error_aaudio(aaudio_result_t res) {
    const char* text = AAudio_convertResultToText(res);
    set_error(text ? text : "AAudio error");
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
    size_t frames = ms_to_frames(sample_rate, DSD_AAUDIO_OUTPUT_CHUNK_MS);
    if (frames < 1) {
        frames = 1;
    }
    return frames;
}

/*============================================================================
 * Ring Helpers
 *============================================================================*/

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

/*============================================================================
 * Stream Open / Close
 *============================================================================*/

static void
aaudio_discard_handle(AAudioStream* handle) {
    if (!handle) {
        return;
    }
    (void)AAudioStream_requestStop(handle);
    (void)AAudioStream_close(handle);
}

static aaudio_result_t
aaudio_build_and_open(const dsd_audio_stream* stream, int32_t sample_rate, AAudioStream** out) {
    AAudioStreamBuilder* builder = NULL;
    aaudio_result_t res = AAudio_createStreamBuilder(&builder);
    if (res != AAUDIO_OK) {
        return res;
    }
    if (!builder) {
        return AAUDIO_ERROR_NO_MEMORY;
    }

    AAudioStreamBuilder_setDirection(builder, stream->is_input ? AAUDIO_DIRECTION_INPUT : AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setChannelCount(builder, stream->channels);
    AAudioStreamBuilder_setSampleRate(builder, sample_rate);
    /* Deliberately not LOW_LATENCY: a scanner has no use for a few ms of latency,
     * and the shallow fast-path buffer that comes with it starves whenever the pump
     * thread is scheduled late (measured: ~280 device xruns/s, audibly stuttering).
     * The normal mixer path with a deep buffer runs xrun-free. */
    AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_NONE);
    if (!stream->is_input) {
        AAudioStreamBuilder_setUsage(builder, AAUDIO_USAGE_MEDIA);
        /* Ask for room up front: the granted capacity caps the buffer size set
         * after the open, and the default capacity can be only a few bursts. */
        const size_t capacity_frames = ms_to_frames(sample_rate, DSD_AAUDIO_BUFFER_CAPACITY_MS);
        if (capacity_frames > 0 && capacity_frames <= (size_t)INT32_MAX) {
            AAudioStreamBuilder_setBufferCapacityInFrames(builder, (int32_t)capacity_frames);
        }
    }

    res = AAudioStreamBuilder_openStream(builder, out);
    (void)AAudioStreamBuilder_delete(builder);
    return res;
}

static void
aaudio_reset_resampler(dsd_audio_stream* stream) {
    stream->resample_phase_q16 = 0;
    stream->resample_prev[0] = 0;
    stream->resample_prev[1] = 0;
    stream->resample_prev_valid = 0;
}

/**
 * @brief Decide how the granted device format maps onto the requested one.
 *
 * @return 1 when the stream is usable as-is or convertible, 0 when the granted
 *         format cannot be served (input streams are never converted).
 */
static int
aaudio_init_conversion(dsd_audio_stream* stream) {
    stream->needs_convert =
        (stream->device_sample_rate != stream->sample_rate || stream->device_channels != stream->channels) ? 1 : 0;
    if (!stream->needs_convert) {
        return 1;
    }
    if (stream->is_input || stream->channels > DSD_AAUDIO_MAX_CONV_CHANNELS
        || stream->device_channels > DSD_AAUDIO_MAX_CONV_CHANNELS) {
        return 0;
    }

    uint64_t step = ((uint64_t)stream->sample_rate << 16) / (uint64_t)stream->device_sample_rate;
    if (step == 0 || step > UINT32_MAX) {
        return 0;
    }
    stream->resample_step_q16 = (uint32_t)step;
    aaudio_reset_resampler(stream);
    return 1;
}

/**
 * @brief Size the device-side buffer the pump keeps ahead of the mixer.
 *
 * Rounded up to whole bursts and clamped to the capacity the device granted.
 */
static void
aaudio_apply_output_buffer_size(const dsd_audio_stream* stream, AAudioStream* handle) {
    const int32_t burst = AAudioStream_getFramesPerBurst(handle);
    if (burst <= 0) {
        return;
    }

    int32_t want = burst * DSD_AAUDIO_BUFFER_BURSTS;
    const size_t target = ms_to_frames(stream->device_sample_rate, DSD_AAUDIO_BUFFER_TARGET_MS);
    if (target > (size_t)want && target <= (size_t)INT32_MAX) {
        /* Round up to a whole number of bursts; AAudio works in bursts. */
        want = (int32_t)(((target + (size_t)burst - 1U) / (size_t)burst) * (size_t)burst);
    }

    const int32_t capacity = AAudioStream_getBufferCapacityInFrames(handle);
    if (capacity > 0 && want > capacity) {
        want = capacity;
    }

    (void)AAudioStream_setBufferSizeInFrames(handle, want);
}

static int
aaudio_stream_open(dsd_audio_stream* stream) {
    AAudioStream* handle = NULL;
    aaudio_result_t res = aaudio_build_and_open(stream, stream->sample_rate, &handle);
    if (res != AAUDIO_OK && !stream->is_input) {
        /* Devices that refuse an 8 kHz open still open at their own rate. */
        res = aaudio_build_and_open(stream, AAUDIO_UNSPECIFIED, &handle);
    }
    if (res != AAUDIO_OK || !handle) {
        set_error_aaudio(res != AAUDIO_OK ? res : AAUDIO_ERROR_NULL);
        return -1;
    }

    stream->device_sample_rate = (int)AAudioStream_getSampleRate(handle);
    stream->device_channels = (int)AAudioStream_getChannelCount(handle);
    if (stream->device_sample_rate <= 0 || stream->device_channels <= 0) {
        set_error("AAudio reported an invalid stream format");
        aaudio_discard_handle(handle);
        return -1;
    }
    if (!aaudio_init_conversion(stream)) {
        set_error("AAudio granted an unusable rate/channel combination");
        aaudio_discard_handle(handle);
        return -1;
    }

    if (!stream->is_input) {
        aaudio_apply_output_buffer_size(stream, handle);
    }

    res = AAudioStream_requestStart(handle);
    if (res != AAUDIO_OK) {
        set_error_aaudio(res);
        aaudio_discard_handle(handle);
        return -1;
    }

    stream->handle = handle;
    return 0;
}

/**
 * @brief Tear the device stream down and schedule a reopen.
 *
 * Route changes (headphones, Bluetooth) disconnect the stream, and a device in
 * the middle of such a transition also refuses to reopen for a while. Both
 * directions are therefore best-effort: drop the audio that cannot be moved,
 * retry roughly every DSD_AAUDIO_REOPEN_BACKOFF_MS, and come back on their own
 * once the route settles.
 */
static void
aaudio_schedule_reopen(dsd_audio_stream* stream) {
    aaudio_discard_handle(stream->handle);
    stream->handle = NULL;
    /* The first failure after healthy playback retries on the next buffer: an
     * ordinary route change has already settled by then, and a half-second of
     * silence would be audible. Repeated failures back off instead. */
    stream->reopen_debt_frames =
        (stream->recovery_streak == 0) ? 0 : ms_to_frames(stream->sample_rate, DSD_AAUDIO_REOPEN_BACKOFF_MS);
    stream->recovery_streak++;
}

/**
 * @brief Make the device handle usable again if the backoff has been paid.
 *
 * @param pending_frames Frames the caller is about to move (and will drop, or fail
 *                       to read, when this returns 0), used to pay down the backoff.
 * @return 1 when the stream is open, 0 while the device is unavailable.
 */
static int
aaudio_recover_handle(dsd_audio_stream* stream, size_t pending_frames) {
    if (stream->handle) {
        return 1;
    }
    if (stream->reopen_debt_frames > pending_frames) {
        stream->reopen_debt_frames -= pending_frames;
        return 0;
    }

    stream->reopen_debt_frames = 0;
    if (aaudio_stream_open(stream) != 0) {
        aaudio_schedule_reopen(stream);
        return 0;
    }
    stream->reopen_count++;
    return 1;
}

/*============================================================================
 * Format Conversion (output fallback path)
 *============================================================================*/

/**
 * @brief Map one interpolated source frame onto the device's channel layout.
 *
 * Conversion is gated to mono/stereo when the stream opens, and an open that reports
 * a non-positive channel count is rejected outright. Both widths are clamped anyway:
 * @p in_channels so the fixed-size interpolation frame can never be indexed out of
 * range, and @p out_channels because it decides how much of the caller's output slot
 * this writes. Leaving the output width untrusted meant a non-positive value returned
 * with the slot never written at all, handing a frame of uninitialized heap straight
 * to the device -- the conversion buffer comes from realloc(), which does not zero.
 */
static void
aaudio_map_frame(const int16_t* in, int in_channels, int16_t* out, int out_channels) {
    int src_channels = in_channels;
    if (src_channels > DSD_AAUDIO_MAX_CONV_CHANNELS) {
        src_channels = DSD_AAUDIO_MAX_CONV_CHANNELS;
    }
    if (src_channels < 1) {
        src_channels = 1;
    }

    int dst_channels = out_channels;
    if (dst_channels > DSD_AAUDIO_MAX_CONV_CHANNELS) {
        dst_channels = DSD_AAUDIO_MAX_CONV_CHANNELS;
    }
    if (dst_channels < 1) {
        dst_channels = 1;
    }

    if (src_channels == 2 && dst_channels == 1) {
        out[0] = (int16_t)(((int32_t)in[0] + (int32_t)in[1]) / 2);
        return;
    }
    for (int c = 0; c < dst_channels; c++) {
        const int src = (c < src_channels) ? c : (src_channels - 1);
        out[c] = in[src];
    }
}

/** @brief Device frames @p src_frames logical frames turn into, ignoring phase. */
static size_t
aaudio_device_frames_for(const dsd_audio_stream* stream, size_t src_frames) {
    if (!stream->needs_convert) {
        return src_frames;
    }
    uint64_t frames = ((uint64_t)src_frames * (uint64_t)stream->device_sample_rate) / (uint64_t)stream->sample_rate;
    return (frames > SIZE_MAX) ? SIZE_MAX : (size_t)frames;
}

/**
 * @brief Output frames aaudio_convert_frames() can emit for @p src_frames input.
 *
 * Derived from the loop's own arithmetic rather than from the exact rate ratio: the
 * loop runs while `phase < src_frames << 16`, advancing by the *truncated*
 * resample_step_q16 from a carried phase in [0, step), so it takes at most
 * ceil((src_frames << 16) / step) iterations. That bound is exact.
 *
 * A fixed slack over the exact ratio does not work here. The step's truncation error
 * is a per-iteration deficit, so the surplus over the ideal frame count grows with
 * src_frames -- at 8 kHz into 48 kHz a constant two frames of headroom is outgrown
 * past ~2730 input frames, and the conversion then silently drops the tail of the
 * buffer. The async pump only ever passes 20 ms chunks, but dsd_audio_write() hands
 * the caller's buffer straight through in synchronous mode.
 */
static size_t
aaudio_convert_capacity(const dsd_audio_stream* stream, size_t src_frames) {
    if (!stream->needs_convert) {
        return src_frames;
    }
    if (src_frames == 0 || stream->resample_step_q16 == 0) {
        return 0;
    }
    if (src_frames > (SIZE_MAX >> 16)) {
        return 0;
    }

    const uint64_t span = (uint64_t)src_frames << 16;
    const uint64_t step = (uint64_t)stream->resample_step_q16;
    /* step >= 1, so the quotient never exceeds span and stays inside size_t. */
    return (size_t)((span + step - 1U) / step);
}

static int
aaudio_ensure_convert_buf(dsd_audio_stream* stream, size_t frames) {
    size_t samples = 0;
    size_t bytes = 0;

    if (!size_mul_nonzero(frames, (size_t)stream->device_channels, &samples)
        || !size_mul_nonzero(samples, sizeof(int16_t), &bytes)) {
        set_error("Invalid conversion buffer size");
        return 0;
    }
    if (stream->convert_buf && stream->convert_buf_samples >= samples) {
        return 1;
    }

    int16_t* grown = realloc(stream->convert_buf, bytes);
    if (!grown) {
        set_error("Out of memory");
        return 0;
    }
    stream->convert_buf = grown;
    stream->convert_buf_samples = samples;
    return 1;
}

/**
 * @brief Resample and remap @p src_frames logical frames into the device format.
 *
 * Linear interpolation with a Q16 phase carried across calls, so back-to-back
 * chunks stay continuous. Returns the number of device frames written.
 */
static size_t
aaudio_convert_frames(dsd_audio_stream* stream, const int16_t* src, size_t src_frames, int16_t* dst,
                      size_t dst_capacity_frames) {
    const size_t in_ch = (size_t)stream->channels;
    const size_t out_ch = (size_t)stream->device_channels;
    const uint64_t src_span = (uint64_t)src_frames << 16;
    uint64_t phase = stream->resample_phase_q16;
    size_t out_frames = 0;
    int16_t frame[DSD_AAUDIO_MAX_CONV_CHANNELS] = {0};

    while (phase < src_span && out_frames < dst_capacity_frames) {
        size_t idx = (size_t)(phase >> 16);
        int32_t frac = (int32_t)(phase & 0xFFFFU);
        const int16_t* b = &src[idx * in_ch];
        const int16_t* a = b;
        if (idx > 0) {
            a = &src[(idx - 1) * in_ch];
        } else if (stream->resample_prev_valid) {
            a = stream->resample_prev;
        }

        for (size_t c = 0; c < in_ch; c++) {
            int64_t delta = (int64_t)b[c] - (int64_t)a[c];
            frame[c] = (int16_t)((int32_t)a[c] + (int32_t)((delta * frac) >> 16));
        }
        aaudio_map_frame(frame, stream->channels, &dst[out_frames * out_ch], stream->device_channels);
        out_frames++;
        phase += stream->resample_step_q16;
    }

    if (src_frames > 0) {
        const int16_t* last = &src[(src_frames - 1) * in_ch];
        for (size_t c = 0; c < in_ch; c++) {
            stream->resample_prev[c] = last[c];
        }
        stream->resample_prev_valid = 1;
    }
    if (phase >= src_span) {
        stream->resample_phase_q16 = (uint32_t)(phase - src_span);
    } else {
        /* Capacity-bound exit: the unconsumed tail of this buffer is dropped, so only
         * the sub-sample offset can carry. Zeroing the whole phase would restart the
         * interpolation mid-sample. Unreachable while aaudio_convert_capacity() is
         * derived from the same step; kept correct rather than merely unreached. */
        stream->resample_phase_q16 = (uint32_t)(phase & 0xFFFFU);
    }
    return out_frames;
}

/*============================================================================
 * Device Writes
 *============================================================================*/

/**
 * @brief Push device-format frames at the device, dropping what it will not take.
 *
 * Deliberately returns nothing: a failed device write is never fatal here. The
 * stream is recycled, the rest of the buffer is dropped and a later write reopens
 * it, because audio must never stall or permanently kill the decoder. Only
 * aaudio_write_frames() has a failure worth reporting (a conversion buffer it
 * could not allocate).
 */
static void
aaudio_write_device_frames(dsd_audio_stream* stream, const int16_t* buf, size_t frames) {
    const size_t stride = (size_t)stream->device_channels;
    const int16_t* cursor = buf;
    size_t remaining = frames;

    while (remaining > 0) {
        int32_t want = (remaining > (size_t)INT32_MAX) ? INT32_MAX : (int32_t)remaining;
        aaudio_result_t res = AAudioStream_write(stream->handle, cursor, want, DSD_AAUDIO_IO_TIMEOUT_NS);
        if (res > 0) {
            remaining -= (size_t)res;
            cursor += (size_t)res * stride;
            stream->device_frames += (uint64_t)res;
            stream->recovery_streak = 0;
            continue;
        }

        /* No progress at all: a disconnect, a device error, or a long stall. */
        set_error_aaudio(res == 0 ? AAUDIO_ERROR_TIMEOUT : res);
        aaudio_schedule_reopen(stream);
        stream->device_drops += remaining * stride;
        return;
    }
}

static int
aaudio_write_frames(dsd_audio_stream* stream, const int16_t* buf, size_t frames) {
    if (frames == 0) {
        return 0;
    }
    if (!aaudio_recover_handle(stream, frames)) {
        /* Counted in device samples like every other device_drops update, so the
         * two sides of the stats line stay in the same unit when the resampling
         * fallback is engaged and a logical frame is not a device frame. */
        stream->device_drops += aaudio_device_frames_for(stream, frames) * (size_t)stream->device_channels;
        return 0;
    }
    if (!stream->needs_convert) {
        aaudio_write_device_frames(stream, buf, frames);
        return 0;
    }

    size_t capacity = aaudio_convert_capacity(stream, frames);
    if (capacity == 0 || !aaudio_ensure_convert_buf(stream, capacity)) {
        return -1;
    }

    size_t converted = aaudio_convert_frames(stream, buf, frames, stream->convert_buf, capacity);
    if (converted == 0) {
        return 0;
    }
    aaudio_write_device_frames(stream, stream->convert_buf, converted);
    return 0;
}

/**
 * @brief Wait until the device has consumed everything written so far.
 *
 * AAudio has no drain call; frames-written vs frames-read is the equivalent.
 */
static int
aaudio_wait_drained(dsd_audio_stream* stream) {
    if (!stream->handle) {
        /* Nothing queued in a device that is currently being recovered. */
        return 0;
    }

    for (int waited_ms = 0; waited_ms <= DSD_AAUDIO_DRAIN_TIMEOUT_MS; waited_ms += DSD_AAUDIO_DRAIN_POLL_MS) {
        int64_t written = AAudioStream_getFramesWritten(stream->handle);
        int64_t played = AAudioStream_getFramesRead(stream->handle);
        if (written < 0 || played < 0) {
            set_error_aaudio((aaudio_result_t)((written < 0) ? written : played));
            return -1;
        }
        if (played >= written) {
            return 0;
        }
        dsd_sleep_us((uint64_t)DSD_AAUDIO_DRAIN_POLL_MS * 1000U);
    }

    set_error("AAudio output drain timed out");
    return -1;
}

/**
 * @brief Frames written to the device but not yet played, or -1 if unknown.
 *
 * This is the cushion the device itself is still holding. The pump uses it to
 * tell a real starvation (device about to run dry) from a decoder gap that the
 * device buffer is already covering.
 */
static int64_t
aaudio_queued_frames(const dsd_audio_stream* stream) {
    if (!stream->handle) {
        return -1;
    }

    int64_t written = AAudioStream_getFramesWritten(stream->handle);
    int64_t played = AAudioStream_getFramesRead(stream->handle);
    if (written < 0 || played < 0 || played > written) {
        return -1;
    }

    return written - played;
}

/**
 * @brief Whether the device is close enough to running dry to need concealment.
 *
 * An empty ring is not by itself a starvation: the device is still holding
 * everything written to it, and the decoder is bursty, so audio very often
 * arrives well before the device runs out. Concealment written in that window is
 * not filling a gap -- it is queued *ahead* of the next real chunk, so it lands
 * inside the call and delays the speech behind it. Only once the device is down
 * to its last few milliseconds is padding the right answer.
 *
 * An unknown queue depth (stream being recovered) counts as starved, which is
 * the conservative direction: keep the device fed.
 *
 * Must be called with stream->mu released: the AAudio getters take the stream's
 * own lock, and stream->mu is on the decoder's write path.
 */
static int
aaudio_output_device_starved(const dsd_audio_stream* stream) {
    const int64_t queued = aaudio_queued_frames(stream);
    if (queued < 0) {
        return 1;
    }

    const int rate = stream->device_sample_rate > 0 ? stream->device_sample_rate : stream->sample_rate;
    const size_t low_water = ms_to_frames(rate, DSD_AAUDIO_CONCEAL_LOW_WATER_MS);

    return queued < (int64_t)low_water;
}

/*============================================================================
 * Async Output Pump
 *============================================================================*/

static void
aaudio_output_request_stop(dsd_audio_stream* stream) {
    dsd_mutex_lock(&stream->mu);
    stream->stop = 1;
    dsd_cond_broadcast(&stream->cv);
    dsd_mutex_unlock(&stream->mu);
}

/* What the pump should do after one timed wait. */
enum aaudio_wait_step {
    AAUDIO_WAIT_CONTINUE = 0, /* keep waiting for real audio */
    AAUDIO_WAIT_PLAY,         /* push what is queued */
    AAUDIO_WAIT_CONCEAL,      /* device is running dry; pad it */
};

/**
 * @brief Decide what to do after one timed wait.
 *
 * @param device_starved Queue depth verdict sampled by the caller with the ring
 *                       mutex released; only consulted when the ring is empty.
 */
static enum aaudio_wait_step
aaudio_output_classify_wait_locked(const dsd_audio_stream* stream, int waited_ms, int device_starved) {
    if (stream->stop || stream->drain_requested) {
        return AAUDIO_WAIT_PLAY; /* the caller re-checks both */
    }

    if (stream->ring_samples_count == 0U) {
        if (device_starved && stream->conceal_inited && stream->conceal_has_good) {
            return AAUDIO_WAIT_CONCEAL;
        }
        return AAUDIO_WAIT_CONTINUE;
    }

    if (waited_ms >= DSD_AAUDIO_PRIME_FLUSH_MS) {
        /* Data is waiting but the cushion never filled: this is a tail. */
        return AAUDIO_WAIT_PLAY;
    }

    return AAUDIO_WAIT_CONTINUE;
}

/**
 * @brief Sample the device queue for the wait loop, with the ring mutex dropped.
 *
 * Entered and left holding stream->mu, but the AAudio query itself runs without it:
 * the getters take the stream's own lock, and every dsd_audio_write() from the
 * decoder contends for stream->mu, so holding it across the call would put AAudio's
 * lock on the decoder's hot path. Only the pump thread opens, closes or replaces
 * stream->handle, so it cannot change across the window, and the caller re-reads
 * everything the answer is combined with after the relock.
 *
 * @return Nonzero when the device is close to running dry and concealment is
 *         available to cover it; 0 when there is nothing to decide.
 */
static int
aaudio_output_sample_starvation_locked(dsd_audio_stream* stream) {
    if (stream->stop || stream->drain_requested || stream->ring_samples_count != 0U) {
        return 0;
    }
    if (!stream->conceal_inited || !stream->conceal_has_good) {
        return 0;
    }

    dsd_mutex_unlock(&stream->mu);
    const int starved = aaudio_output_device_starved(stream);
    dsd_mutex_lock(&stream->mu);
    return starved;
}

static int
aaudio_output_wait_for_work_locked(dsd_audio_stream* stream) {
    int synthesize_underrun = 0;
    int waited_ms = 0;

    /* Sleep until enough audio is queued to push, or a control event (drain/stop)
     * arrives. While priming, "enough" is the cushion; once playing, any queued
     * samples go out immediately so the pump stays paced by the device. */
    while (!stream->stop && !stream->drain_requested) {
        const size_t have = stream->ring_samples_count;
        const size_t need = stream->priming ? stream->prime_samples : 1U;
        if (have >= need) {
            break;
        }

        /* An untimed wait is only safe with the ring completely empty and nothing
         * to conceal with; anything else must be able to give up, or a tail shorter
         * than the cushion would never play. */
        if (have == 0U && !(stream->conceal_inited && stream->conceal_has_good)) {
            (void)dsd_cond_wait(&stream->cv, &stream->mu);
            continue;
        }

        (void)dsd_cond_timedwait(&stream->cv, &stream->mu, DSD_AAUDIO_OUTPUT_CHUNK_MS);
        waited_ms += DSD_AAUDIO_OUTPUT_CHUNK_MS;

        const int device_starved = aaudio_output_sample_starvation_locked(stream);
        const enum aaudio_wait_step step = aaudio_output_classify_wait_locked(stream, waited_ms, device_starved);
        if (step == AAUDIO_WAIT_CONCEAL) {
            synthesize_underrun = 1;
            break;
        }
        if (step == AAUDIO_WAIT_PLAY) {
            break;
        }
    }

    /* The cushion is built once, at stream start, and every exit from the loop above
     * other than a stop leads straight into feeding the device -- so the initial fill
     * is over here regardless of whether the ring ever reached prime_samples.
     *
     * Clearing this only on the ring >= prime_samples exit left priming set when a
     * short tail was released by DSD_AAUDIO_PRIME_FLUSH_MS instead, which put the
     * *next* burst behind the same 200 ms wait. On a channel whose first transmission
     * is shorter than the cushion, that is every burst of the session.
     *
     * Rebuilding the cushion after a gap is actively harmful on a real-time feed: the
     * decoder only ever supplies audio as fast as it arrives, so withholding output
     * until a fresh cushion accumulates starves a device that is already low and turns
     * one gap into a self-sustaining stutter. Past the initial fill, the device buffer
     * is the cushion and DSD_AAUDIO_CONCEAL_LOW_WATER_MS is what protects it. */
    stream->priming = 0;

    if (stream->stop) {
        return -1;
    }

    return synthesize_underrun;
}

static int
aaudio_output_handle_drain_locked(dsd_audio_stream* stream) {
    int drain_failed = 0;

    /* Flush remaining queued audio without padding, then wait for the device. */
    while (!stream->stop && stream->ring_samples_count > 0) {
        size_t take = stream->ring_samples_count;
        if (take > stream->chunk_samples) {
            take = stream->chunk_samples;
        }
        (void)ring_read_samples(stream, stream->chunk, take);
        dsd_mutex_unlock(&stream->mu);

        if (aaudio_write_frames(stream, stream->chunk, take / (size_t)stream->channels) < 0) {
            aaudio_output_request_stop(stream);
            return -1;
        }

        dsd_mutex_lock(&stream->mu);
    }

    if (!stream->stop) {
        dsd_mutex_unlock(&stream->mu);
        if (aaudio_wait_drained(stream) < 0) {
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
aaudio_output_prepare_chunk_locked(dsd_audio_stream* stream, int synthesize_underrun) {
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
        if (take > 0) {
            stream->underruns_partial++;
        }
        if (stream->last_write_ns != 0U) {
            /* Concealment this soon after real audio is a gap inside a call, not
             * the tail of one. */
            const uint64_t now_ns = dsd_time_monotonic_ns();
            if (now_ns >= stream->last_write_ns
                && (now_ns - stream->last_write_ns) < (uint64_t)DSD_AAUDIO_MIDSPEECH_MS * 1000000U) {
                stream->underruns_midspeech++;
            }
        }
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
aaudio_output_write_chunk(dsd_audio_stream* stream) {
    if (aaudio_write_frames(stream, stream->chunk, stream->chunk_frames) < 0) {
        aaudio_output_request_stop(stream);
        return -1;
    }
    return 0;
}

static DSD_THREAD_RETURN_TYPE
aaudio_output_pump_thread(void* arg) {
    dsd_audio_stream* stream = (dsd_audio_stream*)arg;
    if (!stream || !stream->handle) {
        /* Raise stop before leaving: dsd_audio_drain() waits on drain_completed or
         * stop, and a pump that exits without setting either turns a startup failure
         * into a hang. Unreachable while the thread is only started after a
         * successful open, which is exactly why it must not be left to chance. */
        if (stream) {
            aaudio_output_request_stop(stream);
        }
        DSD_THREAD_RETURN;
    }

    while (1) {
        dsd_mutex_lock(&stream->mu);
        int synthesize_underrun = aaudio_output_wait_for_work_locked(stream);
        if (synthesize_underrun < 0) {
            dsd_mutex_unlock(&stream->mu);
            break;
        }

        if (stream->drain_requested) {
            if (aaudio_output_handle_drain_locked(stream) < 0) {
                DSD_THREAD_RETURN;
            }
            continue;
        }

        aaudio_output_prepare_chunk_locked(stream, synthesize_underrun);
        dsd_mutex_unlock(&stream->mu);
        if (aaudio_output_write_chunk(stream) < 0) {
            break;
        }
    }

    DSD_THREAD_RETURN;
}

/*============================================================================
 * Async Output Setup / Teardown
 *============================================================================*/

static void
aaudio_output_init_async_state(dsd_audio_stream* stream, int async_output) {
    stream->use_async = async_output ? 1 : 0;
    stream->thread_started = 0;
    stream->stop = 0;
    stream->drain_requested = 0;
    stream->drain_completed = 0;
    stream->drain_failed = 0;
    stream->underruns = 0;
    stream->underruns_partial = 0;
    stream->underruns_midspeech = 0;
    stream->last_write_ns = 0;
    stream->drops = 0;
    stream->in_samples = 0;
    stream->conceal_has_good = 0;
}

static int
aaudio_output_init_async_sync(dsd_audio_stream* stream) {
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
aaudio_output_prepare_async_buffers(dsd_audio_stream* stream) {
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
    /* Cushion, bounded so it can never exceed what the ring can hold. */
    {
        size_t prime_frames = ms_to_frames(stream->sample_rate, DSD_AAUDIO_OUTPUT_PRIME_MS);
        if (prime_frames < stream->chunk_frames) {
            prime_frames = stream->chunk_frames;
        }
        if (!size_mul_nonzero(prime_frames, channel_count, &stream->prime_samples)) {
            return 0;
        }
        stream->priming = 1;
    }

    ring_frames = ms_to_frames(stream->sample_rate, DSD_AAUDIO_OUTPUT_RING_MS);
    if (ring_frames < min_ring_frames) {
        ring_frames = min_ring_frames;
    }
    if (!size_mul_nonzero(ring_frames, channel_count, &stream->ring_samples_capacity)) {
        return 0;
    }

    if (stream->prime_samples > stream->ring_samples_capacity / 2U) {
        stream->prime_samples = stream->ring_samples_capacity / 2U;
    }

    stream->ring = calloc(stream->ring_samples_capacity, sizeof(int16_t));
    stream->ring_samples_head = 0;
    stream->ring_samples_tail = 0;
    stream->ring_samples_count = 0;
    return stream->chunk && stream->ring;
}

static int
aaudio_output_start_async_thread(dsd_audio_stream* stream) {
    if (dsd_thread_create(&stream->thread, aaudio_output_pump_thread, stream) != 0) {
        return 0;
    }
    stream->thread_started = 1;
    return 1;
}

static void
aaudio_output_disable_async(dsd_audio_stream* stream, int async_sync_inited) {
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

static void
aaudio_fill_device(dsd_audio_device* dev, int is_input) {
    if (!dev) {
        return;
    }
    dev->index = 0;
    DSD_STRNCPY(dev->name, is_input ? "default input" : "default output", sizeof(dev->name) - 1);
    dev->name[sizeof(dev->name) - 1] = '\0';
    DSD_STRNCPY(dev->description, is_input ? "AAudio default input" : "AAudio default output",
                sizeof(dev->description) - 1);
    dev->description[sizeof(dev->description) - 1] = '\0';
    dev->is_input = is_input;
    dev->is_output = !is_input;
    dev->initialized = 1;
}

/**
 * @brief Report the granted device format and error counters on close.
 *
 * Unlike the PulseAudio backend this prints whenever DSD_NEO_AUDIO_STATS is set,
 * even with clean counters: which rate/layout the device actually granted (and
 * therefore whether the resampling fallback engaged) is the first thing worth
 * knowing when triaging audio on an unfamiliar phone.
 */
static void
aaudio_report_stats(const dsd_audio_stream* stream) {
    const char* stats_env = getenv("DSD_NEO_AUDIO_STATS");
    if (!stats_env || stats_env[0] == '\0') {
        return;
    }

    int32_t xruns = stream->handle ? AAudioStream_getXRunCount(stream->handle) : -1;

    DSD_FPRINTF(stderr,
                "AAudio %s stats: rate=%d ch=%d dev_rate=%d dev_ch=%d converted=%d async=%d in_frames=%llu "
                "frames=%llu reopens=%d underruns=%llu underruns_partial=%llu underruns_midspeech=%llu drops=%llu "
                "device_drops=%llu "
                "xruns=%d\n",
                stream->is_input ? "input" : "output", stream->sample_rate, stream->channels,
                stream->device_sample_rate, stream->device_channels, stream->needs_convert, stream->use_async,
                (unsigned long long)(stream->channels > 0 ? stream->in_samples / (uint64_t)stream->channels : 0U),
                (unsigned long long)stream->device_frames, stream->reopen_count, (unsigned long long)stream->underruns,
                (unsigned long long)stream->underruns_partial, (unsigned long long)stream->underruns_midspeech,
                (unsigned long long)stream->drops, (unsigned long long)stream->device_drops, (int)xruns);
}

/*============================================================================
 * Public API Implementation
 *============================================================================*/

int
dsd_audio_init(void) {
    if (s_initialized) {
        return 0;
    }
    /* AAudio has no global init; streams are created on demand. */
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

    /* AAudio routing is OS-managed: report the default endpoints only. */
    if (inputs) {
        DSD_MEMSET(inputs, 0, (size_t)max_count * sizeof(dsd_audio_device));
        aaudio_fill_device(&inputs[0], 1);
    }
    if (outputs) {
        DSD_MEMSET(outputs, 0, (size_t)max_count * sizeof(dsd_audio_device));
        aaudio_fill_device(&outputs[0], 0);
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
    if (!validate_stream_params(params)) {
        return NULL;
    }

    dsd_audio_stream* stream = calloc(1, sizeof(dsd_audio_stream));
    if (!stream) {
        set_error("Out of memory");
        return NULL;
    }

    stream->is_input = 1;
    stream->channels = params->channels;
    stream->sample_rate = params->sample_rate;

    if (aaudio_stream_open(stream) != 0) {
        free(stream);
        return NULL;
    }

    return stream;
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

    stream->is_input = 0;
    stream->channels = params->channels;
    stream->sample_rate = params->sample_rate;

    if (aaudio_stream_open(stream) != 0) {
        free(stream);
        return NULL;
    }

    aaudio_output_init_async_state(stream, params->async_output);
    if (stream->use_async) {
        int async_sync_inited = aaudio_output_init_async_sync(stream);
        if (!async_sync_inited || !aaudio_output_prepare_async_buffers(stream)
            || !aaudio_output_start_async_thread(stream)) {
            aaudio_output_disable_async(stream, async_sync_inited);
        }
    }

    return stream;
}

int
dsd_audio_read(dsd_audio_stream* stream, int16_t* buffer, size_t frames) {
    /* A NULL handle is legal here, as it is for output: capture between a disconnect
     * and a successful reopen still has a stream, just not a device. */
    if (!stream || !buffer) {
        set_error("Invalid arguments");
        return -1;
    }

    if (!stream->is_input) {
        set_error("Cannot read from output stream");
        return -1;
    }

    if (!aaudio_recover_handle(stream, frames)) {
        set_error("AAudio input stream is disconnected");
        return -1;
    }

    const size_t stride = (size_t)stream->channels;
    int16_t* cursor = buffer;
    size_t remaining = frames;
    int reopened = 0;

    while (remaining > 0) {
        int32_t want = (remaining > (size_t)INT32_MAX) ? INT32_MAX : (int32_t)remaining;
        aaudio_result_t res = AAudioStream_read(stream->handle, cursor, want, DSD_AAUDIO_IO_TIMEOUT_NS);
        if (res > 0) {
            remaining -= (size_t)res;
            cursor += (size_t)res * stride;
            stream->device_frames += (uint64_t)res;
            stream->recovery_streak = 0;
            continue;
        }
        if (res == AAUDIO_ERROR_DISCONNECTED && !reopened) {
            /* Capture follows the route too. Reopen once inline, so an ordinary route
             * change costs nothing but the samples already lost; if the device is
             * still mid-transition, report the disconnect and leave the reopen owed.
             * A later call pays the backoff down and tries again — abandoning the
             * handle here would make one bad moment permanent. */
            reopened = 1;
            aaudio_schedule_reopen(stream);
            if (aaudio_recover_handle(stream, remaining)) {
                continue;
            }
            set_error_aaudio(res);
            return -1;
        }
        set_error_aaudio(res == 0 ? AAUDIO_ERROR_TIMEOUT : res);
        return -1;
    }

    return (int)frames;
}

int
dsd_audio_write(dsd_audio_stream* stream, const int16_t* buffer, size_t frames) {
    /* A NULL handle is legal here: output tolerates a stream being recovered. */
    if (!stream || !buffer) {
        set_error("Invalid arguments");
        return -1;
    }

    if (stream->is_input) {
        set_error("Cannot write to input stream");
        return -1;
    }

    if (!stream->use_async) {
        if (aaudio_write_frames(stream, buffer, frames) < 0) {
            return -1;
        }
        return (int)frames;
    }

    size_t samples = 0;
    if (!size_mul_nonzero(frames, (size_t)stream->channels, &samples)) {
        /* Nothing to write is a no-op; the channel count is validated at open, so the
         * only other way to get here is an overflow. Sizing the ring write off a
         * wrapped sample count would read past what the caller allocated. */
        if (frames == 0) {
            return 0;
        }
        set_error("Frame count overflows the sample count");
        return -1;
    }

    /* Sampled before the lock: stream->mu is the one the pump goes out of its way not
     * to hold across anything slow (see aaudio_output_sample_starvation_locked), and
     * this is the decoder's hot path. */
    const uint64_t write_ns = dsd_time_monotonic_ns();

    dsd_mutex_lock(&stream->mu);

    if (stream->stop) {
        dsd_mutex_unlock(&stream->mu);
        return -1;
    }

    stream->in_samples += samples;
    stream->last_write_ns = write_ns;

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
        /* Keep only the newest window. Both the ring content being thrown away and
         * the head of this buffer that cannot fit are drops; counting only the
         * former hid the larger loss in exactly the case the counter exists for. */
        const int16_t* src = buffer + (samples - stream->ring_samples_capacity);
        stream->drops += stream->ring_samples_count + (samples - stream->ring_samples_capacity);
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
        aaudio_output_request_stop(stream);

        if (stream->thread_started) {
            (void)dsd_thread_join(stream->thread);
            stream->thread_started = 0;
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

    aaudio_report_stats(stream);

    aaudio_discard_handle(stream->handle);
    stream->handle = NULL;

    if (stream->convert_buf) {
        free(stream->convert_buf);
        stream->convert_buf = NULL;
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

    if (!stream->use_async) {
        return aaudio_wait_drained(stream);
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
    return audio_error_get();
}

const char*
dsd_audio_backend_name(void) {
    return "aaudio";
}
