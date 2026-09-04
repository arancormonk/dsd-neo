// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Exercises the whole 12-function audio surface against the null backend.
 *
 * Registered only when -DDSD_AUDIO_BACKEND=none is selected.
 */

#include <dsd-neo/platform/audio.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "dsd-neo/core/safe_api.h"

static int
expect_int(const char* label, int got, int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "FAIL: %s: got=%d want=%d\n", label, got, want);
        return 1;
    }
    return 0;
}

static int
expect_true(const char* label, int cond) {
    if (!cond) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", label);
        return 1;
    }
    return 0;
}

static dsd_audio_params
make_params(void) {
    dsd_audio_params params;
    DSD_MEMSET(&params, 0, sizeof params);
    params.sample_rate = 8000;
    params.channels = 2;
    params.bits_per_sample = 16;
    params.device = NULL;
    params.app_name = "dsd-neo-test";
    params.async_output = 0;
    return params;
}

static int
test_backend_identity(void) {
    int rc = 0;
    const char* name = dsd_audio_backend_name();
    rc |= expect_true("backend name is non-NULL", name != NULL);
    rc |= expect_true("backend name is \"null\"", name && strcmp(name, "null") == 0);
    rc |= expect_true("error string is non-NULL", dsd_audio_get_error() != NULL);
    return rc;
}

static int
test_enumerate(void) {
    int rc = 0;
    dsd_audio_device inputs[4];
    dsd_audio_device outputs[4];

    rc |= expect_int("enumerate rejects zero-size arrays", dsd_audio_enumerate_devices(inputs, outputs, 0), -1);
    rc |= expect_int("enumerate succeeds", dsd_audio_enumerate_devices(inputs, outputs, 4), 0);
    rc |= expect_true("one output device reported", outputs[0].initialized && outputs[0].is_output);
    rc |= expect_true("output device is named null", strcmp(outputs[0].name, "null") == 0);
    rc |= expect_true("one input device reported", inputs[0].initialized && inputs[0].is_input);
    rc |= expect_true("input device is named null", strcmp(inputs[0].name, "null") == 0);
    rc |= expect_true("trailing slots stay cleared", outputs[1].initialized == 0 && inputs[1].initialized == 0);
    rc |= expect_int("enumerate tolerates NULL arrays", dsd_audio_enumerate_devices(NULL, NULL, 4), 0);
    rc |= expect_int("list_devices succeeds", dsd_audio_list_devices(), 0);
    return rc;
}

static int
test_open_rejects_bad_params(void) {
    int rc = 0;
    dsd_audio_params params = make_params();

    rc |= expect_true("NULL params rejected", dsd_audio_open_output(NULL) == NULL);

    params.sample_rate = 0;
    rc |= expect_true("zero sample rate rejected", dsd_audio_open_output(&params) == NULL);

    params = make_params();
    params.channels = 0;
    rc |= expect_true("zero channel count rejected", dsd_audio_open_input(&params) == NULL);
    return rc;
}

static int
test_output_lifecycle(void) {
    int rc = 0;
    dsd_audio_params params = make_params();
    int16_t frames[64 * 2];
    DSD_MEMSET(frames, 0, sizeof frames);

    dsd_audio_stream* stream = dsd_audio_open_output(&params);
    if (!stream) {
        DSD_FPRINTF(stderr, "FAIL: output open failed: %s\n", dsd_audio_get_error());
        return 1;
    }

    rc |= expect_int("write accepts a full buffer", dsd_audio_write(stream, frames, 64), 64);
    rc |= expect_int("write accepts zero frames", dsd_audio_write(stream, frames, 0), 0);
    rc |= expect_int("write rejects a NULL buffer", dsd_audio_write(stream, NULL, 64), -1);
    rc |= expect_int("read rejects an output stream", dsd_audio_read(stream, frames, 64), -1);
    rc |= expect_int("drain succeeds", dsd_audio_drain(stream), 0);

    dsd_audio_close(stream);
    return rc;
}

static int
test_input_lifecycle(void) {
    int rc = 0;
    dsd_audio_params params = make_params();
    int16_t frames[32 * 2];

    dsd_audio_stream* stream = dsd_audio_open_input(&params);
    if (!stream) {
        DSD_FPRINTF(stderr, "FAIL: input open failed: %s\n", dsd_audio_get_error());
        return 1;
    }

    DSD_MEMSET(frames, 0x5A, sizeof frames);
    rc |= expect_int("read returns the requested frames", dsd_audio_read(stream, frames, 32), 32);

    int silent = 1;
    for (size_t i = 0; i < sizeof frames / sizeof frames[0]; i++) {
        if (frames[i] != 0) {
            silent = 0;
            break;
        }
    }
    rc |= expect_true("read yields silence", silent);

    rc |= expect_int("read accepts zero frames", dsd_audio_read(stream, frames, 0), 0);
    rc |= expect_int("read rejects a NULL buffer", dsd_audio_read(stream, NULL, 32), -1);
    rc |= expect_int("write rejects an input stream", dsd_audio_write(stream, frames, 32), -1);
    rc |= expect_int("drain is a no-op on input", dsd_audio_drain(stream), 0);

    dsd_audio_close(stream);
    return rc;
}

static int
test_null_stream_guards(void) {
    int rc = 0;
    int16_t frames[8];
    DSD_MEMSET(frames, 0, sizeof frames);

    rc |= expect_int("read rejects a NULL stream", dsd_audio_read(NULL, frames, 4), -1);
    rc |= expect_int("write rejects a NULL stream", dsd_audio_write(NULL, frames, 4), -1);
    rc |= expect_int("drain rejects a NULL stream", dsd_audio_drain(NULL), -1);
    dsd_audio_close(NULL);
    return rc;
}

int
main(void) {
    int rc = 0;

    rc |= expect_int("init succeeds", dsd_audio_init(), 0);
    rc |= expect_int("init is idempotent", dsd_audio_init(), 0);

    rc |= test_backend_identity();
    rc |= test_enumerate();
    rc |= test_open_rejects_bad_params();
    rc |= test_output_lifecycle();
    rc |= test_input_lifecycle();
    rc |= test_null_stream_guards();

    dsd_audio_cleanup();
    rc |= expect_int("init succeeds after cleanup", dsd_audio_init(), 0);
    dsd_audio_cleanup();

    if (rc == 0) {
        printf("PLATFORM_AUDIO_NULL: OK\n");
    }
    return rc;
}
