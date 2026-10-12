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
#include <dsd-neo/core/audio_activity.h>
#include <dsd-neo/core/audio_filters.h>
#include <dsd-neo/core/audio_sink.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/dibit.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/string_utils.h>
#include <dsd-neo/core/voice_chain.h>
#include <dsd-neo/dsp/resampler.h>
#include <dsd-neo/platform/audio.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/platform/posix_compat.h>
#include <dsd-neo/platform/sockets.h>
#include <dsd-neo/runtime/input_failure.h>
#include <dsd-neo/runtime/log.h>
#include <dsd-neo/runtime/net_audio_input_hooks.h>
#include <dsd-neo/runtime/udp_audio_hooks.h>
#include <errno.h>
#include <math.h>
#include <sndfile.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

void
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
    /* The streams below go; what was reported about them goes with them. */
    dsd_audio_sink_device_reset();
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

/* Device, application name and async policy shared by every local output stream. */
static void
dsd_audio_output_params_init(const dsd_opts* opts, dsd_audio_params* params) {
    DSD_MEMSET(params, 0, sizeof(*params));
    params->device = (opts->pa_output_idx[0] != '\0') ? opts->pa_output_idx : NULL;
    params->app_name = "DSD-neo";
    params->async_output = dsd_audio_should_use_async_output(opts);
    params->bits_per_sample = 16;
}

/* Raw/analog stream: analog monitor, ProVoice and the -8 source monitor write here. */
static dsd_audio_stream*
dsd_audio_open_raw_output_stream(const dsd_opts* opts, dsd_audio_params* params) {
    params->sample_rate = dsd_opts_analog_sink_rate_hz(opts);
    params->channels = opts->pulse_raw_out_channels;
    return dsd_audio_open_output(params);
}

/* Main digital voice stream. */
static dsd_audio_stream*
dsd_audio_open_digital_output_stream(const dsd_opts* opts, dsd_audio_params* params) {
    params->sample_rate = opts->pulse_digi_rate_out;
    params->channels = opts->pulse_digi_out_channels;
    return dsd_audio_open_output(params);
}

