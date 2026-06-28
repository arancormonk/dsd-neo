// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/dibit.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/platform/posix_compat.h>
#include <errno.h>
#include <math.h>
#include <sndfile.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include "../../src/core/audio/dsd_audio_internal.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

enum { DSD_AUDIO_TEST_PATH_MAX = 512 };

static int
expect_true(const char* label, int cond) {
    if (!cond) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", label);
        return 1;
    }
    return 0;
}

static int
expect_int_eq(const char* label, int got, int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "FAIL: %s (got %d want %d)\n", label, got, want);
        return 1;
    }
    return 0;
}

static int
expect_float_close(const char* label, float got, float want, float tol) {
    if (fabsf(got - want) > tol) {
        DSD_FPRINTF(stderr, "FAIL: %s (got %.6f want %.6f tol %.6f)\n", label, got, want, tol);
        return 1;
    }
    return 0;
}

static int
create_temp_wav_path(const char* prefix, char* out_path, size_t out_path_sz) {
    if (DSD_SNPRINTF(out_path, out_path_sz, "/tmp/%s_XXXXXX", prefix) >= (int)out_path_sz) {
        DSD_FPRINTF(stderr, "FAIL: temp path too long for %s\n", prefix);
        return 1;
    }
    int fd = dsd_mkstemp(out_path);
    if (fd < 0) {
        DSD_FPRINTF(stderr, "FAIL: dsd_mkstemp failed for %s\n", prefix);
        return 1;
    }
    (void)dsd_close(fd);
    (void)remove(out_path);
    return 0;
}

static int
create_temp_file_fd(const char* prefix, char* out_path, size_t out_path_sz) {
    if (DSD_SNPRINTF(out_path, out_path_sz, "/tmp/%s_XXXXXX", prefix) >= (int)out_path_sz) {
        DSD_FPRINTF(stderr, "FAIL: temp file path too long for %s\n", prefix);
        return -1;
    }

    int fd = dsd_mkstemp(out_path);
    if (fd < 0) {
        DSD_FPRINTF(stderr, "FAIL: dsd_mkstemp failed for %s\n", prefix);
    }
    return fd;
}

static int
create_temp_file_with_suffix(const char* prefix, const char* suffix, char* out_path, size_t out_path_sz) {
    char base_path[DSD_AUDIO_TEST_PATH_MAX] = {0};
    int fd = create_temp_file_fd(prefix, base_path, sizeof base_path);
    if (fd < 0) {
        return -1;
    }

    if (DSD_SNPRINTF(out_path, out_path_sz, "%s%s", base_path, suffix) >= (int)out_path_sz) {
        DSD_FPRINTF(stderr, "FAIL: suffixed temp path too long for %s\n", prefix);
        (void)dsd_close(fd);
        (void)remove(base_path);
        return -1;
    }
    if (rename(base_path, out_path) != 0) {
        DSD_FPRINTF(stderr, "FAIL: rename temp file to %s failed: %s\n", out_path, strerror(errno));
        (void)dsd_close(fd);
        (void)remove(base_path);
        return -1;
    }
    return fd;
}

static int
create_temp_dir_with_suffix(const char* prefix, const char* suffix, char* out_path, size_t out_path_sz) {
    char base_path[DSD_AUDIO_TEST_PATH_MAX] = {0};
    if (DSD_SNPRINTF(base_path, sizeof base_path, "/tmp/%s_XXXXXX", prefix) >= (int)sizeof base_path) {
        DSD_FPRINTF(stderr, "FAIL: temp dir path too long for %s\n", prefix);
        return 1;
    }
    if (dsd_mkdtemp(base_path) == NULL) {
        DSD_FPRINTF(stderr, "FAIL: dsd_mkdtemp failed for %s\n", prefix);
        return 1;
    }
    if (DSD_SNPRINTF(out_path, out_path_sz, "%s%s", base_path, suffix) >= (int)out_path_sz) {
        DSD_FPRINTF(stderr, "FAIL: suffixed temp dir path too long for %s\n", prefix);
        (void)remove(base_path);
        return 1;
    }
    if (rename(base_path, out_path) != 0) {
        DSD_FPRINTF(stderr, "FAIL: rename temp dir to %s failed: %s\n", out_path, strerror(errno));
        (void)remove(base_path);
        return 1;
    }
    return 0;
}

static SNDFILE*
open_temp_wav_writer(const char* path, int channels) {
    SF_INFO info;
    DSD_MEMSET(&info, 0, sizeof info);
    info.samplerate = 8000;
    info.channels = channels;
    info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;
    return sf_open(path, SFM_WRITE, &info);
}

static int
read_wav_samples(const char* path, int channels, short* out, size_t out_count) {
    SF_INFO info;
    DSD_MEMSET(&info, 0, sizeof info);
    SNDFILE* file = sf_open(path, SFM_READ, &info);
    if (!file) {
        DSD_FPRINTF(stderr, "FAIL: sf_open read failed for %s: %s\n", path, sf_strerror(NULL));
        return 1;
    }

    int rc = 0;
    rc |= expect_int_eq("wav read channel count", info.channels, channels);
    sf_count_t nread = sf_read_short(file, out, (sf_count_t)out_count);
    rc |= expect_int_eq("wav read sample count", (int)nread, (int)out_count);
    sf_close(file);
    return rc;
}

static int
read_short_file_samples(const char* path, short* out, size_t out_count) {
    FILE* file = dsd_fopen_existing_regular_file(path, "rb");
    if (!file) {
        DSD_FPRINTF(stderr, "FAIL: fopen read failed for %s\n", path);
        return 1;
    }

    size_t nread = fread(out, sizeof(short), out_count, file);
    fclose(file);
    return expect_int_eq("short file read sample count", (int)nread, (int)out_count);
}

