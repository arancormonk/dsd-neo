// SPDX-License-Identifier: GPL-3.0-or-later
// Coverage fixtures intentionally use synthetic sentinels, wrapper symbols, and
// test-only internal helper linkage to exercise guarded behavior.
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Direct tests for pure dsd_audio2.c helper policy. These helpers are static in
 * production; the target uses section garbage collection so uncalled playback
 * paths do not require live audio/device stubs.
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <sndfile.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>

#include <mbelib-neo/mbelib.h>
#include "core/audio/dsd_audio2_internal.h"
#include "core/audio/dsd_audio_internal.h"
#include "dsd-neo/core/audio.h"
#include "dsd-neo/core/audio_filters.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "dsd-neo/core/synctype_ids.h"
#include "dsd-neo/platform/audio.h"
#include "dsd-neo/platform/file_compat.h"
#include "dsd-neo/runtime/log.h"
#include "dsd-neo/runtime/p25_p2_audio_ring.h"
#include "dsd-neo/runtime/udp_audio_hooks.h"

void
dsd_neo_log_write(dsd_neo_log_level_t level, const char* format, ...) {
    (void)level;
    (void)format;
}

static int g_audio_write_calls;
static size_t g_audio_write_frames;
static int16_t g_audio_write_samples[640];
static int g_udp_blast_calls;
static size_t g_udp_blast_bytes;
static uint8_t g_udp_blast_data[256];
static int g_dsd_write_calls;
static int g_dsd_write_fd;
static size_t g_dsd_write_bytes;
static uint8_t g_dsd_write_data[256];
static int g_dmr_missing_alg_key_allowed[2];
static int g_dmr_voice_slot_allowed[2];
static int g_gate_mono_forced_enc = -1;
static int g_gate_dual_forced_enc_l = -1;
static int g_gate_dual_forced_enc_r = -1;

void
mbe_floattoshort(const float* in, short* out) {
    if (in == NULL || out == NULL) {
        return;
    }
    for (size_t i = 0; i < 160U; i++) {
        out[i] = (short)in[i];
    }
}

int
p25_p2_audio_ring_pop(dsd_state* state, int slot, float* out160) {
    (void)state;
    (void)slot;
    if (out160 != NULL) {
        for (size_t i = 0; i < 160U; i++) {
            out160[i] = 0.0f;
        }
    }
    return 0;
}

void
hpf_dL(dsd_state* state, short* input, int len) {
    (void)state;
    (void)input;
    (void)len;
}

void
hpf_dR(dsd_state* state, short* input, int len) {
    (void)state;
    (void)input;
    (void)len;
}

void
audio_mono_to_stereo_s16(const short* in, short* out, size_t n) {
    if (in == NULL || out == NULL) {
        return;
    }
    for (size_t i = 0; i < n; i++) {
        out[(i * 2U) + 0U] = in[i];
        out[(i * 2U) + 1U] = in[i];
    }
}

void
audio_mix_interleave_stereo_s16(const short* left, const short* right, size_t n, int encL, int encR,
                                short* stereo_out) {
    if (stereo_out == NULL) {
        return;
    }
    for (size_t i = 0; i < n; i++) {
        stereo_out[(i * 2U) + 0U] = (encL || left == NULL) ? 0 : left[i];
        stereo_out[(i * 2U) + 1U] = (encR || right == NULL) ? 0 : right[i];
    }
}

sf_count_t
sf_write_short(SNDFILE* sndfile, const short* ptr, sf_count_t items) {
    (void)sndfile;
    (void)ptr;
    return items;
}

void
dsd_audio_write_wav_short_block(SNDFILE* file, const short* samples, sf_count_t sample_count, const char* context) {
    (void)context;
    if (file != NULL && samples != NULL && sample_count > 0) {
        (void)sf_write_short(file, samples, sample_count);
    }
}

void
agf(const dsd_opts* opts, dsd_state* state, float samp[160], int slot) {
    (void)opts;
    (void)state;
    (void)samp;
    (void)slot;
}

void
audio_apply_gain_f32(float* buf, size_t n, float gain) {
    if (!buf) {
        return;
    }
    for (size_t i = 0; i < n; i++) {
        buf[i] *= gain;
    }
}

void
audio_mono_to_stereo_f32(const float* in, float* out, size_t n) {
    if (!in || !out) {
        return;
    }
    for (size_t i = 0; i < n; i++) {
        out[(i * 2U) + 0U] = in[i];
        out[(i * 2U) + 1U] = in[i];
    }
}

void
audio_mix_interleave_stereo_f32(const float* left, const float* right, size_t n, int encL, int encR,
                                float* stereo_out) {
    if (!stereo_out) {
        return;
    }
    for (size_t i = 0; i < n; i++) {
        stereo_out[(i * 2U) + 0U] = (encL || !left) ? 0.0f : left[i];
        stereo_out[(i * 2U) + 1U] = (encR || !right) ? 0.0f : right[i];
    }
}

void
audio_mix_mono_from_slots_f32(const float* left, const float* right, size_t n, int l_on, int r_on, float* mono_out) {
    if (!mono_out) {
        return;
    }
    for (size_t i = 0; i < n; i++) {
        float sum = 0.0f;
        int count = 0;
        if (l_on && left) {
            sum += left[i];
            count++;
        }
        if (r_on && right) {
            sum += right[i];
            count++;
        }
        mono_out[i] = (count == 0) ? 0.0f : (sum / (float)count);
    }
}

