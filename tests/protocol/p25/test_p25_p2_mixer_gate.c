// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Validate P25 Phase 2 stereo mixer gating uses per-slot gates and does not
 * cross-mute the opposite slot.
 */

#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/runtime/p25_p2_audio_ring.h>
#include <dsd-neo/runtime/udp_audio_hooks.h>
#include <stdint.h>
#include <stdio.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

static unsigned char g_audio_capture[2048];
static size_t g_audio_capture_bytes = 0;
static int g_audio_capture_calls = 0;

static int
expect_eq(const char* tag, int got, int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %d want %d\n", tag, got, want);
        return 1;
    }
    return 0;
}

static int
expect_true(const char* tag, int cond) {
    if (!cond) {
        DSD_FPRINTF(stderr, "%s failed\n", tag);
        return 1;
    }
    return 0;
}

static int
copy_capture_bytes(const char* tag, void* out, size_t expected_bytes) {
    if (g_audio_capture_bytes != expected_bytes) {
        DSD_FPRINTF(stderr, "%s: got %zu want %zu\n", tag, g_audio_capture_bytes, expected_bytes);
        return 1;
    }
    DSD_MEMCPY(out, g_audio_capture, expected_bytes);
    return 0;
}

static void
capture_blast(const dsd_opts* opts, dsd_state* state, size_t bytes, const void* data) {
    (void)opts;
    (void)state;
    g_audio_capture_calls++;
    if (g_audio_capture_calls == 1 && data && bytes <= sizeof(g_audio_capture)) {
        DSD_MEMCPY(g_audio_capture, data, bytes);
        g_audio_capture_bytes = bytes;
    }
}

static void
reset_capture(void) {
    DSD_MEMSET(g_audio_capture, 0, sizeof(g_audio_capture));
    g_audio_capture_bytes = 0;
    g_audio_capture_calls = 0;
}

static void
fill_f32_frame(float frame[160], float value) {
    for (int i = 0; i < 160; i++) {
        frame[i] = value;
    }
}

static int
short_18_blocks_are_clear(short frames[18][160]) {
    for (int frame = 0; frame < 18; frame++) {
        for (int i = 0; i < 160; i++) {
            if (frames[frame][i] != 0) {
                return 0;
            }
        }
    }
    return 1;
}

static int
short_block_is_clear(const short* samples, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (samples[i] != 0) {
            return 0;
        }
    }
    return 1;
}

static int
float_block_is_clear(const float* samples, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (samples[i] < -1.0e-6f || samples[i] > 1.0e-6f) {
            return 0;
        }
    }
    return 1;
}

static int
run_fs4_left_active_case_ext(int enc_lockout_enabled, int expect_right_silent, int muted_slot_algid,
                             int muted_slot_aes_loaded, unsigned long long muted_slot_key, int muted_slot_svc,
                             int muted_slot_marker) {
    static dsd_opts opts;
    static dsd_state st;
    float frame[160];
    int rc = 0;

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&st, 0, sizeof(st));
    reset_capture();

    opts.audio_out = 1;
    opts.audio_out_type = 8;
    opts.pulse_digi_out_channels = 2;
    opts.slot1_on = 1;
    opts.slot2_on = 1;
    opts.trunk_tune_enc_calls = enc_lockout_enabled ? 0 : 1;
    opts.audio_gain = 25;
    st.aout_gain = 49.0f;
    st.aout_gainR = 49.0f;
    st.p25_p2_audio_allowed[0] = 1;
    st.p25_p2_audio_allowed[1] = 0;
    st.dmr_soR = muted_slot_svc;
    st.payload_algidR = muted_slot_algid;
    st.aes_key_loaded[1] = muted_slot_aes_loaded;
    st.RR = muted_slot_key;
    st.p25_p2_enc_lockout_muted[1] = (uint8_t)(muted_slot_marker ? 1U : 0U);

    fill_f32_frame(frame, 384.0f);
    rc |= expect_eq("fs4 push left", p25_p2_audio_ring_push(&st, 0, frame), 1);
    playSynthesizedVoiceFS4(&opts, &st);

    float out[160 * 2] = {0.0f};
    const size_t expected_bytes = (size_t)160 * 2U * sizeof(out[0]);
    rc |= expect_eq("fs4 captured calls", g_audio_capture_calls >= 1, 1);
    int copied = copy_capture_bytes("fs4 captured bytes", out, expected_bytes);
    rc |= copied;
    if (copied == 0) {
        rc |= expect_true("fs4 left audible", out[0] != 0.0f);
        rc |= expect_eq("fs4 right state", out[1] == 0.0f, expect_right_silent);
    }
    return rc;
}