static int
test_agsm_applies_gain_to_entire_block(void) {
    static dsd_opts opts = {0};
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (!state) {
        DSD_FPRINTF(stderr, "FAIL: alloc state\n");
        return 1;
    }

    short in[64];
    for (int i = 0; i < 64; i++) {
        in[i] = 1000;
    }

    agsm(&opts, state, in, 64);

    int rc = 0;
    /* nom/max = 4.8, clamped to 3.0 -> all samples should scale to 3000 */
    for (int i = 0; i < 64; i++) {
        char label[64];
        DSD_SNPRINTF(label, sizeof label, "agsm scales sample %d", i);
        rc |= expect_int_eq(label, in[i], 3000);
    }
    rc |= expect_float_close("agsm stores applied gain", state->aout_gainA, 3.0f, 1e-6f);

    free(state);
    return rc;
}

static int
test_agsm_handles_silence_without_invalid_values(void) {
    static dsd_opts opts = {0};
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (!state) {
        DSD_FPRINTF(stderr, "FAIL: alloc state\n");
        return 1;
    }

    short in[32] = {0};
    agsm(&opts, state, in, 32);

    int rc = 0;
    for (int i = 0; i < 32; i++) {
        char label[64];
        DSD_SNPRINTF(label, sizeof label, "agsm keeps silent sample %d", i);
        rc |= expect_int_eq(label, in[i], 0);
    }
    rc |= expect_true("agsm gain state is finite", isfinite(state->aout_gainA) ? 1 : 0);

    free(state);
    return rc;
}

static int
test_audio_apply_gain_f32_multiplies_block(void) {
    float in[4] = {1.0f, -2.0f, 0.5f, 0.0f};
    audio_apply_gain_f32(in, 4, 2.5f);

    int rc = 0;
    rc |= expect_float_close("gain f32 sample 0", in[0], 2.5f, 1e-6f);
    rc |= expect_float_close("gain f32 sample 1", in[1], -5.0f, 1e-6f);
    rc |= expect_float_close("gain f32 sample 2", in[2], 1.25f, 1e-6f);
    rc |= expect_float_close("gain f32 sample 3", in[3], 0.0f, 1e-6f);

    audio_apply_gain_f32(NULL, 4, 3.0f);
    return rc;
}

static int
test_audio_mono_to_stereo_duplicates_samples(void) {
    const float in_f[3] = {1.0f, -2.5f, 0.25f};
    float out_f[6] = {0.0f};
    audio_mono_to_stereo_f32(in_f, out_f, 3);

    const short in_s[3] = {-4, 0, 1234};
    short out_s[6] = {0};
    audio_mono_to_stereo_s16(in_s, out_s, 3);

    int rc = 0;
    for (int i = 0; i < 3; i++) {
        const size_t out_idx = (size_t)i * 2U;
        char label[64];
        DSD_SNPRINTF(label, sizeof label, "mono f32 left %d", i);
        rc |= expect_float_close(label, out_f[out_idx], in_f[i], 1e-6f);
        DSD_SNPRINTF(label, sizeof label, "mono f32 right %d", i);
        rc |= expect_float_close(label, out_f[out_idx + 1U], in_f[i], 1e-6f);
        DSD_SNPRINTF(label, sizeof label, "mono s16 left %d", i);
        rc |= expect_int_eq(label, out_s[out_idx], in_s[i]);
        DSD_SNPRINTF(label, sizeof label, "mono s16 right %d", i);
        rc |= expect_int_eq(label, out_s[out_idx + 1U], in_s[i]);
    }

    float guard_f[2] = {7.0f, 8.0f};
    audio_mono_to_stereo_f32(NULL, guard_f, 1);
    audio_mono_to_stereo_f32(in_f, NULL, 1);
    rc |= expect_float_close("mono f32 null input guard", guard_f[0], 7.0f, 1e-6f);
    rc |= expect_float_close("mono f32 null output guard", guard_f[1], 8.0f, 1e-6f);

    short guard_s[2] = {7, 8};
    audio_mono_to_stereo_s16(NULL, guard_s, 1);
    audio_mono_to_stereo_s16(in_s, NULL, 1);
    rc |= expect_int_eq("mono s16 null input guard", guard_s[0], 7);
    rc |= expect_int_eq("mono s16 null output guard", guard_s[1], 8);
    return rc;
}

static int
test_audio_mix_helpers_apply_channel_gates(void) {
    const float left_f[2] = {1.0f, 2.0f};
    const float right_f[2] = {10.0f, 20.0f};
    float stereo_f[4] = {-1.0f, -1.0f, -1.0f, -1.0f};
    audio_mix_interleave_stereo_f32(left_f, right_f, 2, 0, 1, stereo_f);

    const short left_s[2] = {1, 2};
    const short right_s[2] = {10, 20};
    short stereo_s[4] = {-1, -1, -1, -1};
    audio_mix_interleave_stereo_s16(left_s, right_s, 2, 1, 0, stereo_s);

    float mono[2] = {-1.0f, -1.0f};
    audio_mix_mono_from_slots_f32(left_f, right_f, 2, 1, 1, mono);

    int rc = 0;
    rc |= expect_float_close("mix f32 left 0", stereo_f[0], 1.0f, 1e-6f);
    rc |= expect_float_close("mix f32 muted right 0", stereo_f[1], 0.0f, 1e-6f);
    rc |= expect_float_close("mix f32 left 1", stereo_f[2], 2.0f, 1e-6f);
    rc |= expect_float_close("mix f32 muted right 1", stereo_f[3], 0.0f, 1e-6f);
    rc |= expect_int_eq("mix s16 muted left 0", stereo_s[0], 0);
    rc |= expect_int_eq("mix s16 right 0", stereo_s[1], 10);
    rc |= expect_int_eq("mix s16 muted left 1", stereo_s[2], 0);
    rc |= expect_int_eq("mix s16 right 1", stereo_s[3], 20);
    rc |= expect_float_close("mono average 0", mono[0], 5.5f, 1e-6f);
    rc |= expect_float_close("mono average 1", mono[1], 11.0f, 1e-6f);

    audio_mix_mono_from_slots_f32(left_f, right_f, 2, 1, 0, mono);
    rc |= expect_float_close("mono left only", mono[0], 1.0f, 1e-6f);
    audio_mix_mono_from_slots_f32(left_f, right_f, 2, 0, 1, mono);
    rc |= expect_float_close("mono right only", mono[0], 10.0f, 1e-6f);
    audio_mix_mono_from_slots_f32(left_f, right_f, 2, 0, 0, mono);
    rc |= expect_float_close("mono neither slot", mono[0], 0.0f, 1e-6f);

    float guard_f[2] = {7.0f, 8.0f};
    audio_mix_interleave_stereo_f32(NULL, right_f, 1, 0, 0, guard_f);
    rc |= expect_float_close("mix f32 null guard left", guard_f[0], 7.0f, 1e-6f);
    rc |= expect_float_close("mix f32 null guard right", guard_f[1], 8.0f, 1e-6f);
    return rc;
}

