// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Host stand-in for the NDK's <aaudio/AAudio.h>.
 *
 * Declares only the surface src/platform/audio_aaudio.c uses so the AAudio
 * backend compiles on the build host and can be exercised against fakes defined
 * by the test. Enumerator values mirror the NDK header wherever the backend
 * compares against them.
 */

#ifndef DSD_NEO_TESTS_PLATFORM_AAUDIO_STUB_AAUDIO_H
#define DSD_NEO_TESTS_PLATFORM_AAUDIO_STUB_AAUDIO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AAUDIO_UNSPECIFIED 0

enum { AAUDIO_DIRECTION_OUTPUT, AAUDIO_DIRECTION_INPUT };

typedef int32_t aaudio_direction_t;

enum { AAUDIO_FORMAT_INVALID = -1, AAUDIO_FORMAT_UNSPECIFIED = 0, AAUDIO_FORMAT_PCM_I16 };

typedef int32_t aaudio_format_t;

/* Every enumerator is spelled out because the NDK list leaves reserved gaps; deriving values from
   the previous member would silently shift them if a member is ever added or removed here. */
enum {
    AAUDIO_OK = 0,
    AAUDIO_ERROR_BASE = -900,
    AAUDIO_ERROR_DISCONNECTED = -899,
    AAUDIO_ERROR_ILLEGAL_ARGUMENT = -898,
    /* -897 reserved */
    AAUDIO_ERROR_INTERNAL = -896,
    AAUDIO_ERROR_INVALID_STATE = -895,
    /* -894, -893 reserved */
    AAUDIO_ERROR_INVALID_HANDLE = -892,
    /* -891 reserved */
    AAUDIO_ERROR_UNIMPLEMENTED = -890,
    AAUDIO_ERROR_UNAVAILABLE = -889,
    AAUDIO_ERROR_NO_FREE_HANDLES = -888,
    AAUDIO_ERROR_NO_MEMORY = -887,
    AAUDIO_ERROR_NULL = -886,
    AAUDIO_ERROR_TIMEOUT = -885
};

typedef int32_t aaudio_result_t;

enum { AAUDIO_PERFORMANCE_MODE_NONE = 10, AAUDIO_PERFORMANCE_MODE_POWER_SAVING, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY };

typedef int32_t aaudio_performance_mode_t;

enum { AAUDIO_USAGE_MEDIA = 1 };

typedef int32_t aaudio_usage_t;

typedef struct AAudioStreamStruct AAudioStream;
typedef struct AAudioStreamBuilderStruct AAudioStreamBuilder;

const char* AAudio_convertResultToText(aaudio_result_t result);
aaudio_result_t AAudio_createStreamBuilder(AAudioStreamBuilder** builder);

void AAudioStreamBuilder_setDirection(AAudioStreamBuilder* builder, aaudio_direction_t direction);
void AAudioStreamBuilder_setFormat(AAudioStreamBuilder* builder, aaudio_format_t format);
void AAudioStreamBuilder_setChannelCount(AAudioStreamBuilder* builder, int32_t channel_count);
void AAudioStreamBuilder_setSampleRate(AAudioStreamBuilder* builder, int32_t sample_rate);
void AAudioStreamBuilder_setPerformanceMode(AAudioStreamBuilder* builder, aaudio_performance_mode_t mode);
void AAudioStreamBuilder_setUsage(AAudioStreamBuilder* builder, aaudio_usage_t usage);
void AAudioStreamBuilder_setBufferCapacityInFrames(AAudioStreamBuilder* builder, int32_t frames);
aaudio_result_t AAudioStreamBuilder_openStream(AAudioStreamBuilder* builder, AAudioStream** stream);
aaudio_result_t AAudioStreamBuilder_delete(AAudioStreamBuilder* builder);

aaudio_result_t AAudioStream_requestStart(AAudioStream* stream);
aaudio_result_t AAudioStream_requestStop(AAudioStream* stream);
aaudio_result_t AAudioStream_close(AAudioStream* stream);
aaudio_result_t AAudioStream_write(AAudioStream* stream, const void* buffer, int32_t frames, int64_t timeout_ns);
aaudio_result_t AAudioStream_read(AAudioStream* stream, void* buffer, int32_t frames, int64_t timeout_ns);
aaudio_result_t AAudioStream_setBufferSizeInFrames(AAudioStream* stream, int32_t frames);
int32_t AAudioStream_getSampleRate(AAudioStream* stream);
int32_t AAudioStream_getChannelCount(AAudioStream* stream);
int32_t AAudioStream_getFramesPerBurst(AAudioStream* stream);
int32_t AAudioStream_getBufferCapacityInFrames(AAudioStream* stream);
int32_t AAudioStream_getXRunCount(AAudioStream* stream);
int64_t AAudioStream_getFramesWritten(AAudioStream* stream);
int64_t AAudioStream_getFramesRead(AAudioStream* stream);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_TESTS_PLATFORM_AAUDIO_STUB_AAUDIO_H */
