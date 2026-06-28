// SPDX-License-Identifier: ISC
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */
/*
 * Copyright (C) 2010 DSD Author
 * GPG Key ID: 0x3F1D7FD0 (74EF 430D F7F2 0A48 FCE6  F630 FAA2 635D 3F1D 7FD0)
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND ISC DISCLAIMS ALL WARRANTIES WITH
 * REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS.  IN NO EVENT SHALL ISC BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE
 * OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/audio_filters.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/string_utils.h>
#include <dsd-neo/platform/audio.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/platform/posix_compat.h>
#include <dsd-neo/runtime/log.h>
#include <dsd-neo/runtime/net_audio_input_hooks.h>
#include <dsd-neo/runtime/udp_audio_hooks.h>
#include <math.h>
#include <sndfile.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include "dsd-neo/core/dibit.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "dsd-neo/dsp/resampler.h"
#include "dsd_audio_internal.h"

static int
dsd_audio_path_is_wav_container(const char* path) {
    const char* dot = path ? strrchr(path, '.') : NULL;
    return (dot != NULL && dsd_strcasecmp(dot, ".wav") == 0);
}

static int
dsd_audio_file_has_wav_family_header(const char* path) {
    if (!path || path[0] == '\0') {
        return 0;
    }

    FILE* fp = dsd_fopen_existing_regular_file(path, "rb");
    if (!fp) {
        return 0;
    }

    unsigned char header[12] = {0};
    size_t nread = fread(header, 1, sizeof header, fp);
    fclose(fp);

    if (nread != sizeof header || memcmp(header + 8, "WAVE", 4) != 0) {
        return 0;
    }

    return memcmp(header, "RIFF", 4) == 0 || memcmp(header, "RIFX", 4) == 0 || memcmp(header, "RF64", 4) == 0;
}

static void
dsd_audio_init_raw_pcm16_info(SF_INFO* info, int sample_rate_hz) {
    if (!info) {
        return;
    }

    if (sample_rate_hz <= 0) {
        sample_rate_hz = 48000;
    }

    DSD_MEMSET(info, 0, sizeof(*info));
    info->samplerate = sample_rate_hz;
    info->channels = 1;
    info->seekable = 0;
    info->format = SF_FORMAT_RAW | SF_FORMAT_PCM_16 | SF_ENDIAN_LITTLE;
}

static int
dsd_audio_default_sample_rate_hz(int configured_sample_rate_hz) {
    return (configured_sample_rate_hz > 0) ? configured_sample_rate_hz : 48000;
}

static void
dsd_audio_write_wav_short_block(SNDFILE* file, const short* samples, sf_count_t sample_count, const char* context) {
    if (file == NULL || samples == NULL || sample_count <= 0) {
        return;
    }
    sf_count_t written = sf_write_short(file, samples, sample_count);
    if (written != sample_count) {
        LOG_WARN("%s: wrote %lld/%lld samples to WAV output\n", context, (long long)written, (long long)sample_count);
    }
}

static int
dsd_audio_try_open_mono_wav_container(const char* path, SF_INFO* info, SNDFILE** out_file, int* out_sample_rate_hz) {
    if (!dsd_audio_path_is_wav_container(path) || !dsd_audio_file_has_wav_family_header(path)) {
        return 0;
    }

    *out_file = sf_open(path, SFM_READ, info);
    if (!*out_file) {
        return -1;
    }
    if (info->channels != 1) {
        sf_close(*out_file);
        *out_file = NULL;
        return -1;
    }
    if (out_sample_rate_hz && info->samplerate > 0) {
        *out_sample_rate_hz = info->samplerate;
    }
    return 1;
}

static SNDFILE*
dsd_audio_open_raw_mono_pcm16(const char* path, SF_INFO* info, int configured_sample_rate_hz) {
    dsd_audio_init_raw_pcm16_info(info, configured_sample_rate_hz);
    return sf_open(path, SFM_READ, info);
}

int
dsd_audio_open_mono_file_input(const char* path, int configured_sample_rate_hz, SNDFILE** out_file, SF_INFO** out_info,
                               int* out_sample_rate_hz, int* out_opened_as_container) {
    int default_sample_rate_hz = dsd_audio_default_sample_rate_hz(configured_sample_rate_hz);

    if (!path || path[0] == '\0' || !out_file || !out_info) {
        return -1;
    }

    *out_file = NULL;
    *out_info = NULL;
    if (out_sample_rate_hz) {
        *out_sample_rate_hz = default_sample_rate_hz;
    }
    if (out_opened_as_container) {
        *out_opened_as_container = 0;
    }

    SF_INFO* info = (SF_INFO*)calloc(1, sizeof(*info));
    if (!info) {
        return -1;
    }

    SNDFILE* file = NULL;
    int opened_as_container = 0;
    int active_sample_rate_hz = default_sample_rate_hz;
    int container_open_status = dsd_audio_try_open_mono_wav_container(path, info, &file, &active_sample_rate_hz);

    if (container_open_status < 0) {
        free(info);
        return -1;
    }

    if (container_open_status > 0) {
        opened_as_container = 1;
    } else {
        file = dsd_audio_open_raw_mono_pcm16(path, info, configured_sample_rate_hz);
        if (!file) {
            free(info);
            return -1;
        }
    }

    *out_file = file;
    *out_info = info;
    if (out_sample_rate_hz) {
        *out_sample_rate_hz = active_sample_rate_hz;
    }
    if (out_opened_as_container) {
        *out_opened_as_container = opened_as_container;
    }
    return 0;
}

size_t
dsd_audio_linear_upsample_block_f32(float previous, float current, size_t factor, float* out, size_t out_cap) {
    if (!out || factor == 0 || out_cap < factor) {
        return 0;
    }

    float diff = current - previous;
    for (size_t n = 0; n < factor; n++) {
        out[n] = previous + (diff * ((float)(n + 1) / (float)factor));
    }
    return factor;
}

void
dsd_audio_rescale_symbol_timing(dsd_state* state, int old_rate_hz, int new_rate_hz) {
    if (!state) {
        return;
    }

    if (old_rate_hz <= 0) {
        old_rate_hz = 48000;
    }
    if (new_rate_hz <= 0) {
        new_rate_hz = old_rate_hz;
    }

    init_audio_filters(state, new_rate_hz);
    dsd_state_rescale_symbol_timing(state, old_rate_hz, new_rate_hz);
}

void
dsd_audio_apply_input_sample_rate(dsd_opts* opts, dsd_state* state, int old_effective_rate_hz, int sample_rate_hz) {
    if (!opts) {
        return;
    }

    if (old_effective_rate_hz <= 0) {
        old_effective_rate_hz = dsd_opts_effective_input_rate(opts);
    }
    if (sample_rate_hz <= 0) {
        sample_rate_hz = 48000;
    }

    dsd_opts_apply_input_sample_rate(opts, sample_rate_hz);
    /* Keep dsd_opts_* inline helpers link-safe for runtime-only targets. */
    dsd_resampler_reset(&opts->input_resampler);

    dsd_audio_rescale_symbol_timing(state, old_effective_rate_hz, dsd_opts_effective_input_rate(opts));
}