static int
test_upsample_repeats_into_selected_slot_buffer(void) {
    static dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));

    static float left[6];
    static float right[6];
    for (int i = 0; i < 6; i++) {
        left[i] = -1.0f;
        right[i] = -2.0f;
    }
    state.audio_out_float_buf_p = left;
    state.audio_out_float_buf_pR = right;
    upsample(&state, 1.25f);

    int rc = 0;
    for (int i = 0; i < 6; i++) {
        char label[64];
        DSD_SNPRINTF(label, sizeof label, "upsample left repeat %d", i);
        rc |= expect_float_close(label, left[i], 1.25f, 1e-6f);
        DSD_SNPRINTF(label, sizeof label, "upsample right unchanged %d", i);
        rc |= expect_float_close(label, right[i], -2.0f, 1e-6f);
    }

    DSD_MEMSET(&state, 0, sizeof(state));
    for (int i = 0; i < 6; i++) {
        left[i] = -1.0f;
        right[i] = -2.0f;
    }
    state.audio_out_float_buf_p = left;
    state.audio_out_float_buf_pR = right;
    state.dmr_stereo = 1;
    state.currentslot = 1;
    upsample(&state, -0.5f);

    for (int i = 0; i < 6; i++) {
        char label[64];
        DSD_SNPRINTF(label, sizeof label, "upsample stereo left unchanged %d", i);
        rc |= expect_float_close(label, left[i], -1.0f, 1e-6f);
        DSD_SNPRINTF(label, sizeof label, "upsample stereo right repeat %d", i);
        rc |= expect_float_close(label, right[i], -0.5f, 1e-6f);
    }

    upsample(NULL, 9.0f);
    state.audio_out_float_buf_pR = NULL;
    upsample(&state, 9.0f);
    return rc;
}

static int
test_manual_and_float_autogain_helpers(void) {
    static dsd_opts opts = {0};
    static dsd_state state = {0};
    int rc = 0;

    opts.audio_gainA = 40.0f;
    short short_samples[3] = {100, -200, 0};
    analog_gain(&opts, &state, short_samples, 3);
    rc |= expect_int_eq("analog gain short positive", short_samples[0], 200);
    rc |= expect_int_eq("analog gain short negative", short_samples[1], -400);
    rc |= expect_int_eq("analog gain short zero", short_samples[2], 0);

    opts.audio_gainA = 50.0f;
    opts.audio_in_type = AUDIO_IN_RTL;
    float rtl_samples[2] = {0.1f, -0.2f};
    analog_gain_f(&opts, &state, rtl_samples, 2);
    rc |= expect_float_close("analog gain rtl positive", rtl_samples[0], 1200.0f, 1e-3f);
    rc |= expect_float_close("analog gain rtl negative", rtl_samples[1], -2400.0f, 1e-3f);

    opts.audio_in_type = AUDIO_IN_WAV;
    float wav_samples[2] = {100.0f, -200.0f};
    analog_gain_f(&opts, &state, wav_samples, 2);
    rc |= expect_float_close("analog gain wav positive", wav_samples[0], 250.0f, 1e-3f);
    rc |= expect_float_close("analog gain wav negative", wav_samples[1], -500.0f, 1e-3f);

    float auto_samples[2] = {1.0f, -0.5f};
    agsm_f(&opts, &state, auto_samples, 2);
    rc |= expect_float_close("agsm_f positive", auto_samples[0], 4800.0f, 1e-3f);
    rc |= expect_float_close("agsm_f negative", auto_samples[1], -2400.0f, 1e-3f);
    rc |= expect_float_close("agsm_f stores gain", state.aout_gainA, 4800.0f, 1e-3f);

    float quiet_samples[2] = {0.0f, 0.0f};
    agsm_f(&opts, &state, quiet_samples, 2);
    rc |= expect_float_close("agsm_f keeps silence", quiet_samples[0], 0.0f, 1e-6f);
    rc |= expect_float_close("agsm_f caps silent gain", state.aout_gainA, 6000.0f, 1e-3f);
    return rc;
}

static int
test_agf_scales_nonzero_float_block_and_updates_slot_gain(void) {
    static dsd_opts opts = {0};
    static dsd_state state = {0};
    opts.audio_gain = 25.0f;
    state.aout_gain = 49.0f;

    float samp[160];
    for (int i = 0; i < 160; i++) {
        samp[i] = 384.0f;
    }
    agf(&opts, &state, samp, 0);

    int rc = 0;
    rc |= expect_float_close("agf first clipped sample", samp[0], 0.72f, 1e-5f);
    rc |= expect_true("agf updates left gain", state.aout_gain < 49.0f);

    float silent[160] = {0.0f};
    float before_gain = state.aout_gain;
    agf(&opts, &state, silent, 0);
    rc |= expect_float_close("agf silence leaves gain", state.aout_gain, before_gain, 1e-6f);
    return rc;
}

