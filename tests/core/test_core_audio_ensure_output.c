// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * A session that changes receive family at runtime needs the sink the new family
 * writes to. dsd_audio_ensure_analog_output() and dsd_audio_ensure_digital_output()
 * open what openAudioOutput() would have opened for the current options, with the
 * same parameters, and are idempotent: a sink that is already open is left alone,
 * and nothing is opened for a muted session or a non-device output.
 *
 * The device layer is replaced at link time with a recording null backend.
 */

#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/platform/audio.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef DSD_NEO_TEST_AUDIO_WRAP

typedef struct {
    int sample_rate;
    int channels;
    int bits_per_sample;
    int async_output;
} opened_params;

static uintptr_t g_streams[8];
static opened_params g_opened[8];
static int g_open_count;
static int g_close_count;
static int g_fail_opens;

// GNU ld --wrap requires these exact external symbol names.
// NOLINTBEGIN(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,misc-use-internal-linkage)
dsd_audio_stream* __wrap_dsd_audio_open_output(const dsd_audio_params* params);
void __wrap_dsd_audio_close(dsd_audio_stream* stream);

dsd_audio_stream*
__wrap_dsd_audio_open_output(const dsd_audio_params* params) {
    if (g_fail_opens) {
        return NULL;
    }
    const int index = g_open_count < 8 ? g_open_count : 7;
    g_opened[index].sample_rate = params ? params->sample_rate : 0;
    g_opened[index].channels = params ? params->channels : 0;
    g_opened[index].bits_per_sample = params ? params->bits_per_sample : 0;
    g_opened[index].async_output = params ? params->async_output : 0;
    g_open_count++;
    return (dsd_audio_stream*)&g_streams[index];
}

void
__wrap_dsd_audio_close(dsd_audio_stream* stream) {
    (void)stream;
    g_close_count++;
}

// NOLINTEND(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,misc-use-internal-linkage)

static int g_failures;

static void
expect_int(const char* label, int got, int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "FAIL: %s: got %d want %d\n", label, got, want);
        g_failures++;
    }
}

static void
reset_backend(void) {
    DSD_MEMSET(g_opened, 0, sizeof g_opened);
    g_open_count = 0;
    g_close_count = 0;
    g_fail_opens = 0;
}

static void
device_session(dsd_opts* opts) {
    DSD_MEMSET(opts, 0, sizeof(*opts));
    opts->audio_out = 1;
    opts->audio_out_type = 0;
    opts->pulse_raw_rate_out = 48000;
    opts->pulse_raw_out_channels = 1;
    opts->pulse_digi_rate_out = 8000;
    opts->pulse_digi_out_channels = 2;
}

static void
test_analog_sink(void) {
    static dsd_opts opts;
    device_session(&opts);
    reset_backend();
    expect_int("analog ensure opens the raw sink", dsd_audio_ensure_analog_output(&opts), 0);
    expect_int("one open", g_open_count, 1);
    expect_int("raw sink kept", opts.audio_raw_out != NULL, 1);
    expect_int("raw rate", g_opened[0].sample_rate, 48000);
    expect_int("raw channels", g_opened[0].channels, 1);
    expect_int("raw sample width", g_opened[0].bits_per_sample, 16);
    expect_int("digital sink left closed", opts.audio_out_stream == NULL, 1);

    expect_int("second analog ensure", dsd_audio_ensure_analog_output(&opts), 0);
    expect_int("idempotent", g_open_count, 1);
    expect_int("nothing closed", g_close_count, 0);
}

static void
test_digital_sink(void) {
    static dsd_opts opts;
    device_session(&opts);
    reset_backend();
    expect_int("digital ensure", dsd_audio_ensure_digital_output(&opts), 0);
    expect_int("digital sink opened once", g_open_count, 1);
    expect_int("digital rate", g_opened[0].sample_rate, 8000);
    expect_int("digital channels", g_opened[0].channels, 2);
    expect_int("raw sink not needed", opts.audio_raw_out == NULL, 1);
    expect_int("second digital ensure", dsd_audio_ensure_digital_output(&opts), 0);
    expect_int("digital idempotent", g_open_count, 1);

    /* ProVoice and the -8 source monitor also write the raw sink, as openAudioOutput() opens it for them. */
    opts.frame_provoice = 1;
    expect_int("provoice ensure", dsd_audio_ensure_digital_output(&opts), 0);
    expect_int("provoice adds the raw sink", g_open_count, 2);
    expect_int("provoice raw rate", g_opened[1].sample_rate, 48000);
    expect_int("provoice idempotent", dsd_audio_ensure_digital_output(&opts), 0);
    expect_int("provoice opens once", g_open_count, 2);
}

static void
test_nothing_to_open(void) {
    static dsd_opts opts;
    device_session(&opts);
    reset_backend();
    opts.audio_out = 0; /* muted: unmuting reopens every sink the mode needs */
    expect_int("muted analog", dsd_audio_ensure_analog_output(&opts), 0);
    expect_int("muted digital", dsd_audio_ensure_digital_output(&opts), 0);
    opts.audio_out = 1;
    opts.audio_out_type = 9; /* -o null */
    expect_int("null analog", dsd_audio_ensure_analog_output(&opts), 0);
    opts.audio_out_type = 8; /* UDP */
    expect_int("udp digital", dsd_audio_ensure_digital_output(&opts), 0);
    expect_int("no device opened", g_open_count, 0);
    expect_int("null opts", dsd_audio_ensure_analog_output(NULL), -1);
    expect_int("null opts digital", dsd_audio_ensure_digital_output(NULL), -1);
}

static void
test_open_failure(void) {
    static dsd_opts opts;
    device_session(&opts);
    reset_backend();
    g_fail_opens = 1;
    expect_int("failed analog ensure reports", dsd_audio_ensure_analog_output(&opts), -1);
    expect_int("failed ensure leaves no sink", opts.audio_raw_out == NULL, 1);
    expect_int("failed analog ensure again", dsd_audio_ensure_analog_output(&opts), -1);
    expect_int("failed digital ensure reports", dsd_audio_ensure_digital_output(&opts), -1);
    g_fail_opens = 0;
    expect_int("recovered analog ensure", dsd_audio_ensure_analog_output(&opts), 0);
    expect_int("recovered sink", opts.audio_raw_out != NULL, 1);
}

int
main(void) {
    test_analog_sink();
    test_digital_sink();
    test_nothing_to_open();
    test_open_failure();
    if (g_failures) {
        DSD_FPRINTF(stderr, "CORE_AUDIO_ENSURE_OUTPUT: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("CORE_AUDIO_ENSURE_OUTPUT: OK\n");
    return 0;
}

#else

int
main(void) {
    /* Needs link-time wrapping of the audio backend (GNU ld); skipped elsewhere. */
    printf("CORE_AUDIO_ENSURE_OUTPUT: skipped (no link-time audio wrap on this toolchain)\n");
    return 77;
}

#endif
