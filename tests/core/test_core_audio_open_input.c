// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * openAudioInput() is the one place a Pulse input stream opens: at startup, on a config switch to another device, when
 * a playback stops or a file runs out. Every open that succeeds is a new stream, so it moves
 * dsd_opts::pcm_input_generation, which the PCM noise squelch keys its references on (issue #628): it forgets what it
 * learned on the stream before, whichever path reopened it. An open that fails moves nothing.
 *
 * The device layer is replaced at link time with a recording backend.
 */

#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/platform/audio.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static uintptr_t g_stream;
static int g_open_count;
static int g_close_count;
static int g_fail_opens;
static int g_opened_rate;

// GNU ld --wrap requires these exact external symbol names.
// NOLINTBEGIN(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,misc-use-internal-linkage)
dsd_audio_stream* __wrap_dsd_audio_open_input(const dsd_audio_params* params);
void __wrap_dsd_audio_close(dsd_audio_stream* stream);

dsd_audio_stream*
__wrap_dsd_audio_open_input(const dsd_audio_params* params) {
    g_open_count++;
    if (g_fail_opens) {
        return NULL;
    }
    g_opened_rate = params ? params->sample_rate : 0;
    return (dsd_audio_stream*)&g_stream;
}

void
__wrap_dsd_audio_close(dsd_audio_stream* stream) {
    (void)stream;
    g_close_count++;
}

// NOLINTEND(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,misc-use-internal-linkage)

static int g_failures;

static void
expect_int(const char* label, long long got, long long want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "FAIL: %s: got %lld want %lld\n", label, got, want);
        g_failures++;
    }
}

int
main(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    if (!opts) {
        DSD_FPRINTF(stderr, "FAIL: out of memory\n");
        return 1;
    }
    opts->audio_in_type = AUDIO_IN_PULSE;
    opts->pulse_digi_rate_in = 48000;
    opts->pulse_digi_in_channels = 1;
    const uint32_t start = opts->pcm_input_generation;

    g_fail_opens = 1;
    expect_int("a failed open reports it", openAudioInput(opts), -1);
    expect_int("a failed open leaves no stream", opts->audio_in_stream == NULL, 1);
    expect_int("a failed open is no new stream", opts->pcm_input_generation, start);

    g_fail_opens = 0;
    expect_int("an open succeeds", openAudioInput(opts), 0);
    expect_int("the open asked for the input rate", g_opened_rate, 48000);
    expect_int("the open holds the stream", opts->audio_in_stream == (dsd_audio_stream*)&g_stream, 1);
    expect_int("an open is a new stream", opts->pcm_input_generation, (long long)start + 1);

    closeAudioInput(opts);
    expect_int("the close releases the stream", g_close_count, 1);
    expect_int("a close alone is no new stream", opts->pcm_input_generation, (long long)start + 1);
    expect_int("a reopen succeeds", openAudioInput(opts), 0);
    expect_int("a reopen is another new stream", opts->pcm_input_generation, (long long)start + 2);
    expect_int("the device was opened three times", g_open_count, 3);

    closeAudioInput(opts);
    free(opts);
    if (g_failures) {
        return 1;
    }
    DSD_FPRINTF(stderr, "CORE_AUDIO_OPEN_INPUT: OK\n");
    return 0;
}