static int
test_process_audio_native_left_clamps_and_tracks_output(void) {
    static dsd_opts opts = {0};
    static dsd_state state = {0};
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&state, 0, sizeof state);

    opts.audio_gain = 1.0f;
    opts.pulse_digi_rate_out = 8000;
    state.aout_gain = 2.0f;
    static short out[200];
    static float out_float[200];
    DSD_MEMSET(out, 0, sizeof(out));
    DSD_MEMSET(out_float, 0, sizeof(out_float));
    state.audio_out_buf = out;
    state.audio_out_float_buf = out_float;
    state.audio_out_temp_buf[0] = 20000.0f;
    state.audio_out_temp_buf[1] = -20000.0f;
    state.audio_out_temp_buf[2] = 100.0f;
    state.audio_out_buf_p = state.audio_out_buf;
    state.audio_out_float_buf_p = state.audio_out_float_buf;

    processAudio(&opts, &state);

    int rc = 0;
    rc |= expect_int_eq("left clamp positive output", out[0], 32767);
    rc |= expect_int_eq("left clamp negative output", out[1], -32768);
    rc |= expect_int_eq("left scaled sample output", out[2], 200);
    rc |= expect_int_eq("left mirror positive", state.s_l[0], 32767);
    rc |= expect_int_eq("left mirror negative", state.s_l[1], -32768);
    rc |= expect_int_eq("left native index", state.audio_out_idx, 160);
    rc |= expect_int_eq("left native long index", state.audio_out_idx2, 160);
    rc |= expect_true("left output pointer advanced", state.audio_out_buf_p == state.audio_out_buf + 160);
    rc |= expect_float_close("left manual gain unchanged", state.aout_gain, 2.0f, 1e-6f);
    return rc;
}

static int
test_process_audio_native_right_clamps_and_tracks_output(void) {
    static dsd_opts opts = {0};
    static dsd_state state = {0};
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&state, 0, sizeof state);

    opts.audio_gainR = 1.0f;
    opts.pulse_digi_rate_out = 8000;
    state.aout_gainR = 3.0f;
    static short out[200];
    static float out_float[200];
    DSD_MEMSET(out, 0, sizeof(out));
    DSD_MEMSET(out_float, 0, sizeof(out_float));
    state.audio_out_bufR = out;
    state.audio_out_float_bufR = out_float;
    state.audio_out_temp_bufR[0] = 12000.0f;
    state.audio_out_temp_bufR[1] = -12000.0f;
    state.audio_out_temp_bufR[2] = 100.0f;
    state.audio_out_buf_pR = state.audio_out_bufR;
    state.audio_out_float_buf_pR = state.audio_out_float_bufR;

    processAudioR(&opts, &state);

    int rc = 0;
    rc |= expect_int_eq("right clamp positive output", out[0], 32767);
    rc |= expect_int_eq("right clamp negative output", out[1], -32768);
    rc |= expect_int_eq("right scaled sample output", out[2], 300);
    rc |= expect_int_eq("right mirror positive", state.s_r[0], 32767);
    rc |= expect_int_eq("right mirror negative", state.s_r[1], -32768);
    rc |= expect_int_eq("right native index", state.audio_out_idxR, 160);
    rc |= expect_int_eq("right native long index", state.audio_out_idx2R, 160);
    rc |= expect_true("right output pointer advanced", state.audio_out_buf_pR == state.audio_out_bufR + 160);
    rc |= expect_float_close("right manual gain unchanged", state.aout_gainR, 3.0f, 1e-6f);
    return rc;
}

static int
test_process_audio_auto_gain_ramps_from_peak_history(void) {
    static dsd_opts opts = {0};
    static dsd_state state = {0};
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&state, 0, sizeof state);

    opts.audio_gain = 0.0f;
    opts.pulse_digi_rate_out = 8000;
    state.aout_gain = 1.0f;
    state.aout_max_buf_p = state.aout_max_buf;
    static short out[200];
    static float out_float[200];
    DSD_MEMSET(out, 0, sizeof(out));
    DSD_MEMSET(out_float, 0, sizeof(out_float));
    state.audio_out_buf = out;
    state.audio_out_float_buf = out_float;
    for (int i = 0; i < 160; i++) {
        state.audio_out_temp_buf[i] = 1000.0f;
    }
    state.audio_out_buf_p = state.audio_out_buf;
    state.audio_out_float_buf_p = state.audio_out_float_buf;

    processAudio(&opts, &state);

    int rc = 0;
    rc |= expect_int_eq("auto gain first sample", out[0], 1000);
    rc |= expect_int_eq("auto gain last sample ramps", out[159], 1049);
    rc |= expect_float_close("auto gain capped ramp target", state.aout_gain, 1.05f, 1e-6f);
    rc |= expect_int_eq("auto gain peak history index", state.aout_max_buf_idx, 1);
    rc |= expect_float_close("auto gain peak history stores block", state.aout_max_buf[0], 1000.0f, 1e-6f);
    rc |= expect_int_eq("auto gain output index", state.audio_out_idx, 160);
    return rc;
}

static int
test_process_audio_right_auto_gain_tracks_peak_history(void) {
    static dsd_opts opts = {0};
    static dsd_state state = {0};
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&state, 0, sizeof state);

    opts.audio_gainR = 0.0f;
    opts.pulse_digi_rate_out = 8000;
    state.aout_gainR = 10.0f;
    state.aout_max_bufR[3] = 20000.0f;
    state.aout_max_buf_idxR = 24;
    state.aout_max_buf_pR = state.aout_max_bufR + 24;
    static short out[200];
    static float out_float[200];
    DSD_MEMSET(out, 0, sizeof(out));
    DSD_MEMSET(out_float, 0, sizeof(out_float));
    state.audio_out_bufR = out;
    state.audio_out_float_bufR = out_float;
    state.audio_out_temp_bufR[0] = 1000.0f;
    state.audio_out_temp_bufR[1] = -2000.0f;
    state.audio_out_temp_bufR[2] = 250.0f;
    state.audio_out_temp_bufR[3] = -4000.0f;
    state.audio_out_buf_pR = state.audio_out_bufR;
    state.audio_out_float_buf_pR = state.audio_out_float_bufR;

    processAudioR(&opts, &state);

    int rc = 0;
    rc |= expect_int_eq("right auto gain first sample", out[0], 1500);
    rc |= expect_int_eq("right auto gain negative sample", out[1], -3000);
    rc |= expect_int_eq("right auto gain small sample", out[2], 375);
    rc |= expect_int_eq("right auto gain peak sample", out[3], -6000);
    rc |= expect_int_eq("right auto gain mirror sample", state.s_r[0], 1500);
    rc |= expect_float_close("right auto gain falls to peak target", state.aout_gainR, 1.5f, 1e-6f);
    rc |= expect_float_close("right auto gain stores block peak", state.aout_max_bufR[24], 4000.0f, 1e-6f);
    rc |= expect_int_eq("right auto gain wraps history index", state.aout_max_buf_idxR, 0);
    rc |= expect_true("right auto gain wraps history pointer", state.aout_max_buf_pR == state.aout_max_bufR);
    rc |= expect_int_eq("right auto gain output index", state.audio_out_idxR, 160);
    rc |= expect_int_eq("right auto gain long output index", state.audio_out_idx2R, 160);
    rc |= expect_true("right auto gain output pointer advanced", state.audio_out_buf_pR == state.audio_out_bufR + 160);
    return rc;
}