int
dsd_audio_group_gate_mono(const dsd_opts* opts, const dsd_state* state, unsigned long tg, int enc_in, int* enc_out) {
    (void)opts;
    (void)state;
    (void)tg;
    if (enc_out) {
        *enc_out = (g_gate_mono_forced_enc >= 0) ? g_gate_mono_forced_enc : enc_in;
    }
    return 0;
}

int
dsd_audio_group_gate_dual(const dsd_opts* opts, const dsd_state* state, unsigned long tgL, unsigned long tgR,
                          int encL_in, int encR_in, int* encL_out, int* encR_out) {
    (void)opts;
    (void)state;
    (void)tgL;
    (void)tgR;
    if (encL_out) {
        *encL_out = (g_gate_dual_forced_enc_l >= 0) ? g_gate_dual_forced_enc_l : encL_in;
    }
    if (encR_out) {
        *encR_out = (g_gate_dual_forced_enc_r >= 0) ? g_gate_dual_forced_enc_r : encR_in;
    }
    return 0;
}

int
dsd_audio_write(dsd_audio_stream* stream, const int16_t* buffer, size_t frames) {
    (void)stream;
    g_audio_write_calls++;
    g_audio_write_frames = frames;
    size_t samples = frames * 2U;
    if (samples > sizeof(g_audio_write_samples) / sizeof(g_audio_write_samples[0])) {
        samples = sizeof(g_audio_write_samples) / sizeof(g_audio_write_samples[0]);
    }
    if (buffer != NULL) {
        DSD_MEMCPY(g_audio_write_samples, buffer, samples * sizeof(g_audio_write_samples[0]));
    }
    return (int)frames;
}

void
dsd_udp_audio_hook_blast(const dsd_opts* opts, dsd_state* state, size_t nsam, const void* data) {
    (void)opts;
    (void)state;
    g_udp_blast_calls++;
    g_udp_blast_bytes = nsam;
    if (data != NULL) {
        size_t copy = nsam;
        if (copy > sizeof(g_udp_blast_data)) {
            copy = sizeof(g_udp_blast_data);
        }
        DSD_MEMCPY(g_udp_blast_data, data, copy);
    }
}

ssize_t
dsd_write(int fd, const void* buf, size_t count) {
    g_dsd_write_calls++;
    g_dsd_write_fd = fd;
    g_dsd_write_bytes = count;
    if (buf != NULL) {
        size_t copy = count;
        if (copy > sizeof(g_dsd_write_data)) {
            copy = sizeof(g_dsd_write_data);
        }
        DSD_MEMCPY(g_dsd_write_data, buf, copy);
    }
    return (ssize_t)count;
}

int
dsd_dmr_missing_alg_key_can_decrypt(const dsd_state* state, int slot) {
    (void)state;
    if (slot < 0 || slot > 1) {
        return 0;
    }
    return g_dmr_missing_alg_key_allowed[slot];
}

int
dsd_dmr_voice_slot_can_decrypt(const dsd_state* state, int slot, int algid, unsigned long long r_key) {
    (void)state;
    (void)algid;
    (void)r_key;
    if (slot < 0 || slot > 1) {
        return 0;
    }
    return g_dmr_voice_slot_allowed[slot];
}

static void
reset_sink_capture(void) {
    g_audio_write_calls = 0;
    g_audio_write_frames = 0;
    DSD_MEMSET(g_audio_write_samples, 0, sizeof(g_audio_write_samples));
    g_udp_blast_calls = 0;
    g_udp_blast_bytes = 0;
    DSD_MEMSET(g_udp_blast_data, 0, sizeof(g_udp_blast_data));
    g_dsd_write_calls = 0;
    g_dsd_write_fd = -1;
    g_dsd_write_bytes = 0;
    DSD_MEMSET(g_dsd_write_data, 0, sizeof(g_dsd_write_data));
}

static void
reset_dmr_decrypt_capture(void) {
    g_dmr_missing_alg_key_allowed[0] = 0;
    g_dmr_missing_alg_key_allowed[1] = 0;
    g_dmr_voice_slot_allowed[0] = 0;
    g_dmr_voice_slot_allowed[1] = 0;
}

static void
reset_gate_capture(void) {
    g_gate_mono_forced_enc = -1;
    g_gate_dual_forced_enc_l = -1;
    g_gate_dual_forced_enc_r = -1;
}

static int
expect_int(const char* label, int got, int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %d want %d\n", label, got, want);
        return 1;
    }
    return 0;
}

static int
expect_size(const char* label, size_t got, size_t want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %zu want %zu\n", label, got, want);
        return 1;
    }
    return 0;
}

static int
expect_float(const char* label, float got, float want) {
    const float diff = got > want ? got - want : want - got;
    if (diff > 1e-6f) {
        DSD_FPRINTF(stderr, "%s: got %.6f want %.6f diff %.6f\n", label, (double)got, (double)want, (double)diff);
        return 1;
    }
    return 0;
}

static int
expect_bytes(const char* label, const void* got, const void* want, size_t len) {
    if (memcmp(got, want, len) != 0) {
        DSD_FPRINTF(stderr, "%s: bytes differ\n", label);
        return 1;
    }
    return 0;
}