void
closeAudioOutput(dsd_opts* opts) {
    /* Close primary audio output stream */
    if (opts->audio_out_stream) {
        dsd_audio_close(opts->audio_out_stream);
        opts->audio_out_stream = NULL;
    }
    /* Close secondary output stream (slot 2/right) */
    if (opts->audio_out_streamR) {
        dsd_audio_close(opts->audio_out_streamR);
        opts->audio_out_streamR = NULL;
    }
    /* Close raw/analog audio output stream */
    if (opts->audio_raw_out) {
        dsd_audio_close(opts->audio_raw_out);
        opts->audio_raw_out = NULL;
    }
}

void
closeAudioInput(dsd_opts* opts) {
    if (opts->audio_in_stream) {
        dsd_audio_close(opts->audio_in_stream);
        opts->audio_in_stream = NULL;
    }
}

static int
dsd_audio_should_use_async_output(const dsd_opts* opts) {
    if (!opts) {
        return 0;
    }
    return dsd_audio_input_type_uses_async_output(opts->audio_in_type, opts->playfiles, opts->audio_in_dev,
                                                  opts->m17decoderip);
}

int
openAudioOutput(dsd_opts* opts) {
    const char* dev = NULL;
    if (opts->pa_output_idx[0] != '\0') {
        dev = opts->pa_output_idx;
    }

    dsd_audio_params params;
    DSD_MEMSET(&params, 0, sizeof(params));
    params.device = dev;
    params.app_name = "DSD-neo";
    params.async_output = dsd_audio_should_use_async_output(opts);

    /* Open raw/analog output stream for ProVoice or analog monitor mode */
    if (opts->frame_provoice == 1 || opts->monitor_input_audio == 1) {
        params.sample_rate = opts->pulse_raw_rate_out;
        params.channels = opts->pulse_raw_out_channels;
        params.bits_per_sample = 16;
        opts->audio_raw_out = dsd_audio_open_output(&params);
        if (!opts->audio_raw_out) {
            LOG_ERROR("Failed to open raw audio output: %s", dsd_audio_get_error());
            return -1;
        }
    }

    /* Open main digital audio output stream (unless in analog-only mode) */
    if (opts->analog_only == 0) {
        params.sample_rate = opts->pulse_digi_rate_out;
        params.channels = opts->pulse_digi_out_channels;
        params.bits_per_sample = 16;
        opts->audio_out_stream = dsd_audio_open_output(&params);
        if (!opts->audio_out_stream) {
            LOG_ERROR("Failed to open audio output: %s", dsd_audio_get_error());
            if (opts->audio_raw_out) {
                dsd_audio_close(opts->audio_raw_out);
                opts->audio_raw_out = NULL;
            }
            return -1;
        }
    }
    opts->audio_output_async_policy = params.async_output ? 1 : 0;
    return 0;
}