static int
test_process_audio_upsampled_left_clamps_and_tracks_output(void) {
    static dsd_opts opts = {0};
    static dsd_state state = {0};
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&state, 0, sizeof state);

    opts.audio_gain = 1.0f;
    opts.pulse_digi_rate_out = 48000;
    state.aout_gain = 1.0f;
    static short out[1000];
    static float out_float[1000];
    DSD_MEMSET(out, 0, sizeof(out));
    DSD_MEMSET(out_float, 0, sizeof(out_float));
    state.audio_out_buf = out;
    state.audio_out_float_buf = out_float;
    state.audio_out_buf_p = state.audio_out_buf;
    state.audio_out_float_buf_p = state.audio_out_float_buf;
    state.audio_out_temp_buf[0] = 40000.0f;
    state.audio_out_temp_buf[1] = -40000.0f;
    state.audio_out_temp_buf[2] = 123.0f;

    processAudio(&opts, &state);

    int rc = 0;
    rc |= expect_int_eq("left upsample clamp positive first", out[0], 32767);
    rc |= expect_int_eq("left upsample clamp positive repeat", out[5], 32767);
    rc |= expect_int_eq("left upsample clamp negative first", out[6], -32768);
    rc |= expect_int_eq("left upsample clamp negative repeat", out[11], -32768);
    rc |= expect_int_eq("left upsample repeated sample", out[12], 123);
    rc |= expect_int_eq("left upsample mirror positive", state.s_lu[0], 32767);
    rc |= expect_int_eq("left upsample mirror negative", state.s_lu[6], -32768);
    rc |= expect_int_eq("left upsample index", state.audio_out_idx, 960);
    rc |= expect_int_eq("left upsample long index", state.audio_out_idx2, 960);
    rc |= expect_true("left upsample output pointer advanced", state.audio_out_buf_p == state.audio_out_buf + 960);
    rc |= expect_true("left upsample float pointer advanced",
                      state.audio_out_float_buf_p == state.audio_out_float_buf + 960);
    return rc;
}

static int
test_process_audio_upsampled_right_clamps_and_tracks_output(void) {
    static dsd_opts opts = {0};
    static dsd_state state = {0};
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&state, 0, sizeof state);

    opts.audio_gainR = 1.0f;
    opts.pulse_digi_rate_out = 48000;
    state.aout_gainR = 1.0f;
    state.dmr_stereo = 1;
    state.currentslot = 1;
    static short out[1000];
    static float out_float[1000];
    DSD_MEMSET(out, 0, sizeof(out));
    DSD_MEMSET(out_float, 0, sizeof(out_float));
    state.audio_out_bufR = out;
    state.audio_out_float_bufR = out_float;
    state.audio_out_buf_pR = state.audio_out_bufR;
    state.audio_out_float_buf_pR = state.audio_out_float_bufR;
    state.audio_out_temp_bufR[0] = 50000.0f;
    state.audio_out_temp_bufR[1] = -50000.0f;
    state.audio_out_temp_bufR[2] = -321.0f;

    processAudioR(&opts, &state);

    int rc = 0;
    rc |= expect_int_eq("right upsample clamp positive first", out[0], 32767);
    rc |= expect_int_eq("right upsample clamp positive repeat", out[5], 32767);
    rc |= expect_int_eq("right upsample clamp negative first", out[6], -32768);
    rc |= expect_int_eq("right upsample clamp negative repeat", out[11], -32768);
    rc |= expect_int_eq("right upsample repeated sample", out[12], -321);
    rc |= expect_int_eq("right upsample mirror positive", state.s_ru[0], 32767);
    rc |= expect_int_eq("right upsample mirror negative", state.s_ru[6], -32768);
    rc |= expect_int_eq("right upsample index", state.audio_out_idxR, 960);
    rc |= expect_int_eq("right upsample long index", state.audio_out_idx2R, 960);
    rc |= expect_true("right upsample output pointer advanced", state.audio_out_buf_pR == state.audio_out_bufR + 960);
    rc |= expect_true("right upsample float pointer advanced",
                      state.audio_out_float_buf_pR == state.audio_out_float_bufR + 960);
    return rc;
}

static int
test_play_synthesized_voice_slot_off_clears_pending_output(void) {
    static dsd_opts opts = {0};
    static dsd_state state = {0};
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&state, 0, sizeof state);

    opts.slot1_on = 0;
    static short out[200];
    static float out_float[200];
    for (int i = 0; i < 200; i++) {
        out[i] = (short)(i + 1);
        out_float[i] = (float)(i + 1);
    }
    state.audio_out_buf = out;
    state.audio_out_float_buf = out_float;
    state.audio_out_buf_p = out + 55;
    state.audio_out_float_buf_p = out_float + 55;
    state.audio_out_idx = 55;
    state.audio_out_idx2 = 77;

    playSynthesizedVoice(&opts, &state);

    int rc = 0;
    for (int i = 0; i < 100; i++) {
        char label[64];
        DSD_SNPRINTF(label, sizeof label, "slot-off clears short %d", i);
        rc |= expect_int_eq(label, out[i], 0);
        DSD_SNPRINTF(label, sizeof label, "slot-off clears float %d", i);
        rc |= expect_float_close(label, out_float[i], 0.0f, 1e-6f);
    }
    rc |= expect_true("slot-off short pointer reset", state.audio_out_buf_p == state.audio_out_buf + 100);
    rc |= expect_true("slot-off float pointer reset", state.audio_out_float_buf_p == state.audio_out_float_buf + 100);
    rc |= expect_int_eq("slot-off index reset", state.audio_out_idx, 0);
    rc |= expect_int_eq("slot-off long index reset", state.audio_out_idx2, 0);
    return rc;
}