static int
run_fs4_left_active_case(int enc_lockout_enabled, int expect_right_silent, int muted_slot_algid,
                         int muted_slot_aes_loaded, unsigned long long muted_slot_key) {
    return run_fs4_left_active_case_ext(enc_lockout_enabled, expect_right_silent, muted_slot_algid,
                                        muted_slot_aes_loaded, muted_slot_key, 0x40, 0);
}

static int
run_fs4_right_active_case_ext(int enc_lockout_enabled, int expect_left_silent, int muted_slot_algid,
                              int muted_slot_aes_loaded, unsigned long long muted_slot_key, int muted_slot_svc,
                              int muted_slot_marker) {
    static dsd_opts opts;
    static dsd_state st;
    float frame[160];
    int rc = 0;

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&st, 0, sizeof(st));
    reset_capture();

    opts.audio_out = 1;
    opts.audio_out_type = 8;
    opts.pulse_digi_out_channels = 2;
    opts.slot1_on = 1;
    opts.slot2_on = 1;
    opts.trunk_tune_enc_calls = enc_lockout_enabled ? 0 : 1;
    opts.audio_gain = 25;
    st.aout_gain = 49.0f;
    st.aout_gainR = 49.0f;
    st.p25_p2_audio_allowed[0] = 0;
    st.p25_p2_audio_allowed[1] = 1;
    st.dmr_so = muted_slot_svc;
    st.payload_algid = muted_slot_algid;
    st.aes_key_loaded[0] = muted_slot_aes_loaded;
    st.R = muted_slot_key;
    st.p25_p2_enc_lockout_muted[0] = (uint8_t)(muted_slot_marker ? 1U : 0U);

    fill_f32_frame(frame, 384.0f);
    rc |= expect_eq("fs4 push right", p25_p2_audio_ring_push(&st, 1, frame), 1);
    playSynthesizedVoiceFS4(&opts, &st);

    float out[160 * 2] = {0.0f};
    const size_t expected_bytes = (size_t)160 * 2U * sizeof(out[0]);
    rc |= expect_eq("fs4 right captured calls", g_audio_capture_calls >= 1, 1);
    int copied = copy_capture_bytes("fs4 right captured bytes", out, expected_bytes);
    rc |= copied;
    if (copied == 0) {
        rc |= expect_eq("fs4 left state", out[0] == 0.0f, expect_left_silent);
        rc |= expect_true("fs4 right audible", out[1] != 0.0f);
    }
    return rc;
}

static int
run_fs4_right_active_case(int enc_lockout_enabled, int expect_left_silent, int muted_slot_algid,
                          int muted_slot_aes_loaded, unsigned long long muted_slot_key) {
    return run_fs4_right_active_case_ext(enc_lockout_enabled, expect_left_silent, muted_slot_algid,
                                         muted_slot_aes_loaded, muted_slot_key, 0x40, 0);
}

static int
run_ss18_left_active_case_ext(int enc_lockout_enabled, int expect_right_silent, int muted_slot_algid,
                              int muted_slot_aes_loaded, unsigned long long muted_slot_key, int muted_slot_svc,
                              int muted_slot_marker) {
    static dsd_opts opts;
    static dsd_state st;
    int rc = 0;

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&st, 0, sizeof(st));
    reset_capture();

    opts.audio_out = 1;
    opts.audio_out_type = 8;
    opts.slot1_on = 1;
    opts.slot2_on = 1;
    opts.trunk_tune_enc_calls = enc_lockout_enabled ? 0 : 1;
    st.p25_p2_audio_allowed[0] = 1;
    st.p25_p2_audio_allowed[1] = 0;
    st.dmrburstL = 21;
    st.dmrburstR = 0;
    st.dmr_soR = muted_slot_svc;
    st.payload_algidR = muted_slot_algid;
    st.aes_key_loaded[1] = muted_slot_aes_loaded;
    st.RR = muted_slot_key;
    st.p25_p2_enc_lockout_muted[1] = (uint8_t)(muted_slot_marker ? 1U : 0U);

    for (int i = 0; i < 160; i++) {
        st.s_l4[0][i] = 100;
    }
    playSynthesizedVoiceSS18(&opts, &st);

    short out[160 * 2] = {0};
    const size_t expected_bytes = (size_t)160 * 2U * sizeof(out[0]);
    rc |= expect_eq("ss18 captured calls", g_audio_capture_calls >= 1, 1);
    int copied = copy_capture_bytes("ss18 captured bytes", out, expected_bytes);
    rc |= copied;
    if (copied == 0) {
        rc |= expect_eq("ss18 left audible", out[0], 100);
        rc |= expect_eq("ss18 right state", out[1] == 0, expect_right_silent);
    }
    return rc;
}

static int
run_ss18_left_active_case(int enc_lockout_enabled, int expect_right_silent, int muted_slot_algid,
                          int muted_slot_aes_loaded, unsigned long long muted_slot_key) {
    return run_ss18_left_active_case_ext(enc_lockout_enabled, expect_right_silent, muted_slot_algid,
                                         muted_slot_aes_loaded, muted_slot_key, 0x40, 0);
}