int
dsd_audio_reconfigure_output_for_input_policy(dsd_opts* opts) {
    if (!opts) {
        return -1;
    }
    if (opts->audio_out != 1 || opts->audio_out_type != 0) {
        return 0;
    }
    if (!opts->audio_out_stream && !opts->audio_out_streamR && !opts->audio_raw_out) {
        return 0;
    }

    int async_policy = dsd_audio_should_use_async_output(opts);
    if (opts->audio_output_async_policy == async_policy) {
        return 0;
    }

    if (opts->audio_output_async_policy == 0) {
        dsd_drain_audio_output(opts);
    }
    closeAudioOutput(opts);
    if (openAudioOutput(opts) != 0) {
        LOG_ERROR("Failed to reconfigure audio output for input policy\n");
        return -1;
    }
    return 0;
}

int
openAudioInput(dsd_opts* opts) {
    const char* dev = NULL;
    if (opts->pa_input_idx[0] != '\0') {
        dev = opts->pa_input_idx;
    }

    dsd_audio_params params;
    DSD_MEMSET(&params, 0, sizeof(params));
    params.sample_rate = opts->pulse_digi_rate_in;
    params.channels = opts->pulse_digi_in_channels;
    params.bits_per_sample = 16;
    params.device = dev;
    params.app_name = (opts->m17encoder == 1) ? "DSD-neo M17" : "DSD-neo";

    opts->audio_in_stream = dsd_audio_open_input(&params);
    if (!opts->audio_in_stream) {
        LOG_ERROR("Failed to open audio input: %s", dsd_audio_get_error());
        return -1;
    }
    return 0;
}

void
dsd_drain_audio_output(dsd_opts* opts) {
    if (!opts) {
        return;
    }
    /* Only act if audio output is enabled */
    if (opts->audio_out != 1) {
        return;
    }
    /* Audio stream: drain any queued samples */
    if (opts->audio_out_type == 0) {
        if (opts->audio_out_stream) {
            (void)dsd_audio_drain(opts->audio_out_stream);
        }
        if (opts->audio_out_streamR) {
            (void)dsd_audio_drain(opts->audio_out_streamR);
        }
        if (opts->audio_raw_out) {
            (void)dsd_audio_drain(opts->audio_raw_out);
        }
        return;
    }
    /* UDP/STDOUT: nothing meaningful to drain; attempt fsync for file descriptors */
    if (opts->audio_out_type == 1 || opts->audio_out_type == 8) {
        if (opts->audio_out_fd >= 0) {
            (void)dsd_fsync(opts->audio_out_fd);
        }
        return;
    }
}

void
parse_audio_input_string(dsd_opts* opts, char* input) {
    const char* curr;
    char* saveptr = NULL;
    curr = dsd_strtok_r(input, ":", &saveptr);
    if (curr != NULL) {
        DSD_STRNCPY(opts->pa_input_idx, curr, 99);
        opts->pa_input_idx[99] = '\0';
        DSD_FPRINTF(stderr, "\n");
        DSD_FPRINTF(stderr, "Audio Input Device: %s; ", opts->pa_input_idx);
        DSD_FPRINTF(stderr, "\n");
    }
}

void
parse_audio_output_string(dsd_opts* opts, char* input) {
    const char* curr;
    char* saveptr = NULL;
    curr = dsd_strtok_r(input, ":", &saveptr);
    if (curr != NULL) {
        DSD_STRNCPY(opts->pa_output_idx, curr, 99);
        opts->pa_output_idx[99] = '\0';
        DSD_FPRINTF(stderr, "\n");
        DSD_FPRINTF(stderr, "Audio Output Device: %s; ", opts->pa_output_idx);
        DSD_FPRINTF(stderr, "\n");
    }
}

static float
dsd_audio_clamp_pcm16_f32(float sample) {
    if (sample > 32767.0F) {
        return 32767.0F;
    }
    if (sample < -32768.0F) {
        return -32768.0F;
    }
    return sample;
}

static float
dsd_audio_detect_block_peak_left(dsd_state* state) {
    int n;
    float max = 0.0F;

    state->audio_out_temp_buf_p = state->audio_out_temp_buf;
    for (n = 0; n < 160; n++) {
        float aout_abs = fabsf(*state->audio_out_temp_buf_p);
        if (aout_abs > max) {
            max = aout_abs;
        }
        state->audio_out_temp_buf_p++;
    }
    return max;
}

static float
dsd_audio_update_peak_history_left(dsd_state* state, float block_max) {
    float max = block_max;
    int i;

    *state->aout_max_buf_p = block_max;
    state->aout_max_buf_p++;
    state->aout_max_buf_idx++;

    if (state->aout_max_buf_idx > 24) {
        state->aout_max_buf_idx = 0;
        state->aout_max_buf_p = state->aout_max_buf;
    }

    for (i = 0; i < 25; i++) {
        float maxbuf = state->aout_max_buf[i];
        if (maxbuf > max) {
            max = maxbuf;
        }
    }
    return max;
}

static float
dsd_audio_compute_gain_delta_left(const dsd_opts* opts, dsd_state* state) {
    float gaindelta;
    float gainfactor;
    float max;

    if (opts->audio_gain != 0.0F) {
        return 0.0F;
    }

    max = dsd_audio_detect_block_peak_left(state);
    max = dsd_audio_update_peak_history_left(state, max);

    if (max > 0.0F) {
        gainfactor = 30000.0F / max;
    } else {
        gainfactor = 50.0F;
    }

    if (gainfactor < state->aout_gain) {
        state->aout_gain = gainfactor;
        gaindelta = 0.0F;
    } else {
        if (gainfactor > 50.0F) {
            gainfactor = 50.0F;
        }
        gaindelta = gainfactor - state->aout_gain;
        if (gaindelta > (0.05F * state->aout_gain)) {
            gaindelta = 0.05F * state->aout_gain;
        }
    }

    return gaindelta / 160.0F;
}