static int
test_play_synthesized_voice_fd_writes_pending_pcm_and_resets_index(void) {
    static dsd_opts opts = {0};
    static dsd_state state = {0};
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&state, 0, sizeof state);

    char path[DSD_AUDIO_TEST_PATH_MAX] = {0};
    int fd = create_temp_file_fd("dsdneo_audio_fd", path, sizeof path);
    if (fd < 0) {
        return 1;
    }

    opts.slot1_on = 1;
    opts.audio_out = 1;
    opts.audio_out_type = 1;
    opts.audio_out_fd = fd;
    opts.delay = 2;
    static short out[8];
    static float out_float[8];
    static const short initial_out[8] = {101, -202, 303, -404, 505, -606, 707, -808};
    DSD_MEMCPY(out, initial_out, sizeof(out));
    DSD_MEMSET(out_float, 0, sizeof(out_float));
    state.audio_out_buf = out;
    state.audio_out_float_buf = out_float;
    state.audio_out_buf_p = out + 4;
    state.audio_out_float_buf_p = out_float + 4;
    state.audio_out_idx = 4;
    state.audio_out_idx2 = 4;

    playSynthesizedVoice(&opts, &state);
    (void)dsd_close(fd);
    opts.audio_out_fd = -1;

    short samples[4] = {0};
    int rc = read_short_file_samples(path, samples, 4);
    rc |= expect_int_eq("fd write sample 0", samples[0], 101);
    rc |= expect_int_eq("fd write sample 1", samples[1], -202);
    rc |= expect_int_eq("fd write sample 2", samples[2], 303);
    rc |= expect_int_eq("fd write sample 3", samples[3], -404);
    rc |= expect_int_eq("fd write resets index", state.audio_out_idx, 0);
    rc |= expect_int_eq("fd write leaves long index", state.audio_out_idx2, 4);
    (void)remove(path);
    return rc;
}

static int
test_play_synthesized_voice_bad_fd_drops_pending_pcm(void) {
    static dsd_opts opts = {0};
    static dsd_state state = {0};
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&state, 0, sizeof state);

    opts.slot1_on = 1;
    opts.audio_out = 1;
    opts.audio_out_type = 1;
    opts.audio_out_fd = -1;
    opts.delay = 0;
    static short out[4];
    static const short initial_out[4] = {1, 2, 3, 4};
    DSD_MEMCPY(out, initial_out, sizeof(out));
    state.audio_out_buf = out;
    state.audio_out_buf_p = out + 4;
    state.audio_out_idx = 4;

    playSynthesizedVoice(&opts, &state);

    return expect_int_eq("bad fd drops pending index", state.audio_out_idx, 0);
}

static int
test_drain_audio_output_guards_and_fd_sink(void) {
    dsd_drain_audio_output(NULL);

    static dsd_opts opts = {0};
    DSD_MEMSET(&opts, 0, sizeof opts);
    opts.audio_out = 0;
    opts.audio_out_type = 1;
    opts.audio_out_fd = -1;
    dsd_drain_audio_output(&opts);

    char path[DSD_AUDIO_TEST_PATH_MAX] = {0};
    int fd = create_temp_file_fd("dsdneo_audio_drain", path, sizeof path);
    if (fd < 0) {
        return 1;
    }

    opts.audio_out = 1;
    opts.audio_out_type = 1;
    opts.audio_out_fd = fd;
    dsd_drain_audio_output(&opts);
    (void)dsd_close(fd);
    (void)remove(path);
    return 0;
}

static int
test_write_synthesized_voice_clamps_and_writes_left_mono(void) {
    static dsd_opts opts = {0};
    static dsd_state state = {0};
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&state, 0, sizeof state);

    char path[DSD_AUDIO_TEST_PATH_MAX] = {0};
    if (create_temp_wav_path("dsdneo_audio_left", path, sizeof path) != 0) {
        return 1;
    }
    opts.wav_out_f = open_temp_wav_writer(path, 1);
    if (!opts.wav_out_f) {
        DSD_FPRINTF(stderr, "FAIL: sf_open write failed for %s: %s\n", path, sf_strerror(NULL));
        (void)remove(path);
        return 1;
    }

    state.audio_out_temp_buf[0] = 40000.0f;
    state.audio_out_temp_buf[1] = -40000.0f;
    state.audio_out_temp_buf[2] = 1234.0f;
    writeSynthesizedVoice(&opts, &state);
    sf_close(opts.wav_out_f);
    opts.wav_out_f = NULL;

    short samples[160] = {0};
    int rc = read_wav_samples(path, 1, samples, 160);
    rc |= expect_int_eq("write left clamp high", samples[0], 32767);
    rc |= expect_int_eq("write left clamp low", samples[1], -32768);
    rc |= expect_int_eq("write left normal sample", samples[2], 1234);
    rc |= expect_float_close("write left temp clamped high", state.audio_out_temp_buf[0], 32767.0f, 1e-6f);
    rc |= expect_float_close("write left temp clamped low", state.audio_out_temp_buf[1], -32768.0f, 1e-6f);
    (void)remove(path);
    return rc;
}