static int
run_ss18_right_active_case_ext(int enc_lockout_enabled, int expect_left_silent, int muted_slot_algid,
                               int muted_slot_aes_loaded, unsigned long long muted_slot_key, int muted_slot_svc,
                               int muted_slot_marker) {
    static dsd_opts opts;
    static dsd_state st;
    int rc = 0;

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&st, 0, sizeof(st));
    reset_capture();

    opts.audio_out = 1;
    opts.audio_out_type = 8;
    opts.slot1_on = 1;
    opts.slot2_on = 1;
    opts.trunk_tune_enc_calls = enc_lockout_enabled ? 0 : 1;
    st.p25_p2_audio_allowed[0] = 0;
    st.p25_p2_audio_allowed[1] = 1;
    st.dmrburstL = 0;
    st.dmrburstR = 21;
    st.dmr_so = muted_slot_svc;
    st.payload_algid = muted_slot_algid;
    st.aes_key_loaded[0] = muted_slot_aes_loaded;
    st.R = muted_slot_key;
    st.p25_p2_enc_lockout_muted[0] = (uint8_t)(muted_slot_marker ? 1U : 0U);

    for (int i = 0; i < 160; i++) {
        st.s_r4[0][i] = 100;
    }
    playSynthesizedVoiceSS18(&opts, &st);

    short out[160 * 2] = {0};
    const size_t expected_bytes = (size_t)160 * 2U * sizeof(out[0]);
    rc |= expect_eq("ss18 right captured calls", g_audio_capture_calls >= 1, 1);
    int copied = copy_capture_bytes("ss18 right captured bytes", out, expected_bytes);
    rc |= copied;
    if (copied == 0) {
        rc |= expect_eq("ss18 left state", out[0] == 0, expect_left_silent);
        rc |= expect_eq("ss18 right audible", out[1], 100);
    }
    return rc;
}

static int
run_ss18_right_active_case(int enc_lockout_enabled, int expect_left_silent, int muted_slot_algid,
                           int muted_slot_aes_loaded, unsigned long long muted_slot_key) {
    return run_ss18_right_active_case_ext(enc_lockout_enabled, expect_left_silent, muted_slot_algid,
                                          muted_slot_aes_loaded, muted_slot_key, 0x40, 0);
}

static int
run_ss18_partial_flush_case(void) {
    static dsd_opts opts;
    static dsd_state st;
    int rc = 0;

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&st, 0, sizeof(st));
    reset_capture();

    opts.audio_out = 1;
    opts.audio_out_type = 8;
    opts.floating_point = 0;
    opts.pulse_digi_rate_out = 8000;
    opts.slot1_on = 1;
    opts.slot2_on = 1;
    st.s_l4[0][0] = 321;
    st.s_r4[0][0] = -654;
    st.voice_counter[0] = 7;
    st.voice_counter[1] = 8;
    st.p25_p2_audio_allowed[0] = 0;
    st.p25_p2_audio_allowed[1] = 0;

    dsd_p25p2_flush_partial_audio(&opts, &st);

    short out[160 * 2] = {0};
    const size_t expected_bytes = (size_t)160 * 2U * sizeof(out[0]);
    rc |= expect_eq("partial flush captured calls", g_audio_capture_calls >= 1, 1);
    int copied = copy_capture_bytes("partial flush captured bytes", out, expected_bytes);
    rc |= copied;
    if (copied == 0) {
        rc |= expect_eq("partial flush left sample", out[0], 321);
        rc |= expect_eq("partial flush right sample", out[1], -654);
    }
    rc |= expect_eq("partial flush allowed left", st.p25_p2_audio_allowed[0], 1);
    rc |= expect_eq("partial flush allowed right", st.p25_p2_audio_allowed[1], 1);
    rc |= expect_eq("partial flush voice counter left", st.voice_counter[0], 0);
    rc |= expect_eq("partial flush voice counter right", st.voice_counter[1], 0);
    rc |= expect_eq("partial flush clears left frames", short_18_blocks_are_clear(st.s_l4), 1);
    rc |= expect_eq("partial flush clears right frames", short_18_blocks_are_clear(st.s_r4), 1);
    return rc;
}