static void
dsd_audio_apply_gain_left(const dsd_opts* opts, dsd_state* state, float gaindelta) {
    int n;

    if (opts->audio_gain < 0) {
        return;
    }

    state->audio_out_temp_buf_p = state->audio_out_temp_buf;
    for (n = 0; n < 160; n++) {
        *state->audio_out_temp_buf_p = (state->aout_gain + ((float)n * gaindelta)) * (*state->audio_out_temp_buf_p);
        state->audio_out_temp_buf_p++;
    }
    state->aout_gain += 160.0F * gaindelta;
}

static void
dsd_audio_emit_upsampled_left(const dsd_opts* opts, dsd_state* state) {
    int n;

    state->audio_out_temp_buf_p = state->audio_out_temp_buf;
    for (n = 0; n < 160; n++) {
        upsample(state, *state->audio_out_temp_buf_p);
        state->audio_out_temp_buf_p++;
        state->audio_out_float_buf_p += 6;
        state->audio_out_idx += 6;
        state->audio_out_idx2 += 6;
    }

    state->audio_out_float_buf_p -= (960 + opts->playoffset);
    for (n = 0; n < 960; n++) {
        *state->audio_out_float_buf_p = dsd_audio_clamp_pcm16_f32(*state->audio_out_float_buf_p);
        *state->audio_out_buf_p = (short)*state->audio_out_float_buf_p;
        state->s_lu[n] = (short)*state->audio_out_float_buf_p;
        state->audio_out_buf_p++;
        state->audio_out_float_buf_p++;
    }
    state->audio_out_float_buf_p += opts->playoffset;
}

static void
dsd_audio_emit_native_left(dsd_state* state) {
    int n;

    state->audio_out_temp_buf_p = state->audio_out_temp_buf;
    for (n = 0; n < 160; n++) {
        *state->audio_out_temp_buf_p = dsd_audio_clamp_pcm16_f32(*state->audio_out_temp_buf_p);
        *state->audio_out_buf_p = (short)*state->audio_out_temp_buf_p;
        state->s_l[n] = (short)*state->audio_out_temp_buf_p;
        state->audio_out_buf_p++;
        state->audio_out_temp_buf_p++;
        state->audio_out_idx++;
        state->audio_out_idx2++;
    }
}

static void
dsd_audio_emit_output_left(const dsd_opts* opts, dsd_state* state) {
    if (opts->pulse_digi_rate_out > 8000) {
        dsd_audio_emit_upsampled_left(opts, state);
        return;
    }
    dsd_audio_emit_native_left(state);
}

void
processAudio(const dsd_opts* opts, dsd_state* state) {
    float gaindelta = dsd_audio_compute_gain_delta_left(opts, state);

    dsd_audio_apply_gain_left(opts, state, gaindelta);
    dsd_audio_emit_output_left(opts, state);
}

static float
dsd_audio_detect_block_peak_right(dsd_state* state) {
    int n;
    float max = 0.0F;

    state->audio_out_temp_buf_pR = state->audio_out_temp_bufR;
    for (n = 0; n < 160; n++) {
        float aout_abs = fabsf(*state->audio_out_temp_buf_pR);
        if (aout_abs > max) {
            max = aout_abs;
        }
        state->audio_out_temp_buf_pR++;
    }
    return max;
}

static float
dsd_audio_update_peak_history_right(dsd_state* state, float block_max) {
    float max = block_max;
    int i;

    *state->aout_max_buf_pR = block_max;
    state->aout_max_buf_pR++;
    state->aout_max_buf_idxR++;

    if (state->aout_max_buf_idxR > 24) {
        state->aout_max_buf_idxR = 0;
        state->aout_max_buf_pR = state->aout_max_bufR;
    }

    for (i = 0; i < 25; i++) {
        float maxbuf = state->aout_max_bufR[i];
        if (maxbuf > max) {
            max = maxbuf;
        }
    }
    return max;
}

static float
dsd_audio_compute_gain_delta_right(const dsd_opts* opts, dsd_state* state) {
    float gaindelta;
    float gainfactor;
    float max;

    if (opts->audio_gainR != 0.0F) {
        return 0.0F;
    }

    max = dsd_audio_detect_block_peak_right(state);
    max = dsd_audio_update_peak_history_right(state, max);

    if (max > 0.0F) {
        gainfactor = 30000.0F / max;
    } else {
        gainfactor = 50.0F;
    }

    if (gainfactor < state->aout_gainR) {
        state->aout_gainR = gainfactor;
        gaindelta = 0.0F;
    } else {
        if (gainfactor > 50.0F) {
            gainfactor = 50.0F;
        }
        gaindelta = gainfactor - state->aout_gainR;
        if (gaindelta > (0.05F * state->aout_gainR)) {
            gaindelta = 0.05F * state->aout_gainR;
        }
    }

    return gaindelta / 160.0F;
}