static int
test_write_synthesized_voice_r_clamps_and_writes_right_mono(void) {
    static dsd_opts opts = {0};
    static dsd_state state = {0};
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&state, 0, sizeof state);

    char path[DSD_AUDIO_TEST_PATH_MAX] = {0};
    if (create_temp_wav_path("dsdneo_audio_right", path, sizeof path) != 0) {
        return 1;
    }
    opts.wav_out_fR = open_temp_wav_writer(path, 1);
    if (!opts.wav_out_fR) {
        DSD_FPRINTF(stderr, "FAIL: sf_open write failed for %s: %s\n", path, sf_strerror(NULL));
        (void)remove(path);
        return 1;
    }

    state.audio_out_temp_bufR[0] = 45000.0f;
    state.audio_out_temp_bufR[1] = -45000.0f;
    state.audio_out_temp_bufR[2] = -2345.0f;
    writeSynthesizedVoiceR(&opts, &state);
    sf_close(opts.wav_out_fR);
    opts.wav_out_fR = NULL;

    short samples[160] = {0};
    int rc = read_wav_samples(path, 1, samples, 160);
    rc |= expect_int_eq("write right clamp high", samples[0], 32767);
    rc |= expect_int_eq("write right clamp low", samples[1], -32768);
    rc |= expect_int_eq("write right normal sample", samples[2], -2345);
    rc |= expect_float_close("write right temp clamped high", state.audio_out_temp_bufR[0], 32767.0f, 1e-6f);
    rc |= expect_float_close("write right temp clamped low", state.audio_out_temp_bufR[1], -32768.0f, 1e-6f);
    (void)remove(path);
    return rc;
}

static int
test_write_synthesized_voice_ms_duplicates_left_into_stereo_wav(void) {
    static dsd_opts opts = {0};
    static dsd_state state = {0};
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&state, 0, sizeof state);

    char path[DSD_AUDIO_TEST_PATH_MAX] = {0};
    if (create_temp_wav_path("dsdneo_audio_ms", path, sizeof path) != 0) {
        return 1;
    }
    opts.wav_out_f = open_temp_wav_writer(path, 2);
    if (!opts.wav_out_f) {
        DSD_FPRINTF(stderr, "FAIL: sf_open write failed for %s: %s\n", path, sf_strerror(NULL));
        (void)remove(path);
        return 1;
    }

    state.audio_out_temp_buf[0] = 111.0f;
    state.audio_out_temp_buf[1] = -222.0f;
    writeSynthesizedVoiceMS(&opts, &state);
    sf_close(opts.wav_out_f);
    opts.wav_out_f = NULL;

    short samples[320] = {0};
    int rc = read_wav_samples(path, 2, samples, 320);
    rc |= expect_int_eq("write ms left duplicate 0", samples[0], 111);
    rc |= expect_int_eq("write ms right duplicate 0", samples[1], 111);
    rc |= expect_int_eq("write ms left duplicate 1", samples[2], -222);
    rc |= expect_int_eq("write ms right duplicate 1", samples[3], -222);
    (void)remove(path);
    return rc;
}

static int
test_open_audio_in_device_rejects_null_arguments(void) {
    static dsd_opts opts = {0};
    static dsd_state state = {0};
    return expect_int_eq("open input rejects null opts", openAudioInDevice(NULL, &state), -1)
           | expect_int_eq("open input rejects null state", openAudioInDevice(&opts, NULL), -1);
}

static int
test_open_audio_in_device_bin_symbol_file_resets_replay_state(void) {
    static dsd_opts opts = {0};
    static dsd_state state = {0};
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&state, 0, sizeof state);

    char path[DSD_AUDIO_TEST_PATH_MAX] = {0};
    int fd = create_temp_file_with_suffix("dsdneo_audio_symbols", ".bin", path, sizeof path);
    if (fd < 0) {
        return 1;
    }
    const unsigned char payload[4] = {0x02, 0x03, 0x01, 0x00};
    if (dsd_write(fd, payload, sizeof payload) != (ssize_t)sizeof payload) {
        DSD_FPRINTF(stderr, "FAIL: write bin symbol payload\n");
        (void)dsd_close(fd);
        (void)remove(path);
        return 1;
    }
    (void)dsd_close(fd);

    opts.audio_in_type = AUDIO_IN_PULSE;
    opts.wav_sample_rate = 48000;
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", path);
    state.use_throttle = 0;
    state.symbol_replay_next_deadline_ns = 12345;
    state.symbol_replay_format = DSD_SYMBOL_REPLAY_FORMAT_SOFT;
    state.symbol_replay_header_checked = 1;
    state.symbol_replay_has_soft = 1;

    int rc = expect_int_eq("open bin symbol input succeeds", openAudioInDevice(&opts, &state), 0);
    rc |= expect_int_eq("bin symbol input type", opts.audio_in_type, AUDIO_IN_SYMBOL_BIN);
    rc |= expect_true("bin symbol file opened", opts.symbolfile != NULL);
    rc |= expect_int_eq("bin symbol replay enables throttle", state.use_throttle, 1);
    rc |= expect_true("bin symbol replay clears deadline", state.symbol_replay_next_deadline_ns == 0);
    rc |=
        expect_int_eq("bin symbol replay resets format", state.symbol_replay_format, DSD_SYMBOL_REPLAY_FORMAT_UNKNOWN);
    rc |= expect_int_eq("bin symbol replay clears header check", state.symbol_replay_header_checked, 0);
    rc |= expect_int_eq("bin symbol replay clears soft flag", state.symbol_replay_has_soft, 0);
    if (opts.symbolfile) {
        fclose(opts.symbolfile);
        opts.symbolfile = NULL;
    }
    (void)remove(path);
    return rc;
}