static int
run_ss18_partial_flush_guard_case(void) {
    static dsd_opts opts;
    static dsd_state st;
    int rc = 0;

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&st, 0, sizeof(st));
    reset_capture();

    opts.audio_out = 1;
    opts.audio_out_type = 8;
    opts.floating_point = 1;
    opts.pulse_digi_rate_out = 8000;
    opts.slot1_on = 1;
    opts.slot2_on = 1;
    st.s_l4[0][0] = 111;
    st.voice_counter[0] = 3;

    dsd_p25p2_flush_partial_audio(&opts, &st);

    rc |= expect_eq("partial flush fp guard calls", g_audio_capture_calls, 0);
    rc |= expect_eq("partial flush fp guard preserves sample", st.s_l4[0][0], 111);
    rc |= expect_eq("partial flush fp guard preserves counter", st.voice_counter[0], 3);

    opts.floating_point = 0;
    opts.pulse_digi_rate_out = 48000;
    dsd_p25p2_flush_partial_audio(&opts, &st);
    rc |= expect_eq("partial flush rate guard calls", g_audio_capture_calls, 0);
    rc |= expect_eq("partial flush rate guard preserves sample", st.s_l4[0][0], 111);
    return rc;
}

static int
run_ss18_partial_flush_slot_case(void) {
    static dsd_opts opts;
    static dsd_state st;
    int rc = 0;

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&st, 0, sizeof(st));
    reset_capture();

    opts.audio_out = 1;
    opts.audio_out_type = 8;
    opts.floating_point = 0;
    opts.pulse_digi_rate_out = 8000;
    opts.slot1_on = 1;
    opts.slot2_on = 1;
    st.dmrburstL = 21;
    st.dmrburstR = 21;
    st.s_l4[0][0] = 321;
    st.s_r4[0][0] = -654;
    st.voice_counter[0] = 7;
    st.voice_counter[1] = 8;
    st.p25_p2_audio_allowed[0] = 1;
    st.p25_p2_audio_allowed[1] = 1;

    dsd_p25p2_flush_partial_audio_slot(&opts, &st, 0);

    short out[160 * 2] = {0};
    const size_t expected_bytes = (size_t)160 * 2U * sizeof(out[0]);
    rc |= expect_eq("partial slot flush captured calls", g_audio_capture_calls >= 1, 1);
    int copied = copy_capture_bytes("partial slot flush captured bytes", out, expected_bytes);
    rc |= copied;
    if (copied == 0) {
        rc |= expect_eq("partial slot flush left sample", out[0], 321);
        rc |= expect_eq("partial slot flush masks right sample", out[1], 0);
    }
    rc |= expect_eq("partial slot flush allowed left", st.p25_p2_audio_allowed[0], 1);
    rc |= expect_eq("partial slot flush allowed right preserved", st.p25_p2_audio_allowed[1], 1);
    rc |= expect_eq("partial slot flush voice counter left", st.voice_counter[0], 0);
    rc |= expect_eq("partial slot flush voice counter right preserved", st.voice_counter[1], 8);
    rc |= expect_eq("partial slot flush clears left frames", short_18_blocks_are_clear(st.s_l4), 1);
    rc |= expect_eq("partial slot flush preserves right sample", st.s_r4[0][0], -654);

    DSD_MEMSET(&st, 0, sizeof(st));
    reset_capture();

    st.dmrburstL = 21;
    st.dmrburstR = 21;
    st.s_l4[0][0] = 111;
    st.s_r4[0][0] = -222;
    st.voice_counter[0] = 3;
    st.voice_counter[1] = 4;
    st.p25_p2_audio_allowed[0] = 1;
    st.p25_p2_audio_allowed[1] = 1;

    dsd_p25p2_flush_partial_audio_slot(&opts, &st, 1);

    short out2[160 * 2] = {0};
    rc |= expect_eq("partial slot flush right captured calls", g_audio_capture_calls >= 1, 1);
    copied = copy_capture_bytes("partial slot flush right captured bytes", out2, expected_bytes);
    rc |= copied;
    if (copied == 0) {
        rc |= expect_eq("partial slot flush masks left sample", out2[0], 0);
        rc |= expect_eq("partial slot flush right sample", out2[1], -222);
    }
    rc |= expect_eq("partial slot flush left allowed preserved", st.p25_p2_audio_allowed[0], 1);
    rc |= expect_eq("partial slot flush right allowed", st.p25_p2_audio_allowed[1], 1);
    rc |= expect_eq("partial slot flush left counter preserved", st.voice_counter[0], 3);
    rc |= expect_eq("partial slot flush right counter", st.voice_counter[1], 0);
    rc |= expect_eq("partial slot flush preserves left sample", st.s_l4[0][0], 111);
    rc |= expect_eq("partial slot flush clears right frames", short_18_blocks_are_clear(st.s_r4), 1);
    return rc;
}

