// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <assert.h>
#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/frontend_types.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/power.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/dsp/analog_audio.h>
#include <dsd-neo/dsp/symbol.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/runtime/exitflag.h>
#include <dsd-neo/runtime/shutdown.h>
#include <math.h>
#include <sndfile.h>
#include <stdio.h>
#include <stdlib.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "test_support.h"

static int g_cleanup_calls = 0;
static int g_open_audio_input_rc = -1;
static int g_open_audio_input_calls = 0;

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

int
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_analog_audio_process_f(const dsd_opts* opts, dsd_state* state, dsd_analog_audio_chain chain, float* buf, size_t n,
                           dsd_analog_audio_source source, int rate_hz, unsigned int flags) {
    (void)opts;
    (void)state;
    (void)chain;
    (void)buf;
    (void)n;
    (void)source;
    (void)rate_hz;
    (void)flags;
    return 0;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_analog_audio_block_begin(const dsd_opts* opts, dsd_state* state, dsd_analog_audio_chain chain) {
    (void)opts;
    (void)state;
    (void)chain;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_analog_audio_note_reception(const dsd_state* state) {
    (void)state;
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
 * An interactive terminal session keeps going on live Pulse input when the file ends. The switch to Pulse replaces the
 * input under the decoder, so it is the engine's, between frames and under the tick guard (issue #634): a frame-sync
 * hunt read hands it over at once, with no symbol and no shutdown, and the engine's switch starts the new reception
 * (ENGINE_INPUT_FALLBACK, which also checks the received tone goes with the file, issue #522).
 */
static void
test_eof_in_the_hunt_hands_the_fallback_to_the_engine(void) {
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

    assert(fabsf(getSymbol(&opts, &state, 0) - 1234.0f) < 1e-3f);
    assert(state.input_fallback_pending == 0 && state.input_interrupted == 0);
    assert(getSymbol(&opts, &state, 0) == 0.0f);
    assert(state.input_fallback_pending == 1);
    assert(state.input_interrupted == 1);
    assert(opts.audio_in_file == NULL);
    assert(g_cleanup_calls == 0 && exitflag == 0);
    /* The reader opens nothing itself: the engine switches. */
    assert(g_open_audio_input_calls == 0);
    assert(opts.audio_in_type == AUDIO_IN_WAV);

    /* Read again before the engine got to it: still handed over, still no shutdown. */
    state.input_interrupted = 0;
    assert(getSymbol(&opts, &state, 0) == 0.0f);
    assert(state.input_interrupted == 1 && state.input_fallback_pending == 1);
    assert(g_cleanup_calls == 0 && exitflag == 0);

    free(opts.audio_in_file_info);
    opts.audio_in_file_info = NULL;
    remove(wav_path);
}

/* A file that ends while a frame is being decoded (getSymbol() with sync, under the guarded processFrame()) gives the
   rest of the frame silence: nothing of the new input is read into a frame of the old one (issue #634). */
static void
test_eof_in_frame_decoding_reads_silence_until_the_hunt(void) {
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

    /* The file's one sample in the hunt; it ends in the frame that follows. */
    assert(fabsf(getSymbol(&opts, &state, 0) - 1234.0f) < 1e-3f);
    for (int i = 0; i < 4; i++) {
        assert(getSymbol(&opts, &state, 1) == 0.0f);
        assert(state.input_fallback_pending == 1);
        assert(state.input_interrupted == 0);
        assert(g_cleanup_calls == 0 && exitflag == 0);
    }
    assert(opts.audio_in_file == NULL);
    /* The frame over, the hunt's first read hands the fallback over. */
    assert(getSymbol(&opts, &state, 0) == 0.0f);
    assert(state.input_interrupted == 1);

    free(opts.audio_in_file_info);
    opts.audio_in_file_info = NULL;
    remove(wav_path);
}

int
main(void) {
    test_eof_in_the_hunt_hands_the_fallback_to_the_engine();
    test_eof_in_frame_decoding_reads_silence_until_the_hunt();

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
