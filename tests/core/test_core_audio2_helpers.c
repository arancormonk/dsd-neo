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

#include <assert.h>
#include <dsd-neo/core/audio_activity.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/p25p2_playout.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <math.h>
#include <sndfile.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mbelib-neo/mbelib.h>
#include "core/audio/dsd_audio2_internal.h"
#include "core/audio/dsd_audio_internal.h"
#include "dsd-neo/core/audio.h"
#include "dsd-neo/core/audio_filters.h"
#include "dsd-neo/core/file_io.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "dsd-neo/core/synctype_ids.h"
#include "dsd-neo/platform/audio.h"
#include "dsd-neo/platform/file_compat.h"
#include "dsd-neo/runtime/log.h"
#include "dsd-neo/runtime/udp_audio_hooks.h"

void
dsd_neo_log_write(dsd_neo_log_level_t level, const char* format, ...) {
    (void)level;
    (void)format;
}

// The mixer's decision-trace diagnostic writes through the P25 SM log; these
// helpers link dsd_audio2.c standalone, so stub it disabled.
int
dsd_p25_sm_log_enabled(const dsd_opts* opts) {
    (void)opts;
    return 0;
}

void
dsd_p25_sm_logf(dsd_opts* opts, const char* format, ...) {
    (void)opts;
    (void)format;
}

static int g_audio_write_calls;
static size_t g_audio_write_frames;
static size_t g_audio_write_frame_log[8];
static int g_audio_write_channels = 2;
static int16_t g_audio_write_samples[1920];
static int g_udp_blast_calls;
static size_t g_udp_blast_bytes;
static uint8_t g_udp_blast_data[1920 * sizeof(float)];
/* Every datagram's length, and their bytes end to end, since reset_sink_capture(). */
static size_t g_udp_blast_sizes[8];
static uint8_t g_udp_blast_stream[sizeof(float) * 4U * 1920U];
static size_t g_udp_blast_stream_bytes;
static int g_dsd_write_calls;
static int g_dsd_write_fd;
static size_t g_dsd_write_bytes;
static uint8_t g_dsd_write_data[1920 * sizeof(float)];
static int g_sf_write_short_calls;
static short g_sf_written[18 * 320];
static size_t g_sf_written_count;
static int g_record_policy_blocked[2];
static int g_gate_mono_forced_enc = -1;
static int g_gate_dual_forced_enc_l = -1;
static int g_gate_dual_forced_enc_r = -1;
static unsigned long g_gate_mono_tg;