static int
run_short_mono_playback_case(void) {
    static dsd_opts opts;
    static dsd_state st;
    int rc = 0;

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&st, 0, sizeof(st));
    reset_capture();

    opts.audio_out = 1;
    opts.audio_out_type = 8;
    opts.slot1_on = 1;
    st.audio_out_idx = 160;
    st.audio_out_idx2 = 800000;
    st.audio_out_buf_p = st.audio_out_buf + 100;
    st.audio_out_float_buf_p = st.audio_out_float_buf + 100;
    for (int i = 0; i < 160; i++) {
        st.s_l[i] = (short)(i + 1);
    }

    playSynthesizedVoiceMS(&opts, &st);

    short out[160] = {0};
    rc |= expect_eq("short mono captured calls", g_audio_capture_calls, 1);
    int copied = copy_capture_bytes("short mono captured bytes", out, sizeof(out));
    rc |= copied;
    if (copied == 0) {
        rc |= expect_eq("short mono first sample", out[0], 1);
        rc |= expect_eq("short mono last sample", out[159], 160);
    }
    rc |= expect_eq("short mono resets idx", st.audio_out_idx, 0);
    rc |= expect_eq("short mono resets ring span", st.audio_out_idx2, 0);
    rc |= expect_eq("short mono clears s_l", short_block_is_clear(st.s_l, 160), 1);
    rc |= expect_true("short mono resets short ptr", st.audio_out_buf_p == st.audio_out_buf + 100);
    rc |= expect_true("short mono resets float ptr", st.audio_out_float_buf_p == st.audio_out_float_buf + 100);
    return rc;
}

static int
run_float_mono_playback_case(void) {
    static dsd_opts opts;
    static dsd_state st;
    int rc = 0;

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&st, 0, sizeof(st));
    reset_capture();

    opts.audio_out = 1;
    opts.audio_out_type = 8;
    opts.slot1_on = 1;
    opts.audio_gain = 25;
    st.aout_gain = 49.0f;
    st.audio_out_idx2 = 800000;
    st.audio_out_buf_p = st.audio_out_buf + 100;
    st.audio_out_float_buf_p = st.audio_out_float_buf + 100;
    fill_f32_frame(st.f_l, 0.25f);

    playSynthesizedVoiceFM(&opts, &st);

    float out[160] = {0.0f};
    rc |= expect_eq("float mono captured calls", g_audio_capture_calls, 1);
    int copied = copy_capture_bytes("float mono captured bytes", out, sizeof(out));
    rc |= copied;
    if (copied == 0) {
        rc |= expect_true("float mono audible", out[0] != 0.0f);
    }
    rc |= expect_eq("float mono resets ring span", st.audio_out_idx2, 0);
    rc |= expect_eq("float mono clears f_l", float_block_is_clear(st.f_l, 160), 1);
    rc |= expect_true("float mono resets short ptr", st.audio_out_buf_p == st.audio_out_buf + 100);
    rc |= expect_true("float mono resets float ptr", st.audio_out_float_buf_p == st.audio_out_float_buf + 100);
    return rc;
}

static int
run_short_stereo_fdma_playback_case(void) {
    static dsd_opts opts;
    static dsd_state st;
    int rc = 0;

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&st, 0, sizeof(st));
    reset_capture();

    opts.audio_out = 1;
    opts.audio_out_type = 8;
    opts.slot1_on = 1;
    st.audio_out_idx = 160;
    st.audio_out_idxR = 160;
    st.audio_out_idx2 = 800000;
    st.audio_out_idx2R = 800000;
    st.audio_out_buf_p = st.audio_out_buf + 100;
    st.audio_out_buf_pR = st.audio_out_bufR + 100;
    st.audio_out_float_buf_p = st.audio_out_float_buf + 100;
    st.audio_out_float_buf_pR = st.audio_out_float_bufR + 100;
    for (int i = 0; i < 160; i++) {
        st.s_l[i] = (short)(i + 10);
        st.s_r[i] = (short)(-i - 10);
    }

    playSynthesizedVoiceSS(&opts, &st);

    short out[160 * 2] = {0};
    rc |= expect_eq("short stereo fdma calls", g_audio_capture_calls, 1);
    int copied = copy_capture_bytes("short stereo fdma bytes", out, sizeof(out));
    rc |= copied;
    if (copied == 0) {
        rc |= expect_eq("short stereo fdma left sample", out[0], 10);
        rc |= expect_eq("short stereo fdma right sample", out[1], 10);
        rc |= expect_eq("short stereo fdma last left", out[318], 169);
        rc |= expect_eq("short stereo fdma last right", out[319], 169);
    }
    rc |= expect_eq("short stereo fdma clears left", short_block_is_clear(st.s_l, 160), 1);
    rc |= expect_eq("short stereo fdma clears right", short_block_is_clear(st.s_r, 160), 1);
    rc |= expect_eq("short stereo fdma resets left idx", st.audio_out_idx, 0);
    rc |= expect_eq("short stereo fdma resets right idx", st.audio_out_idxR, 0);
    rc |= expect_eq("short stereo fdma resets left ring", st.audio_out_idx2, 0);
    rc |= expect_eq("short stereo fdma resets right ring", st.audio_out_idx2R, 0);
    rc |= expect_true("short stereo fdma resets left short ptr", st.audio_out_buf_p == st.audio_out_buf + 100);
    rc |= expect_true("short stereo fdma resets right short ptr", st.audio_out_buf_pR == st.audio_out_bufR + 100);
    return rc;
}

