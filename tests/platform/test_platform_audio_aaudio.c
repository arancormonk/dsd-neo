// SPDX-License-Identifier: GPL-3.0-or-later
// Coverage fixtures intentionally use private-source inclusion, synthetic sentinels,
// invalid-value negative vectors, or wrapper symbols to exercise guarded behavior.
// NOLINTBEGIN(bugprone-suspicious-include)
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Exercises the Android AAudio backend on the build host: the NDK header is
 * replaced by tests/platform/aaudio_stub and every AAudio entry point is faked
 * here, so the rate/channel fallback, the disconnect recovery and the pump
 * helpers stay covered on machines that have no Android at all.
 */

#define DSD_NEO_THREADING_NO_INLINE_CREATE 1

#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "../../src/platform/audio_aaudio.c"
#include "aaudio/AAudio.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/platform/audio.h"
#include "dsd-neo/platform/audio_concealment.h"
#include "dsd-neo/platform/threading.h"
#include "dsd-neo/platform/timing.h"

#define FAKE_CAPTURE_SAMPLES 8192

#define FAKE_BUILDER         ((AAudioStreamBuilder*)0x1)
#define FAKE_STREAM          ((AAudioStream*)0x2)

static struct {
    aaudio_result_t builder_result;
    int open_calls;
    aaudio_result_t open_result;
    int reject_explicit_rate;
    int32_t last_rate;
    int32_t last_channels;
    int32_t last_format;
    int32_t last_direction;
    int32_t last_performance;
    int32_t last_usage;
    int usage_calls;
    int32_t device_rate;
    int32_t device_channels;
    int32_t frames_per_burst;
    int32_t buffer_capacity_frames;
    int32_t requested_capacity_frames;
    int32_t buffer_size_frames;
    int start_calls;
    int stop_calls;
    int close_calls;
    int delete_calls;
    aaudio_result_t start_result;
    int write_calls;
    int write_result_pending;
    aaudio_result_t write_result;
    int32_t write_frame_limit;
    int read_calls;
    int read_result_pending;
    aaudio_result_t read_result;
    int64_t frames_written;
    int64_t frames_read;
    int64_t frames_read_step;
    int frames_read_error;
    int thread_create_calls;
    int thread_join_calls;
    /* Drives the device queue during a pump wait: each timed wait plays this
     * many frames, so a buffered device drains toward starvation the way a real
     * one does. */
    int64_t frames_played_per_wait;
    int cond_timedwait_calls;
    /* Ring-mutex depth, and whether any AAudio query ran while it was held. The
     * decoder contends for that mutex on every write, so a device call underneath
     * it puts AAudio's own lock on the decoder's hot path. */
    int mutex_depth;
    int queries_under_mutex;
    /* Fake monotonic clock, so "was the decoder producing just now" is decided
     * by the test rather than by how fast it happens to run. */
    uint64_t now_ns;
    int16_t captured[FAKE_CAPTURE_SAMPLES];
    size_t captured_samples;
} g;

static void
reset_fakes(void) {
    DSD_MEMSET(&g, 0, sizeof(g));
    g.device_rate = 8000;
    g.device_channels = 2;
    g.frames_per_burst = 96;
    g.buffer_capacity_frames = 16000;
    g.requested_capacity_frames = 0;
    g.frames_read_step = INT64_MAX / 4;
    /* Nonzero: a zero timestamp reads as "the decoder never wrote". */
    g.now_ns = 1000000000ULL;
}

uint64_t
dsd_time_monotonic_ns(void) {
    return g.now_ns;
}

/*============================================================================
 * Platform fakes
 *============================================================================*/

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
    g.mutex_depth++;
    return 0;
}

int
dsd_mutex_unlock(dsd_mutex_t* mutex) {
    (void)mutex;
    g.mutex_depth--;
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
    g.cond_timedwait_calls++;
    if (g.frames_played_per_wait > 0) {
        g.frames_read += g.frames_played_per_wait;
        if (g.frames_read > g.frames_written) {
            g.frames_read = g.frames_written;
        }
    }
    return 0;
}

int
dsd_thread_create_impl(dsd_thread_t* thread, void* arg, dsd_thread_fn func) {
    (void)thread;
    (void)arg;
    (void)func;
    g.thread_create_calls++;
    return 0;
}

int
dsd_thread_join(dsd_thread_t thread) {
    (void)thread;
    g.thread_join_calls++;
    return 0;
}

void
dsd_sleep_us(uint64_t us) {
    (void)us;
}