void
mbe_floattoshort(const float* in, short* out) {
    if (in == NULL || out == NULL) {
        return;
    }
    for (size_t i = 0; i < 160U; i++) {
        out[i] = (short)in[i];
    }
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

void
audio_mix_mono_from_slots_s16(const short* left, const short* right, size_t n, int l_on, int r_on, short* out) {
    if (out == NULL) {
        return;
    }
    for (size_t i = 0; i < n; i++) {
        if (l_on && r_on && left && right) {
            out[i] = (short)(((int)left[i] + (int)right[i]) / 2);
        } else if (l_on && left) {
            out[i] = left[i];
        } else if (r_on && right) {
            out[i] = right[i];
        } else {
            out[i] = 0;
        }
    }
}

// The P25 Phase 2 flush entry points in dsd_audio2.c hand over to the playout, which this target does not link.
void
dsd_p25p2_playout_drain(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
}

void
dsd_p25p2_playout_reset(dsd_state* state, int slot) {
    (void)state;
    (void)slot;
}

void
dsd_p25p2_playout_close(dsd_state* state, int slot) {
    (void)state;
    (void)slot;
}

void
dsd_p25p2_playout_discard(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
}

// No queued P25 Phase 2 audio here: beeper() sounds every alert at once.
int
dsd_p25p2_playout_defer_alert(const dsd_opts* opts, dsd_state* state, int slot, int id, int ad, int len) {
    (void)opts;
    (void)state;
    (void)slot;
    (void)id;
    (void)ad;
    (void)len;
    return 0;
}

sf_count_t
sf_write_short(SNDFILE* sndfile, const short* ptr, sf_count_t items) {
    (void)sndfile;
    g_sf_write_short_calls++;
    for (sf_count_t i = 0;
         ptr != NULL && i < items && g_sf_written_count < sizeof(g_sf_written) / sizeof(g_sf_written[0]); i++) {
        g_sf_written[g_sf_written_count++] = ptr[i];
    }
    return items;
}

void
dsd_audio_write_wav_short_block(SNDFILE* file, const short* samples, sf_count_t sample_count, const char* context) {
    (void)context;
    if (file != NULL && samples != NULL && sample_count > 0) {
        (void)sf_write_short(file, samples, sample_count);
    }
}

int
dsd_audio_group_gate_mono(const dsd_opts* opts, const dsd_state* state, unsigned long tg, int enc_in, int* enc_out) {
    (void)opts;
    (void)state;
    g_gate_mono_tg = tg;
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
dsd_audio_record_policy_gate_slot(const dsd_opts* opts, const dsd_state* state, int slot, int* allow_out) {
    (void)opts;
    (void)state;
    if (allow_out == NULL || slot < 0 || slot > 1) {
        return -1;
    }
    *allow_out = !g_record_policy_blocked[slot];
    return 0;
}

int
dsd_audio_write(dsd_audio_stream* stream, const int16_t* buffer, size_t frames) {
    (void)stream;
    if (g_audio_write_calls < (int)(sizeof(g_audio_write_frame_log) / sizeof(g_audio_write_frame_log[0]))) {
        g_audio_write_frame_log[g_audio_write_calls] = frames;
    }
    g_audio_write_calls++;
    g_audio_write_frames = frames;
    size_t samples = frames * (size_t)g_audio_write_channels;
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
    if (g_udp_blast_calls < (int)(sizeof(g_udp_blast_sizes) / sizeof(g_udp_blast_sizes[0]))) {
        g_udp_blast_sizes[g_udp_blast_calls] = nsam;
    }
    g_udp_blast_calls++;
    g_udp_blast_bytes = nsam;
    if (data != NULL && g_udp_blast_stream_bytes + nsam <= sizeof(g_udp_blast_stream)) {
        DSD_MEMCPY(g_udp_blast_stream + g_udp_blast_stream_bytes, data, nsam);
        g_udp_blast_stream_bytes += nsam;
    }
    if (data != NULL) {
        size_t copy = nsam;
        if (copy > sizeof(g_udp_blast_data)) {
            copy = sizeof(g_udp_blast_data);
        }
        DSD_MEMCPY(g_udp_blast_data, data, copy);
    }
}

/* The audio sinks (audio_sink.c, compiled in) write the -o - descriptor through this. */
dsd_write_frames_status
dsd_write_frames(int fd, const void* buf, size_t bytes, size_t frame_bytes, size_t* written) {
    (void)frame_bytes;
    g_dsd_write_calls++;
    g_dsd_write_fd = fd;
    g_dsd_write_bytes = bytes;
    if (buf != NULL) {
        size_t copy = bytes;
        if (copy > sizeof(g_dsd_write_data)) {
            copy = sizeof(g_dsd_write_data);
        }
        DSD_MEMCPY(g_dsd_write_data, buf, copy);
    }
    if (written != NULL) {
        *written = bytes;
    }
    return DSD_WRITE_FRAMES_OK;
}

int
dsd_audio_output_health_get(dsd_audio_stream* stream, dsd_audio_output_health* out) {
    (void)stream;
    DSD_MEMSET(out, 0, sizeof(*out));
    return 0;
}

static void
reset_sink_capture(void) {
    g_audio_write_calls = 0;
    g_audio_write_frames = 0;
    DSD_MEMSET(g_audio_write_frame_log, 0, sizeof(g_audio_write_frame_log));
    DSD_MEMSET(g_audio_write_samples, 0, sizeof(g_audio_write_samples));
    g_udp_blast_calls = 0;
    g_udp_blast_bytes = 0;
    DSD_MEMSET(g_udp_blast_data, 0, sizeof(g_udp_blast_data));
    DSD_MEMSET(g_udp_blast_sizes, 0, sizeof(g_udp_blast_sizes));
    g_udp_blast_stream_bytes = 0;
    g_dsd_write_calls = 0;
    g_dsd_write_fd = -1;
    g_dsd_write_bytes = 0;
    DSD_MEMSET(g_dsd_write_data, 0, sizeof(g_dsd_write_data));
    g_sf_write_short_calls = 0;
    g_sf_written_count = 0;
    DSD_MEMSET(g_sf_written, 0, sizeof(g_sf_written));
}

static void
reset_gate_capture(void) {
    g_gate_mono_forced_enc = -1;
    g_gate_dual_forced_enc_l = -1;
    g_gate_dual_forced_enc_r = -1;
    g_gate_mono_tg = 0UL;
    g_record_policy_blocked[0] = 0;
    g_record_policy_blocked[1] = 0;
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
test_p25_decrypt_gate_helpers(void) {
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

static void
fill_s16_frame(short* frame, short first, short rest) {
    frame[0] = first;
    for (int i = 1; i < 160; i++) {
        frame[i] = rest;
    }
}

// The f32 sample a float-output block carries at @p index (in the bytes captured from UDP).
static float
captured_f32(const uint8_t* bytes, size_t index) {
    float v = 0.0f;
    DSD_MEMCPY(&v, bytes + (index * sizeof(float)), sizeof(v));
    return v;
}

/* Issue #652: float output (-y) writes the one 16-bit chain's blocks as f32, s16 / 32768 (exact), to UDP and the raw
   fd, one write per block for every size a mixer writes (up to 960 stereo frames), and a larger block in whole frames.
   The local device stream is 16-bit in either format and gets the block unchanged. */
static int
test_float_output_writes_the_s16_block_as_f32(void) {
    static dsd_opts opts;
    static dsd_state state;
    static short block[1500 * 2];
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    for (size_t i = 0; i < sizeof(block) / sizeof(block[0]); i++) {
        block[i] = (short)((int)((i * 7919U) % 65536U) - 32768);
    }
    block[0] = -32768;
    block[1] = 32767;
    opts.audio_out = 1;
    opts.floating_point = 1;
    opts.audio_out_stream = (dsd_audio_stream*)&opts;
    opts.audio_out_fd = 42;

    int rc = 0;

    static const struct {
        int type;
        int channels;
        size_t frames;
        int writes;
    } cases[] = {
        {8, 1, 160U, 1}, {8, 2, 160U, 1}, {8, 1, 960U, 1},  {8, 2, 960U, 1},
        {1, 1, 160U, 1}, {1, 2, 960U, 1}, {8, 2, 1500U, 2},
    };

    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        char tag[96];
        opts.audio_out_type = cases[c].type;
        reset_sink_capture();
        dsd_output_s16_block(&opts, &state, block, cases[c].frames, cases[c].channels);
        const size_t samples = cases[c].frames * (size_t)cases[c].channels;
        const int udp = cases[c].type == 8;
        DSD_SNPRINTF(tag, sizeof(tag), "-y %s %dch %zu frames: writes", udp ? "udp" : "fd", cases[c].channels,
                     cases[c].frames);
        rc |= expect_int(tag, udp ? g_udp_blast_calls : g_dsd_write_calls, cases[c].writes);
        if (udp) {
            // Whole frames per datagram: 960 stereo frames, then the rest.
            const size_t first = (cases[c].writes == 1) ? samples : 1920U;
            DSD_SNPRINTF(tag, sizeof(tag), "-y udp %dch %zu frames: datagram 1", cases[c].channels, cases[c].frames);
            rc |= expect_size(tag, g_udp_blast_sizes[0], first * sizeof(float));
            if (cases[c].writes == 2) {
                DSD_SNPRINTF(tag, sizeof(tag), "-y udp %dch %zu frames: datagram 2", cases[c].channels,
                             cases[c].frames);
                rc |= expect_size(tag, g_udp_blast_sizes[1], (samples - first) * sizeof(float));
            }
        } else {
            DSD_SNPRINTF(tag, sizeof(tag), "-y fd %dch %zu frames: bytes", cases[c].channels, cases[c].frames);
            rc |= expect_size(tag, g_dsd_write_bytes, samples * sizeof(float));
            rc |= expect_int("-y fd number", g_dsd_write_fd, 42);
        }
        const uint8_t* bytes = udp ? g_udp_blast_stream : g_dsd_write_data;
        int exact = 1;
        for (size_t i = 0; i < samples; i++) {
            // Bit for bit: s16 / 32768 is exact in f32.
            const float want = (float)block[i] / 32768.0f;
            uint32_t want_bits = 0;
            uint32_t got_bits = 0;
            DSD_MEMCPY(&want_bits, &want, sizeof(want_bits));
            DSD_MEMCPY(&got_bits, bytes + (i * sizeof(float)), sizeof(got_bits));
            if (got_bits != want_bits) {
                exact = 0;
            }
        }
        DSD_SNPRINTF(tag, sizeof(tag), "-y %s %dch %zu frames: every sample s16 / 32768, in order", udp ? "udp" : "fd",
                     cases[c].channels, cases[c].frames);
        rc |= expect_int(tag, exact, 1);
    }
    rc |= expect_float("-y full-scale negative", captured_f32(g_udp_blast_stream, 0), -1.0f);

    // The local device stream takes the 16-bit block itself.
    opts.audio_out_type = 0;
    g_audio_write_channels = 2;
    reset_sink_capture();
    dsd_output_s16_block(&opts, &state, block, 160U, 2);
    rc |= expect_int("-y device writes", g_audio_write_calls, 1);
    rc |= expect_size("-y device frames", g_audio_write_frames, 160U);
    rc |= expect_bytes("-y device samples are the s16 block", g_audio_write_samples, block, 320U * sizeof(short));
    rc |= expect_int("-y device writes no f32", g_udp_blast_calls + g_dsd_write_calls, 0);

    // A block of no channels, or more than two, is not written.
    opts.audio_out_type = 8;
    reset_sink_capture();
    dsd_output_s16_block(&opts, &state, block, 160U, 0);
    dsd_output_s16_block(&opts, &state, block, 160U, 3);
    rc |= expect_int("-y bad channel counts write nothing", g_udp_blast_calls, 0);

    // 16-bit output keeps writing s16 to UDP and the fd.
    opts.floating_point = 0;
    reset_sink_capture();
    dsd_output_s16_block(&opts, &state, block, 960U, 2);
    rc |= expect_size("s16 udp bytes", g_udp_blast_bytes, 1920U * sizeof(short));
    rc |= expect_bytes("s16 udp samples", g_udp_blast_data, block, 1920U * sizeof(short));
    return rc;
}

/* Issue #659: a float block longer than the conversion buffer goes to the device in chunks, each written with the
   frame count it converted, in the caller's channel count, so the backend never reads past what was converted. */
static int
test_float_device_write_converts_in_bounded_chunks(void) {
    static dsd_opts opts;
    static float block[400 * 2];
    DSD_MEMSET(&opts, 0, sizeof(opts));
    opts.audio_out_stream = (dsd_audio_stream*)&opts;
    opts.pulse_digi_out_channels = 1; /* not what the caller writes: its own count is used */
    for (size_t i = 0; i < sizeof(block) / sizeof(block[0]); i++) {
        block[i] = (float)(i % 100U) / 1000.0f;
    }

    int rc = 0;
    reset_sink_capture();
    g_audio_write_channels = 2;
    dsd_write_float_audio_device(&opts, block, 400, 2);
    rc |= expect_int("float-chunks-calls", g_audio_write_calls, 2);
    rc |= expect_size("float-chunks-first", g_audio_write_frame_log[0], 320);
    rc |= expect_size("float-chunks-second", g_audio_write_frame_log[1], 80);
    /* The last chunk starts at frame 320 (sample 640): 640 % 100 = 40 -> 0.040 * 32767. */
    rc |= expect_int("float-chunks-second-first-sample", g_audio_write_samples[0], (int)(0.040f * 32767.0f));

    reset_sink_capture();
    dsd_write_float_audio_device(&opts, block, 160, 1);
    rc |= expect_int("float-mono-chunks-calls", g_audio_write_calls, 1);
    rc |= expect_size("float-mono-chunks-frames", g_audio_write_frame_log[0], 160);

    reset_sink_capture();
    dsd_write_float_audio_device(&opts, block, 400, 0);
    rc |= expect_int("float-chunks-no-channels", g_audio_write_calls, 0);
    opts.audio_out_stream = NULL;
    dsd_write_float_audio_device(&opts, block, 400, 2);
    rc |= expect_int("float-chunks-no-stream", g_audio_write_calls, 0);
    return rc;
}

static int
test_output_block_helpers_skip_silent_trailing_blocks(void) {
    static dsd_opts opts;
    static dsd_state state;
    dsd_audio_stream* fake_stream = (dsd_audio_stream*)&opts;
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
test_dmr_slot_mute_and_mono_gate_helpers(void) {
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
    dsd_dmr_ss3_init_enc_flags(&opts, &state, &encL, &encR);

    int rc = 0;
    rc |= expect_int("dmr left encrypted muted", encL, 1);
    rc |= expect_int("dmr right clear unmuted", encR, 0);

    state.baofeng_ap = 1;
    dsd_dmr_ss3_init_enc_flags(&opts, &state, &encL, &encR);
    rc |= expect_int("forced privacy unmutes left", encL, 0);
    rc |= expect_int("forced privacy keeps right unmuted", encR, 0);
    state.baofeng_ap = 0;

    state.synctype = DSD_SYNC_DMR_MS_VOICE;
    opts.dmr_mono = 1;
    state.dmr_mono_slot = 0;
    encL = 0;
    encR = 0;
    dsd_dmr_apply_mono_slot_gate(&opts, &state, &encL, &encR);
    rc |= expect_int("dmr mono slot 1 keeps left active", encL, 0);
    rc |= expect_int("dmr mono slot 1 mutes right", encR, 1);

    state.dmr_mono_slot = 1;
    encL = 0;
    encR = 0;
    dsd_dmr_apply_mono_slot_gate(&opts, &state, &encL, &encR);
    rc |= expect_int("dmr mono slot 2 mutes left", encL, 1);
    rc |= expect_int("dmr mono slot 2 keeps right active", encR, 0);

    state.synctype = DSD_SYNC_P25P1_POS;
    encL = 0;
    encR = 0;
    dsd_dmr_apply_mono_slot_gate(&opts, &state, &encL, &encR);
    rc |= expect_int("dmr mono gate leaves other protocols", encL + encR, 0);
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
    // Encrypted audio muted, the session default (initOpts()); 0 is the encrypted-audio unmute.
    opts.dmr_mute_encL = 1;
    opts.dmr_mute_encR = 1;

    // SS3 follows the vocoder's slot flags (issue #574): the vocoder flags a slot encrypted when the service options
    // carry the encryption bit and no loaded key decrypts it, and clears the flag when one does.
    state.dmr_so = 0x40;
    state.dmr_soR = 0x40;
    state.dmr_encL = 1;
    state.dmr_encR = 1;
    dsd_dmr_ss3_init_enc_flags(&opts, &state, &encL, &encR);

    int rc = 0;
    rc |= expect_int("ss3 missing keys keep left muted", encL, 1);
    rc |= expect_int("ss3 missing keys keep right muted", encR, 1);

    // A loaded key decrypts both slots, so the vocoder clears their flags and SS3 follows them.
    state.dmr_encL = 0;
    state.dmr_encR = 0;
    dsd_dmr_ss3_init_enc_flags(&opts, &state, &encL, &encR);
    rc |= expect_int("ss3 follows the vocoder's cleared left flag", encL, 0);
    rc |= expect_int("ss3 follows the vocoder's cleared right flag", encR, 0);

    // Bit set and no keys again, so both slots are flagged encrypted and only the forced-privacy override unmutes them.
    state.dmr_encL = 1;
    state.dmr_encR = 1;
    state.baofeng_ap = 1;
    dsd_dmr_ss3_init_enc_flags(&opts, &state, &encL, &encR);
    rc |= expect_int("ss3 forced privacy unmutes left", encL, 0);
    rc |= expect_int("ss3 forced privacy unmutes right", encR, 0);

    // Issue #574: the encrypted-audio mute flag is 0 after the user's unmute toggle and after any key load. The vocoder
    // then stages a slot it flagged encrypted, but SS3 keeps it muted (in both output formats, #652), so loading a key
    // does not unmute every call that key cannot decrypt.
    state.baofeng_ap = 0;
    state.dmr_encL = 1;
    state.dmr_encR = 1;
    opts.dmr_mute_encL = 0;
    opts.dmr_mute_encR = 0;
    dsd_dmr_ss3_init_enc_flags(&opts, &state, &encL, &encR);
    rc |= expect_int("ss3 keeps undecryptable left muted with encrypted audio unmuted", encL, 1);
    rc |= expect_int("ss3 keeps undecryptable right muted with encrypted audio unmuted", encR, 1);
    state.dmr_encL = 0;
    state.dmr_encR = 0;
    opts.dmr_mute_encL = 1;
    opts.dmr_mute_encR = 1;

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

    opts.dmr_mono = 1;
    state.synctype = DSD_SYNC_DMR_BS_VOICE_POS;
    state.dmr_mono_slot = 1;
    encL = 0;
    encR = 0;
    dsd_dmr_apply_mono_slot_gate(&opts, &state, &encL, &encR);
    rc |= expect_int("ss3 trunked mono remutes adjacent held left", encL, 1);
    rc |= expect_int("ss3 trunked mono leaves granted right unmuted", encR, 0);
    opts.dmr_mono = 0;

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

    DSD_MEMSET(state.s_l4, 0, sizeof(state.s_l4));
    DSD_MEMSET(state.s_r4, 0, sizeof(state.s_r4));
    opts.dmr_mono = 1;
    opts.slot1_on = 1;
    opts.slot2_on = 1;
    opts.slot_preference = 2;
    state.synctype = DSD_SYNC_DMR_BS_VOICE_POS;
    state.dmrburstL = 16;
    state.dmrburstR = 16;
    state.dmr_mono_slot = 1;
    state.s_l4[0][0] = 11;
    state.s_r4[0][0] = 77;
    encL = 0;
    encR = 0;
    dsd_dmr_apply_mono_slot_gate(&opts, &state, &encL, &encR);
    dsd_dmr_apply_stereo_output_policy_ss3(&opts, &state, encL, encR);
    rc |= expect_int("ss3 mono slot2 duplicates right to left with dual voice", state.s_l4[0][0], 77);
    rc |= expect_int("ss3 mono slot2 preserves right with dual voice", state.s_r4[0][0], 77);

    DSD_MEMSET(state.s_l4, 0, sizeof(state.s_l4));
    DSD_MEMSET(state.s_r4, 0, sizeof(state.s_r4));
    state.dmr_mono_slot = 0;
    state.s_l4[0][0] = 88;
    state.s_r4[0][0] = 22;
    encL = 0;
    encR = 0;
    dsd_dmr_apply_mono_slot_gate(&opts, &state, &encL, &encR);
    dsd_dmr_apply_stereo_output_policy_ss3(&opts, &state, encL, encR);
    rc |= expect_int("ss3 mono slot1 preserves left with dual voice", state.s_l4[0][0], 88);
    rc |= expect_int("ss3 mono slot1 duplicates left to right with dual voice", state.s_r4[0][0], 88);

    return rc;
}

static int
test_dmr_ss3_enc_locked_companion_duplicates_clear_slot(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

    int rc = 0;
    // Regression: dual active voice bursts with the right slot enc-muted must
    // still duplicate the clear left slot into the muted right channel.
    opts.slot1_on = 1;
    opts.slot2_on = 1;
    opts.slot_preference = 2;
    state.dmrburstL = 16;
    state.dmrburstR = 16;
    state.s_l4[0][0] = 11;
    state.s_r4[0][0] = 99;
    dsd_dmr_apply_stereo_output_policy_ss3(&opts, &state, 0, 1);
    rc |= expect_int("ss3 enc right companion cleared before copy", state.s_r4[0][0], 11);
    rc |= expect_int("ss3 enc right companion keeps clear left", state.s_l4[0][0], 11);

    DSD_MEMSET(state.s_l4, 0, sizeof(state.s_l4));
    DSD_MEMSET(state.s_r4, 0, sizeof(state.s_r4));
    state.s_l4[0][0] = 55;
    state.s_r4[0][0] = 22;
    dsd_dmr_apply_stereo_output_policy_ss3(&opts, &state, 1, 0);
    rc |= expect_int("ss3 enc left companion cleared before copy", state.s_l4[0][0], 22);
    rc |= expect_int("ss3 enc left companion keeps clear right", state.s_r4[0][0], 22);
    return rc;
}

// The mute flag alone must drive duplication, not the burst hint. dmrburstL/R
// record the last burst decoded for a slot, so an audible slot keeps emitting
// voice while its hint reads PI (0), VLC (1), TLC (2), DATA (6), IDLE (9), NULL
// (15) or the ERR init value (17). Gating the copy on hint 16 collapsed all of
// those spans into one ear next to a locked-out companion.
static int
run_ss3_muted_companion_burst_hint_case(int clear_slot, int clear_burst, int companion_burst) {
    static dsd_opts opts;
    static dsd_state state;
    int rc = 0;
    char tag[96];

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

    opts.slot1_on = 1;
    opts.slot2_on = 1;
    opts.slot_preference = 2;

    // The muted companion still holds pre-mute audio, which must never survive
    // into either channel.
    if (clear_slot == 0) {
        state.dmrburstL = clear_burst;
        state.dmrburstR = companion_burst;
        state.s_l4[0][0] = 33;
        state.s_r4[0][0] = -3000;
    } else {
        state.dmrburstR = clear_burst;
        state.dmrburstL = companion_burst;
        state.s_r4[0][0] = 33;
        state.s_l4[0][0] = -3000;
    }

    dsd_dmr_apply_stereo_output_policy_ss3(&opts, &state, (clear_slot == 0) ? 0 : 1, (clear_slot == 0) ? 1 : 0);

    DSD_SNPRINTF(tag, sizeof(tag), "ss3 burst hint clear=%d/%d companion=%d left", clear_slot, clear_burst,
                 companion_burst);
    rc |= expect_int(tag, state.s_l4[0][0], 33);
    DSD_SNPRINTF(tag, sizeof(tag), "ss3 burst hint clear=%d/%d companion=%d right", clear_slot, clear_burst,
                 companion_burst);
    rc |= expect_int(tag, state.s_r4[0][0], 33);
    return rc;
}

static int
test_dmr_ss3_muted_companion_burst_hint_matrix(void) {
    static const int bursts[] = {0, 1, 2, 6, 9, 15, 16, 17};
    const size_t count = sizeof(bursts) / sizeof(bursts[0]);
    int rc = 0;

    for (int clear_slot = 0; clear_slot < 2; clear_slot++) {
        for (size_t clear_idx = 0; clear_idx < count; clear_idx++) {
            for (size_t companion_idx = 0; companion_idx < count; companion_idx++) {
                rc |= run_ss3_muted_companion_burst_hint_case(clear_slot, bursts[clear_idx], bursts[companion_idx]);
            }
        }
    }
    return rc;
}

static int
test_short_dmr_mono_honors_slot_controls_and_one_channel_output(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    reset_gate_capture();
    opts.audio_out = 1;
    opts.audio_out_type = 8;
    opts.dmr_mono = 1;
    opts.pulse_digi_out_channels = 2;
    opts.slot1_on = 0;
    opts.slot2_on = 1;
    state.synctype = DSD_SYNC_DMR_BS_VOICE_POS;
    state.dmr_mono_slot = 0;
    state.s_l4[0][0] = 111;

    int rc = 0;
    reset_sink_capture();
    playSynthesizedVoiceSS3(&opts, &state);
    rc |= expect_int("ss3 mono muted selected slot1 skips output", g_udp_blast_calls, 0);

    DSD_MEMSET(&state, 0, sizeof(state));
    opts.slot1_on = 1;
    opts.slot2_on = 0;
    state.synctype = DSD_SYNC_DMR_BS_VOICE_POS;
    state.dmr_mono_slot = 1;
    state.s_r4[0][0] = 222;
    reset_sink_capture();
    playSynthesizedVoiceSS3(&opts, &state);
    rc |= expect_int("ss3 mono muted selected slot2 skips output", g_udp_blast_calls, 0);

    DSD_MEMSET(&state, 0, sizeof(state));
    opts.pulse_digi_out_channels = 1;
    opts.slot1_on = 1;
    opts.slot2_on = 1;
    state.synctype = DSD_SYNC_DMR_BS_VOICE_POS;
    state.dmr_mono_slot = 0;
    state.s_l4[2][0] = 333;
    state.s_r4[2][0] = 999;
    reset_sink_capture();
    playSynthesizedVoiceSS3(&opts, &state);
    rc |= expect_int("ss3 mono one-channel output calls", g_udp_blast_calls, 3);
    rc |= expect_size("ss3 mono one-channel output bytes", g_udp_blast_bytes, 160U * sizeof(short));
    rc |= expect_int("ss3 mono one-channel selected sample", ((const short*)g_udp_blast_data)[0], 333);
    return rc;
}

/* Issue #652: float output (-y) is no longer a mixer chain of its own. The mixers 16-bit output uses play it, with
   their gates, routing and level, and only the samples written to UDP and the raw fd change: f32, s16 / 32768. */
static int
test_float_output_plays_the_short_mixers(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    reset_gate_capture();
    opts.audio_out = 1;
    opts.audio_out_type = 8;
    opts.floating_point = 1;
    opts.slot1_on = 1;
    opts.slot2_on = 1;
    opts.dmr_mute_encL = 1;
    opts.dmr_mute_encR = 1;
    opts.pulse_digi_out_channels = 2;
    state.synctype = DSD_SYNC_P25P1_POS;
    state.p25_crypto_state[0] = DSD_P25_CRYPTO_CLEAR;

    // Stereo FDMA (P25 Phase 1, D-STAR, YSF, X2 under -fa) plays at full level in both ears; FS halved it.
    int rc = 0;
    fill_s16_frame(state.s_l, 16384, -8192);
    reset_sink_capture();
    playSynthesizedVoiceSS(&opts, &state);
    rc |= expect_int("-y stereo fdma output call", g_udp_blast_calls, 1);
    rc |= expect_size("-y stereo fdma output bytes", g_udp_blast_bytes, 320U * sizeof(float));
    rc |= expect_float("-y stereo fdma left at full level", captured_f32(g_udp_blast_data, 0), 0.5f);
    rc |= expect_float("-y stereo fdma right at full level", captured_f32(g_udp_blast_data, 1), 0.5f);
    rc |= expect_float("-y stereo fdma next left", captured_f32(g_udp_blast_data, 2), -0.25f);

    opts.reverse_mute = 1;
    fill_s16_frame(state.s_l, 24576, 0);
    reset_sink_capture();
    playSynthesizedVoiceSS(&opts, &state);
    rc |= expect_int("-y stereo fdma reverse mute suppresses clear P25", g_udp_blast_calls, 0);
    opts.reverse_mute = 0;

    state.payload_algid = 0x81;
    state.R = 0;
    state.p25_crypto_state[0] = DSD_P25_CRYPTO_BLOCKED;
    fill_s16_frame(state.s_l, 24576, 0);
    reset_sink_capture();
    playSynthesizedVoiceSS(&opts, &state);
    rc |= expect_int("-y stereo fdma encrypted without key skips output", g_udp_blast_calls, 0);

    opts.trunk_tune_enc_calls = 1;
    opts.unmute_encrypted_p25 = 1;
    fill_s16_frame(state.s_l, 24576, 0);
    reset_sink_capture();
    playSynthesizedVoiceSS(&opts, &state);
    rc |= expect_int("-y stereo fdma explicit P25 unmute emits undeciphered audio", g_udp_blast_calls, 1);

    opts.trunk_tune_enc_calls = 0;
    fill_s16_frame(state.s_l, 24576, 0);
    reset_sink_capture();
    playSynthesizedVoiceSS(&opts, &state);
    rc |= expect_int("-y stereo fdma P25 lockout probe overrides explicit unmute", g_udp_blast_calls, 0);
    opts.unmute_encrypted_p25 = 0;
    opts.trunk_tune_enc_calls = 1;

    opts.reverse_mute = 1;
    fill_s16_frame(state.s_l, 24576, 0);
    reset_sink_capture();
    playSynthesizedVoiceSS(&opts, &state);
    rc |= expect_int("-y stereo fdma reverse mute emits encrypted P25", g_udp_blast_calls, 1);
    opts.reverse_mute = 0;

    // SDRTrunk JSON playback bypasses the live P25 crypto state; a live call of unknown crypto stays muted.
    DSD_MEMSET(&state, 0, sizeof(state));
    opts.pulse_digi_out_channels = 1;
    state.synctype = DSD_SYNC_P25P1_POS;
    state.mbe_file_type = 3;
    state.p25_crypto_state[0] = DSD_P25_CRYPTO_UNKNOWN;
    fill_s16_frame(state.s_l, 20480, 0);
    state.audio_out_idx = 160;
    reset_sink_capture();
    playSynthesizedVoiceMS(&opts, &state);
    rc |= expect_int("-y sdrtrunk json P25 playback bypasses live crypto state", g_udp_blast_calls, 1);
    rc |= expect_size("-y sdrtrunk json P25 playback bytes", g_udp_blast_bytes, 160U * sizeof(float));
    rc |= expect_float("-y sdrtrunk json P25 playback sample", captured_f32(g_udp_blast_data, 0), 0.625f);

    state.mbe_file_type = 0;
    fill_s16_frame(state.s_l, 24576, 0);
    state.audio_out_idx = 160;
    reset_sink_capture();
    playSynthesizedVoiceMS(&opts, &state);
    rc |= expect_int("-y live P25 unknown crypto remains muted", g_udp_blast_calls, 0);

    /* A single DMR call plays in both ears, and at full level on a one-channel output. FS3 left the idle companion's
       channel silent (it duplicated only over a muted slot) and averaged it into a mono output. */
    for (int channels = 2; channels >= 1; channels--) {
        DSD_MEMSET(&state, 0, sizeof(state));
        opts.pulse_digi_out_channels = channels;
        state.synctype = DSD_SYNC_DMR_BS_VOICE_POS;
        state.dmrburstL = 9;
        state.dmrburstR = 16;
        for (int k = 0; k < 3; k++) {
            fill_s16_frame(state.s_r4[k], (short)(8192 * (k + 1)), (short)(-4096 * (k + 1)));
        }
        reset_sink_capture();
        playSynthesizedVoiceSS3(&opts, &state);
        char tag[96];
        DSD_SNPRINTF(tag, sizeof(tag), "-y dmr single call %dch: blocks", channels);
        rc |= expect_int(tag, g_udp_blast_calls, 3);
        DSD_SNPRINTF(tag, sizeof(tag), "-y dmr single call %dch: bytes", channels);
        rc |= expect_size(tag, g_udp_blast_stream_bytes, sizeof(float) * 3U * 160U * (size_t)channels);
        int both_ears = 1;
        int full_level = 1;
        for (int k = 0; k < 3; k++) {
            for (int i = 0; i < 160; i++) {
                const size_t at = (((size_t)k * 160U) + (size_t)i) * (size_t)channels;
                const float want = (float)((i == 0) ? 8192 * (k + 1) : -4096 * (k + 1)) / 32768.0f;
                const float left = captured_f32(g_udp_blast_stream, at);
                const float right = captured_f32(g_udp_blast_stream, at + (size_t)channels - 1U);
                if (fabsf(left - want) > 1e-9f) {
                    full_level = 0;
                }
                if (fabsf(left - right) > 1e-9f) {
                    both_ears = 0;
                }
            }
        }
        DSD_SNPRINTF(tag, sizeof(tag), "-y dmr single call %dch: in both ears", channels);
        rc |= expect_int(tag, both_ears, 1);
        DSD_SNPRINTF(tag, sizeof(tag), "-y dmr single call %dch: at full level", channels);
        rc |= expect_int(tag, full_level, 1);
    }

    DSD_MEMSET(&state, 0, sizeof(state));
    opts.pulse_digi_out_channels = 2;
    state.synctype = DSD_SYNC_DMR_BS_VOICE_POS;
    state.dmr_encL = 1;
    state.dmr_encR = 1;
    fill_s16_frame(state.s_l4[0], 1000, 1000);
    fill_s16_frame(state.s_r4[0], 1000, 1000);
    reset_sink_capture();
    playSynthesizedVoiceSS3(&opts, &state);
    rc |= expect_int("-y dmr both muted skips output", g_udp_blast_calls, 0);
    reset_gate_capture();
    return rc;
}

static int
test_audio_gate_target_preserves_p25_ota_identity(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    opts.slot1_on = 1;

    dsd_call_observation observation = {
        .protocol = DSD_SYNC_P25P1_POS,
        .slot = 0U,
        .kind = DSD_CALL_KIND_GROUP_VOICE,
        .ota_target_id = 9400U,
        .policy_target_id = 9502U,
    };
    int rc =
        expect_int("P25 patch call observed", dsd_call_state_observe(&state, &observation, DSD_CALL_BOUNDARY_BEGIN), 1);
    state.synctype = DSD_SYNC_P25P1_POS;
    reset_gate_capture();
    playSynthesizedVoiceMS(&opts, &state);
    rc |= expect_int("P25 gate receives OTA supergroup", (int)g_gate_mono_tg, 9400);
    dsd_state_ext_free_all(&state);

    DSD_MEMSET(&state, 0, sizeof(state));
    observation.protocol = DSD_SYNC_DMR_BS_VOICE_POS;
    rc |= expect_int("DMR call observed", dsd_call_state_observe(&state, &observation, DSD_CALL_BOUNDARY_BEGIN), 1);
    state.synctype = DSD_SYNC_DMR_BS_VOICE_POS;
    reset_gate_capture();
    playSynthesizedVoiceMS(&opts, &state);
    rc |= expect_int("non-P25 gate receives policy target", (int)g_gate_mono_tg, 9502);
    dsd_state_ext_free_all(&state);
    return rc;
}

/* The short mono path (1-channel integer output: -f1, NXDN, dPMR, ProVoice,
 * D-STAR and MBE playback) applies the talkgroup gate like every other output
 * path. Crypto was already settled upstream, so only the policy verdict mutes
 * it, and a muted frame reaches neither a sink nor the static WAV. */
static int
test_mono_short_voice_honors_talkgroup_gate(void) {
    dsd_opts* opts = calloc(1, sizeof(*opts));
    dsd_state* state = calloc(1, sizeof(*state));
    assert(opts && state);
    static short history[960];
    const int sinks[] = {0, 1, 8};
    int rc = 0;
    opts->audio_out = 1;
    opts->slot1_on = 1;
    opts->floating_point = 0;
    opts->pulse_digi_out_channels = 1;
    opts->audio_out_stream = (dsd_audio_stream*)opts;
    opts->audio_out_fd = 42;
    opts->wav_out_f = (SNDFILE*)opts;
    opts->static_wav_file = 1;
    g_audio_write_channels = 1;

    // A patched P25 call: the gate is asked about the supergroup it shows.
    const dsd_call_observation observation = {
        .protocol = DSD_SYNC_P25P1_POS,
        .slot = 0U,
        .kind = DSD_CALL_KIND_GROUP_VOICE,
        .ota_target_id = 9400U,
        .policy_target_id = 9502U,
    };
    rc |=
        expect_int("mono gate call observed", dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_BEGIN), 1);
    state->synctype = DSD_SYNC_P25P1_POS;
    // A live Phase 1 call reaches this path only once it classified clear or decryptable.
    state->p25_crypto_state[0] = DSD_P25_CRYPTO_CLEAR;

    for (size_t sink = 0; sink < sizeof(sinks) / sizeof(sinks[0]); sink++) {
        opts->audio_out_type = sinks[sink];
        for (size_t frames = 160U; frames <= 960U; frames += 800U) {
            for (int muted = 1; muted >= 0; --muted) {
                for (size_t i = 0; i < 160U; i++) {
                    state->s_l[i] = (short)(i + 1U);
                }
                state->audio_out_idx = (int)frames;
                state->audio_out_buf_p = history + frames;
                reset_sink_capture();
                reset_gate_capture();
                g_gate_mono_forced_enc = muted;
                playSynthesizedVoiceMS(opts, state);
                const int writes = g_audio_write_calls + g_dsd_write_calls + g_udp_blast_calls;
                rc |= expect_int("mono gate receives OTA supergroup", (int)g_gate_mono_tg, 9400);
                rc |= expect_int(muted ? "muted mono frame reaches no sink" : "allowed mono frame reaches its sink",
                                 writes, muted ? 0 : 1);
                rc |= expect_int(muted ? "muted mono frame skips static wav" : "allowed mono frame writes static wav",
                                 g_sf_write_short_calls, muted ? 0 : 1);
                // The 960-sample history rewind nets to zero either way.
                rc |= expect_int("mono frame keeps history position", state->audio_out_buf_p == history + frames, 1);
                rc |= expect_int("mono frame resets its length", state->audio_out_idx, 0);
                rc |= expect_int("mono frame working state reset", state->s_l[0], 0);
            }
        }
    }
    reset_gate_capture();
    g_audio_write_channels = 2;
    dsd_state_ext_free_all(state);
    free(state);
    free(opts);
    return rc;
}

/* Reverse mute (-q) silences clear live P25 Phase 1 audio on the short mono
 * path as on the stereo path, while SDRTrunk JSON playback keeps
 * bypassing the live crypto state. */
static int
test_mono_short_voice_honors_p25_reverse_mute(void) {
    dsd_opts* opts = calloc(1, sizeof(*opts));
    dsd_state* state = calloc(1, sizeof(*state));
    assert(opts && state);

    static const struct {
        const char* tag;
        int reverse_mute;
        int mbe_file_type;
        int want_writes;
    } cases[] = {
        {"reverse mute silences clear live P25 mono", 1, 0, 0},
        {"clear live P25 mono plays without reverse mute", 0, 0, 1},
        {"sdrtrunk json P25 mono bypasses live reverse mute", 1, 3, 1},
    };

    int rc = 0;
    opts->audio_out = 1;
    opts->audio_out_type = 8;
    opts->slot1_on = 1;
    opts->floating_point = 0;
    opts->pulse_digi_out_channels = 1;
    g_audio_write_channels = 1;
    state->synctype = DSD_SYNC_P25P1_POS;
    state->p25_crypto_state[0] = DSD_P25_CRYPTO_CLEAR;
    reset_gate_capture();
    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        opts->reverse_mute = cases[c].reverse_mute;
        state->mbe_file_type = cases[c].mbe_file_type;
        for (size_t i = 0; i < 160U; i++) {
            state->s_l[i] = (short)(i + 1U);
        }
        state->audio_out_idx = 160;
        reset_sink_capture();
        playSynthesizedVoiceMS(opts, state);
        rc |= expect_int(cases[c].tag, g_udp_blast_calls, cases[c].want_writes);
    }
    g_audio_write_channels = 2;
    free(state);
    free(opts);
    return rc;
}

/* A mono vocoder frame must retain its duration, pitch and samples when the
 * startup preset leaves a stereo device open during a mixed-protocol scan, in
 * either output format: float output (-y) plays the same mixer and writes the
 * samples to UDP and the raw fd as f32, s16 / 32768 (issue #652). */
static int
test_mono_voice_preserves_samples_in_configured_output(void) {
    dsd_opts* opts = calloc(1, sizeof(*opts));
    dsd_state* state = calloc(1, sizeof(*state));
    assert(opts && state);
    static short history[960];
    short expected_short[1920];
    const int sinks[] = {0, 1, 8};
    int rc = 0;
    opts->audio_out = 1;
    opts->slot1_on = 1;
    opts->audio_out_stream = (dsd_audio_stream*)opts;
    state->synctype = DSD_SYNC_NXDN_POS;
    reset_gate_capture();

    for (int channels = 1; channels <= 2; channels++) {
        opts->pulse_digi_out_channels = channels;
        g_audio_write_channels = channels;
        for (int format = 0; format < 4; format++) {
            const int floating = format & 1;
            const size_t frames = format >= 2 ? 960U : 160U;
            opts->floating_point = floating;
            for (size_t sink = 0; sink < sizeof(sinks) / sizeof(sinks[0]); sink++) {
                opts->audio_out_type = sinks[sink];
                state->audio_out_idx = (int)frames;
                state->audio_out_buf_p = history + frames;
                for (size_t i = 0; i < frames; i++) {
                    short sample = (short)((int)i - 480);
                    history[i] = sample;
                    if (i < 160U) {
                        state->s_l[i] = sample;
                    }
                    for (int channel = 0; channel < channels; channel++) {
                        size_t index = i * (size_t)channels + (size_t)channel;
                        expected_short[index] = sample;
                    }
                }
                reset_sink_capture();
                playSynthesizedVoiceMS(opts, state);
                size_t count = frames * (size_t)channels;
                if (opts->audio_out_type == 0) {
                    rc |= expect_size("mono voice device frame duration", g_audio_write_frames, frames);
                    rc |= expect_bytes("mono voice device channel samples", g_audio_write_samples, expected_short,
                                       count * sizeof(short));
                } else {
                    const void* data = opts->audio_out_type == 1 ? g_dsd_write_data : g_udp_blast_data;
                    size_t bytes = opts->audio_out_type == 1 ? g_dsd_write_bytes : g_udp_blast_bytes;
                    rc |= expect_size("mono voice stream frame duration", bytes,
                                      count * (floating ? sizeof(float) : sizeof(short)));
                    if (floating) {
                        for (size_t i = 0; i < count; i++) {
                            float sample;
                            DSD_MEMCPY(&sample, (const uint8_t*)data + i * sizeof(float), sizeof(sample));
                            rc |= expect_float("mono voice stream channel sample", sample,
                                               (float)expected_short[i] / 32768.0f);
                        }
                    } else {
                        rc |= expect_bytes("mono voice stream channel samples", data, expected_short,
                                           count * sizeof(short));
                    }
                }
            }
        }
    }
    g_audio_write_channels = 2;
    free(state);
    free(opts);
    return rc;
}

/* Issue #574: whether the code under test noted the audible-audio stamp since stamp_clear(). main() arms it. */
static void
stamp_clear(void) {
    dsd_audio_activity_reset();
}

static int
stamp_noted(void) {
    uint64_t stamp = 0U;
    dsd_audio_activity_read(&stamp, NULL);
    return stamp != 0U;
}

/* What a DMR mix row stages for a slot: nothing, or decoded short media. */
enum { STAGE_NONE = 0, STAGE_OWN = 1 };

typedef struct {
    const char* tag;
    int staged[2];
    int enc[2];
    int slot_on[2];
    int mono_slot; /* -1: DMR mono off */
    int gate_mute_left;
    int audio_out;
    int want_blocks;
    int want_stamp;
} dmr_stamp_row;

/* Issue #574: the DMR mix runs on a timer of bursts, skipped ones included, and replays each slot's buffers whatever
   was decoded into them, so it stamps audible audio only for a slot holding short media staged since the last mix,
   whose every gate passes, in either output format (#652: float output plays SS3 too). The staged samples here are all
   zero: valid decoded silence counts all the same. */
static int
run_dmr_stamp_row(const dmr_stamp_row* row, int floating_point, int channels) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    reset_sink_capture();
    reset_gate_capture();
    opts.audio_out = row->audio_out;
    opts.audio_out_type = 8;
    opts.floating_point = floating_point;
    opts.pulse_digi_out_channels = channels;
    opts.slot1_on = row->slot_on[0];
    opts.slot2_on = row->slot_on[1];
    opts.dmr_mute_encL = 1;
    opts.dmr_mute_encR = 1;
    state.synctype = DSD_SYNC_DMR_BS_VOICE_POS;
    state.dmr_encL = row->enc[0];
    state.dmr_encR = row->enc[1];
    if (row->mono_slot >= 0) {
        opts.dmr_mono = 1;
        state.dmr_mono_slot = (short)row->mono_slot;
    }
    g_gate_dual_forced_enc_l = row->gate_mute_left ? 1 : -1;
    for (int slot = 0; slot < 2; slot++) {
        dsd_audio_dmr_mix_media_discard(slot);
        if (row->staged[slot] != STAGE_NONE) {
            dsd_audio_dmr_mix_media_staged(slot, DSD_DMR_MIX_MEDIA_SHORT);
        }
    }
    stamp_clear();
    playSynthesizedVoiceSS3(&opts, &state);
    char tag[160];
    DSD_SNPRINTF(tag, sizeof(tag), "%s %dch %s: blocks", floating_point ? "ss3 -y" : "ss3", channels, row->tag);
    int rc = expect_int(tag, g_udp_blast_calls, row->want_blocks);
    DSD_SNPRINTF(tag, sizeof(tag), "%s %dch %s: stamp", floating_point ? "ss3 -y" : "ss3", channels, row->tag);
    rc |= expect_int(tag, stamp_noted(), row->want_stamp);
    reset_gate_capture();
    return rc;
}

static int
test_dmr_mixes_stamp_only_fresh_audible_slots(void) {
    static const dmr_stamp_row rows[] = {
        {"clear slot 1 media", {STAGE_OWN, STAGE_NONE}, {0, 0}, {1, 1}, -1, 0, 1, 3, 1},
        {"clear slot 2 media", {STAGE_NONE, STAGE_OWN}, {0, 0}, {1, 1}, -1, 0, 1, 3, 1},
        {"slot 2 media beside a muted slot 1", {STAGE_OWN, STAGE_OWN}, {1, 0}, {1, 1}, -1, 0, 1, 3, 1},
        {"no media staged (a skipped burst)", {STAGE_NONE, STAGE_NONE}, {0, 0}, {1, 1}, -1, 0, 1, 3, 0},
        {"muted media beside an idle slot", {STAGE_OWN, STAGE_NONE}, {1, 0}, {1, 1}, -1, 0, 1, 3, 0},
        {"slot 1 switched off", {STAGE_OWN, STAGE_NONE}, {0, 0}, {0, 1}, -1, 0, 1, 3, 0},
        {"DMR mono playing slot 2", {STAGE_OWN, STAGE_NONE}, {0, 0}, {1, 1}, 1, 0, 1, 3, 0},
        {"DMR mono playing slot 1", {STAGE_OWN, STAGE_NONE}, {0, 0}, {1, 1}, 0, 0, 1, 3, 1},
        {"talkgroup gate mutes slot 1", {STAGE_OWN, STAGE_NONE}, {0, 0}, {1, 1}, -1, 1, 1, 3, 0},
        {"output muted", {STAGE_OWN, STAGE_OWN}, {0, 0}, {1, 1}, -1, 0, 0, 0, 0},
    };
    int rc = 0;
    for (int floating_point = 1; floating_point >= 0; floating_point--) {
        for (int channels = 1; channels <= 2; channels++) {
            for (size_t r = 0U; r < sizeof(rows) / sizeof(rows[0]); r++) {
                rc |= run_dmr_stamp_row(&rows[r], floating_point, channels);
            }
        }
    }

    /* Each mix takes the flags: a second mix with nothing staged since (the skipped-burst path's) stamps nothing, nor
       does one after the muted output took them, nor one after the staged audio was discarded. */
    static const dmr_stamp_row clear = {"clear", {STAGE_OWN, STAGE_NONE}, {0, 0}, {1, 1}, -1, 0, 1, 3, 1};
    static dsd_opts opts;
    static dsd_state state;
    for (int floating_point = 1; floating_point >= 0; floating_point--) {
        const char* mix = floating_point ? "ss3 -y" : "ss3";
        const unsigned int own = DSD_DMR_MIX_MEDIA_SHORT;
        char tag[96];
        rc |= run_dmr_stamp_row(&clear, floating_point, 2);
        DSD_MEMSET(&opts, 0, sizeof(opts));
        DSD_MEMSET(&state, 0, sizeof(state));
        opts.audio_out = 1;
        opts.audio_out_type = 8;
        opts.floating_point = floating_point;
        opts.pulse_digi_out_channels = 2;
        opts.slot1_on = 1;
        opts.slot2_on = 1;
        state.synctype = DSD_SYNC_DMR_BS_VOICE_POS;
        for (int step = 0; step < 3; step++) {
            if (step == 1) {
                dsd_audio_dmr_mix_media_staged(0, own);
                opts.audio_out = 0;
                playSynthesizedVoiceSS3(&opts, &state);
                opts.audio_out = 1;
            } else if (step == 2) {
                dsd_audio_dmr_mix_media_staged(0, own);
                dsd_audio_dmr_mix_media_staged(1, own);
                dsd_audio_dmr_mix_media_discard(0);
                dsd_audio_dmr_mix_media_discard(1);
            }
            reset_sink_capture();
            stamp_clear();
            playSynthesizedVoiceSS3(&opts, &state);
            static const char* const steps[] = {"a repeat mix with nothing new",
                                                "a mix after a muted one took the media",
                                                "a mix after the staged audio was discarded"};
            DSD_SNPRINTF(tag, sizeof(tag), "%s %s: blocks", mix, steps[step]);
            rc |= expect_int(tag, g_udp_blast_calls, 3);
            DSD_SNPRINTF(tag, sizeof(tag), "%s %s: stamp", mix, steps[step]);
            rc |= expect_int(tag, stamp_noted(), 0);
        }
    }
    return rc;
}

/* Issue #574: the FDMA mixers are called once per decoded frame, so each stamps audible audio when it plays its frame:
   clear or decryptable, its talkgroup allowed, slot 1 on and the output unmuted, in either output format. Valid
   all-zero decoded audio counts; the mono mix with no frame loaded plays nothing and stamps nothing. */
static int
test_fdma_mixers_stamp_what_they_play(void) {
    static const struct {
        const char* tag;
        int crypto_muted;
        int gate_muted;
        int slot1_on;
        int audio_out;
        int frames;
        int want;
    } cases[] = {
        {"clear frame", 0, 0, 1, 1, 160, 1},       {"crypto-muted frame", 1, 0, 1, 1, 160, 0},
        {"blocked talkgroup", 0, 1, 1, 1, 160, 0}, {"slot 1 switched off", 0, 0, 0, 1, 160, 0},
        {"output muted", 0, 0, 1, 0, 160, 0},      {"no frame loaded", 0, 0, 1, 1, 0, 0},
    };

    static const char* const mixers[] = {"ss -y", "ms -y", "ss", "ms"};
    static dsd_opts opts;
    static dsd_state state;
    int rc = 0;
    for (int m = 0; m < 4; m++) {
        for (size_t c = 0U; c < sizeof(cases) / sizeof(cases[0]); c++) {
            if (cases[c].frames == 0 && (m & 1) == 0) {
                continue; // Only the mono mix reads a frame length.
            }
            DSD_MEMSET(&opts, 0, sizeof(opts));
            DSD_MEMSET(&state, 0, sizeof(state));
            reset_sink_capture();
            reset_gate_capture();
            opts.audio_out = cases[c].audio_out;
            opts.audio_out_type = 8;
            opts.slot1_on = cases[c].slot1_on;
            opts.floating_point = m < 2 ? 1 : 0;
            opts.pulse_digi_out_channels = (m == 0 || m == 2) ? 2 : 1;
            state.synctype = DSD_SYNC_P25P1_POS;
            state.p25_crypto_state[0] = cases[c].crypto_muted ? DSD_P25_CRYPTO_BLOCKED : DSD_P25_CRYPTO_CLEAR;
            state.payload_algid = cases[c].crypto_muted ? 0x81 : 0x80;
            state.audio_out_idx = cases[c].frames;
            g_gate_mono_forced_enc = cases[c].gate_muted ? 1 : -1;
            stamp_clear();
            if ((m & 1) == 0) {
                playSynthesizedVoiceSS(&opts, &state);
            } else {
                playSynthesizedVoiceMS(&opts, &state);
            }
            char tag[96];
            DSD_SNPRINTF(tag, sizeof(tag), "%s %s: blocks", mixers[m], cases[c].tag);
            rc |= expect_int(tag, g_udp_blast_calls, cases[c].want);
            DSD_SNPRINTF(tag, sizeof(tag), "%s %s: stamp", mixers[m], cases[c].tag);
            rc |= expect_int(tag, stamp_noted(), cases[c].want);
        }
    }
    reset_gate_capture();
    return rc;
}

/* Issue #574: a mix stamps audible audio only when an output receives its blocks (dsd_output_*_block()): the local
   stream while it is open, UDP, or the raw fd, in either output format. The null output (-o null, which keeps output
   type 9 once unmuted) and a local stream that is not open receive nothing, so they stamp nothing. */
static int
test_mixers_stamp_only_what_an_output_takes(void) {
    static const struct {
        const char* tag;
        int out_type;
        int stream_open;
        int want;
    } outputs[] = {
        {"local stream", 0, 1, 1}, {"local stream not open", 0, 0, 0}, {"udp", 8, 1, 1},
        {"raw fd", 1, 1, 1},       {"null output", 9, 1, 0},
    };

    static const char* const mixers[] = {"ss3 -y", "ss3", "ss -y", "ms -y", "ss", "ms"};
    static dsd_opts opts;
    static dsd_state state;
    int rc = 0;
    for (size_t m = 0U; m < sizeof(mixers) / sizeof(mixers[0]); m++) {
        for (size_t o = 0U; o < sizeof(outputs) / sizeof(outputs[0]); o++) {
            DSD_MEMSET(&opts, 0, sizeof(opts));
            DSD_MEMSET(&state, 0, sizeof(state));
            reset_sink_capture();
            reset_gate_capture();
            opts.audio_out = 1;
            opts.audio_out_type = outputs[o].out_type;
            opts.audio_out_stream = outputs[o].stream_open ? (dsd_audio_stream*)&opts : NULL;
            opts.audio_out_fd = 42;
            opts.slot1_on = 1;
            opts.slot2_on = 1;
            opts.floating_point = (m == 0U || m == 2U || m == 3U) ? 1 : 0;
            opts.pulse_digi_out_channels = (m == 3U || m == 5U) ? 1 : 2;
            g_audio_write_channels = opts.pulse_digi_out_channels;
            if (m < 2U) {
                state.synctype = DSD_SYNC_DMR_BS_VOICE_POS;
                dsd_audio_dmr_mix_media_discard(0);
                dsd_audio_dmr_mix_media_discard(1);
                dsd_audio_dmr_mix_media_staged(0, DSD_DMR_MIX_MEDIA_SHORT);
            } else {
                state.synctype = DSD_SYNC_P25P1_POS;
                state.p25_crypto_state[0] = DSD_P25_CRYPTO_CLEAR;
                state.payload_algid = 0x80;
                state.audio_out_idx = 160;
            }
            stamp_clear();
            switch (m) {
                case 0:
                case 1: playSynthesizedVoiceSS3(&opts, &state); break;
                case 2:
                case 4: playSynthesizedVoiceSS(&opts, &state); break;
                default: playSynthesizedVoiceMS(&opts, &state); break;
            }
            const int received = g_audio_write_calls + g_udp_blast_calls + g_dsd_write_calls;
            char tag[96];
            DSD_SNPRINTF(tag, sizeof(tag), "%s %s: an output receives blocks", mixers[m], outputs[o].tag);
            rc |= expect_int(tag, received > 0, outputs[o].want);
            DSD_SNPRINTF(tag, sizeof(tag), "%s %s: stamp", mixers[m], outputs[o].tag);
            rc |= expect_int(tag, stamp_noted(), outputs[o].want);
        }
    }
    g_audio_write_channels = 2;
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

/* The static WAV (-w) is a recording: a talkgroup whose row allows audio but not
 * recording is heard but not written. Mono and mono-to-stereo paths skip the
 * frame; the stereo mixes render the WAV as if that slot were muted, so the
 * other slot still records and a mirrored channel never carries it. */
static int
test_static_wav_honors_record_policy_mono(void) {
    static dsd_opts opts;
    static dsd_state state;
    int rc = 0;
    for (int stereo = 0; stereo <= 1; ++stereo) {
        for (int blocked = 1; blocked >= 0; --blocked) {
            DSD_MEMSET(&opts, 0, sizeof(opts));
            DSD_MEMSET(&state, 0, sizeof(state));
            reset_gate_capture();
            reset_sink_capture();
            opts.audio_out = 1;
            opts.audio_out_type = 8;
            opts.slot1_on = 1;
            opts.pulse_digi_out_channels = stereo ? 2 : 1;
            opts.wav_out_f = (SNDFILE*)&opts;
            opts.static_wav_file = 1;
            g_audio_write_channels = opts.pulse_digi_out_channels;
            state.synctype = DSD_SYNC_NXDN_POS;
            state.audio_out_idx = 160;
            for (size_t i = 0; i < 160U; i++) {
                state.s_l[i] = (short)(i + 1U);
            }
            g_record_policy_blocked[0] = blocked;
            if (stereo) {
                playSynthesizedVoiceSS(&opts, &state);
            } else {
                playSynthesizedVoiceMS(&opts, &state);
            }
            rc |= expect_int("record-blocked frame still plays", g_udp_blast_calls, 1);
            rc |= expect_int(blocked ? "record-blocked frame skips static wav" : "recordable frame writes static wav",
                             g_sf_write_short_calls, blocked ? 0 : 1);
        }
    }
    reset_gate_capture();
    g_audio_write_channels = 2;
    return rc;
}

static int
test_static_wav_honors_record_policy_stereo(void) {
    static dsd_opts opts;
    static dsd_state state;
    int rc = 0;
    {
        const size_t blocks = 3U;
        for (int blocked_slot = 0; blocked_slot < 2; ++blocked_slot) {
            DSD_MEMSET(&opts, 0, sizeof(opts));
            DSD_MEMSET(&state, 0, sizeof(state));
            reset_gate_capture();
            reset_sink_capture();
            opts.audio_out = 1;
            opts.audio_out_type = 8;
            opts.slot1_on = opts.slot2_on = 1;
            opts.pulse_digi_out_channels = 2;
            opts.wav_out_f = (SNDFILE*)&opts;
            opts.static_wav_file = 1;
            g_gate_dual_forced_enc_l = 0;
            g_gate_dual_forced_enc_r = 0;
            state.synctype = DSD_SYNC_DMR_BS_VOICE_POS;
            for (size_t j = 0; j < blocks; j++) {
                for (size_t i = 0; i < 160U; i++) {
                    state.s_l4[j][i] = 1111;
                    state.s_r4[j][i] = 2222;
                }
            }
            g_record_policy_blocked[blocked_slot] = 1;
            playSynthesizedVoiceSS3(&opts, &state);
            short played[2] = {0, 0};
            DSD_MEMCPY(played, g_udp_blast_data, sizeof(played));
            rc |= expect_int("record-blocked slot still plays", played[blocked_slot], blocked_slot ? 2222 : 1111);
            rc |= expect_int("other slot still plays", played[blocked_slot ^ 1], blocked_slot ? 1111 : 2222);
            rc |= expect_size("stereo static wav keeps every block", g_sf_written_count, blocks * 320U);
            // The WAV is rendered as if the blocked slot were muted, so the output
            // policy may mirror the other slot into both channels, as heard.
            const short blocked_value = blocked_slot ? 2222 : 1111;
            const short kept_value = blocked_slot ? 1111 : 2222;
            int blocked_absent = 1;
            int kept_present = 0;
            for (size_t i = 0; i < g_sf_written_count; i++) {
                blocked_absent &= g_sf_written[i] != blocked_value;
                kept_present |= g_sf_written[i] == kept_value;
            }
            rc |= expect_int("stereo static wav omits the record-blocked slot", blocked_absent, 1);
            rc |= expect_int("stereo static wav keeps the other slot", kept_present, 1);
        }
        // A record-blocked slot whose companion is muted leaves nothing to record,
        // even though the audible mix mirrors it into both channels.
        DSD_MEMSET(&opts, 0, sizeof(opts));
        DSD_MEMSET(&state, 0, sizeof(state));
        reset_gate_capture();
        reset_sink_capture();
        opts.audio_out = 1;
        opts.audio_out_type = 8;
        opts.slot1_on = opts.slot2_on = 1;
        opts.pulse_digi_out_channels = 2;
        opts.wav_out_f = (SNDFILE*)&opts;
        opts.static_wav_file = 1;
        g_gate_dual_forced_enc_l = 0;
        g_gate_dual_forced_enc_r = 1;
        state.synctype = DSD_SYNC_DMR_BS_VOICE_POS;
        for (size_t j = 0; j < blocks; j++) {
            for (size_t i = 0; i < 160U; i++) {
                state.s_l4[j][i] = 1111;
            }
        }
        g_record_policy_blocked[0] = 1;
        playSynthesizedVoiceSS3(&opts, &state);
        rc |= expect_int("record-blocked sole audible slot still plays", g_udp_blast_calls > 0, 1);
        int wav_silent = 1;
        for (size_t i = 0; i < g_sf_written_count; i++) {
            wav_silent &= g_sf_written[i] == 0;
        }
        rc |= expect_int("record-blocked sole audible slot never reaches the wav", wav_silent, 1);
    }
    reset_gate_capture();
    return rc;
}

/* The static WAV follows the mix's routing: when the output mirrors one slot over
 * the other (a disabled slot, or a burst hint), the unplayed slot is not recorded
 * either, so blocking the played slot leaves nothing to write. */
static int
test_static_wav_follows_stereo_routing(void) {
    static dsd_opts opts;
    static dsd_state state;
    int rc = 0;
    {
        const size_t blocks = 3U;
        for (int scenario = 0; scenario < 2; ++scenario) {
            DSD_MEMSET(&opts, 0, sizeof(opts));
            DSD_MEMSET(&state, 0, sizeof(state));
            reset_gate_capture();
            reset_sink_capture();
            opts.audio_out = 1;
            opts.audio_out_type = 8;
            opts.slot1_on = scenario == 0 ? 0 : 1;
            opts.slot2_on = 1;
            opts.pulse_digi_out_channels = 2;
            opts.wav_out_f = (SNDFILE*)&opts;
            opts.static_wav_file = 1;
            g_gate_dual_forced_enc_l = 0;
            g_gate_dual_forced_enc_r = 0;
            state.synctype = DSD_SYNC_DMR_BS_VOICE_POS;
            if (scenario == 1) {
                state.dmrburstR = 16;
            }
            for (size_t j = 0; j < blocks; j++) {
                for (size_t i = 0; i < 160U; i++) {
                    state.s_l4[j][i] = 1111;
                    state.s_r4[j][i] = 2222;
                }
            }
            g_record_policy_blocked[1] = 1;
            playSynthesizedVoiceSS3(&opts, &state);
            short played[2] = {0, 0};
            DSD_MEMCPY(played, g_udp_blast_data, sizeof(played));
            rc |= expect_int("mirrored mix plays the second slot", played[0] == 2222 && played[1] == 2222, 1);
            int unheard_absent = 1;
            int blocked_absent = 1;
            for (size_t i = 0; i < g_sf_written_count; i++) {
                unheard_absent &= g_sf_written[i] != 1111;
                blocked_absent &= g_sf_written[i] != 2222;
            }
            rc |= expect_int("static wav never records the unplayed slot", unheard_absent, 1);
            rc |= expect_int("static wav omits the record-blocked played slot", blocked_absent, 1);
        }
    }
    reset_gate_capture();
    return rc;
}

/* X2-TDMA stages either timeslot in the mono buffer, so the mono paths ask the
 * gate and the record policy about the slot being played, in either output
 * format (float output plays the same mixers, issue #652). */
static int
test_x2_mono_voice_uses_playing_slot(void) {
    static dsd_opts opts;
    static dsd_state state;
    int rc = 0;
    for (int combo = 0; combo < 4; ++combo) {
        const int stereo = combo & 1;
        for (int blocked_slot = 0; blocked_slot < 2; ++blocked_slot) {
            DSD_MEMSET(&opts, 0, sizeof(opts));
            DSD_MEMSET(&state, 0, sizeof(state));
            reset_gate_capture();
            reset_sink_capture();
            opts.floating_point = combo >> 1;
            opts.audio_out = 1;
            opts.audio_out_type = 8;
            opts.slot1_on = 1;
            opts.pulse_digi_out_channels = stereo ? 2 : 1;
            opts.wav_out_f = (SNDFILE*)&opts;
            opts.static_wav_file = 1;
            g_audio_write_channels = opts.pulse_digi_out_channels;
            for (uint8_t slot = 0; slot < 2; slot++) {
                const dsd_call_observation call = {.protocol = DSD_SYNC_X2TDMA_VOICE_POS,
                                                   .slot = slot,
                                                   .kind = DSD_CALL_KIND_GROUP_VOICE,
                                                   .ota_target_id = slot ? 200U : 100U,
                                                   .policy_target_id = slot ? 200U : 100U};
                rc |= expect_int("x2 call", dsd_call_state_observe(&state, &call, DSD_CALL_BOUNDARY_BEGIN), 1);
            }
            state.synctype = DSD_SYNC_X2TDMA_VOICE_POS;
            state.currentslot = 1;
            state.audio_out_idx = 160;
            for (size_t i = 0; i < 160U; i++) {
                state.s_l[i] = (short)(i + 1U);
            }
            g_record_policy_blocked[blocked_slot] = 1;
            if (stereo) {
                playSynthesizedVoiceSS(&opts, &state);
            } else {
                playSynthesizedVoiceMS(&opts, &state);
            }
            rc |= expect_int("x2 gate asks about the playing slot", (int)g_gate_mono_tg, 200);
            g_record_policy_blocked[blocked_slot] = 1;
            rc |=
                expect_int(blocked_slot ? "x2 playing slot record-blocked skips wav" : "x2 other slot's policy ignored",
                           g_sf_write_short_calls, blocked_slot ? 0 : 1);
            dsd_state_ext_free_all(&state);
        }
    }
    reset_gate_capture();
    g_audio_write_channels = 2;
    return rc;
}

static int
test_ss3_hold_respects_policy_mute(void) {
    static dsd_opts opts;
    static dsd_state state;
    int rc = 0;
    for (uint8_t slot = 0; slot < 2; ++slot) {
        DSD_MEMSET(&opts, 0, sizeof(opts));
        DSD_MEMSET(&state, 0, sizeof(state));
        reset_gate_capture();
        opts.audio_out = 1;
        opts.audio_out_type = 8;
        opts.slot1_on = opts.slot2_on = 1;
        opts.pulse_digi_out_channels = 2;
        state.tg_hold = 123;
        const dsd_call_observation call = {.protocol = DSD_SYNC_DMR_BS_VOICE_POS,
                                           .slot = slot,
                                           .kind = DSD_CALL_KIND_GROUP_VOICE,
                                           .ota_target_id = 123,
                                           .policy_target_id = 123,
                                           .observed_m = 1.0};
        rc |= expect_int("ss3 held call", dsd_call_state_observe(&state, &call, DSD_CALL_BOUNDARY_BEGIN), 1);
        for (int muted = 1; muted >= 0; --muted) {
            reset_sink_capture();
            g_gate_dual_forced_enc_l = slot == 0 ? muted : 1;
            g_gate_dual_forced_enc_r = slot == 1 ? muted : 1;
            state.s_l4[0][0] = state.s_r4[0][0] = 1234;
            playSynthesizedVoiceSS3(&opts, &state);
            rc |= expect_int("ss3 held policy controls UDP audio", g_udp_blast_calls, muted ? 0 : 3);
        }
        dsd_state_ext_free_all(&state);
    }
    reset_gate_capture();
    return rc;
}

int
main(void) {
    int rc = 0;
    dsd_audio_activity_arm();
    rc |= test_p25_decrypt_gate_helpers();
    rc |= test_output_helpers_dispatch_to_configured_sinks();
    rc |= test_float_output_writes_the_s16_block_as_f32();
    rc |= test_float_device_write_converts_in_bounded_chunks();
    rc |= test_output_block_helpers_skip_silent_trailing_blocks();
    rc |= test_output_ring_reset_helpers_preserve_below_threshold_and_clear_at_limit();
    rc |= test_dmr_slot_mute_and_mono_gate_helpers();
    rc |= test_dmr_ss3_decrypt_hold_and_copy_policy_helpers();
    rc |= test_dmr_ss3_enc_locked_companion_duplicates_clear_slot();
    rc |= test_dmr_ss3_muted_companion_burst_hint_matrix();
    rc |= test_short_dmr_mono_honors_slot_controls_and_one_channel_output();
    rc |= test_float_output_plays_the_short_mixers();
    rc |= test_audio_gate_target_preserves_p25_ota_identity();
    rc |= test_mono_voice_preserves_samples_in_configured_output();
    rc |= test_mono_short_voice_honors_talkgroup_gate();
    rc |= test_mono_short_voice_honors_p25_reverse_mute();
    rc |= test_static_wav_honors_record_policy_mono();
    rc |= test_static_wav_honors_record_policy_stereo();
    rc |= test_static_wav_follows_stereo_routing();
    rc |= test_x2_mono_voice_uses_playing_slot();
    rc |= test_ss3_hold_respects_policy_mute();
    rc |= test_silent_s16_helper();
    rc |= test_dmr_mixes_stamp_only_fresh_audible_slots();
    rc |= test_fdma_mixers_stamp_what_they_play();
    rc |= test_mixers_stamp_only_what_an_output_takes();
    return rc;
}