static void
dsd_audio_apply_gain_right(const dsd_opts* opts, dsd_state* state, float gaindelta) {
    int n;

    if (opts->audio_gainR < 0) {
        return;
    }

    state->audio_out_temp_buf_pR = state->audio_out_temp_bufR;
    for (n = 0; n < 160; n++) {
        *state->audio_out_temp_buf_pR = (state->aout_gainR + ((float)n * gaindelta)) * (*state->audio_out_temp_buf_pR);
        state->audio_out_temp_buf_pR++;
    }
    state->aout_gainR += 160.0F * gaindelta;
}

static void
dsd_audio_emit_upsampled_right(const dsd_opts* opts, dsd_state* state) {
    int n;

    state->audio_out_temp_buf_pR = state->audio_out_temp_bufR;
    for (n = 0; n < 160; n++) {
        upsample(state, *state->audio_out_temp_buf_pR);
        state->audio_out_temp_buf_pR++;
        state->audio_out_float_buf_pR += 6;
        state->audio_out_idxR += 6;
        state->audio_out_idx2R += 6;
    }

    state->audio_out_float_buf_pR -= (960 + opts->playoffsetR);
    for (n = 0; n < 960; n++) {
        *state->audio_out_float_buf_pR = dsd_audio_clamp_pcm16_f32(*state->audio_out_float_buf_pR);
        *state->audio_out_buf_pR = (short)*state->audio_out_float_buf_pR;
        state->s_ru[n] = (short)*state->audio_out_float_buf_pR;
        state->audio_out_buf_pR++;
        state->audio_out_float_buf_pR++;
    }
    state->audio_out_float_buf_pR += opts->playoffsetR;
}

static void
dsd_audio_emit_native_right(dsd_state* state) {
    int n;

    state->audio_out_temp_buf_pR = state->audio_out_temp_bufR;
    for (n = 0; n < 160; n++) {
        *state->audio_out_temp_buf_pR = dsd_audio_clamp_pcm16_f32(*state->audio_out_temp_buf_pR);
        *state->audio_out_buf_pR = (short)*state->audio_out_temp_buf_pR;
        state->s_r[n] = (short)*state->audio_out_temp_buf_pR;
        state->audio_out_buf_pR++;
        state->audio_out_temp_buf_pR++;
        state->audio_out_idxR++;
        state->audio_out_idx2R++;
    }
}

static void
dsd_audio_emit_output_right(const dsd_opts* opts, dsd_state* state) {
    if (opts->pulse_digi_rate_out > 8000) {
        dsd_audio_emit_upsampled_right(opts, state);
        return;
    }
    dsd_audio_emit_native_right(state);
}

void
processAudioR(const dsd_opts* opts, dsd_state* state) {
    float gaindelta = dsd_audio_compute_gain_delta_right(opts, state);

    dsd_audio_apply_gain_right(opts, state, gaindelta);
    dsd_audio_emit_output_right(opts, state);
}

void
writeSynthesizedVoice(dsd_opts* opts, dsd_state* state) {
    int n;
    short aout_buf[160];
    short* aout_buf_p;

    aout_buf_p = aout_buf;
    state->audio_out_temp_buf_p = state->audio_out_temp_buf;

    for (n = 0; n < 160; n++) {
        if (*state->audio_out_temp_buf_p > (float)32767) {
            *state->audio_out_temp_buf_p = (float)32767;
        } else if (*state->audio_out_temp_buf_p < (float)-32768) {
            *state->audio_out_temp_buf_p = (float)-32768;
        }
        *aout_buf_p = (short)*state->audio_out_temp_buf_p;
        aout_buf_p++;
        state->audio_out_temp_buf_p++;
    }

    dsd_audio_write_wav_short_block(opts->wav_out_f, aout_buf, 160, "writeSynthesizedVoice");
}

void
writeSynthesizedVoiceR(dsd_opts* opts, dsd_state* state) {
    int n;
    short aout_buf[160];
    short* aout_buf_p;

    aout_buf_p = aout_buf;
    state->audio_out_temp_buf_pR = state->audio_out_temp_bufR;

    for (n = 0; n < 160; n++) {
        if (*state->audio_out_temp_buf_pR > (float)32767) {
            *state->audio_out_temp_buf_pR = (float)32767;
        } else if (*state->audio_out_temp_buf_pR < (float)-32768) {
            *state->audio_out_temp_buf_pR = (float)-32768;
        }
        *aout_buf_p = (short)*state->audio_out_temp_buf_pR;
        aout_buf_p++;
        state->audio_out_temp_buf_pR++;
    }

    dsd_audio_write_wav_short_block(opts->wav_out_fR, aout_buf, 160, "writeSynthesizedVoiceR");
}