int
openAudioOutput(dsd_opts* opts) {
    dsd_audio_params params;
    dsd_audio_output_params_init(opts, &params);

    /* Open raw/analog output stream for ProVoice or analog monitor mode */
    if (opts->frame_provoice == 1 || opts->monitor_input_audio == 1) {
        opts->audio_raw_out = dsd_audio_open_raw_output_stream(opts, &params);
        if (!opts->audio_raw_out) {
            LOG_ERROR("Failed to open raw audio output: %s", dsd_audio_get_error());
            return -1;
        }
    }

    /* Open main digital audio output stream (unless in analog-only mode) */
    if (opts->analog_only == 0) {
        opts->audio_out_stream = dsd_audio_open_digital_output_stream(opts, &params);
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

/* A runtime family switch that cannot open its sink leaves that family silent; say so once, not on every switch. */
static int g_ensure_raw_failure_logged;
static int g_ensure_digital_failure_logged;
static int g_ensure_udp_analog_failure_logged;

/* A muted local-device session opens no stream here: unmuting closes and reopens every device sink the current mode
 * needs (openAudioOutput()). */
static int
dsd_audio_ensure_is_device_output(const dsd_opts* opts) {
    return (opts->audio_out == 1 && opts->audio_out_type == 0) ? 1 : 0;
}

/* UDP output opens its sockets muted or not. Unmuting reopens local devices only, so a socket skipped while muted would
 * stay closed for the rest of the session; opening one makes no sound, and the writers send nothing while muted. */
static int
dsd_audio_ensure_is_udp_output(const dsd_opts* opts) {
    return opts->audio_out_type == 8 ? 1 : 0;
}

static int
dsd_audio_ensure_raw_output(dsd_opts* opts) {
    if (opts->audio_raw_out) {
        return 0;
    }
    dsd_audio_params params;
    dsd_audio_output_params_init(opts, &params);
    opts->audio_raw_out = dsd_audio_open_raw_output_stream(opts, &params);
    if (!opts->audio_raw_out) {
        if (!g_ensure_raw_failure_logged) {
            LOG_ERROR("Failed to open raw audio output: %s\n", dsd_audio_get_error());
            g_ensure_raw_failure_logged = 1;
        }
        return -1;
    }
    g_ensure_raw_failure_logged = 0;
    opts->audio_output_async_policy = params.async_output ? 1 : 0;
    return 0;
}

/* With UDP output the analog monitor, ProVoice and the -8 source monitor go to port + 2. Session start opens that
 * socket only when one of them is on at start. */
static int
dsd_audio_ensure_udp_analog_output(dsd_opts* opts) {
    if (opts->udp_sockfdA != DSD_INVALID_SOCKET) {
        return 0;
    }
    if (dsd_udp_audio_hook_connect_analog(opts) != 0) {
        if (!g_ensure_udp_analog_failure_logged) {
            LOG_ERROR("Failed to open the UDP analog audio output on %s:%d\n", opts->udp_hostname,
                      opts->udp_portno + 2);
            g_ensure_udp_analog_failure_logged = 1;
        }
        return -1;
    }
    g_ensure_udp_analog_failure_logged = 0;
    return 0;
}

/* The sink the raw/analog stream writes to for this output type, if it has one. */
static int
dsd_audio_ensure_raw_sink(dsd_opts* opts) {
    if (dsd_audio_ensure_is_device_output(opts)) {
        return dsd_audio_ensure_raw_output(opts);
    }
    if (dsd_audio_ensure_is_udp_output(opts)) {
        return dsd_audio_ensure_udp_analog_output(opts);
    }
    return 0;
}

int
dsd_audio_ensure_analog_output(dsd_opts* opts) {
    if (!opts) {
        return -1;
    }
    return dsd_audio_ensure_raw_sink(opts);
}

static int
dsd_audio_ensure_digital_stream(dsd_opts* opts) {
    if (opts->audio_out_stream) {
        return 0;
    }
    dsd_audio_params params;
    dsd_audio_output_params_init(opts, &params);
    opts->audio_out_stream = dsd_audio_open_digital_output_stream(opts, &params);
    if (!opts->audio_out_stream) {
        if (!g_ensure_digital_failure_logged) {
            LOG_ERROR("Failed to open audio output: %s\n", dsd_audio_get_error());
            g_ensure_digital_failure_logged = 1;
        }
        return -1;
    }
    g_ensure_digital_failure_logged = 0;
    opts->audio_output_async_policy = params.async_output ? 1 : 0;
    return 0;
}

int
dsd_audio_ensure_digital_output(dsd_opts* opts) {
    if (!opts) {
        return -1;
    }
    int rc = 0;
    /* UDP output opened the digital socket at session start whatever the mode; only a local device may lack it. */
    if (dsd_audio_ensure_is_device_output(opts) && dsd_audio_ensure_digital_stream(opts) != 0) {
        rc = -1;
    }
    /* openAudioOutput() and the UDP setup also open the raw sink for ProVoice and the -8 source monitor. */
    if ((opts->frame_provoice == 1 || opts->monitor_input_audio == 1) && dsd_audio_ensure_raw_sink(opts) != 0) {
        rc = -1;
    }
    return rc;
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
    /* Every Pulse open is a new stream, whatever asked for it (a config switch, the end of a playback, a file that
       ran out): the PCM noise squelch forgets the old stream's references (issue #628). */
    dsd_opts_note_pcm_stream(opts);
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

/* One slot's gain state: the auto gain's level and peak history, and the fixed `-g` setting (issue #653). */
typedef struct {
    float* gain;
    float* history;
    float** history_p;
    int* history_idx;
    float fixed;
} dsd_voice_gain_slot;

static dsd_voice_gain_slot
dsd_voice_gain_slot_of(const dsd_opts* opts, dsd_state* state, int slot) {
    dsd_voice_gain_slot g;
    if (slot == 1) {
        g.gain = &state->aout_gainR;
        g.history = state->aout_max_bufR;
        g.history_p = &state->aout_max_buf_pR;
        g.history_idx = &state->aout_max_buf_idxR;
        g.fixed = opts->audio_gainR;
    } else {
        g.gain = &state->aout_gain;
        g.history = state->aout_max_buf;
        g.history_p = &state->aout_max_buf_p;
        g.history_idx = &state->aout_max_buf_idx;
        g.fixed = opts->audio_gain;
    }
    return g;
}

enum {
    DSD_VOICE_GAIN_HISTORY = 25, /* frames of peak history the auto gain looks back over: 500 ms */
};

/* The peak of @p frame together with the slot's last DSD_VOICE_GAIN_HISTORY - 1 frame peaks, after recording this
   frame's peak in the history. The ring is walked by index, so a state whose pointer was never set still works. */
static float
dsd_voice_gain_history_peak(const dsd_voice_gain_slot* g, const float* frame) {
    float block_max = 0.0F;
    for (int n = 0; n < DSD_VOICE_FRAME_SAMPLES; n++) {
        const float a = fabsf(frame[n]);
        if (a > block_max) {
            block_max = a;
        }
    }
    int idx = *g->history_idx;
    if (idx < 0 || idx >= DSD_VOICE_GAIN_HISTORY) {
        idx = 0;
    }
    g->history[idx] = block_max;
    idx++;
    if (idx >= DSD_VOICE_GAIN_HISTORY) {
        idx = 0;
    }
    *g->history_idx = idx;
    *g->history_p = &g->history[idx];

    float max = block_max;
    for (int i = 0; i < DSD_VOICE_GAIN_HISTORY; i++) {
        if (g->history[i] > max) {
            max = g->history[i];
        }
    }
    return max;
}

/* The auto gain: normalize the history's peak to 30000, at most x50; a louder peak cuts the gain at once, a quieter one
   lets it rise by at most 5% per frame, ramped across the frame. Returns the per-sample step. */
static float
dsd_voice_auto_gain_step(const dsd_voice_gain_slot* g, const float* frame) {
    // Only a level no gain law can leave (a zeroed or corrupt state) starts over at unity; a legitimate gain below 1,
    // set by a peak above 30000, keeps its 5% release.
    if (!(*g->gain > 0.0F) || !(*g->gain <= 50.0F)) {
        *g->gain = 1.0F;
    }
    const float max = dsd_voice_gain_history_peak(g, frame);
    float gainfactor = (max > 0.0F) ? (30000.0F / max) : 50.0F;
    if (gainfactor < *g->gain) {
        *g->gain = gainfactor;
        return 0.0F;
    }
    if (gainfactor > 50.0F) {
        gainfactor = 50.0F;
    }
    float delta = gainfactor - *g->gain;
    if (delta > (0.05F * *g->gain)) {
        delta = 0.05F * *g->gain;
    }
    return delta / (float)DSD_VOICE_FRAME_SAMPLES;
}

static void
dsd_voice_gain_apply(const dsd_opts* opts, dsd_state* state, int slot, float* frame) {
    dsd_voice_gain_slot g = dsd_voice_gain_slot_of(opts, state, slot);
    if (g.fixed < 0.0F) {
        return;
    }
    if (g.fixed > 0.0F) {
        // A fixed gain is a plain multiplier, the same in both output formats.
        *g.gain = g.fixed;
        for (int n = 0; n < DSD_VOICE_FRAME_SAMPLES; n++) {
            frame[n] *= g.fixed;
        }
        return;
    }
    const float step = dsd_voice_auto_gain_step(&g, frame);
    for (int n = 0; n < DSD_VOICE_FRAME_SAMPLES; n++) {
        frame[n] *= *g.gain + ((float)n * step);
    }
    *g.gain += (float)DSD_VOICE_FRAME_SAMPLES * step;
}

/* The slot's filter state starts over at a reception boundary: a new call on the slot (one nonzero epoch to another)
   or a frame of another protocol family. Frames before a call is known, and a call's first frames after them, keep
   it, so no step is cut into a running stream. */
static void
dsd_voice_hpf_follow_reception(dsd_state* state, int slot) {
    dsd_voice_hpf_state* f = &state->voice_hpf[slot];
    dsd_call_snapshot call;
    uint64_t epoch = 0;
    // The protocol the frame was dispatched under, as dsd_mbe_out_frame_protocol() reads it: YSF and scrambled dPMR
    // decode under a borrowed synctype but stay their own.
    int protocol = state->frame_dispatch_active ? state->frame_dispatch_synctype : state->synctype;
    if (dsd_call_state_get(state, (uint8_t)slot, &call) > 0 && call.phase == DSD_CALL_PHASE_ACTIVE) {
        epoch = call.epoch;
        protocol = call.protocol;
    }
    const int family = dsd_call_state_protocol_family(protocol);
    const int new_call = epoch != 0U && f->call_epoch != 0U && epoch != f->call_epoch;
    const int new_family = family != 0 && f->protocol_family != 0 && family != f->protocol_family;
    if (new_call || new_family) {
        dsd_voice_hpf_reset(state, slot);
    }
    if (epoch != 0U) {
        f->call_epoch = epoch;
    }
    if (family != 0) {
        f->protocol_family = family;
    }
}

void
dsd_voice_chain_run(const dsd_opts* opts, dsd_state* state, int slot, float* frame) {
    if (!opts || !state || !frame || slot < 0 || slot > 1) {
        return;
    }
    dsd_voice_hpf_follow_reception(state, slot);
    if (opts->use_hpf_d == 1) {
        dsd_voice_hpf_process(state, slot, frame, DSD_VOICE_FRAME_SAMPLES);
    } else {
        dsd_voice_hpf_reset(state, slot);
    }
    dsd_voice_gain_apply(opts, state, slot, frame);
    for (int n = 0; n < DSD_VOICE_FRAME_SAMPLES; n++) {
        frame[n] = dsd_audio_clamp_pcm16_f32(frame[n]);
    }
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

void
dsd_voice_stage_short_left(const dsd_opts* opts, dsd_state* state) {
    // This count describes only this frame, even when its predecessor reached no mixer (issue #654).
    state->audio_out_idx = 0;
    if (opts->pulse_digi_rate_out > 8000) {
        dsd_audio_emit_upsampled_left(opts, state);
        return;
    }
    dsd_audio_emit_native_left(state);
}

void
processAudio(const dsd_opts* opts, dsd_state* state) {
    dsd_voice_chain_run(opts, state, 0, state->audio_out_temp_buf);
    dsd_voice_stage_short_left(opts, state);
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

void
dsd_voice_stage_short_right(const dsd_opts* opts, dsd_state* state) {
    if (opts->pulse_digi_rate_out > 8000) {
        dsd_audio_emit_upsampled_right(opts, state);
        return;
    }
    dsd_audio_emit_native_right(state);
}

void
processAudioR(const dsd_opts* opts, dsd_state* state) {
    dsd_voice_chain_run(opts, state, 1, state->audio_out_temp_bufR);
    dsd_voice_stage_short_right(opts, state);
}

// Writes the mono (left) frame in audio_out_temp_buf, clamped to 16 bits in place, as one mono block of @p file.
static void
dsd_audio_write_mono_frame_wav(SNDFILE* file, dsd_state* state, const char* context) {
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

    dsd_audio_write_wav_short_block(file, aout_buf, 160, context);
}

void
writeSynthesizedVoice(const dsd_opts* opts, dsd_state* state) {
    dsd_audio_write_mono_frame_wav(opts->wav_out_f, state, "writeSynthesizedVoice");
}

void
dsd_write_mono_voice_to_slot_wav(dsd_opts* opts, dsd_state* state, int slot) {
    if (!opts || !state) {
        return;
    }
    if (slot == 1) {
        dsd_audio_write_mono_frame_wav(opts->wav_out_fR, state, "dsd_write_mono_voice_to_slot_wav slot 2");
    } else {
        dsd_audio_write_mono_frame_wav(opts->wav_out_f, state, "dsd_write_mono_voice_to_slot_wav slot 1");
    }
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

// The legacy short output's pending samples (state->audio_out_idx of them) to the output, then none pending.
static void
dsd_audio_legacy_write_pending(const dsd_opts* opts, dsd_state* state) {
    dsd_output_s16_block(opts, state, state->audio_out_buf_p - state->audio_out_idx, (size_t)state->audio_out_idx, 1);
    state->audio_out_idx = 0;
}

void
// cppcheck-suppress constParameterPointer -- signature fixed by dsd-neo/core/audio.h
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

    // A muted call's pending samples are dropped, as on every other output path.
    if (dsd_audio_mono_output_muted(opts, state)) {
        state->audio_out_idx = 0;
        goto end_psv;
    }

    if (state->audio_out_idx > opts->delay) {
        // Stamped only when an output below receives the samples: the local stream while open, the raw fd or UDP.
        if (dsd_audio_activity_armed() && dsd_audio_activity_output_plays(opts, opts->audio_out_stream, 1)) {
            dsd_audio_activity_note();
        }
        dsd_audio_legacy_write_pending(opts, state);
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

void
closeAudioInDevice(dsd_opts* opts) {
    if (opts == NULL) {
        return;
    }

    closeAudioInput(opts);
    dsd_audio_release_input_sources(opts);
    if (opts->tcp_sockfd != DSD_INVALID_SOCKET) {
        dsd_socket_close(opts->tcp_sockfd);
        opts->tcp_sockfd = DSD_INVALID_SOCKET;
    }
}

void
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
dsd_audio_select_radio_input(dsd_opts* opts) {
#ifdef USE_RADIO
    opts->audio_in_type = AUDIO_IN_RTL;
    return 1;
#else
    LOG_ERROR("Radio input '%s' requires a build with radio pipeline support.\n", opts->audio_in_dev);
    return -1;
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
        || dsd_opts_audio_in_dev_is_soapy_spec(opts->audio_in_dev)
        || dsd_opts_audio_in_dev_is_airspy_spec(opts->audio_in_dev)) {
        return dsd_audio_select_radio_input(opts);
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
        dsd_input_failure_report(DSD_INPUT_FAILURE_FILE, errno);
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
        dsd_input_failure_report(DSD_INPUT_FAILURE_FILE, errno);
        return -1;
    }
    return 0;
}

void
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
        dsd_input_failure_report(DSD_INPUT_FAILURE_FILE, errno);
        return -1;
    }
    if (!dsd_stat_is_regular(&stat_buf)) {
        LOG_ERROR("Error, %s input path is not a regular file: %s\n", label, opts->audio_in_dev);
        dsd_input_failure_report(DSD_INPUT_FAILURE_FILE, errno);
        return -1;
    }

    opts->symbolfile = dsd_fopen_existing_regular_file(opts->audio_in_dev, "rb");
    if (opts->symbolfile == NULL) {
        LOG_ERROR("Error, couldn't open %s file %s\n", label, opts->audio_in_dev);
        dsd_input_failure_report(DSD_INPUT_FAILURE_FILE, errno);
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
        dsd_input_failure_report(DSD_INPUT_FAILURE_FILE, errno);
        return -1;
    }
    if (!dsd_stat_is_regular(&stat_buf)) {
        LOG_ERROR("Error, couldn't open input file.\n");
        dsd_input_failure_report(DSD_INPUT_FAILURE_FILE, errno);
        return -1;
    }

    opts->audio_in_type = AUDIO_IN_WAV;
    const int raw_sample_rate = opts->wav_sample_rate;
    int configured_file_sample_rate = dsd_opts_requested_file_sample_rate(opts);
    int active_sample_rate = configured_file_sample_rate;
    int opened_as_container = 0;
    if (dsd_audio_open_mono_file_input(opts->audio_in_dev, configured_file_sample_rate, &opts->audio_in_file,
                                       &opts->audio_in_file_info, &active_sample_rate, &opened_as_container)
        != 0) {
        LOG_ERROR("Error, couldn't open input file %s\n", opts->audio_in_dev);
        dsd_input_failure_report(DSD_INPUT_FAILURE_FILE, errno);
        return -1;
    }

    if (active_sample_rate != opts->wav_sample_rate) {
        if (opened_as_container) {
            LOG_INFO("NOTICE: WAV header sample rate %d Hz overrides configured %d Hz for %s\n", active_sample_rate,
                     configured_file_sample_rate, opts->audio_in_dev);
        }
        dsd_audio_apply_input_sample_rate(opts, state, old_effective_input_rate, active_sample_rate);
    }
    /* A header rate holds for this file only: a switch away from it opens the next PCM input at the raw rate again
       (issue #634). */
    opts->wav_header_replaced_rate =
        (opened_as_container && active_sample_rate != raw_sample_rate) ? raw_sample_rate : 0;
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
    opts->wav_header_replaced_rate = 0;

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
