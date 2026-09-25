// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <assert.h>
#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/audio_filters.h>
#include <dsd-neo/core/frontend_types.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/power.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/dsp/symbol.h>
#include <dsd-neo/io/rigctl_client.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/platform/sockets.h>
#include <dsd-neo/runtime/exitflag.h>
#include <dsd-neo/runtime/shutdown.h>
#include <math.h>
#include <sndfile.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "test_support.h"

static int g_cleanup_calls = 0;
static int g_open_audio_input_rc = -1;
static int g_open_audio_input_calls = 0;

dsd_socket_t
// NOLINTNEXTLINE(misc-use-internal-linkage)
Connect(char* hostname, int portno) {
    (void)hostname;
    (void)portno;
    return (dsd_socket_t)0;
}

int
// NOLINTNEXTLINE(misc-use-internal-linkage)
openAudioInput(dsd_opts* opts) {
    (void)opts;
    g_open_audio_input_calls++;
    return g_open_audio_input_rc;
}

int
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_audio_reconfigure_output_for_input_policy(dsd_opts* opts) {
    (void)opts;
    return 0;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_request_shutdown(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    g_cleanup_calls++;
    exitflag = 1;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_audio_rescale_symbol_timing(dsd_state* state, int old_rate_hz, int new_rate_hz) {
    (void)state;
    (void)old_rate_hz;
    (void)new_rate_hz;
}

double
// NOLINTNEXTLINE(misc-use-internal-linkage)
pwr_to_dB(double mean_power) {
    (void)mean_power;
    return 0.0;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
lpf_f(dsd_state* state, float* input, int len) {
    (void)state;
    (void)input;
    (void)len;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
hpf_f(dsd_state* state, float* input, int len) {
    (void)state;
    (void)input;
    (void)len;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
pbf_f(dsd_state* state, float* input, int len) {
    (void)state;
    (void)input;
    (void)len;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
analog_gain_f(const dsd_opts* opts, dsd_state* state, float* input, int len) {
    (void)opts;
    (void)state;
    (void)input;
    (void)len;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
agsm_f(dsd_opts* opts, dsd_state* state, float* input, int len) {
    (void)opts;
    (void)state;
    (void)input;
    (void)len;
}

static int
create_one_sample_wav(char* out_path, size_t out_path_size) {
    int fd = dsd_test_mkstemp(out_path, out_path_size, "dsdneo_wav_eof");
    if (fd < 0) {
        return -1;
    }
    dsd_close(fd);

    SF_INFO info;
    DSD_MEMSET(&info, 0, sizeof(info));
    info.samplerate = 48000;
    info.channels = 1;
    info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;

    SNDFILE* wav = sf_open(out_path, SFM_WRITE, &info);
    if (wav == NULL) {
        return -1;
    }

    short sample = 1234;
    int ok = sf_write_short(wav, &sample, 1) == 1;
    sf_close(wav);
    return ok ? 0 : -1;
}

/*
 * An interactive terminal session keeps going on live Pulse input when the file ends. That is
 * a new receiver: the tone the file carried at its end must not stay on screen for the live
 * audio (issue #522), which at the same rate nothing else would tell the analog tap.
 */
static void
test_eof_onto_live_input_clears_received_tone(void) {
    char wav_path[DSD_TEST_PATH_MAX];
    assert(create_one_sample_wav(wav_path, sizeof(wav_path)) == 0);

    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    opts.audio_in_file_info = (SF_INFO*)calloc(1, sizeof(*opts.audio_in_file_info));
    assert(opts.audio_in_file_info != NULL);
    opts.audio_in_file = sf_open(wav_path, SFM_READ, opts.audio_in_file_info);
    assert(opts.audio_in_file != NULL);
    opts.audio_in_type = AUDIO_IN_WAV;
    opts.audio_out_type = 0;
    opts.frontend_kind = DSD_FRONTEND_TERMINAL;
    opts.input_volume_multiplier = 1;
    opts.wav_sample_rate = 48000;
    DSD_SNPRINTF(opts.audio_in_dev, sizeof(opts.audio_in_dev), "%s", wav_path);
    state.samplesPerSymbol = 1;
    state.symbolCenter = 0;
    state.rf_mod = 0;
    exitflag = 0;
    g_cleanup_calls = 0;
    g_open_audio_input_rc = 0;
    g_open_audio_input_calls = 0;

    /* A tone the tap locked on the file. */
    state.analog_rx.carrier_open = 1;
    state.analog_rx.tone_state = DSD_ANALOG_TONE_STATE_LOCKED;
    state.analog_rx.tone_kind = DSD_ANALOG_TONE_KIND_CTCSS;
    state.analog_rx.ctcss_tenths_hz = 1000;
    const uint32_t seeded = state.analog_rx.generation;

    assert(fabsf(getSymbol(&opts, &state, 0) - 1234.0f) < 1e-3f);
    (void)getSymbol(&opts, &state, 0);
    assert(g_open_audio_input_calls == 1);
    assert(g_cleanup_calls == 0 && exitflag == 0);
    assert(opts.audio_in_type == AUDIO_IN_PULSE);
    assert(opts.audio_in_file == NULL);
    assert(state.analog_rx.tone_state != DSD_ANALOG_TONE_STATE_LOCKED);
    assert(state.analog_rx.tone_kind == DSD_ANALOG_TONE_KIND_NONE);
    assert(state.analog_rx.ctcss_tenths_hz == 0 && state.analog_rx.carrier_open == 0);
    assert(state.analog_rx.generation != seeded);

    g_open_audio_input_rc = -1;
    free(opts.audio_in_file_info);
    opts.audio_in_file_info = NULL;
    remove(wav_path);
}

int
main(void) {
    test_eof_onto_live_input_clears_received_tone();

    char wav_path[DSD_TEST_PATH_MAX];
    assert(create_one_sample_wav(wav_path, sizeof(wav_path)) == 0);

    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

    opts.audio_in_file_info = (SF_INFO*)calloc(1, sizeof(*opts.audio_in_file_info));
    assert(opts.audio_in_file_info != NULL);
    opts.audio_in_file = sf_open(wav_path, SFM_READ, opts.audio_in_file_info);
    assert(opts.audio_in_file != NULL);
    opts.audio_in_type = AUDIO_IN_WAV;
    opts.audio_out_type = 1;
    opts.input_volume_multiplier = 1;
    opts.wav_sample_rate = 48000;
    DSD_SNPRINTF(opts.audio_in_dev, sizeof(opts.audio_in_dev), "%s", wav_path);

    state.samplesPerSymbol = 1;
    state.symbolCenter = 0;
    state.rf_mod = 0;
    exitflag = 0;
    g_cleanup_calls = 0;

    assert(fabsf(getSymbol(&opts, &state, 0) - 1234.0f) < 1e-3f);
    assert(g_cleanup_calls == 0);

    assert(getSymbol(&opts, &state, 0) == 0.0f);
    assert(g_cleanup_calls == 1);
    assert(exitflag == 1);
    assert(opts.audio_in_file == NULL);

    assert(getSymbol(&opts, &state, 0) == 0.0f);
    assert(g_cleanup_calls == 1);
    assert(opts.audio_in_file == NULL);

    free(opts.audio_in_file_info);
    opts.audio_in_file_info = NULL;
    remove(wav_path);
    return 0;
}