//short Mono to Stereo version for new static .wav files in stereo format for TDMA
void
writeSynthesizedVoiceMS(dsd_opts* opts, dsd_state* state) {
    int n;
    short aout_buf[160];
    short* aout_buf_p;

    aout_buf_p = aout_buf;
    state->audio_out_temp_buf_p = state->audio_out_temp_buf;

    for (n = 0; n < 160; n++) {
        if (*state->audio_out_temp_buf_p > (float)32767) {
            *state->audio_out_temp_buf_p = (float)32767;
        } else if (*state->audio_out_temp_buf_p < (float)-32768) {
            *state->audio_out_temp_buf_p = (float)-32768;
        }
        *aout_buf_p = (short)*state->audio_out_temp_buf_p;
        aout_buf_p++;
        state->audio_out_temp_buf_p++;
    }

    short ss[320];
    for (n = 0; n < 160; n++) {
        ss[(n * 2) + 0] = aout_buf[n];
        ss[(n * 2) + 1] = aout_buf[n];
    }

    dsd_audio_write_wav_short_block(opts->wav_out_f, ss, 320, "writeSynthesizedVoiceMS");
}

void
playSynthesizedVoice(dsd_opts* opts, dsd_state* state) {

    //don't synthesize voice if slot is turned off
    if (opts->slot1_on == 0) {
        //clear any previously buffered audio
        state->audio_out_float_buf_p = state->audio_out_float_buf + 100;
        state->audio_out_buf_p = state->audio_out_buf + 100;
        DSD_MEMSET(state->audio_out_float_buf, 0, 100 * sizeof(float));
        DSD_MEMSET(state->audio_out_buf, 0, 100 * sizeof(short));
        state->audio_out_idx2 = 0;
        state->audio_out_idx = 0;
        goto end_psv;
    }

    if (state->audio_out_idx > opts->delay) {
        if (opts->audio_out == 1 && opts->audio_out_type == 1) {
            ssize_t written = dsd_write(opts->audio_out_fd, (state->audio_out_buf_p - state->audio_out_idx),
                                        (size_t)state->audio_out_idx * sizeof(short));
            if (written < 0) {
                LOG_WARN("playSynthesizedVoice: failed to write %zu bytes to audio_out_fd",
                         (size_t)state->audio_out_idx * sizeof(short));
            }
            state->audio_out_idx = 0;
        } else if (opts->audio_out == 1 && opts->audio_out_type == 0) {
            /* Use audio abstraction layer */
            if (opts->audio_out_stream) {
                dsd_audio_write(opts->audio_out_stream, (state->audio_out_buf_p - state->audio_out_idx),
                                (size_t)state->audio_out_idx);
            }
            state->audio_out_idx = 0;
        } else if (opts->audio_out == 1
                   && opts->audio_out_type == 8) //UDP Audio Out -- Forgot some things still use this for now
        {
            dsd_udp_audio_hook_blast(opts, state, (size_t)state->audio_out_idx * sizeof(short),
                                     (state->audio_out_buf_p - state->audio_out_idx));
            state->audio_out_idx = 0;
        } else {
            state->audio_out_idx = 0; //failsafe for audio_out == 0
        }
    }

end_psv:

    if (state->audio_out_idx2 >= 800000) {
        state->audio_out_float_buf_p = state->audio_out_float_buf + 100;
        state->audio_out_buf_p = state->audio_out_buf + 100;
        DSD_MEMSET(state->audio_out_float_buf, 0, 100 * sizeof(float));
        DSD_MEMSET(state->audio_out_buf, 0, 100 * sizeof(short));
        state->audio_out_idx2 = 0;
    }
}

static void
dsd_audio_release_input_sources(dsd_opts* opts) {
    if (opts->audio_in_file) {
        sf_close(opts->audio_in_file);
        opts->audio_in_file = NULL;
    }
    if (opts->audio_in_file_info) {
        free(opts->audio_in_file_info);
        opts->audio_in_file_info = NULL;
    }
    if (opts->symbolfile) {
        fclose(opts->symbolfile);
        opts->symbolfile = NULL;
    }
    if (opts->tcp_in_ctx) {
        dsd_net_audio_input_hook_tcp_close(opts->tcp_in_ctx);
        opts->tcp_in_ctx = NULL;
    }
    if (opts->udp_in_ctx) {
        dsd_net_audio_input_hook_udp_stop(opts);
    }
    dsd_opts_reset_pcm_input_state(opts);
}

static void
dsd_audio_reset_symbol_replay_pacing(dsd_state* state) {
    if (!state) {
        return;
    }
    state->use_throttle = 0;
    state->symbol_replay_next_deadline_ns = 0;
}

static SF_INFO*
dsd_audio_alloc_pcm16_mono_info_exact_rate(int sample_rate_hz) {
    SF_INFO* info = calloc(1, sizeof(*info));
    if (!info) {
        return NULL;
    }

    info->samplerate = sample_rate_hz;
    info->channels = 1;
    info->seekable = 0;
    info->format = SF_FORMAT_RAW | SF_FORMAT_PCM_16 | SF_ENDIAN_LITTLE;
    return info;
}

static int
dsd_audio_open_stdin_input(dsd_opts* opts) {
    opts->audio_in_type = AUDIO_IN_STDIN;
    opts->audio_in_file_info = dsd_audio_alloc_pcm16_mono_info_exact_rate(opts->wav_sample_rate);
    if (opts->audio_in_file_info == NULL) {
        LOG_ERROR("Error, couldn't allocate memory for audio input\n");
        return -1;
    }

    opts->audio_in_file = sf_open_fd(dsd_fileno(stdin), SFM_READ, opts->audio_in_file_info, 0);
    if (opts->audio_in_file == NULL) {
        LOG_ERROR("Error, couldn't open stdin with libsndfile: %s\n", sf_strerror(NULL));
        free(opts->audio_in_file_info);
        opts->audio_in_file_info = NULL;
        return -1;
    }
    return 0;
}

