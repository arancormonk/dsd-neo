// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

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

    float left[6] = {-1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f};
    float right[6] = {-2.0f, -2.0f, -2.0f, -2.0f, -2.0f, -2.0f};
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
    return rc;
}