static int
test_open_audio_in_device_float_symbol_file_preserves_soft_metadata(void) {
    static dsd_opts opts = {0};
    static dsd_state state = {0};
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&state, 0, sizeof state);

    char path[DSD_AUDIO_TEST_PATH_MAX] = {0};
    int fd = create_temp_file_with_suffix("dsdneo_audio_symbols", ".raw", path, sizeof path);
    if (fd < 0) {
        return 1;
    }
    const float payload[2] = {1.0f, -1.0f};
    if (dsd_write(fd, payload, sizeof payload) != (ssize_t)sizeof payload) {
        DSD_FPRINTF(stderr, "FAIL: write raw symbol payload\n");
        (void)dsd_close(fd);
        (void)remove(path);
        return 1;
    }
    (void)dsd_close(fd);

    opts.audio_in_type = AUDIO_IN_PULSE;
    opts.wav_sample_rate = 48000;
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", path);
    state.use_throttle = 1;
    state.symbol_replay_next_deadline_ns = 9876;
    state.symbol_replay_format = DSD_SYMBOL_REPLAY_FORMAT_SOFT;
    state.symbol_replay_header_checked = 1;
    state.symbol_replay_has_soft = 1;

    int rc = expect_int_eq("open float symbol input succeeds", openAudioInDevice(&opts, &state), 0);
    rc |= expect_int_eq("float symbol input type", opts.audio_in_type, AUDIO_IN_SYMBOL_FLT);
    rc |= expect_true("float symbol file opened", opts.symbolfile != NULL);
    rc |= expect_int_eq("float symbol replay clears throttle", state.use_throttle, 0);
    rc |= expect_true("float symbol replay clears deadline", state.symbol_replay_next_deadline_ns == 0);
    rc |= expect_int_eq("float symbol replay keeps format", state.symbol_replay_format, DSD_SYMBOL_REPLAY_FORMAT_SOFT);
    rc |= expect_int_eq("float symbol replay keeps header check", state.symbol_replay_header_checked, 1);
    rc |= expect_int_eq("float symbol replay keeps soft flag", state.symbol_replay_has_soft, 1);
    if (opts.symbolfile) {
        fclose(opts.symbolfile);
        opts.symbolfile = NULL;
    }
    (void)remove(path);
    return rc;
}

static int
test_open_audio_in_device_symbol_directory_falls_back_to_pulse(void) {
    static dsd_opts opts = {0};
    static dsd_state state = {0};
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&state, 0, sizeof state);

    char path[DSD_AUDIO_TEST_PATH_MAX] = {0};
    if (create_temp_dir_with_suffix("dsdneo_audio_symbols", ".bin", path, sizeof path) != 0) {
        return 1;
    }

    opts.audio_in_type = AUDIO_IN_WAV;
    opts.wav_sample_rate = 48000;
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", path);
    state.use_throttle = 1;
    state.symbol_replay_next_deadline_ns = 4567;

    int rc = expect_int_eq("open symbol directory succeeds as fallback", openAudioInDevice(&opts, &state), 0);
    rc |= expect_int_eq("symbol directory selects pulse fallback", opts.audio_in_type, AUDIO_IN_PULSE);
    rc |= expect_true("symbol directory does not open symbol file", opts.symbolfile == NULL);
    rc |= expect_int_eq("symbol directory clears throttle", state.use_throttle, 0);
    rc |= expect_true("symbol directory clears deadline", state.symbol_replay_next_deadline_ns == 0);
    (void)remove(path);
    return rc;
}

static int
test_async_output_policy_keeps_file_replays_synchronous(void) {
    int rc = 0;
    rc |= expect_int_eq("stdin output is synchronous", dsd_audio_input_type_uses_async_output(AUDIO_IN_STDIN, 0), 0);
    rc |= expect_int_eq("wav replay output is synchronous", dsd_audio_input_type_uses_async_output(AUDIO_IN_WAV, 0), 0);
    rc |= expect_int_eq("bin symbol replay output is synchronous",
                        dsd_audio_input_type_uses_async_output(AUDIO_IN_SYMBOL_BIN, 0), 0);
    rc |= expect_int_eq("float symbol replay output is synchronous",
                        dsd_audio_input_type_uses_async_output(AUDIO_IN_SYMBOL_FLT, 0), 0);
    rc |=
        expect_int_eq("null input output is synchronous", dsd_audio_input_type_uses_async_output(AUDIO_IN_NULL, 0), 0);
    rc |=
        expect_int_eq("pulse input output may be async", dsd_audio_input_type_uses_async_output(AUDIO_IN_PULSE, 0), 1);
    rc |= expect_int_eq("playfiles forces synchronous output",
                        dsd_audio_input_type_uses_async_output(AUDIO_IN_PULSE, 1), 0);
    return rc;
}

int
main(void) {
    int rc = 0;
    rc |= test_agsm_applies_gain_to_entire_block();
    rc |= test_agsm_handles_silence_without_invalid_values();
    rc |= test_audio_apply_gain_f32_multiplies_block();
    rc |= test_audio_mono_to_stereo_duplicates_samples();
    rc |= test_audio_mix_helpers_apply_channel_gates();
    rc |= test_upsample_repeats_into_selected_slot_buffer();
    rc |= test_manual_and_float_autogain_helpers();
    rc |= test_agf_scales_nonzero_float_block_and_updates_slot_gain();
    rc |= test_process_audio_native_left_clamps_and_tracks_output();
    rc |= test_process_audio_native_right_clamps_and_tracks_output();
    rc |= test_process_audio_auto_gain_ramps_from_peak_history();
    rc |= test_process_audio_right_auto_gain_tracks_peak_history();
    rc |= test_process_audio_upsampled_left_clamps_and_tracks_output();
    rc |= test_process_audio_upsampled_right_clamps_and_tracks_output();
    rc |= test_play_synthesized_voice_slot_off_clears_pending_output();
    rc |= test_play_synthesized_voice_fd_writes_pending_pcm_and_resets_index();
    rc |= test_play_synthesized_voice_bad_fd_drops_pending_pcm();
    rc |= test_drain_audio_output_guards_and_fd_sink();
    rc |= test_write_synthesized_voice_clamps_and_writes_left_mono();
    rc |= test_write_synthesized_voice_r_clamps_and_writes_right_mono();
    rc |= test_write_synthesized_voice_ms_duplicates_left_into_stereo_wav();
    rc |= test_open_audio_in_device_rejects_null_arguments();
    rc |= test_open_audio_in_device_bin_symbol_file_resets_replay_state();
    rc |= test_open_audio_in_device_float_symbol_file_preserves_soft_metadata();
    rc |= test_open_audio_in_device_symbol_directory_falls_back_to_pulse();
    rc |= test_async_output_policy_keeps_file_replays_synchronous();
    return rc;
}