static int
dsd_audio_open_udp_input(dsd_opts* opts) {
    opts->audio_in_type = AUDIO_IN_UDP;

    if (opts->udp_in_portno == 0) {
        opts->udp_in_portno = 7355;
    }
    if (opts->udp_in_bindaddr[0] == '\0') {
        DSD_SNPRINTF(opts->udp_in_bindaddr, sizeof(opts->udp_in_bindaddr), "%s", "127.0.0.1");
    }

    if (dsd_net_audio_input_hook_udp_start(opts, opts->udp_in_bindaddr, opts->udp_in_portno, opts->wav_sample_rate)
        < 0) {
        DSD_FPRINTF(stderr, "Error, couldn't start UDP input on %s:%d\n", opts->udp_in_bindaddr, opts->udp_in_portno);
        return -1;
    }

    DSD_FPRINTF(stderr, "Waiting for UDP audio on %s:%d ...\n", opts->udp_in_bindaddr, opts->udp_in_portno);
    return 0;
}

static int
dsd_audio_open_tcp_input(dsd_opts* opts) {
    opts->audio_in_type = AUDIO_IN_TCP;
    opts->tcp_in_ctx = dsd_net_audio_input_hook_tcp_open(opts->tcp_sockfd, opts->wav_sample_rate);
    if (opts->tcp_in_ctx == NULL) {
        LOG_ERROR("Error, couldn't open TCP audio input\n");
        return -1;
    }
    return 0;
}

static int
dsd_audio_select_radio_or_pulse_input(dsd_opts* opts) {
#ifdef USE_RADIO
    opts->audio_in_type = AUDIO_IN_RTL;
    return 0;
#else
    if (dsd_opts_audio_in_dev_is_iqreplay_spec(opts->audio_in_dev)) {
        LOG_ERROR("IQ replay requires a build with radio pipeline support.\n");
        return -1;
    }
    opts->audio_in_type = AUDIO_IN_PULSE;
    DSD_SNPRINTF(opts->audio_in_dev, sizeof(opts->audio_in_dev), "pulse");
    return 0;
#endif
}

static int
dsd_audio_try_open_named_input(dsd_opts* opts) {
    if (strncmp(opts->audio_in_dev, "-", 1) == 0) {
        return (dsd_audio_open_stdin_input(opts) == 0) ? 1 : -1;
    }
    if (dsd_opts_audio_in_dev_is_m17udp_spec(opts->audio_in_dev)) {
        opts->audio_in_type = AUDIO_IN_NULL;
        return 1;
    }
    if (dsd_opts_audio_in_dev_is_udp_spec(opts->audio_in_dev)) {
        return (dsd_audio_open_udp_input(opts) == 0) ? 1 : -1;
    }
    if (dsd_opts_audio_in_dev_is_tcp_spec(opts->audio_in_dev)) {
        return (dsd_audio_open_tcp_input(opts) == 0) ? 1 : -1;
    }
    if (dsd_opts_audio_in_dev_is_iqreplay_spec(opts->audio_in_dev)
        || dsd_opts_audio_in_dev_is_rtl_spec(opts->audio_in_dev)
        || dsd_opts_audio_in_dev_is_rtltcp_spec(opts->audio_in_dev)
        || dsd_opts_audio_in_dev_is_soapy_spec(opts->audio_in_dev)) {
#ifdef USE_RADIO
        (void)dsd_audio_select_radio_or_pulse_input(opts);
        return 1;
#else
        return (dsd_audio_select_radio_or_pulse_input(opts) == 0) ? 1 : -1;
#endif
    }
    if (dsd_opts_audio_in_dev_is_pulse_spec(opts->audio_in_dev)) {
        opts->audio_in_type = AUDIO_IN_PULSE;
        return 1;
    }
    return 0;
}

static int
dsd_audio_open_headless_wav_input(dsd_opts* opts, int sample_rate_hz, int include_path_in_error) {
    opts->audio_in_type = AUDIO_IN_WAV;
    opts->audio_in_file_info = dsd_audio_alloc_pcm16_mono_info_exact_rate(sample_rate_hz);
    if (opts->audio_in_file_info == NULL) {
        LOG_ERROR("Error, couldn't allocate memory for audio input\n");
        return -1;
    }

    opts->audio_in_file = sf_open(opts->audio_in_dev, SFM_READ, opts->audio_in_file_info);
    if (opts->audio_in_file == NULL) {
        if (include_path_in_error) {
            LOG_ERROR("Error, couldn't open %s with libsndfile: %s\n", opts->audio_in_dev, sf_strerror(NULL));
        } else {
            LOG_ERROR("Error, couldn't open file/pipe with libsndfile: %s\n", sf_strerror(NULL));
        }
        free(opts->audio_in_file_info);
        opts->audio_in_file_info = NULL;
        return -1;
    }
    return 0;
}