static int
run_short_stereo_dmr3_playback_case(void) {
    static dsd_opts opts;
    static dsd_state st;
    int rc = 0;

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&st, 0, sizeof(st));
    reset_capture();

    opts.audio_out = 1;
    opts.audio_out_type = 8;
    opts.slot1_on = 1;
    opts.slot2_on = 1;
    st.audio_out_idx = 160;
    st.audio_out_idxR = 160;
    st.audio_out_idx2 = 800000;
    st.audio_out_idx2R = 800000;
    st.audio_out_buf_p = st.audio_out_buf + 100;
    st.audio_out_buf_pR = st.audio_out_bufR + 100;
    st.audio_out_float_buf_p = st.audio_out_float_buf + 100;
    st.audio_out_float_buf_pR = st.audio_out_float_bufR + 100;
    for (int i = 0; i < 160; i++) {
        st.s_l4[0][i] = (short)(100 + i);
        st.s_r4[0][i] = (short)(-100 - i);
        st.s_l4[1][i] = (short)(200 + i);
        st.s_r4[1][i] = (short)(-200 - i);
        st.s_l4[2][i] = (short)(300 + i);
        st.s_r4[2][i] = (short)(-300 - i);
    }

    playSynthesizedVoiceSS3(&opts, &st);

    short out[160 * 2] = {0};
    rc |= expect_eq("short stereo dmr3 calls", g_audio_capture_calls, 3);
    int copied = copy_capture_bytes("short stereo dmr3 first bytes", out, sizeof(out));
    rc |= copied;
    if (copied == 0) {
        rc |= expect_eq("short stereo dmr3 first left", out[0], 100);
        rc |= expect_eq("short stereo dmr3 first right", out[1], -100);
        rc |= expect_eq("short stereo dmr3 last left", out[318], 259);
        rc |= expect_eq("short stereo dmr3 last right", out[319], -259);
    }
    rc |= expect_eq("short stereo dmr3 clears left", short_18_blocks_are_clear(st.s_l4), 1);
    rc |= expect_eq("short stereo dmr3 clears right", short_18_blocks_are_clear(st.s_r4), 1);
    rc |= expect_eq("short stereo dmr3 resets left ring", st.audio_out_idx2, 0);
    rc |= expect_eq("short stereo dmr3 resets right ring", st.audio_out_idx2R, 0);
    rc |= expect_true("short stereo dmr3 resets left short ptr", st.audio_out_buf_p == st.audio_out_buf + 100);
    rc |= expect_true("short stereo dmr3 resets right short ptr", st.audio_out_buf_pR == st.audio_out_bufR + 100);
    return rc;
}

static int
run_beeper_float_stereo_case(void) {
    static dsd_opts opts;
    static dsd_state st;
    int rc = 0;

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&st, 0, sizeof(st));
    reset_capture();

    opts.audio_out = 1;
    opts.audio_out_type = 8;
    opts.floating_point = 1;
    opts.pulse_digi_out_channels = 2;

    beeper(&opts, &st, 1, 40, 86, 1);

    float out[160 * 2] = {0.0f};
    rc |= expect_eq("beeper float stereo calls", g_audio_capture_calls, 2);
    int copied = copy_capture_bytes("beeper float stereo bytes", out, sizeof(out));
    rc |= copied;
    if (copied == 0) {
        int saw_right_audio = 0;
        int saw_left_audio = 0;
        for (int i = 0; i < 160; i++) {
            if (out[(i * 2) + 0] != 0.0f) {
                saw_left_audio = 1;
            }
            if (out[(i * 2) + 1] != 0.0f) {
                saw_right_audio = 1;
            }
        }
        rc |= expect_eq("beeper float stereo left silent", saw_left_audio, 0);
        rc |= expect_eq("beeper float stereo right audible", saw_right_audio, 1);
    }
    return rc;
}

static int
run_beeper_short_mono_case(void) {
    static dsd_opts opts;
    static dsd_state st;
    int rc = 0;

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&st, 0, sizeof(st));
    reset_capture();

    opts.audio_out = 1;
    opts.audio_out_type = 8;
    opts.floating_point = 0;
    opts.pulse_digi_out_channels = 1;

    beeper(&opts, &st, 0, 40, 86, 1);

    short out[160] = {0};
    rc |= expect_eq("beeper short mono calls", g_audio_capture_calls, 2);
    int copied = copy_capture_bytes("beeper short mono bytes", out, sizeof(out));
    rc |= copied;
    if (copied == 0) {
        int saw_audio = 0;
        for (int i = 0; i < 160; i++) {
            if (out[i] != 0) {
                saw_audio = 1;
            }
        }
        rc |= expect_eq("beeper short mono audible", saw_audio, 1);
    }
    return rc;
}