static int
test_p25_and_nxdn_decrypt_gate_helpers(void) {
    static dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));
    state.synctype = DSD_SYNC_P25P1_POS;

    int rc = 0;
    state.payload_algid = 0x80;
    rc |= expect_int("p25 clear algid not encrypted", dsd_p25_algid_is_encrypted(&state), 0);
    state.payload_algid = 0x81;
    rc |= expect_int("p25 DES algid encrypted", dsd_p25_algid_is_encrypted(&state), 1);
    rc |= expect_int("p25 DES without key blocked", dsd_p25_algid_can_decrypt(&state), 0);
    state.R = 0x123456789ABCDEF0ULL;
    rc |= expect_int("p25 DES with key allowed", dsd_p25_algid_can_decrypt(&state), 1);
    state.R = 0;
    state.payload_algid = 0x84;
    rc |= expect_int("p25 AES without key blocked", dsd_p25_algid_can_decrypt(&state), 0);
    state.aes_key_loaded[0] = 1;
    rc |= expect_int("p25 AES with key allowed", dsd_p25_algid_can_decrypt(&state), 1);
    state.payload_algid = 0x22;
    rc |= expect_int("p25 unsupported algid blocked", dsd_p25_algid_can_decrypt(&state), 0);

    DSD_MEMSET(&state, 0, sizeof(state));
    state.nxdn_cipher_type = 0x1;
    rc |= expect_int("nxdn RC4 without key blocked", dsd_nxdn_can_decrypt(&state), 0);
    state.R = 0x44;
    rc |= expect_int("nxdn RC4 with key allowed", dsd_nxdn_can_decrypt(&state), 1);
    state.R = 0;
    state.nxdn_cipher_type = 0x3;
    rc |= expect_int("nxdn AES without key blocked", dsd_nxdn_can_decrypt(&state), 0);
    state.aes_key_loaded[0] = 1;
    rc |= expect_int("nxdn AES with key allowed", dsd_nxdn_can_decrypt(&state), 1);
    state.nxdn_cipher_type = 0x7;
    rc |= expect_int("nxdn unsupported cipher blocked", dsd_nxdn_can_decrypt(&state), 0);
    return rc;
}

static int
test_output_helpers_dispatch_to_configured_sinks(void) {
    static dsd_opts opts;
    static dsd_state state;
    dsd_audio_stream* fake_stream = (dsd_audio_stream*)&opts;
    float float_samples[4] = {2.0f, -2.0f, 0.5f, -0.5f};
    short short_samples[4] = {101, -202, 303, -404};
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    opts.audio_out_stream = fake_stream;
    opts.pulse_digi_out_channels = 2;

    int rc = 0;
    reset_sink_capture();
    dsd_output_float_block(&opts, &state, float_samples, 2, 2);
    rc |= expect_int("float-output-disabled-audio", g_audio_write_calls, 0);
    dsd_output_s16_block(&opts, &state, short_samples, 2, 2);
    rc |= expect_int("s16-output-disabled-audio", g_audio_write_calls, 0);
    rc |= expect_int("s16-output-disabled-udp", g_udp_blast_calls, 0);
    rc |= expect_int("s16-output-disabled-fd", g_dsd_write_calls, 0);

    opts.audio_out = 1;
    opts.audio_out_type = 0;
    dsd_output_float_block(&opts, &state, NULL, 2, 2);
    rc |= expect_int("float-output-null-audio", g_audio_write_calls, 0);

    dsd_output_float_block(&opts, &state, float_samples, 2, 2);
    rc |= expect_int("float-output-audio-calls", g_audio_write_calls, 1);
    rc |= expect_size("float-output-audio-frames", g_audio_write_frames, 2);
    rc |= expect_int("float-output-clip-high", g_audio_write_samples[0], 32767);
    rc |= expect_int("float-output-clip-low", g_audio_write_samples[1], -32768);
    rc |= expect_int("float-output-half-positive", g_audio_write_samples[2], 16383);
    rc |= expect_int("float-output-half-negative", g_audio_write_samples[3], -16383);

    reset_sink_capture();
    opts.audio_out_type = 8;
    dsd_output_float_block(&opts, &state, float_samples, 2, 2);
    rc |= expect_int("float-output-udp-calls", g_udp_blast_calls, 1);
    rc |= expect_size("float-output-udp-bytes", g_udp_blast_bytes, sizeof(float_samples));
    rc |= expect_bytes("float-output-udp-data", g_udp_blast_data, float_samples, sizeof(float_samples));

    reset_sink_capture();
    opts.audio_out_type = 1;
    opts.audio_out_fd = 42;
    dsd_output_float_block(&opts, &state, float_samples, 2, 2);
    rc |= expect_int("float-output-fd-calls", g_dsd_write_calls, 1);
    rc |= expect_int("float-output-fd", g_dsd_write_fd, 42);
    rc |= expect_size("float-output-fd-bytes", g_dsd_write_bytes, sizeof(float_samples));
    rc |= expect_bytes("float-output-fd-data", g_dsd_write_data, float_samples, sizeof(float_samples));

    reset_sink_capture();
    opts.audio_out_type = 0;
    dsd_output_s16_block(&opts, &state, short_samples, 2, 2);
    rc |= expect_int("s16-output-audio-calls", g_audio_write_calls, 1);
    rc |= expect_size("s16-output-audio-frames", g_audio_write_frames, 2);
    rc |= expect_bytes("s16-output-audio-data", g_audio_write_samples, short_samples, sizeof(short_samples));

    reset_sink_capture();
    opts.audio_out_type = 8;
    dsd_output_s16_block(&opts, &state, short_samples, 2, 2);
    rc |= expect_int("s16-output-udp-calls", g_udp_blast_calls, 1);
    rc |= expect_size("s16-output-udp-bytes", g_udp_blast_bytes, sizeof(short_samples));
    rc |= expect_bytes("s16-output-udp-data", g_udp_blast_data, short_samples, sizeof(short_samples));

    return rc;
}