static void
dsd_audio_enable_bin_symbol_replay(dsd_state* state) {
    if (!state) {
        return;
    }
    state->use_throttle = 1;
    state->symbol_replay_next_deadline_ns = 0;
    state->symbol_replay_format = DSD_SYMBOL_REPLAY_FORMAT_UNKNOWN;
    state->symbol_replay_header_checked = 0;
    state->symbol_replay_has_soft = 0;
}

static int
dsd_audio_open_symbol_input(dsd_opts* opts, dsd_state* state, int symbol_type, const char* label) {
    dsd_stat_t stat_buf;

    if (dsd_stat_path(opts->audio_in_dev, &stat_buf) != 0) {
        LOG_ERROR("Error, couldn't open %s file %s\n", label, opts->audio_in_dev);
        return -1;
    }
    if (!dsd_stat_is_regular(&stat_buf)) {
        opts->audio_in_type = AUDIO_IN_PULSE;
        return 0;
    }

    opts->symbolfile = dsd_fopen_existing_regular_file(opts->audio_in_dev, "rb");
    if (opts->symbolfile == NULL) {
        LOG_ERROR("Error, couldn't open %s file %s\n", label, opts->audio_in_dev);
        return -1;
    }

    opts->audio_in_type = symbol_type;
    if (symbol_type == AUDIO_IN_SYMBOL_BIN) {
        dsd_audio_enable_bin_symbol_replay(state);
    }
    return 0;
}

static int
dsd_audio_open_fallback_file_input(dsd_opts* opts, dsd_state* state, int old_effective_input_rate) {
    dsd_stat_t stat_buf;

    if (dsd_stat_path(opts->audio_in_dev, &stat_buf) != 0) {
        LOG_ERROR("Error, couldn't open input file %s\n", opts->audio_in_dev);
        return -1;
    }
    if (!dsd_stat_is_regular(&stat_buf)) {
        LOG_ERROR("Error, couldn't open input file.\n");
        return -1;
    }

    opts->audio_in_type = AUDIO_IN_WAV;
    int configured_file_sample_rate = dsd_opts_requested_file_sample_rate(opts);
    int active_sample_rate = configured_file_sample_rate;
    int opened_as_container = 0;
    if (dsd_audio_open_mono_file_input(opts->audio_in_dev, configured_file_sample_rate, &opts->audio_in_file,
                                       &opts->audio_in_file_info, &active_sample_rate, &opened_as_container)
        != 0) {
        LOG_ERROR("Error, couldn't open input file %s\n", opts->audio_in_dev);
        return -1;
    }

    if (active_sample_rate != opts->wav_sample_rate) {
        if (opened_as_container) {
            LOG_NOTICE("WAV header sample rate %d Hz overrides configured %d Hz for %s\n", active_sample_rate,
                       configured_file_sample_rate, opts->audio_in_dev);
        }
        dsd_audio_apply_input_sample_rate(opts, state, old_effective_input_rate, active_sample_rate);
    }
    return 0;
}

static int
dsd_audio_open_extension_input(dsd_opts* opts, dsd_state* state, const char* extension, int old_effective_input_rate) {
    if (extension == NULL) {
        return dsd_audio_open_headless_wav_input(opts, opts->wav_sample_rate, 0);
    }
    if (strncmp(extension, ".rrc", 4) == 0) {
        DSD_FPRINTF(stderr, "Opening M17 .rrc headless wav file\n");
        return dsd_audio_open_headless_wav_input(opts, 48000, 1);
    }
    if (strncmp(extension, ".raw", 4) == 0) {
        return dsd_audio_open_symbol_input(opts, state, AUDIO_IN_SYMBOL_FLT, "raw (float)");
    }
    if (strncmp(extension, ".sym", 4) == 0) {
        return dsd_audio_open_symbol_input(opts, state, AUDIO_IN_SYMBOL_FLT, "sym (float)");
    }
    if (strncmp(extension, ".bin", 4) == 0) {
        return dsd_audio_open_symbol_input(opts, state, AUDIO_IN_SYMBOL_BIN, "bin");
    }
    return dsd_audio_open_fallback_file_input(opts, state, old_effective_input_rate);
}

int
openAudioInDevice(dsd_opts* opts, dsd_state* state) {
    if (opts == NULL || state == NULL) {
        return -1;
    }
    int old_effective_input_rate = dsd_opts_current_input_timing_rate(opts);
    int named_input_status;

    dsd_audio_release_input_sources(opts);
    dsd_audio_reset_symbol_replay_pacing(state);

    named_input_status = dsd_audio_try_open_named_input(opts);
    if (named_input_status < 0) {
        return -1;
    }
    if (named_input_status == 0) {
        const char* extension = strrchr(opts->audio_in_dev, '.');
        if (dsd_audio_open_extension_input(opts, state, extension, old_effective_input_rate) != 0) {
            return -1;
        }
    }

    if (opts->split == 1) {
        DSD_FPRINTF(stderr, "Audio In Device: %s\n", opts->audio_in_dev);
    } else {
        DSD_FPRINTF(stderr, "Audio In/Out Device: %s\n", opts->audio_in_dev);
    }
    return dsd_audio_reconfigure_output_for_input_policy(opts);
}