int
audio_conceal_init(struct audio_conceal_state* cs, size_t buffer_frames, int channels) {
    if (cs != NULL) {
        cs->channels = channels;
    }
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

/*============================================================================
 * AAudio fakes
 *============================================================================*/

const char*
AAudio_convertResultToText(aaudio_result_t result) {
    switch (result) {
        case AAUDIO_OK: return "AAUDIO_OK";
        case AAUDIO_ERROR_DISCONNECTED: return "AAUDIO_ERROR_DISCONNECTED";
        case AAUDIO_ERROR_INVALID_STATE: return "AAUDIO_ERROR_INVALID_STATE";
        case AAUDIO_ERROR_INTERNAL: return "AAUDIO_ERROR_INTERNAL";
        case AAUDIO_ERROR_TIMEOUT: return "AAUDIO_ERROR_TIMEOUT";
        default: return "AAUDIO_ERROR";
    }
}

aaudio_result_t
AAudio_createStreamBuilder(AAudioStreamBuilder** builder) {
    if (g.builder_result != AAUDIO_OK) {
        return g.builder_result;
    }
    *builder = FAKE_BUILDER;
    return AAUDIO_OK;
}

void
AAudioStreamBuilder_setDirection(AAudioStreamBuilder* builder, aaudio_direction_t direction) {
    (void)builder;
    g.last_direction = direction;
}

void
AAudioStreamBuilder_setFormat(AAudioStreamBuilder* builder, aaudio_format_t format) {
    (void)builder;
    g.last_format = format;
}

void
AAudioStreamBuilder_setChannelCount(AAudioStreamBuilder* builder, int32_t channel_count) {
    (void)builder;
    g.last_channels = channel_count;
}

void
AAudioStreamBuilder_setSampleRate(AAudioStreamBuilder* builder, int32_t sample_rate) {
    (void)builder;
    g.last_rate = sample_rate;
}

void
AAudioStreamBuilder_setPerformanceMode(AAudioStreamBuilder* builder, aaudio_performance_mode_t mode) {
    (void)builder;
    g.last_performance = mode;
}

void
AAudioStreamBuilder_setUsage(AAudioStreamBuilder* builder, aaudio_usage_t usage) {
    (void)builder;
    g.last_usage = usage;
    g.usage_calls++;
}

aaudio_result_t
AAudioStreamBuilder_openStream(AAudioStreamBuilder* builder, AAudioStream** stream) {
    (void)builder;
    g.open_calls++;
    if (g.open_result != AAUDIO_OK) {
        return g.open_result;
    }
    if (g.reject_explicit_rate && g.last_rate != AAUDIO_UNSPECIFIED) {
        return AAUDIO_ERROR_INVALID_STATE;
    }
    *stream = FAKE_STREAM;
    return AAUDIO_OK;
}

aaudio_result_t
AAudioStreamBuilder_delete(AAudioStreamBuilder* builder) {
    (void)builder;
    g.delete_calls++;
    return AAUDIO_OK;
}

aaudio_result_t
AAudioStream_requestStart(AAudioStream* stream) {
    (void)stream;
    g.start_calls++;
    return g.start_result;
}

aaudio_result_t
AAudioStream_requestStop(AAudioStream* stream) {
    (void)stream;
    g.stop_calls++;
    return AAUDIO_OK;
}

aaudio_result_t
AAudioStream_close(AAudioStream* stream) {
    (void)stream;
    g.close_calls++;
    return AAUDIO_OK;
}

aaudio_result_t
AAudioStream_write(AAudioStream* stream, const void* buffer, int32_t frames, int64_t timeout_ns) {
    (void)stream;
    (void)timeout_ns;
    g.write_calls++;

    if (g.write_result_pending) {
        g.write_result_pending = 0;
        return g.write_result;
    }

    int32_t take = frames;
    if (g.write_frame_limit > 0 && take > g.write_frame_limit) {
        take = g.write_frame_limit;
    }

    size_t samples = (size_t)take * (size_t)g.device_channels;
    const int16_t* src = buffer;
    for (size_t i = 0; src != NULL && i < samples; i++) {
        size_t slot = g.captured_samples + i;
        if (slot >= FAKE_CAPTURE_SAMPLES) {
            break;
        }
        g.captured[slot] = src[i];
    }
    g.captured_samples += samples;
    g.frames_written += take;
    return take;
}

aaudio_result_t
AAudioStream_read(AAudioStream* stream, void* buffer, int32_t frames, int64_t timeout_ns) {
    (void)stream;
    (void)timeout_ns;
    g.read_calls++;

    if (g.read_result_pending) {
        g.read_result_pending = 0;
        return g.read_result;
    }
    if (buffer != NULL) {
        DSD_MEMSET(buffer, 0, (size_t)frames * (size_t)g.device_channels * sizeof(int16_t));
    }
    return frames;
}

aaudio_result_t
AAudioStream_setBufferSizeInFrames(AAudioStream* stream, int32_t frames) {
    (void)stream;
    g.buffer_size_frames = frames;
    return frames;
}

int32_t
AAudioStream_getSampleRate(AAudioStream* stream) {
    (void)stream;
    return g.device_rate;
}

int32_t
AAudioStream_getChannelCount(AAudioStream* stream) {
    (void)stream;
    return g.device_channels;
}

int32_t
AAudioStream_getFramesPerBurst(AAudioStream* stream) {
    (void)stream;
    return g.frames_per_burst;
}

int32_t
AAudioStream_getBufferCapacityInFrames(AAudioStream* stream) {
    (void)stream;
    return g.buffer_capacity_frames;
}

void
AAudioStreamBuilder_setBufferCapacityInFrames(AAudioStreamBuilder* builder, int32_t frames) {
    (void)builder;
    g.requested_capacity_frames = frames;
}

int32_t
AAudioStream_getXRunCount(AAudioStream* stream) {
    (void)stream;
    return 0;
}

int64_t
AAudioStream_getFramesWritten(AAudioStream* stream) {
    (void)stream;
    if (g.mutex_depth > 0) {
        g.queries_under_mutex++;
    }
    return g.frames_written;
}

int64_t
AAudioStream_getFramesRead(AAudioStream* stream) {
    (void)stream;
    if (g.mutex_depth > 0) {
        g.queries_under_mutex++;
    }
    if (g.frames_read_error) {
        return AAUDIO_ERROR_INVALID_STATE;
    }
    if (g.frames_read_step > 0 && g.frames_read < g.frames_written) {
        g.frames_read = g.frames_written;
    }
    return g.frames_read;
}

/*============================================================================
 * Tests
 *============================================================================*/

static dsd_audio_params
valid_params(int sample_rate, int channels) {
    dsd_audio_params params;
    DSD_MEMSET(&params, 0, sizeof(params));
    params.sample_rate = sample_rate;
    params.channels = channels;
    params.bits_per_sample = 16;
    params.app_name = "dsd-neo-aaudio-test";
    return params;
}

static void
test_size_and_ring_helpers(void) {
    dsd_audio_stream stream;
    int16_t ring[5] = {0};
    int16_t first_write[3] = {1, 2, 3};
    int16_t second_write[4] = {4, 5, 6, 7};
    int16_t out[6] = {0};
    size_t result = 99;

    assert(ms_to_frames(0, 20) == 0);
    assert(ms_to_frames(48000, 0) == 0);
    assert(ms_to_frames(1, 1) == 1);
    assert(ms_to_frames(48000, 20) == 960);
    assert(calc_chunk_frames(0) == 1);
    assert(calc_chunk_frames(8000) == 160);

    assert(size_mul_nonzero(3, 4, &result) == 1 && result == 12);
    assert(size_mul_nonzero(0, 4, &result) == 0 && result == 0);
    assert(size_mul_nonzero(SIZE_MAX, 2, &result) == 0);
    assert(size_mul_nonzero(1, 1, NULL) == 0);

    DSD_MEMSET(&stream, 0, sizeof(stream));
    stream.ring = ring;
    stream.ring_samples_capacity = 5;

    ring_write_samples(&stream, first_write, 3);
    assert(stream.ring_samples_count == 3);
    assert(ring_read_samples(&stream, out, 2) == 2);
    assert(out[0] == 1 && out[1] == 2);

    ring_write_samples(&stream, second_write, 4);
    assert(stream.ring_samples_count == 5);
    DSD_MEMSET(out, 0, sizeof(out));
    assert(ring_read_samples(&stream, out, 5) == 5);
    assert(out[0] == 3 && out[1] == 4 && out[2] == 5 && out[3] == 6 && out[4] == 7);

    ring_write_samples(&stream, second_write, 4);
    ring_drop_oldest(&stream, 99);
    assert(stream.ring_samples_count == 0);
    assert(ring_read_samples(&stream, out, 1) == 0);
}

static void
test_open_output_native_format(void) {
    dsd_audio_params params = valid_params(8000, 2);
    int16_t frames[6] = {10, -10, 20, -20, 30, -30};

    reset_fakes();
    dsd_audio_stream* stream = dsd_audio_open_output(&params);
    assert(stream != NULL);

    assert(g.open_calls == 1);
    assert(g.last_direction == AAUDIO_DIRECTION_OUTPUT);
    assert(g.last_format == AAUDIO_FORMAT_PCM_I16);
    assert(g.last_rate == 8000);
    assert(g.last_channels == 2);
    assert(g.last_performance == AAUDIO_PERFORMANCE_MODE_NONE);
    assert(g.last_usage == AAUDIO_USAGE_MEDIA && g.usage_calls == 1);
    assert(g.delete_calls == 1);
    assert(g.start_calls == 1);
    /* The device buffer covers the pump's target depth, rounded up to whole
     * bursts: 160 ms at 8 kHz is 1280 frames, i.e. 14 bursts of 96. */
    assert(g.requested_capacity_frames == 8000 * DSD_AAUDIO_BUFFER_CAPACITY_MS / 1000);
    assert(g.buffer_size_frames == 14 * g.frames_per_burst);
    assert(g.buffer_size_frames >= 8000 * DSD_AAUDIO_BUFFER_TARGET_MS / 1000);
    assert(g.buffer_size_frames > g.frames_per_burst * DSD_AAUDIO_BUFFER_BURSTS);
    assert(g.buffer_size_frames > g.frames_per_burst * DSD_AAUDIO_BUFFER_BURSTS);
    assert(stream->needs_convert == 0);
    assert(stream->use_async == 0);

    assert(dsd_audio_write(stream, frames, 3) == 3);
    assert(g.write_calls == 1);
    assert(g.captured_samples == 6);
    assert(memcmp(g.captured, frames, sizeof(frames)) == 0);

    /* Partial device writes must be looped over, not reported as short. */
    g.captured_samples = 0;
    g.write_frame_limit = 1;
    assert(dsd_audio_write(stream, frames, 3) == 3);
    assert(g.write_calls == 4);
    assert(g.captured_samples == 6);
    g.write_frame_limit = 0;

    assert(dsd_audio_drain(stream) == 0);
    dsd_audio_close(stream);
    assert(g.stop_calls == 1 && g.close_calls == 1);
}

/* A device that grants less capacity than the target depth must be respected:
 * asking for more than the capacity is an error, not a bigger buffer. */
static void
test_output_buffer_size_clamped_to_capacity(void) {
    dsd_audio_params params = valid_params(8000, 2);

    reset_fakes();
    g.buffer_capacity_frames = 480;
    dsd_audio_stream* stream = dsd_audio_open_output(&params);
    assert(stream != NULL);
    assert(g.buffer_size_frames == 480);
    dsd_audio_close(stream);
}

static void
test_open_output_rate_fallback(void) {
    dsd_audio_params params = valid_params(8000, 2);
    int16_t block[320];

    reset_fakes();
    g.reject_explicit_rate = 1;
    g.device_rate = 48000;

    dsd_audio_stream* stream = dsd_audio_open_output(&params);
    assert(stream != NULL);
    assert(g.open_calls == 2);
    assert(g.last_rate == AAUDIO_UNSPECIFIED);
    assert(stream->needs_convert == 1);
    assert(stream->device_sample_rate == 48000);
    assert(stream->device_channels == 2);

    for (size_t i = 0; i < 320; i++) {
        block[i] = 1000;
    }

    /* 160 logical frames at 8 kHz become ~960 device frames at 48 kHz. */
    assert(dsd_audio_write(stream, block, 160) == 160);
    size_t first = g.captured_samples / 2;
    assert(first >= 959 && first <= 962);
    for (size_t i = 0; i < g.captured_samples; i++) {
        assert(g.captured[i] == 1000);
    }

    /* The Q16 phase carries across calls, so the rate stays honest over time. */
    g.captured_samples = 0;
    assert(dsd_audio_write(stream, block, 160) == 160);
    size_t second = g.captured_samples / 2;
    assert(second >= 959 && second <= 962);
    assert(first + second >= 1919 && first + second <= 1922);

    dsd_audio_close(stream);
}

/*
 * device_drops is a count of *device* samples wherever it is touched, so the two
 * halves of the stats line stay comparable. The drop taken while the device is
 * unavailable used to multiply logical frames by device channels, understating the
 * loss by the resampling ratio in exactly the configuration the fallback exists for.
 */
static void
test_device_drop_accounting_while_converting(void) {
    dsd_audio_params params = valid_params(8000, 2);
    int16_t block[320] = {0};

    reset_fakes();
    g.reject_explicit_rate = 1;
    g.device_rate = 48000;

    dsd_audio_stream* stream = dsd_audio_open_output(&params);
    assert(stream != NULL);
    assert(stream->needs_convert == 1);

    /* Fail a write so the device is torn down, then fail the reopen so the backoff
     * arms and the following write is dropped outright rather than converted. */
    g.write_result_pending = 1;
    g.write_result = AAUDIO_ERROR_DISCONNECTED;
    assert(dsd_audio_write(stream, block, 160) == 160);
    assert(stream->handle == NULL);
    const uint64_t after_write_failure = stream->device_drops;
    /* Whatever the converter had already produced, counted in device samples. */
    assert(after_write_failure >= (uint64_t)959 * 2 && after_write_failure <= (uint64_t)962 * 2);

    g.open_result = AAUDIO_ERROR_INVALID_STATE;
    stream->reopen_debt_frames = 0;
    assert(dsd_audio_write(stream, block, 160) == 160);
    assert(stream->handle == NULL);
    assert(stream->reopen_debt_frames > 0U);

    /* 160 logical frames at 8 kHz are 960 device frames at 48 kHz, and the device
     * is stereo: 1920 device samples, not the 320 the logical count would give. */
    stream->reopen_debt_frames = 0;
    const uint64_t before_backoff_drop = stream->device_drops;
    assert(dsd_audio_write(stream, block, 160) == 160);
    assert(stream->device_drops - before_backoff_drop == (uint64_t)960 * 2);

    dsd_audio_close(stream);
}

static void
test_channel_conversion(void) {
    dsd_audio_params params = valid_params(8000, 1);
    int16_t mono[2] = {100, -100};

    /* Mono source, stereo device: each source frame is duplicated. */
    reset_fakes();
    g.device_channels = 2;
    dsd_audio_stream* stream = dsd_audio_open_output(&params);
    assert(stream != NULL);
    assert(stream->needs_convert == 1);

    assert(dsd_audio_write(stream, mono, 2) == 2);
    assert(g.captured_samples == 4);
    assert(g.captured[0] == g.captured[1]);
    assert(g.captured[2] == g.captured[3]);
    dsd_audio_close(stream);

    /* Stereo source, mono device: the two channels are averaged. */
    params = valid_params(8000, 2);
    reset_fakes();
    g.device_channels = 1;
    stream = dsd_audio_open_output(&params);
    assert(stream != NULL);
    assert(stream->needs_convert == 1);

    int16_t stereo[4] = {1000, 2000, 1000, 2000};
    assert(dsd_audio_write(stream, stereo, 2) == 2);
    assert(g.captured_samples == 2);
    assert(g.captured[0] == 1500);
    dsd_audio_close(stream);
}

static void
test_write_recovery_paths(void) {
    dsd_audio_params params = valid_params(8000, 2);
    int16_t frames[4] = {1, 2, 3, 4};

    /* A route change tears the device stream down without failing the write,
     * and the first such failure recovers on the very next buffer. */
    reset_fakes();
    dsd_audio_stream* stream = dsd_audio_open_output(&params);
    assert(stream != NULL);
    g.write_result_pending = 1;
    g.write_result = AAUDIO_ERROR_DISCONNECTED;
    assert(dsd_audio_write(stream, frames, 2) == 2);
    assert(stream->handle == NULL);
    assert(g.close_calls == 1);
    assert(g.open_calls == 1);
    assert(stream->device_drops == 4);
    assert(stream->reopen_debt_frames == 0);
    assert(stream->recovery_streak == 1);

    assert(dsd_audio_write(stream, frames, 2) == 2);
    assert(g.open_calls == 2);
    assert(stream->handle != NULL);
    assert(g.captured_samples == 4);
    assert(stream->recovery_streak == 0);

    /* Back-to-back failures back off instead of reopening once per buffer. */
    g.write_result_pending = 1;
    g.write_result = AAUDIO_ERROR_DISCONNECTED;
    assert(dsd_audio_write(stream, frames, 2) == 2);
    assert(stream->reopen_debt_frames == 0);

    g.write_result_pending = 1;
    g.write_result = AAUDIO_ERROR_DISCONNECTED;
    assert(dsd_audio_write(stream, frames, 2) == 2);
    assert(g.open_calls == 3);
    assert(stream->handle == NULL);
    assert(stream->reopen_debt_frames == 4000); /* 500 ms at 8 kHz */

    /* Audio written during the backoff is dropped, not retried per buffer. */
    assert(dsd_audio_write(stream, frames, 2) == 2);
    assert(g.open_calls == 3);
    assert(stream->reopen_debt_frames == 3998);
    dsd_audio_close(stream);

    /* A write that times out with no progress recycles the stream too. */
    reset_fakes();
    stream = dsd_audio_open_output(&params);
    assert(stream != NULL);
    g.write_result_pending = 1;
    g.write_result = AAUDIO_OK;
    assert(dsd_audio_write(stream, frames, 2) == 2);
    assert(stream->handle == NULL);
    assert(strcmp(dsd_audio_get_error(), "AAUDIO_ERROR_TIMEOUT") == 0);
    assert(stream->device_drops == 4);
    dsd_audio_close(stream);

    /* A failed reopen re-arms the backoff and still never fails the caller. */
    reset_fakes();
    stream = dsd_audio_open_output(&params);
    assert(stream != NULL);
    g.write_result_pending = 1;
    g.write_result = AAUDIO_ERROR_INTERNAL;
    assert(dsd_audio_write(stream, frames, 2) == 2);
    assert(strcmp(dsd_audio_get_error(), "AAUDIO_ERROR_INTERNAL") == 0);

    stream->reopen_debt_frames = 0;
    g.open_result = AAUDIO_ERROR_INVALID_STATE;
    assert(dsd_audio_write(stream, frames, 2) == 2);
    assert(stream->handle == NULL);
    assert(stream->reopen_debt_frames == 4000);

    g.open_result = AAUDIO_OK;
    stream->reopen_debt_frames = 0;
    assert(dsd_audio_write(stream, frames, 2) == 2);
    assert(stream->handle != NULL);
    dsd_audio_close(stream);
}

static void
test_open_failure_paths(void) {
    dsd_audio_params params = valid_params(8000, 2);

    /* Both open attempts fail. */
    reset_fakes();
    g.open_result = AAUDIO_ERROR_INVALID_STATE;
    assert(dsd_audio_open_output(&params) == NULL);
    assert(g.open_calls == 2);
    assert(strcmp(dsd_audio_get_error(), "AAUDIO_ERROR_INVALID_STATE") == 0);

    /* Builder allocation failure. */
    reset_fakes();
    g.builder_result = AAUDIO_ERROR_NO_MEMORY;
    assert(dsd_audio_open_output(&params) == NULL);
    assert(g.open_calls == 0);

    /* Start failure closes the freshly opened stream. */
    reset_fakes();
    g.start_result = AAUDIO_ERROR_INVALID_STATE;
    assert(dsd_audio_open_output(&params) == NULL);
    assert(g.close_calls == 1);

    /* A nonsense granted format is rejected. */
    reset_fakes();
    g.device_rate = 0;
    assert(dsd_audio_open_output(&params) == NULL);
    assert(g.close_calls == 1);
    assert(strcmp(dsd_audio_get_error(), "AAudio reported an invalid stream format") == 0);

    /* Conversion only covers mono/stereo. */
    reset_fakes();
    g.device_channels = 4;
    assert(dsd_audio_open_output(&params) == NULL);
    assert(strcmp(dsd_audio_get_error(), "AAudio granted an unusable rate/channel combination") == 0);
}

static void
test_input_stream(void) {
    dsd_audio_params params = valid_params(48000, 1);
    int16_t buffer[8] = {0};

    reset_fakes();
    g.device_rate = 48000;
    g.device_channels = 1;
    dsd_audio_stream* stream = dsd_audio_open_input(&params);
    assert(stream != NULL);
    assert(g.last_direction == AAUDIO_DIRECTION_INPUT);
    assert(g.usage_calls == 0);
    assert(g.buffer_size_frames == 0); /* input keeps the platform default */

    assert(dsd_audio_read(stream, buffer, 8) == 8);
    assert(g.read_calls == 1);

    assert(dsd_audio_write(stream, buffer, 8) == -1);
    assert(strcmp(dsd_audio_get_error(), "Cannot write to input stream") == 0);
    assert(dsd_audio_drain(stream) == 0);

    g.read_result_pending = 1;
    g.read_result = AAUDIO_ERROR_INTERNAL;
    assert(dsd_audio_read(stream, buffer, 8) == -1);
    assert(strcmp(dsd_audio_get_error(), "AAUDIO_ERROR_INTERNAL") == 0);

    /* A read timeout is reported rather than spun on. */
    g.read_result_pending = 1;
    g.read_result = AAUDIO_OK;
    assert(dsd_audio_read(stream, buffer, 8) == -1);
    assert(strcmp(dsd_audio_get_error(), "AAUDIO_ERROR_TIMEOUT") == 0);
    dsd_audio_close(stream);

    /* Input is never resampled: a mismatched grant is a clean failure. */
    reset_fakes();
    g.device_rate = 44100;
    g.device_channels = 1;
    assert(dsd_audio_open_input(&params) == NULL);
    assert(g.open_calls == 1); /* no unspecified-rate retry for capture */
    assert(strcmp(dsd_audio_get_error(), "AAudio granted an unusable rate/channel combination") == 0);
}

static void
test_drain_paths(void) {
    dsd_audio_params params = valid_params(8000, 2);
    int16_t frames[4] = {1, 2, 3, 4};

    reset_fakes();
    dsd_audio_stream* stream = dsd_audio_open_output(&params);
    assert(stream != NULL);
    assert(dsd_audio_write(stream, frames, 2) == 2);
    assert(dsd_audio_drain(stream) == 0);

    /* The device never catches up: drain gives up instead of hanging. */
    g.frames_read_step = 0;
    assert(dsd_audio_write(stream, frames, 2) == 2);
    assert(dsd_audio_drain(stream) == -1);
    assert(strcmp(dsd_audio_get_error(), "AAudio output drain timed out") == 0);

    /* A device-side error during drain is surfaced. */
    g.frames_read_error = 1;
    assert(dsd_audio_drain(stream) == -1);
    assert(strcmp(dsd_audio_get_error(), "AAUDIO_ERROR_INVALID_STATE") == 0);
    g.frames_read_error = 0;

    dsd_audio_close(stream);
    assert(dsd_audio_drain(NULL) == -1);
}

static void
test_async_output_setup_and_pump(void) {
    dsd_audio_params params = valid_params(8000, 2);
    int16_t frames[8] = {1, 2, 3, 4, 5, 6, 7, 8};

    reset_fakes();
    params.async_output = 1;
    dsd_audio_stream* stream = dsd_audio_open_output(&params);
    assert(stream != NULL);
    assert(stream->use_async == 1);
    assert(g.thread_create_calls == 1);
    assert(stream->chunk_frames == 160);
    assert(stream->chunk_samples == 320);
    assert(stream->ring != NULL && stream->ring_samples_capacity >= 2560);

    /* Async writes queue into the ring; the pump owns the device. */
    assert(dsd_audio_write(stream, frames, 4) == 4);
    assert(stream->ring_samples_count == 8);
    assert(g.write_calls == 0);

    /* Pump body: a short ring conceals the tail and still writes a full chunk. */
    aaudio_output_prepare_chunk_locked(stream, 0);
    assert(stream->ring_samples_count == 0);
    assert(stream->underruns == 1);
    assert(aaudio_output_write_chunk(stream) == 0);
    assert(g.write_calls == 1);
    assert(g.captured_samples == stream->chunk_samples);

    /* A device error recycles the stream; the pump keeps running. */
    g.write_result_pending = 1;
    g.write_result = AAUDIO_ERROR_INTERNAL;
    assert(aaudio_output_write_chunk(stream) == 0);
    assert(stream->stop == 0);
    assert(stream->handle == NULL);
    assert(stream->device_drops == stream->chunk_samples);

    /* wait_for_work reports a stop request to the pump loop. */
    stream->stop = 1;
    assert(aaudio_output_wait_for_work_locked(stream) == -1);
    stream->stop = 0;
    ring_write_samples(stream, frames, 8);
    assert(aaudio_output_wait_for_work_locked(stream) == 0);

    stream->stop = 1;
    dsd_audio_close(stream);
    assert(g.thread_join_calls == 1);
}

/*
 * An empty ring is not a starvation while the device still holds audio. The pump
 * must wait for real audio instead of queueing concealment ahead of it, and only
 * conceal once the device is down to DSD_AAUDIO_CONCEAL_LOW_WATER_MS.
 */
static void
test_pump_waits_while_device_is_buffered(void) {
    dsd_audio_params params = valid_params(8000, 2);

    reset_fakes();
    params.async_output = 1;
    dsd_audio_stream* stream = dsd_audio_open_output(&params);
    assert(stream != NULL);

    /* Something has played, so concealment is available. */
    stream->conceal_inited = 1;
    stream->conceal_has_good = 1;
    stream->priming = 0;
    assert(stream->ring_samples_count == 0);

    /* Device holds 1000 ms; each wait plays one 20 ms chunk. */
    g.frames_read_step = 0;
    g.frames_written = 8000;
    g.frames_read = 0;
    g.frames_played_per_wait = 160;
    g.cond_timedwait_calls = 0;
    g.queries_under_mutex = 0;

    /* Entered the way the pump enters it, so the mutex bookkeeping is faithful. */
    dsd_mutex_lock(&stream->mu);
    assert(aaudio_output_wait_for_work_locked(stream) == 1);
    dsd_mutex_unlock(&stream->mu);
    assert(g.mutex_depth == 0);

    /* It concealed only once the queue fell under the low-water mark (80 ms =
     * 640 frames), i.e. after roughly (8000 - 640) / 160 waits -- not on the
     * first timeout the way an empty ring alone would have triggered. */
    assert(g.cond_timedwait_calls > 40);
    assert(g.frames_written - g.frames_read < 640);

    /* Every one of those queue reads dropped the ring mutex first: holding it
     * across an AAudio call puts AAudio's lock on the decoder's write path. */
    assert(g.queries_under_mutex == 0);

    /* A gap must not put the pump back into priming: on a real-time feed that
     * withholds output from a device that is already low. */
    assert(stream->priming == 0);

    stream->stop = 1;
    dsd_audio_close(stream);
}

/* With the device already dry, concealment is the right answer immediately. */
static void
test_pump_conceals_when_device_is_dry(void) {
    dsd_audio_params params = valid_params(8000, 2);

    reset_fakes();
    params.async_output = 1;
    dsd_audio_stream* stream = dsd_audio_open_output(&params);
    assert(stream != NULL);

    stream->conceal_inited = 1;
    stream->conceal_has_good = 1;
    stream->priming = 0;

    g.frames_read_step = 0;
    g.frames_written = 4096;
    g.frames_read = 4096; /* nothing queued */
    g.frames_played_per_wait = 0;
    g.cond_timedwait_calls = 0;
    g.queries_under_mutex = 0;

    dsd_mutex_lock(&stream->mu);
    assert(aaudio_output_wait_for_work_locked(stream) == 1);
    dsd_mutex_unlock(&stream->mu);

    assert(g.cond_timedwait_calls == 1);
    assert(g.mutex_depth == 0);
    assert(g.queries_under_mutex == 0);

    stream->stop = 1;
    dsd_audio_close(stream);
}

/*
 * Overflowing the ring drops both the audio already queued and the head of the
 * incoming buffer that will not fit. Counting only the former understated the
 * loss in exactly the case the counter exists to expose.
 */
static void
test_ring_overflow_drop_accounting(void) {
    dsd_audio_params params = valid_params(8000, 2);
    int16_t seed[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    static int16_t oversized[20000];

    reset_fakes();
    params.async_output = 1;
    dsd_audio_stream* stream = dsd_audio_open_output(&params);
    assert(stream != NULL);

    const size_t capacity = stream->ring_samples_capacity;
    assert(capacity > 0U && capacity < sizeof oversized / sizeof oversized[0]);

    /* Queue a little first, so the pre-existing ring content is also a drop. */
    assert(dsd_audio_write(stream, seed, 4) == 4);
    assert(stream->ring_samples_count == 8U);
    assert(stream->drops == 0U);

    /* One write strictly larger than the ring: only the newest window survives. */
    const size_t frames = (capacity / 2U) + 100U;
    const size_t samples = frames * 2U;
    for (size_t i = 0; i < samples; i++) {
        oversized[i] = (int16_t)((i & 0x7FFFU) | 1U);
    }
    assert(dsd_audio_write(stream, oversized, frames) == (int)frames);

    assert(stream->ring_samples_count == capacity);
    assert(stream->drops == 8U + (samples - capacity));
    /* The tail of the buffer is what was kept, not the head. */
    assert(stream->ring[(stream->ring_samples_tail + capacity - 1U) % capacity] == oversized[samples - 1U]);

    /* A write into a full ring drops exactly the shortfall, as before. */
    const uint64_t drops_before = stream->drops;
    assert(dsd_audio_write(stream, seed, 4) == 4);
    assert(stream->ring_samples_count == capacity);
    assert(stream->drops == drops_before + 8U);

    /* in_samples counts what the decoder handed over, overflow included. */
    assert(stream->in_samples == 8U + samples + 8U);

    stream->stop = 1;
    dsd_audio_close(stream);
}

/* Concealment landing while the decoder is still producing is the audible
 * failure, and is counted apart from ordinary idle-time padding. */
static void
test_underrun_classification(void) {
    dsd_audio_params params = valid_params(8000, 2);
    int16_t frames[8] = {1, 2, 3, 4, 5, 6, 7, 8};

    reset_fakes();
    params.async_output = 1;
    dsd_audio_stream* stream = dsd_audio_open_output(&params);
    assert(stream != NULL);

    /* Idle: no decoder write has ever happened, so nothing is mid-speech. */
    aaudio_output_prepare_chunk_locked(stream, 1);
    assert(stream->underruns == 1);
    assert(stream->underruns_partial == 0);
    assert(stream->underruns_midspeech == 0);

    /* A partial chunk is a fragment of real audio padded with concealment. */
    assert(dsd_audio_write(stream, frames, 4) == 4);
    assert(stream->in_samples == 8);
    aaudio_output_prepare_chunk_locked(stream, 0);
    assert(stream->underruns == 2);
    assert(stream->underruns_partial == 1);
    /* The write above was just now, so this one landed inside speech. */
    assert(stream->underruns_midspeech == 1);

    stream->stop = 1;
    dsd_audio_close(stream);
}

static void
test_api_guards(void) {
    dsd_audio_params params = valid_params(8000, 2);
    dsd_audio_device inputs[4];
    dsd_audio_device outputs[4];

    reset_fakes();
    assert(strcmp(dsd_audio_backend_name(), "aaudio") == 0);
    assert(dsd_audio_init() == 0);
    assert(dsd_audio_init() == 0);
    dsd_audio_cleanup();
    assert(dsd_audio_init() == 0);

    assert(dsd_audio_open_output(NULL) == NULL);
    assert(strcmp(dsd_audio_get_error(), "NULL parameters") == 0);

    params.sample_rate = 0;
    assert(dsd_audio_open_output(&params) == NULL);
    assert(strcmp(dsd_audio_get_error(), "Invalid sample rate") == 0);

    params = valid_params(8000, 0);
    assert(dsd_audio_open_input(&params) == NULL);
    assert(strcmp(dsd_audio_get_error(), "Invalid channel count") == 0);

    assert(dsd_audio_read(NULL, NULL, 4) == -1);
    assert(dsd_audio_write(NULL, NULL, 4) == -1);
    dsd_audio_close(NULL);

    assert(dsd_audio_enumerate_devices(inputs, outputs, 0) == -1);
    assert(dsd_audio_enumerate_devices(inputs, outputs, 4) == 0);
    assert(inputs[0].initialized == 1 && inputs[0].is_input == 1);
    assert(outputs[0].initialized == 1 && outputs[0].is_output == 1);
    assert(inputs[1].initialized == 0);
    assert(dsd_audio_list_devices() == 0);

    /* Reading an output stream (and writing an input one) is refused. */
    reset_fakes();
    dsd_audio_params output_params = valid_params(8000, 2);
    dsd_audio_stream* stream = dsd_audio_open_output(&output_params);
    assert(stream != NULL);
    int16_t buffer[4] = {0};
    assert(dsd_audio_read(stream, buffer, 2) == -1);
    assert(strcmp(dsd_audio_get_error(), "Cannot read from output stream") == 0);
    dsd_audio_close(stream);
}

int
main(void) {
    test_size_and_ring_helpers();
    test_open_output_native_format();
    test_output_buffer_size_clamped_to_capacity();
    test_open_output_rate_fallback();
    test_channel_conversion();
    test_write_recovery_paths();
    test_open_failure_paths();
    test_input_stream();
    test_drain_paths();
    test_async_output_setup_and_pump();
    test_pump_waits_while_device_is_buffered();
    test_pump_conceals_when_device_is_dry();
    test_device_drop_accounting_while_converting();
    test_ring_overflow_drop_accounting();
    test_underrun_classification();
    test_api_guards();
    return 0;
}

// NOLINTEND(bugprone-suspicious-include)
