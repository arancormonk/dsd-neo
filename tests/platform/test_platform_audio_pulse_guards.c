// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/platform/audio.h>

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

_Static_assert(offsetof(dsd_audio_params, async_output) > offsetof(dsd_audio_params, app_name),
               "async_output must follow existing positional initializer fields");

static dsd_audio_params
valid_params(void) {
    dsd_audio_params params;
    (void)DSD_MEMSET(&params, 0, sizeof(params));
    params.sample_rate = 48000;
    params.channels = 1;
    params.bits_per_sample = 16;
    params.app_name = "dsd-neo-test";
    return params;
}

static void
expect_error(const char* expected) {
    const char* actual = dsd_audio_get_error();
    assert(actual != NULL);
    assert(strcmp(actual, expected) == 0);
}

static void
expect_input_rejects(const dsd_audio_params* params, const char* expected_error) {
    assert(dsd_audio_open_input(params) == NULL);
    expect_error(expected_error);
}

static void
expect_output_rejects(const dsd_audio_params* params, const char* expected_error) {
    assert(dsd_audio_open_output(params) == NULL);
    expect_error(expected_error);
}

static void
test_backend_lifecycle_and_error_reset(void) {
    dsd_audio_cleanup();

    assert(strcmp(dsd_audio_backend_name(), "pulse") == 0);
    assert(dsd_audio_init() == 0);
    expect_error("");

    assert(dsd_audio_init() == 0);
    expect_error("");

    dsd_audio_cleanup();
}

static void
test_open_parameter_guards(void) {
    dsd_audio_params params = valid_params();

    expect_input_rejects(NULL, "NULL parameters");
    expect_output_rejects(NULL, "NULL parameters");

    params.sample_rate = 0;
    expect_input_rejects(&params, "Invalid sample rate");
    expect_output_rejects(&params, "Invalid sample rate");

    params = valid_params();
    params.sample_rate = -48000;
    expect_input_rejects(&params, "Invalid sample rate");
    expect_output_rejects(&params, "Invalid sample rate");

    params = valid_params();
    params.channels = 0;
    expect_input_rejects(&params, "Invalid channel count");
    expect_output_rejects(&params, "Invalid channel count");

    params = valid_params();
    params.channels = 256;
    expect_input_rejects(&params, "Invalid channel count");
    expect_output_rejects(&params, "Invalid channel count");
}

static void
test_stream_operation_guards(void) {
    int16_t sample = 0;

    assert(dsd_audio_read(NULL, &sample, 1) == -1);
    expect_error("Invalid arguments");

    assert(dsd_audio_write(NULL, &sample, 1) == -1);
    expect_error("Invalid arguments");

    assert(dsd_audio_drain(NULL) == -1);
    dsd_audio_close(NULL);
}

static void
test_audio_params_preserves_positional_initializer_prefix(void) {
    dsd_audio_params params = {48000, 1, 16, "pulse-dev", "dsd-neo-positional-test", 0};

    assert(params.sample_rate == 48000);
    assert(params.channels == 1);
    assert(params.bits_per_sample == 16);
    assert(strcmp(params.device, "pulse-dev") == 0);
    assert(strcmp(params.app_name, "dsd-neo-positional-test") == 0);
    assert(params.async_output == 0);
}

int
main(void) {
    test_backend_lifecycle_and_error_reset();
    test_open_parameter_guards();
    test_stream_operation_guards();
    test_audio_params_preserves_positional_initializer_prefix();
    dsd_audio_cleanup();
    return 0;
}
