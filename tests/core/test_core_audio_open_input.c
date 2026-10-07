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
#include <dsd-neo/core/audio_input_switch.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/platform/audio.h>
#include <dsd-neo/platform/sockets.h>
#include <sndfile.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "test_support.h"

/* Two device streams, handed out in turn, so a switch's order of open and close can be told apart. */
static uintptr_t g_streams[2];
static int g_open_count;
static int g_close_count;
static int g_fail_opens;
static int g_opened_rate;
static char g_device_log[128];
static char g_opened_device[64];

static int
stream_index(const dsd_audio_stream* stream) {
    return stream == (const dsd_audio_stream*)&g_streams[1] ? 1 : 0;
}

static void
device_log(const char* what, int index) {
    const size_t used = strlen(g_device_log);
    DSD_SNPRINTF(g_device_log + used, sizeof g_device_log - used, "%s%s:%d", used ? " " : "", what, index);
}

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
    DSD_SNPRINTF(g_opened_device, sizeof g_opened_device, "%s", (params && params->device) ? params->device : "");
    const int index = g_open_count % 2;
    device_log("open", index);
    return (dsd_audio_stream*)&g_streams[index];
}

void
__wrap_dsd_audio_close(dsd_audio_stream* stream) {
    g_close_count++;
    device_log("close", stream_index(stream));
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

static void
write_wav(const char* path, int rate_hz) {
    SF_INFO info;
    DSD_MEMSET(&info, 0, sizeof info);
    info.samplerate = rate_hz;
    info.channels = 1;
    info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;
    SNDFILE* f = sf_open(path, SFM_WRITE, &info);
    short zeros[16] = {0};
    expect_int("wav fixture written", f != NULL && sf_write_short(f, zeros, 16) == 16, 1);
    if (f) {
        sf_close(f);
    }
}

static int
switch_to_pulse(dsd_opts* opts, dsd_state* state, const char* device) {
    dsd_audio_input_request req;
    DSD_MEMSET(&req, 0, sizeof req);
    req.kind = DSD_AUDIO_INPUT_PULSE;
    req.path = device;
    req.tcp_sockfd = DSD_INVALID_SOCKET;
    return dsd_audio_switch_input(opts, state, &req);
}

/*
 * Issue #634: an input switch to Pulse opens the new stream before it closes what ran, and one whose stream does not
 * open changes nothing. Leaving a WAV whose header set 96 kHz, Pulse runs at its own 48 kHz and the raw rate comes
 * back.
 */
static void
test_switches_to_pulse(void) {
    static dsd_opts opts;
    static dsd_state state;
    dsd_test_temp_cwd cwd;
    expect_int("temp working directory", dsd_test_temp_cwd_enter(&cwd, "dsdneo_open_input_switch"), 0);
    write_wav("hi.wav", 96000);

    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&state, 0, sizeof state);
    opts.audio_in_type = AUDIO_IN_PULSE;
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "pulse");
    opts.pulse_digi_rate_in = 48000;
    opts.pulse_digi_in_channels = 1;
    opts.wav_sample_rate = 48000;
    opts.wav_decimator = 48000;
    opts.wav_interpolator = 1;
    opts.tcp_sockfd = DSD_INVALID_SOCKET;
    state.samplesPerSymbol = 10;
    state.symbolCenter = 4;

    dsd_audio_input_request wav;
    DSD_MEMSET(&wav, 0, sizeof wav);
    wav.kind = DSD_AUDIO_INPUT_PCM_FILE;
    wav.path = "hi.wav";
    wav.tcp_sockfd = DSD_INVALID_SOCKET;
    expect_int("to the 96 kHz wav", dsd_audio_switch_input(&opts, &state, &wav), DSD_AUDIO_INPUT_SWITCHED);
    expect_int("wav timing", state.samplesPerSymbol, 20);
    SNDFILE* const file = opts.audio_in_file;

    /* A device that does not open: the WAV plays on, the configured device stays. */
    DSD_SNPRINTF(opts.pa_input_idx, sizeof opts.pa_input_idx, "%s", "mic");
    g_fail_opens = 1;
    const uint32_t before = opts.pcm_input_generation;
    expect_int("a pulse device that does not open", switch_to_pulse(&opts, &state, "usb"), DSD_AUDIO_INPUT_KEPT);
    expect_int("the wav plays on", opts.audio_in_type == AUDIO_IN_WAV && opts.audio_in_file == file, 1);
    expect_int("the configured device stays", strcmp(opts.pa_input_idx, "mic") == 0, 1);
    expect_int("no new stream", opts.pcm_input_generation == before, 1);
    g_fail_opens = 0;

    /* The configured device: a new stream at Pulse's rate; the raw rate comes back for the next PCM input. */
    g_device_log[0] = '\0';
    expect_int("to the configured pulse device", switch_to_pulse(&opts, &state, NULL), DSD_AUDIO_INPUT_SWITCHED);
    expect_int("pulse type", opts.audio_in_type, AUDIO_IN_PULSE);
    expect_int("the wav closed", opts.audio_in_file == NULL && opts.audio_in_file_info == NULL, 1);
    expect_int("the configured device opened", strcmp(g_opened_device, "mic") == 0, 1);
    expect_int("named for the device", strcmp(opts.audio_in_dev, "pulse:mic") == 0, 1);
    expect_int("the raw rate back", opts.wav_sample_rate, 48000);
    expect_int("pulse timing", state.samplesPerSymbol, 10);
    expect_int("a new stream", opts.pcm_input_generation != before, 1);

    /* Another device: its stream opens before the running one closes. */
    dsd_audio_stream* const running = opts.audio_in_stream;
    char expected[64];
    DSD_SNPRINTF(expected, sizeof expected, "open:%d close:%d", 1 - stream_index(running), stream_index(running));
    g_device_log[0] = '\0';
    expect_int("to another pulse device", switch_to_pulse(&opts, &state, "usb"), DSD_AUDIO_INPUT_SWITCHED);
    if (strcmp(g_device_log, expected) != 0) {
        DSD_FPRINTF(stderr, "FAIL: open before close: got \"%s\" want \"%s\"\n", g_device_log, expected);
        g_failures++;
    }
    expect_int("the new stream runs", opts.audio_in_stream != running && opts.audio_in_stream != NULL, 1);
    expect_int("named for the new device", strcmp(opts.audio_in_dev, "pulse:usb") == 0, 1);
    /* The default device: an empty name. */
    expect_int("to the default device", switch_to_pulse(&opts, &state, ""), DSD_AUDIO_INPUT_SWITCHED);
    expect_int("named plain pulse", strcmp(opts.audio_in_dev, "pulse") == 0, 1);

    closeAudioInDevice(&opts);
    (void)remove("hi.wav");
    expect_int("temp working directory removed", dsd_test_temp_cwd_leave(&cwd), 0);
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
    expect_int("the open holds the stream", opts->audio_in_stream != NULL, 1);
    expect_int("an open is a new stream", opts->pcm_input_generation, (long long)start + 1);

    closeAudioInput(opts);
    expect_int("the close releases the stream", g_close_count, 1);
    expect_int("a close alone is no new stream", opts->pcm_input_generation, (long long)start + 1);
    expect_int("a reopen succeeds", openAudioInput(opts), 0);
    expect_int("a reopen is another new stream", opts->pcm_input_generation, (long long)start + 2);
    expect_int("the device was opened three times", g_open_count, 3);

    closeAudioInput(opts);
    free(opts);
    test_switches_to_pulse();
    if (g_failures) {
        return 1;
    }
    DSD_FPRINTF(stderr, "CORE_AUDIO_OPEN_INPUT: OK\n");
    return 0;
}