int
main(void) {
    int rc = 0;
    static dsd_state st;
    DSD_MEMSET(&st, 0, sizeof(st));

    int encL = -1, encR = -1;

    // Case A: slot1 muted (enc), slot2 clear → encL=1, encR=0
    st.p25_p2_audio_allowed[0] = 0;
    st.p25_p2_audio_allowed[1] = 1;
    rc |= expect_eq("gate ret A", dsd_p25p2_mixer_gate(&st, &encL, &encR), 0);
    rc |= expect_eq("A encL", encL, 1);
    rc |= expect_eq("A encR", encR, 0);

    // Case B: slot1 clear, slot2 muted → encL=0, encR=1
    st.p25_p2_audio_allowed[0] = 1;
    st.p25_p2_audio_allowed[1] = 0;
    rc |= expect_eq("gate ret B", dsd_p25p2_mixer_gate(&st, &encL, &encR), 0);
    rc |= expect_eq("B encL", encL, 0);
    rc |= expect_eq("B encR", encR, 1);

    // Case C: both clear → encL=0, encR=0
    st.p25_p2_audio_allowed[0] = 1;
    st.p25_p2_audio_allowed[1] = 1;
    rc |= expect_eq("gate ret C", dsd_p25p2_mixer_gate(&st, &encL, &encR), 0);
    rc |= expect_eq("C encL", encL, 0);
    rc |= expect_eq("C encR", encR, 0);

    // Case D: both muted → encL=1, encR=1
    st.p25_p2_audio_allowed[0] = 0;
    st.p25_p2_audio_allowed[1] = 0;
    rc |= expect_eq("gate ret D", dsd_p25p2_mixer_gate(&st, &encL, &encR), 0);
    rc |= expect_eq("D encL", encL, 1);
    rc |= expect_eq("D encR", encR, 1);

    dsd_udp_audio_hooks_set((dsd_udp_audio_hooks){.blast = capture_blast});
    rc |= run_fs4_left_active_case(/*enc_lockout_enabled*/ 1, /*expect_right_silent*/ 1, /*muted_slot_algid*/ 0,
                                   /*muted_slot_aes_loaded*/ 0, /*muted_slot_key*/ 0ULL);
    rc |= run_fs4_left_active_case_ext(/*enc_lockout_enabled*/ 1, /*expect_right_silent*/ 1,
                                       /*muted_slot_algid*/ 0, /*muted_slot_aes_loaded*/ 0,
                                       /*muted_slot_key*/ 0ULL, /*muted_slot_svc*/ 0, /*muted_slot_marker*/ 1);
    rc |= run_fs4_left_active_case(/*enc_lockout_enabled*/ 0, /*expect_right_silent*/ 0, /*muted_slot_algid*/ 0,
                                   /*muted_slot_aes_loaded*/ 0, /*muted_slot_key*/ 0ULL);
    rc |= run_fs4_left_active_case(/*enc_lockout_enabled*/ 1, /*expect_right_silent*/ 0, /*muted_slot_algid*/ 0x80,
                                   /*muted_slot_aes_loaded*/ 0, /*muted_slot_key*/ 0ULL);
    rc |= run_fs4_left_active_case(/*enc_lockout_enabled*/ 1, /*expect_right_silent*/ 0, /*muted_slot_algid*/ 0x84,
                                   /*muted_slot_aes_loaded*/ 1, /*muted_slot_key*/ 0ULL);
    rc |= run_fs4_left_active_case(/*enc_lockout_enabled*/ 1, /*expect_right_silent*/ 0, /*muted_slot_algid*/ 0x81,
                                   /*muted_slot_aes_loaded*/ 0, /*muted_slot_key*/ 1ULL);
    rc |= run_fs4_right_active_case(/*enc_lockout_enabled*/ 1, /*expect_left_silent*/ 1, /*muted_slot_algid*/ 0,
                                    /*muted_slot_aes_loaded*/ 0, /*muted_slot_key*/ 0ULL);
    rc |= run_fs4_right_active_case_ext(/*enc_lockout_enabled*/ 1, /*expect_left_silent*/ 1,
                                        /*muted_slot_algid*/ 0, /*muted_slot_aes_loaded*/ 0,
                                        /*muted_slot_key*/ 0ULL, /*muted_slot_svc*/ 0, /*muted_slot_marker*/ 1);
    rc |= run_fs4_right_active_case(/*enc_lockout_enabled*/ 0, /*expect_left_silent*/ 0, /*muted_slot_algid*/ 0,
                                    /*muted_slot_aes_loaded*/ 0, /*muted_slot_key*/ 0ULL);
    rc |= run_fs4_right_active_case(/*enc_lockout_enabled*/ 1, /*expect_left_silent*/ 0, /*muted_slot_algid*/ 0x80,
                                    /*muted_slot_aes_loaded*/ 0, /*muted_slot_key*/ 0ULL);
    rc |= run_fs4_right_active_case(/*enc_lockout_enabled*/ 1, /*expect_left_silent*/ 0, /*muted_slot_algid*/ 0x84,
                                    /*muted_slot_aes_loaded*/ 1, /*muted_slot_key*/ 0ULL);
    rc |= run_fs4_right_active_case(/*enc_lockout_enabled*/ 1, /*expect_left_silent*/ 0, /*muted_slot_algid*/ 0x81,
                                    /*muted_slot_aes_loaded*/ 0, /*muted_slot_key*/ 1ULL);
    rc |= run_ss18_left_active_case(/*enc_lockout_enabled*/ 1, /*expect_right_silent*/ 1, /*muted_slot_algid*/ 0,
                                    /*muted_slot_aes_loaded*/ 0, /*muted_slot_key*/ 0ULL);
    rc |= run_ss18_left_active_case_ext(/*enc_lockout_enabled*/ 1, /*expect_right_silent*/ 1,
                                        /*muted_slot_algid*/ 0, /*muted_slot_aes_loaded*/ 0,
                                        /*muted_slot_key*/ 0ULL, /*muted_slot_svc*/ 0, /*muted_slot_marker*/ 1);
    rc |= run_ss18_left_active_case(/*enc_lockout_enabled*/ 0, /*expect_right_silent*/ 0, /*muted_slot_algid*/ 0,
                                    /*muted_slot_aes_loaded*/ 0, /*muted_slot_key*/ 0ULL);
    rc |= run_ss18_left_active_case(/*enc_lockout_enabled*/ 1, /*expect_right_silent*/ 0, /*muted_slot_algid*/ 0x80,
                                    /*muted_slot_aes_loaded*/ 0, /*muted_slot_key*/ 0ULL);
    rc |= run_ss18_left_active_case(/*enc_lockout_enabled*/ 1, /*expect_right_silent*/ 0, /*muted_slot_algid*/ 0x84,
                                    /*muted_slot_aes_loaded*/ 1, /*muted_slot_key*/ 0ULL);
    rc |= run_ss18_left_active_case(/*enc_lockout_enabled*/ 1, /*expect_right_silent*/ 0, /*muted_slot_algid*/ 0x81,
                                    /*muted_slot_aes_loaded*/ 0, /*muted_slot_key*/ 1ULL);
    rc |= run_ss18_right_active_case(/*enc_lockout_enabled*/ 1, /*expect_left_silent*/ 1, /*muted_slot_algid*/ 0,
                                     /*muted_slot_aes_loaded*/ 0, /*muted_slot_key*/ 0ULL);
    rc |= run_ss18_right_active_case_ext(/*enc_lockout_enabled*/ 1, /*expect_left_silent*/ 1,
                                         /*muted_slot_algid*/ 0, /*muted_slot_aes_loaded*/ 0,
                                         /*muted_slot_key*/ 0ULL, /*muted_slot_svc*/ 0, /*muted_slot_marker*/ 1);
    rc |= run_ss18_right_active_case(/*enc_lockout_enabled*/ 0, /*expect_left_silent*/ 0, /*muted_slot_algid*/ 0,
                                     /*muted_slot_aes_loaded*/ 0, /*muted_slot_key*/ 0ULL);
    rc |= run_ss18_right_active_case(/*enc_lockout_enabled*/ 1, /*expect_left_silent*/ 0, /*muted_slot_algid*/ 0x80,
                                     /*muted_slot_aes_loaded*/ 0, /*muted_slot_key*/ 0ULL);
    rc |= run_ss18_right_active_case(/*enc_lockout_enabled*/ 1, /*expect_left_silent*/ 0, /*muted_slot_algid*/ 0x84,
                                     /*muted_slot_aes_loaded*/ 1, /*muted_slot_key*/ 0ULL);
    rc |= run_ss18_right_active_case(/*enc_lockout_enabled*/ 1, /*expect_left_silent*/ 0, /*muted_slot_algid*/ 0x81,
                                     /*muted_slot_aes_loaded*/ 0, /*muted_slot_key*/ 1ULL);
    rc |= run_ss18_partial_flush_case();
    rc |= run_ss18_partial_flush_guard_case();
    rc |= run_ss18_partial_flush_slot_case();
    rc |= run_short_mono_playback_case();
    rc |= run_float_mono_playback_case();
    rc |= run_short_stereo_fdma_playback_case();
    rc |= run_short_stereo_dmr3_playback_case();
    rc |= run_beeper_float_stereo_case();
    rc |= run_beeper_short_mono_case();
    dsd_udp_audio_hooks_set((dsd_udp_audio_hooks){0});

    return rc;
}