static int
test_output_block_helpers_skip_silent_trailing_blocks(void) {
    static dsd_opts opts;
    static dsd_state state;
    dsd_audio_stream* fake_stream = (dsd_audio_stream*)&opts;
    float zero_f[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float tiny_f[4] = {1e-13f, 0.0f, 0.0f, 0.0f};
    float audio_f[4] = {0.25f, 0.0f, 0.0f, 0.0f};
    const float* float_blocks[] = {zero_f, audio_f, tiny_f};
    short zero_s[4] = {0, 0, 0, 0};
    short audio_s[4] = {0, -77, 0, 0};
    const short* short_blocks[] = {zero_s, audio_s, zero_s};
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    opts.audio_out = 1;
    opts.audio_out_type = 8;
    opts.audio_out_stream = fake_stream;
    opts.pulse_digi_out_channels = 2;

    int rc = 0;
    reset_sink_capture();
    dsd_output_float_blocks(&opts, &state, float_blocks, 3, 2, 2, 1);
    rc |= expect_int("float-blocks-skip-silent-calls", g_udp_blast_calls, 1);
    rc |= expect_size("float-blocks-skip-silent-bytes", g_udp_blast_bytes, sizeof(audio_f));
    rc |= expect_bytes("float-blocks-skip-silent-data", g_udp_blast_data, audio_f, sizeof(audio_f));

    reset_sink_capture();
    dsd_output_s16_blocks(&opts, &state, short_blocks, 3, 2, 2, 1);
    rc |= expect_int("s16-blocks-skip-silent-calls", g_udp_blast_calls, 1);
    rc |= expect_size("s16-blocks-skip-silent-bytes", g_udp_blast_bytes, sizeof(audio_s));
    rc |= expect_bytes("s16-blocks-skip-silent-data", g_udp_blast_data, audio_s, sizeof(audio_s));

    return rc;
}

static int
test_output_ring_reset_helpers_preserve_below_threshold_and_clear_at_limit(void) {
    static dsd_state state;
    static short left_s[200];
    static short right_s[200];
    static float left_f[200];
    static float right_f[200];
    DSD_MEMSET(&state, 0, sizeof(state));
    for (int i = 0; i < 200; i++) {
        left_s[i] = (short)(i + 1);
        right_s[i] = (short)(i + 2);
        left_f[i] = (float)(i + 3);
        right_f[i] = (float)(i + 4);
    }
    state.audio_out_buf = left_s;
    state.audio_out_buf_p = left_s + 42;
    state.audio_out_float_buf = left_f;
    state.audio_out_float_buf_p = left_f + 42;
    state.audio_out_bufR = right_s;
    state.audio_out_buf_pR = right_s + 43;
    state.audio_out_float_bufR = right_f;
    state.audio_out_float_buf_pR = right_f + 43;
    state.audio_out_idx2 = 799999;
    state.audio_out_idx2R = 799999;

    int rc = 0;
    dsd_audio_maybe_reset_output_ring_left(&state);
    dsd_audio_maybe_reset_output_ring_right(&state);
    rc |= expect_int("ring-left-below-threshold-index", state.audio_out_idx2, 799999);
    rc |= expect_int("ring-right-below-threshold-index", state.audio_out_idx2R, 799999);
    rc |= expect_int("ring-left-below-threshold-short", left_s[0], 1);
    rc |= expect_float("ring-right-below-threshold-float", right_f[0], 4.0f);

    state.audio_out_idx2 = 800000;
    state.audio_out_idx2R = 800000;
    dsd_audio_maybe_reset_output_ring_left(&state);
    dsd_audio_maybe_reset_output_ring_right(&state);
    rc |= expect_int("ring-left-reset-index", state.audio_out_idx2, 0);
    rc |= expect_int("ring-right-reset-index", state.audio_out_idx2R, 0);
    rc |= expect_int("ring-left-reset-short-zero", left_s[99], 0);
    rc |= expect_int("ring-right-reset-short-zero", right_s[99], 0);
    rc |= expect_float("ring-left-reset-float-zero", left_f[99], 0.0f);
    rc |= expect_float("ring-right-reset-float-zero", right_f[99], 0.0f);
    rc |= expect_int("ring-left-reset-short-tail", left_s[100], 101);
    rc |= expect_int("ring-right-reset-short-tail", right_s[100], 102);
    rc |= expect_int("ring-left-reset-short-pointer", state.audio_out_buf_p == left_s + 100, 1);
    rc |= expect_int("ring-right-reset-short-pointer", state.audio_out_buf_pR == right_s + 100, 1);
    rc |= expect_int("ring-left-reset-float-pointer", state.audio_out_float_buf_p == left_f + 100, 1);
    rc |= expect_int("ring-right-reset-float-pointer", state.audio_out_float_buf_pR == right_f + 100, 1);

    return rc;
}

static int
test_dmr_slot_mute_and_duplication_helpers(void) {
    static dsd_opts opts;
    static dsd_state state;
    int encL = -1;
    int encR = -1;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

    opts.dmr_mute_encL = 1;
    opts.dmr_mute_encR = 1;
    state.dmr_encL = 1;
    state.dmr_encR = 0;
    dsd_dmr_init_slot_mute_flags(&opts, &state, &encL, &encR);

    int rc = 0;
    rc |= expect_int("dmr left encrypted muted", encL, 1);
    rc |= expect_int("dmr right clear unmuted", encR, 0);

    state.baofeng_ap = 1;
    dsd_dmr_init_slot_mute_flags(&opts, &state, &encL, &encR);
    rc |= expect_int("forced privacy unmutes left", encL, 0);
    rc |= expect_int("forced privacy keeps right unmuted", encR, 0);

    float a[320] = {0};
    float b[320] = {0};
    float c[320] = {0};
    a[0] = 1.0f;
    b[0] = 2.0f;
    c[0] = 3.0f;
    encL = 0;
    encR = 1;
    dsd_duplicate_active_float_slot_to_stereo(a, b, c, encL, encR, &encL, &encR);
    rc |= expect_float("copy left to right a", a[1], 1.0f);
    rc |= expect_float("copy left to right b", b[1], 2.0f);
    rc |= expect_float("copy left to right c", c[1], 3.0f);
    rc |= expect_int("right mute cleared", encR, 0);

    a[2] = 4.0f;
    a[3] = 5.0f;
    b[2] = 6.0f;
    b[3] = 7.0f;
    c[2] = 8.0f;
    c[3] = 9.0f;
    encL = 1;
    encR = 0;
    dsd_duplicate_active_float_slot_to_stereo(a, b, c, encL, encR, &encL, &encR);
    rc |= expect_float("copy right to left a", a[2], 5.0f);
    rc |= expect_float("copy right to left b", b[2], 7.0f);
    rc |= expect_float("copy right to left c", c[2], 9.0f);
    rc |= expect_int("left mute cleared", encL, 0);
    return rc;
}

static int
test_dmr_ss3_decrypt_hold_and_copy_policy_helpers(void) {
    static dsd_opts opts;
    static dsd_state state;
    int encL = -1;
    int encR = -1;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    reset_dmr_decrypt_capture();

    state.dmr_so = 0x40;
    state.dmr_soR = 0x40;
    dsd_dmr_ss3_init_enc_flags(&state, &encL, &encR);

    int rc = 0;
    rc |= expect_int("ss3 missing keys keep left muted", encL, 1);
    rc |= expect_int("ss3 missing keys keep right muted", encR, 1);

    g_dmr_missing_alg_key_allowed[0] = 1;
    state.payload_algidR = 0x81;
    g_dmr_voice_slot_allowed[1] = 1;
    dsd_dmr_ss3_init_enc_flags(&state, &encL, &encR);
    rc |= expect_int("ss3 missing-alg key unmutes left", encL, 0);
    rc |= expect_int("ss3 explicit voice key unmutes right", encR, 0);

    reset_dmr_decrypt_capture();
    state.payload_algidR = 0;
    state.baofeng_ap = 1;
    dsd_dmr_ss3_init_enc_flags(&state, &encL, &encR);
    rc |= expect_int("ss3 forced privacy unmutes left", encL, 0);
    rc |= expect_int("ss3 forced privacy unmutes right", encR, 0);

    state.baofeng_ap = 0;
    state.tg_hold = 999;
    encL = 0;
    encR = 0;
    opts.slot_preference = -1;
    dsd_dmr_apply_tg_hold_and_slot_preference_ss3(&opts, &state, 111, 222, &encL, &encR);
    rc |= expect_int("ss3 tg hold mismatch mutes left", encL, 1);
    rc |= expect_int("ss3 tg hold mismatch mutes right", encR, 1);
    rc |= expect_int("ss3 tg hold mismatch sets dual preference", opts.slot_preference, 2);

    encL = 1;
    encR = 1;
    state.tg_hold = 111;
    dsd_dmr_apply_tg_hold_and_slot_preference_ss3(&opts, &state, 111, 222, &encL, &encR);
    rc |= expect_int("ss3 tg hold left unmutes left", encL, 0);
    rc |= expect_int("ss3 tg hold left enables slot1", opts.slot1_on, 1);
    rc |= expect_int("ss3 tg hold left preference", opts.slot_preference, 0);

    encL = 1;
    encR = 1;
    opts.slot2_on = 0;
    state.tg_hold = 222;
    dsd_dmr_apply_tg_hold_and_slot_preference_ss3(&opts, &state, 111, 222, &encL, &encR);
    rc |= expect_int("ss3 tg hold right unmutes right", encR, 0);
    rc |= expect_int("ss3 tg hold right enables slot2", opts.slot2_on, 1);
    rc |= expect_int("ss3 tg hold right preference", opts.slot_preference, 1);

    DSD_MEMSET(state.s_l4, 0, sizeof(state.s_l4));
    DSD_MEMSET(state.s_r4, 0, sizeof(state.s_r4));
    state.s_l4[0][0] = 11;
    state.s_r4[0][0] = 44;
    opts.slot1_on = 0;
    opts.slot2_on = 1;
    dsd_dmr_apply_stereo_output_policy_ss3(&opts, &state, 0, 0);
    rc |= expect_int("ss3 slot2 only copies right to left", state.s_l4[0][0], 44);

    state.s_l4[0][1] = 55;
    state.s_r4[0][1] = 66;
    opts.slot1_on = 1;
    opts.slot2_on = 0;
    dsd_dmr_apply_stereo_output_policy_ss3(&opts, &state, 0, 0);
    rc |= expect_int("ss3 slot1 only copies left to right", state.s_r4[0][1], 55);

    return rc;
}

static int
test_p25p2_encrypted_lockout_slot_helper(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

    int rc = 0;
    state.payload_algidR = 0x81;
    state.p25_crypto_state[1] = DSD_P25_CRYPTO_BLOCKED;
    rc |= expect_int("p25p2 right encrypted without key locks out",
                     dsd_p25p2_encrypted_lockout_slot_muted(&opts, &state, 1, 1), 1);
    state.RR = 0x1234;
    state.p25_crypto_state[1] = DSD_P25_CRYPTO_DECRYPTABLE;
    rc |= expect_int("p25p2 right key suppresses lockout", dsd_p25p2_encrypted_lockout_slot_muted(&opts, &state, 1, 1),
                     0);
    state.RR = 0;
    opts.trunk_tune_enc_calls = 1;
    state.p25_crypto_state[1] = DSD_P25_CRYPTO_ENCRYPTED_PENDING;
    rc |= expect_int("p25p2 pending state remains slot-suppressed",
                     dsd_p25p2_encrypted_lockout_slot_muted(&opts, &state, 1, 1), 1);
    opts.trunk_tune_enc_calls = 0;
    state.p25_crypto_state[1] = DSD_P25_CRYPTO_BLOCKED;
    rc |= expect_int("p25p2 existing lockout label stays muted",
                     dsd_p25p2_encrypted_lockout_slot_muted(&opts, &state, 1, 1), 1);
    rc |=
        expect_int("p25p2 unmuted slot skips lockout", dsd_p25p2_encrypted_lockout_slot_muted(&opts, &state, 1, 0), 0);
    rc |=
        expect_int("p25p2 invalid slot skips lockout", dsd_p25p2_encrypted_lockout_slot_muted(&opts, &state, 2, 1), 0);
    return rc;
}

static int
test_p25p2_ss18_slot_preference_and_copy_policy_helpers(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

    int rc = 0;
    state.tg_hold = 111;
    dsd_p25p2_apply_slot_preference_ss18(&opts, &state, 111, 222);
    rc |= expect_int("ss18 tg hold left enables slot1", opts.slot1_on, 1);
    rc |= expect_int("ss18 tg hold left preference", opts.slot_preference, 0);

    opts.slot2_on = 0;
    state.tg_hold = 222;
    dsd_p25p2_apply_slot_preference_ss18(&opts, &state, 111, 222);
    rc |= expect_int("ss18 tg hold right enables slot2", opts.slot2_on, 1);
    rc |= expect_int("ss18 tg hold right preference", opts.slot_preference, 1);

    state.tg_hold = 333;
    dsd_p25p2_apply_slot_preference_ss18(&opts, &state, 111, 222);
    rc |= expect_int("ss18 tg hold mismatch sets dual preference", opts.slot_preference, 2);

    DSD_MEMSET(state.s_l4, 0, sizeof(state.s_l4));
    DSD_MEMSET(state.s_r4, 0, sizeof(state.s_r4));
    state.s_l4[0][0] = 10;
    state.s_r4[0][0] = 20;
    opts.slot1_on = 0;
    opts.slot2_on = 1;
    state.payload_algid = 0x81;
    state.p25_crypto_state[0] = DSD_P25_CRYPTO_BLOCKED;
    dsd_p25p2_apply_stereo_output_policy_ss18(&opts, &state, 1, 0);
    rc |= expect_int("ss18 encrypted left lockout blocks right-to-left copy", state.s_l4[0][0], 0);
    rc |= expect_int("ss18 right retained when left lockout", state.s_r4[0][0], 20);

    state.s_l4[0][0] = 10;
    state.s_r4[0][0] = 20;
    state.R = 0x12345678ULL;
    state.p25_crypto_state[0] = DSD_P25_CRYPTO_DECRYPTABLE;
    dsd_p25p2_apply_stereo_output_policy_ss18(&opts, &state, 1, 0);
    rc |= expect_int("ss18 keyed left allows right-to-left copy", state.s_l4[0][0], 20);

    state.s_l4[0][1] = 30;
    state.s_r4[0][1] = 40;
    opts.slot1_on = 1;
    opts.slot2_on = 0;
    state.payload_algidR = 0x84;
    state.aes_key_loaded[1] = 0;
    state.p25_crypto_state[1] = DSD_P25_CRYPTO_BLOCKED;
    dsd_p25p2_apply_stereo_output_policy_ss18(&opts, &state, 0, 1);
    rc |= expect_int("ss18 encrypted right lockout blocks left-to-right copy", state.s_r4[0][1], 0);

    state.s_l4[0][1] = 30;
    state.s_r4[0][1] = 40;
    state.aes_key_loaded[1] = 1;
    state.p25_crypto_state[1] = DSD_P25_CRYPTO_DECRYPTABLE;
    dsd_p25p2_apply_stereo_output_policy_ss18(&opts, &state, 0, 1);
    rc |= expect_int("ss18 keyed right allows left-to-right copy", state.s_r4[0][1], 30);

    return rc;
}

static int
test_fs4_mono_mixer_averages_available_unmuted_slots(void) {
    float lf[4][160];
    float rf[4][160];
    float mono[4][160];
    int l_ok[4] = {1, 0, 1, 1};
    int r_ok[4] = {1, 1, 0, 1};
    DSD_MEMSET(lf, 0, sizeof(lf));
    DSD_MEMSET(rf, 0, sizeof(rf));
    DSD_MEMSET(mono, 0, sizeof(mono));
    for (int j = 0; j < 4; j++) {
        lf[j][0] = (float)(10 + j);
        rf[j][0] = (float)(20 + j);
    }

    int rc = 0;
    dsd_fs4_mix_mono_frames(lf, rf, 0, 0, l_ok, r_ok, mono);
    rc |= expect_float("fs4 mono averages both slots", mono[0][0], 15.0f);
    rc |= expect_float("fs4 mono uses right-only frame", mono[1][0], 21.0f);
    rc |= expect_float("fs4 mono uses left-only frame", mono[2][0], 12.0f);
    rc |= expect_float("fs4 mono averages final frame", mono[3][0], 18.0f);

    DSD_MEMSET(mono, 0, sizeof(mono));
    dsd_fs4_mix_mono_frames(lf, rf, 1, 0, l_ok, r_ok, mono);
    rc |= expect_float("fs4 mono left-muted uses right", mono[0][0], 20.0f);
    rc |= expect_float("fs4 mono left-muted no available right stays zero", mono[2][0], 0.0f);

    DSD_MEMSET(mono, 0, sizeof(mono));
    dsd_fs4_mix_mono_frames(lf, rf, 0, 1, l_ok, r_ok, mono);
    rc |= expect_float("fs4 mono right-muted uses left", mono[0][0], 10.0f);
    rc |= expect_float("fs4 mono right-muted no available left stays zero", mono[1][0], 0.0f);
    return rc;
}

static int
test_float_playback_orchestrators_emit_expected_blocks(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    reset_gate_capture();
    opts.audio_out = 1;
    opts.audio_out_type = 8;
    opts.slot1_on = 1;
    opts.slot2_on = 1;
    opts.dmr_mute_encL = 1;
    opts.dmr_mute_encR = 1;
    opts.pulse_digi_out_channels = 2;
    state.synctype = DSD_SYNC_P25P1_POS;
    state.p25_crypto_state[0] = DSD_P25_CRYPTO_CLEAR;
    state.f_l[0] = 0.5f;
    state.f_l[1] = -0.25f;

    int rc = 0;
    reset_sink_capture();
    playSynthesizedVoiceFS(&opts, &state);
    const float* fs_stereo = (const float*)g_udp_blast_data;
    rc |= expect_int("fs stereo output call", g_udp_blast_calls, 1);
    rc |= expect_size("fs stereo output bytes", g_udp_blast_bytes, 320U * sizeof(float));
    rc |= expect_float("fs stereo left sample scaled", fs_stereo[0], 0.25f);
    rc |= expect_float("fs stereo right sample scaled", fs_stereo[1], 0.25f);
    rc |= expect_float("fs stereo next left scaled", fs_stereo[2], -0.125f);
    rc |= expect_float("fs clears temp working buffer", state.audio_out_temp_buf[0], 0.0f);

    opts.reverse_mute = 1;
    state.f_l[0] = 0.75f;
    reset_sink_capture();
    playSynthesizedVoiceFS(&opts, &state);
    rc |= expect_int("fs reverse mute suppresses clear P25", g_udp_blast_calls, 0);
    opts.reverse_mute = 0;

    state.f_l[0] = 0.75f;
    state.payload_algid = 0x81;
    state.R = 0;
    state.p25_crypto_state[0] = DSD_P25_CRYPTO_BLOCKED;
    reset_sink_capture();
    playSynthesizedVoiceFS(&opts, &state);
    rc |= expect_int("fs encrypted without key skips output", g_udp_blast_calls, 0);
    rc |= expect_float("fs encrypted path clears temp buffer", state.audio_out_temp_buf[0], 0.0f);

    opts.trunk_tune_enc_calls = 1;
    opts.unmute_encrypted_p25 = 1;
    state.f_l[0] = 0.75f;
    reset_sink_capture();
    playSynthesizedVoiceFS(&opts, &state);
    rc |= expect_int("fs explicit P25 unmute emits undeciphered audio", g_udp_blast_calls, 1);

    opts.trunk_tune_enc_calls = 0;
    state.f_l[0] = 0.75f;
    reset_sink_capture();
    playSynthesizedVoiceFS(&opts, &state);
    rc |= expect_int("fs P25 lockout probe overrides explicit unmute", g_udp_blast_calls, 0);
    opts.unmute_encrypted_p25 = 0;
    opts.trunk_tune_enc_calls = 1;

    opts.reverse_mute = 1;
    state.f_l[0] = 0.75f;
    reset_sink_capture();
    playSynthesizedVoiceFS(&opts, &state);
    rc |= expect_int("fs reverse mute emits encrypted P25", g_udp_blast_calls, 1);
    opts.reverse_mute = 0;

    DSD_MEMSET(&state, 0, sizeof(state));
    opts.pulse_digi_out_channels = 1;
    opts.reverse_mute = 1;
    state.synctype = DSD_SYNC_P25P1_POS;
    state.p25_crypto_state[0] = DSD_P25_CRYPTO_CLEAR;
    state.f_l[0] = 0.625f;
    reset_sink_capture();
    playSynthesizedVoiceFM(&opts, &state);
    rc |= expect_int("fm reverse mute suppresses clear P25", g_udp_blast_calls, 0);
    opts.reverse_mute = 0;

    DSD_MEMSET(&state, 0, sizeof(state));
    state.synctype = DSD_SYNC_P25P1_POS;
    state.mbe_file_type = 3;
    state.p25_crypto_state[0] = DSD_P25_CRYPTO_UNKNOWN;
    state.f_l[0] = 0.625f;
    reset_sink_capture();
    playSynthesizedVoiceFM(&opts, &state);
    rc |= expect_int("sdrtrunk json P25 playback bypasses live crypto state", g_udp_blast_calls, 1);
    rc |= expect_size("sdrtrunk json P25 playback bytes", g_udp_blast_bytes, 160U * sizeof(float));
    rc |= expect_float("sdrtrunk json P25 playback sample", ((const float*)g_udp_blast_data)[0], 0.625f);

    state.payload_algid = 0x81;
    state.f_l[0] = -0.5f;
    reset_sink_capture();
    playSynthesizedVoiceFM(&opts, &state);
    rc |= expect_int("sdrtrunk json decrypted P25 bypasses live ALGID gate", g_udp_blast_calls, 1);
    rc |= expect_float("sdrtrunk json decrypted P25 sample", ((const float*)g_udp_blast_data)[0], -0.5f);

    state.mbe_file_type = 0;
    state.f_l[0] = 0.75f;
    reset_sink_capture();
    playSynthesizedVoiceFM(&opts, &state);
    rc |= expect_int("live P25 unknown crypto remains muted", g_udp_blast_calls, 0);

    DSD_MEMSET(&state, 0, sizeof(state));
    opts.pulse_digi_out_channels = 1;
    state.f_l4[0][0] = 0.25f;
    state.f_r4[0][0] = 0.75f;
    state.f_l4[1][0] = 0.5f;
    state.f_r4[1][0] = 1.0f;
    state.f_l4[2][0] = -0.25f;
    state.f_r4[2][0] = 0.25f;
    reset_sink_capture();
    playSynthesizedVoiceFS3(&opts, &state);
    const float* fs3_mono = (const float*)g_udp_blast_data;
    rc |= expect_int("fs3 mono output calls", g_udp_blast_calls, 3);
    rc |= expect_size("fs3 mono output bytes", g_udp_blast_bytes, 160U * sizeof(float));
    rc |= expect_float("fs3 mono captures final block average", fs3_mono[0], 0.0f);
    rc |= expect_float("fs3 resets left block", state.f_l4[0][0], 0.0f);

    DSD_MEMSET(&state, 0, sizeof(state));
    state.dmr_encL = 1;
    state.dmr_encR = 1;
    reset_sink_capture();
    playSynthesizedVoiceFS3(&opts, &state);
    rc |= expect_int("fs3 both muted skips output", g_udp_blast_calls, 0);
    reset_gate_capture();
    return rc;
}

static int
test_silent_s16_helper(void) {
    short all_zero[4] = {0, 0, 0, 0};
    short with_audio[4] = {0, 0, -1, 0};
    int rc = 0;
    rc |= expect_int("null s16 is silent", dsd_is_all_zero_s16(NULL, 4), 1);
    rc |= expect_int("zero s16 is silent", dsd_is_all_zero_s16(all_zero, 4), 1);
    rc |= expect_int("nonzero s16 is not silent", dsd_is_all_zero_s16(with_audio, 4), 0);
    return rc;
}

int
main(void) {
    int rc = 0;
    rc |= test_p25_and_nxdn_decrypt_gate_helpers();
    rc |= test_output_helpers_dispatch_to_configured_sinks();
    rc |= test_output_block_helpers_skip_silent_trailing_blocks();
    rc |= test_output_ring_reset_helpers_preserve_below_threshold_and_clear_at_limit();
    rc |= test_dmr_slot_mute_and_duplication_helpers();
    rc |= test_dmr_ss3_decrypt_hold_and_copy_policy_helpers();
    rc |= test_p25p2_encrypted_lockout_slot_helper();
    rc |= test_p25p2_ss18_slot_preference_and_copy_policy_helpers();
    rc |= test_fs4_mono_mixer_averages_available_unmuted_slots();
    rc |= test_float_playback_orchestrators_emit_expected_blocks();
    rc |= test_silent_s16_helper();
    return rc;
}
