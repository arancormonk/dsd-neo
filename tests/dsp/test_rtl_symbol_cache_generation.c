// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <assert.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/dsp/symbol.h>
#include <dsd-neo/io/rtl_stream_c.h>
#include <dsd-neo/platform/sockets.h>
#include <dsd-neo/runtime/rtl_stream_io_hooks.h>
#include <dsd-neo/runtime/rtl_stream_metrics_hooks.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "dsd-neo/core/safe_api.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

static uint32_t g_stream_generation = 1;
static int g_output_kind = RTL_STREAM_OUTPUT_FSK_DISCRIMINATOR;
static unsigned int g_output_rate_hz = 48000U;
static int g_symbol_rate_hz = 4800;
static int g_symbol_levels = 4;
static int g_channel_profile = RTL_STREAM_CHANNEL_PROFILE_P25_C4FM;
static float g_read_base = 1000.0f;
static float g_read_base_step = 0.0f;
static int g_read_calls = 0;
static int g_bump_generation_during_read = 0;
static int g_output_kind_after_bump = -1;
static int g_symbol_rate_hz_after_bump = 0;
static int g_symbol_levels_after_bump = 0;
static int g_channel_profile_after_bump = -1;
static int g_cleanup_calls = 0;
static int g_fail_reads = 0;
static int g_failed_read_calls = 0;
static int g_max_read_calls = 0;

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
    return -1;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
cleanupAndExit(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    g_cleanup_calls++;
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
raw_pwr_f(const float* samples, int len, int step) {
    (void)samples;
    (void)len;
    (void)step;
    return 0.0;
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
analog_gain_f(dsd_opts* opts, dsd_state* state, float* input, int len) {
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
fake_rtl_read(void* rtl_ctx, float* out, size_t count, int* out_got) {
    assert(rtl_ctx != NULL);
    assert(out != NULL);
    assert(out_got != NULL);
    assert(count >= 4U);

    g_read_calls++;
    if (g_max_read_calls > 0 && g_read_calls > g_max_read_calls) {
        DSD_FPRINTF(stderr, "RTL symbol cache exceeded read limit: calls=%d limit=%d\n", g_read_calls,
                    g_max_read_calls);
        exit(3);
    }
    if (g_fail_reads) {
        g_failed_read_calls++;
        if (g_failed_read_calls > 4) {
            DSD_FPRINTF(stderr, "RTL symbol cache retried failed reads instead of returning EMPTY\n");
            exit(2);
        }
        *out_got = 0;
        return -1;
    }
    float read_base = g_read_base;
    if (g_bump_generation_during_read) {
        g_stream_generation++;
        if (g_output_kind_after_bump >= 0) {
            g_output_kind = g_output_kind_after_bump;
            g_output_kind_after_bump = -1;
        }
        if (g_symbol_rate_hz_after_bump > 0) {
            g_symbol_rate_hz = g_symbol_rate_hz_after_bump;
            g_symbol_rate_hz_after_bump = 0;
        }
        if (g_symbol_levels_after_bump > 0) {
            g_symbol_levels = g_symbol_levels_after_bump;
            g_symbol_levels_after_bump = 0;
        }
        if (g_channel_profile_after_bump >= 0) {
            g_channel_profile = g_channel_profile_after_bump;
            g_channel_profile_after_bump = -1;
        }
        g_bump_generation_during_read = 0;
    }
    for (int i = 0; i < 4; i++) {
        out[i] = read_base + (float)i;
    }
    g_read_base += g_read_base_step;
    *out_got = 4;
    return 0;
}

static double
fake_rtl_pwr(const void* rtl_ctx) {
    assert(rtl_ctx != NULL);
    return 0.0;
}

static int
fake_output_kind(void) {
    return g_output_kind;
}

static int
fake_symbol_profile(int* out_symbol_rate_hz, int* out_levels, int* out_channel_profile) {
    if (out_symbol_rate_hz) {
        *out_symbol_rate_hz = g_symbol_rate_hz;
    }
    if (out_levels) {
        *out_levels = g_symbol_levels;
    }
    if (out_channel_profile) {
        *out_channel_profile = g_channel_profile;
    }
    return 0;
}

static uint32_t
fake_stream_generation(void) {
    return g_stream_generation;
}

static unsigned int
fake_output_rate_hz(void) {
    return g_output_rate_hz;
}

static void
reset_stream_fixture(void) {
    g_stream_generation = 1U;
    g_output_kind = RTL_STREAM_OUTPUT_FSK_DISCRIMINATOR;
    g_output_rate_hz = 48000U;
    g_symbol_rate_hz = 4800;
    g_symbol_levels = 4;
    g_channel_profile = RTL_STREAM_CHANNEL_PROFILE_P25_C4FM;
    g_read_base = 1000.0f;
    g_read_base_step = 0.0f;
    g_read_calls = 0;
    g_bump_generation_during_read = 0;
    g_output_kind_after_bump = -1;
    g_symbol_rate_hz_after_bump = 0;
    g_symbol_levels_after_bump = 0;
    g_channel_profile_after_bump = -1;
    g_cleanup_calls = 0;
    g_fail_reads = 0;
    g_failed_read_calls = 0;
    g_max_read_calls = 0;
    dsd_rtl_stream_metrics_hook_symbol_cache_pending_reset();
}

static void
reset_decoder_fixture(dsd_opts* opts, dsd_state* state, void* rtl_context) {
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->symboltiming = 0;
    state->rf_mod = 2;
    state->rtl_ctx = (struct RtlSdrContext*)rtl_context;
}

int
main(void) {
    static dsd_opts opts;
    static dsd_state state;
    static int fake_rtl_context;

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    opts.audio_in_type = AUDIO_IN_RTL;
    opts.symboltiming = 0;
    state.rf_mod = 1;
    state.rtl_ctx = (struct RtlSdrContext*)&fake_rtl_context;

    dsd_rtl_stream_io_hooks_set((dsd_rtl_stream_io_hooks){
        .read = fake_rtl_read,
        .return_pwr = fake_rtl_pwr,
    });
    dsd_rtl_stream_metrics_hooks metrics_hooks = {
        .output_kind = fake_output_kind,
        .output_rate_hz = fake_output_rate_hz,
        .symbol_profile = fake_symbol_profile,
        .stream_generation = fake_stream_generation,
    };
    dsd_rtl_stream_metrics_hooks_set(&metrics_hooks);

    /*
     * Symbol-path setup covers the direct CQPSK output path first, then the
     * discriminator path that seeds min/max state from the active channel
     * profile.
     */
    reset_stream_fixture();
    reset_decoder_fixture(&opts, &state, &fake_rtl_context);
    g_output_kind = RTL_STREAM_OUTPUT_SYMBOL_CQPSK;
    state.rf_mod = 1;
    assert(getSymbol(&opts, &state, 1) == 1000.0f);
    assert(state.min == -3.0f);
    assert(state.max == 3.0f);
    assert(state.lmid == -2.0f);
    assert(state.umid == 2.0f);

    g_output_kind = RTL_STREAM_OUTPUT_FSK_DISCRIMINATOR;
    g_stream_generation = 2U;
    g_output_rate_hz = 48000U;
    g_symbol_rate_hz = 4800;
    g_symbol_levels = 4;
    g_channel_profile = RTL_STREAM_CHANNEL_PROFILE_12K5;
    g_read_base = 30000.0f;
    g_read_base_step = 4.0f;
    state.rf_mod = 0;

    (void)getSymbol(&opts, &state, 1);
    assert(state.min == -30000.0f);
    assert(state.max == 30000.0f);
    assert(state.lmid == -20000.0f);
    assert(state.umid == 20000.0f);
    assert(state.minref == -24000.0f);
    assert(state.maxref == 24000.0f);
    assert(state.minbuf[0] == -30000.0f);
    assert(state.maxbuf[0] == 30000.0f);
    assert(state.minbuf[1023] == -30000.0f);
    assert(state.maxbuf[1023] == 30000.0f);
    assert(state.minmax_sum_window == 0);

    /*
     * Cached CQPSK symbols should be reused until the generation or channel
     * profile changes. Mid-read generation bumps must refresh cached metadata
     * without losing the samples returned by the read hook.
     */
    reset_stream_fixture();
    reset_decoder_fixture(&opts, &state, &fake_rtl_context);

    g_output_kind = RTL_STREAM_OUTPUT_SYMBOL_CQPSK;
    state.rf_mod = 1;
    assert(getSymbol(&opts, &state, 1) == 1000.0f);
    assert(dsd_rtl_stream_metrics_hook_symbol_cache_pending() == 3);
    assert(getSymbol(&opts, &state, 1) == 1001.0f);
    assert(dsd_rtl_stream_metrics_hook_symbol_cache_pending() == 2);
    assert(g_read_calls == 1);
    assert(state.min == -3.0f);
    assert(state.max == 3.0f);
    assert(state.lmid == -2.0f);
    assert(state.umid == 2.0f);

    g_channel_profile = RTL_STREAM_CHANNEL_PROFILE_P25_CQPSK;
    g_read_base = 1250.0f;

    assert(getSymbol(&opts, &state, 1) == 1250.0f);
    assert(dsd_rtl_stream_metrics_hook_symbol_cache_pending() == 3);
    assert(getSymbol(&opts, &state, 1) == 1251.0f);
    assert(dsd_rtl_stream_metrics_hook_symbol_cache_pending() == 2);
    assert(g_read_calls == 2);

    g_symbol_rate_hz = 6000;
    g_read_base = 1500.0f;

    assert(getSymbol(&opts, &state, 1) == 1500.0f);
    assert(getSymbol(&opts, &state, 1) == 1501.0f);
    assert(dsd_rtl_stream_metrics_hook_symbol_cache_pending() == 2);
    assert(g_read_calls == 3);

    g_stream_generation = 2;
    g_read_base = 2000.0f;

    assert(getSymbol(&opts, &state, 1) == 2000.0f);
    assert(getSymbol(&opts, &state, 1) == 2001.0f);
    assert(g_read_calls == 4);

    assert(getSymbol(&opts, &state, 1) == 2002.0f);
    assert(getSymbol(&opts, &state, 1) == 2003.0f);
    assert(dsd_rtl_stream_metrics_hook_symbol_cache_pending() == 0);
    assert(g_read_calls == 4);

    g_read_base = 3000.0f;
    g_read_base_step = 100.0f;
    g_bump_generation_during_read = 1;
    g_symbol_rate_hz_after_bump = 2400;
    g_symbol_levels_after_bump = 2;
    g_channel_profile_after_bump = RTL_STREAM_CHANNEL_PROFILE_6K25;

    assert(getSymbol(&opts, &state, 1) == 3100.0f);
    assert(g_stream_generation == 3U);
    assert(g_read_calls == 6);
    assert(state.rtl_symbol_cache_generation == 3U);
    assert(state.rtl_symbol_cache_symbol_rate_hz == 2400);
    assert(state.rtl_symbol_cache_channel_profile == RTL_STREAM_CHANNEL_PROFILE_6K25);
    assert(state.rtl_symbol_cache_levels == 2);
    assert(state.min == -1.0f);
    assert(state.max == 1.0f);
    assert(getSymbol(&opts, &state, 1) == 3101.0f);
    assert(dsd_rtl_stream_metrics_hook_symbol_cache_pending() == 2);
    assert(getSymbol(&opts, &state, 1) == 3102.0f);
    assert(getSymbol(&opts, &state, 1) == 3103.0f);
    assert(dsd_rtl_stream_metrics_hook_symbol_cache_pending() == 0);
    assert(g_cleanup_calls == 0);

    /*
     * FSK discriminator tests cover nominal sample-per-symbol choices, fractional
     * accumulation for high-rate modes, jitter adjustment, and output-kind changes
     * that happen while a read is in flight.
     */
    reset_stream_fixture();
    reset_decoder_fixture(&opts, &state, &fake_rtl_context);
    g_output_kind = RTL_STREAM_OUTPUT_FSK_DISCRIMINATOR;
    g_output_rate_hz = 48000U;
    g_symbol_rate_hz = 4800;
    g_symbol_levels = 4;
    g_channel_profile = RTL_STREAM_CHANNEL_PROFILE_12K5;
    g_read_base = 1000.0f;
    g_read_base_step = 4.0f;

    assert(getSymbol(&opts, &state, 1) == 1004.0f);
    assert(state.samplesPerSymbol == 10);
    assert(state.symbolCenter == 4);
    assert(state.jitter == -1);
    assert(g_read_calls == 3);
    assert(dsd_rtl_stream_metrics_hook_symbol_cache_pending() == 2);

    g_stream_generation = 4U;
    g_symbol_rate_hz = 2400;
    g_symbol_levels = 2;
    g_channel_profile = RTL_STREAM_CHANNEL_PROFILE_6K25;
    g_read_base = 2000.0f;
    g_read_base_step = 4.0f;
    float nxdn_symbol = getSymbol(&opts, &state, 1);
    if (fabsf(nxdn_symbol - 2009.7778f) >= 0.01f) {
        DSD_FPRINTF(stderr, "FSK discriminator generation-change symbol %.4f\n", nxdn_symbol);
    }
    assert(fabsf(nxdn_symbol - 2009.7778f) < 0.01f);
    assert(state.samplesPerSymbol == 20);
    assert(state.symbolCenter == dsd_opts_symbol_center(20));
    assert(state.rtl_symbol_cache_generation == 4U);
    assert(state.rtl_symbol_cache_symbol_rate_hz == 2400);
    assert(state.rtl_symbol_cache_channel_profile == RTL_STREAM_CHANNEL_PROFILE_6K25);
    assert(state.rtl_symbol_cache_levels == 2);

    reset_stream_fixture();
    reset_decoder_fixture(&opts, &state, &fake_rtl_context);
    g_output_kind = RTL_STREAM_OUTPUT_FSK_DISCRIMINATOR;
    g_output_rate_hz = 24000U;
    g_symbol_rate_hz = 9600;
    g_symbol_levels = 2;
    g_channel_profile = RTL_STREAM_CHANNEL_PROFILE_PROVOICE;
    g_read_base = 5000.0f;
    g_read_base_step = 4.0f;

    g_max_read_calls = 2;
    assert(getSymbol(&opts, &state, 0) == 5000.0f);
    assert(state.samplesPerSymbol == 2);
    assert(state.symbolCenter == dsd_opts_symbol_center(2));
    assert(state.jitter == -1);
    assert(g_read_calls == 1);
    g_max_read_calls = 0;

    g_read_base = 5000.0f;
    g_read_base_step = 4.0f;
    g_stream_generation++;
    assert(getSymbol(&opts, &state, 1) == 5000.0f);
    assert(state.samplesPerSymbol == 2);
    assert(state.symbolCenter == dsd_opts_symbol_center(2));
    assert(state.rtl_fsk_sps_accum == 4800);
    assert(getSymbol(&opts, &state, 1) == 5003.0f);
    assert(state.samplesPerSymbol == 3);
    assert(state.symbolCenter == dsd_opts_symbol_center(3));
    assert(state.rtl_fsk_sps_accum == 0);
    assert(getSymbol(&opts, &state, 1) == 5005.0f);
    assert(state.samplesPerSymbol == 2);
    assert(state.rtl_fsk_sps_accum == 4800);

    reset_stream_fixture();
    reset_decoder_fixture(&opts, &state, &fake_rtl_context);
    g_output_kind = RTL_STREAM_OUTPUT_FSK_DISCRIMINATOR;
    g_output_rate_hz = 48000U;
    g_symbol_rate_hz = 4800;
    g_symbol_levels = 4;
    g_channel_profile = RTL_STREAM_CHANNEL_PROFILE_12K5;
    g_read_base = 7000.0f;
    g_read_base_step = 4.0f;

    assert(getSymbol(&opts, &state, 1) == 7004.0f);
    assert(state.samplesPerSymbol == 10);
    assert(state.symbolCenter == dsd_opts_symbol_center(10));
    assert(dsd_rtl_stream_metrics_hook_symbol_cache_pending() == 2);
    state.jitter = state.symbolCenter;
    float shifted_symbol = getSymbol(&opts, &state, 0);
    if (fabsf(shifted_symbol - 7015.0f) >= 0.01f) {
        DSD_FPRINTF(stderr, "FSK discriminator jitter-adjusted symbol %.4f\n", shifted_symbol);
    }
    assert(fabsf(shifted_symbol - 7015.0f) < 0.01f);
    assert(state.jitter == -1);
    assert(g_read_calls == 6);
    assert(dsd_rtl_stream_metrics_hook_symbol_cache_pending() == 3);

    reset_stream_fixture();
    reset_decoder_fixture(&opts, &state, &fake_rtl_context);
    g_output_kind = RTL_STREAM_OUTPUT_FSK_DISCRIMINATOR;
    g_output_rate_hz = 48000U;
    g_symbol_rate_hz = 4800;
    g_symbol_levels = 4;
    g_channel_profile = RTL_STREAM_CHANNEL_PROFILE_12K5;
    g_read_base = 4000.0f;
    g_read_base_step = 100.0f;
    g_bump_generation_during_read = 1;
    g_output_kind_after_bump = RTL_STREAM_OUTPUT_SYMBOL_CQPSK;
    g_symbol_rate_hz_after_bump = 4800;
    g_symbol_levels_after_bump = 4;
    g_channel_profile_after_bump = RTL_STREAM_CHANNEL_PROFILE_P25_CQPSK;

    assert(getSymbol(&opts, &state, 1) == 0.0f);
    assert(g_cleanup_calls == 0);
    assert(g_stream_generation == 2U);
    assert(g_output_kind == RTL_STREAM_OUTPUT_SYMBOL_CQPSK);
    state.rf_mod = 1;
    assert(getSymbol(&opts, &state, 1) == 4100.0f);
    assert(g_cleanup_calls == 0);

    /*
     * Read failures should surface as the existing empty-symbol path, trigger
     * the legacy cleanup hook once, and leave global hooks reset for later tests.
     */
    reset_stream_fixture();
    reset_decoder_fixture(&opts, &state, &fake_rtl_context);
    g_fail_reads = 1;
    g_failed_read_calls = 0;
    assert(getSymbol(&opts, &state, 1) == 0.0f);
    assert(g_failed_read_calls == 1);
    assert(g_cleanup_calls == 1);

    dsd_rtl_stream_io_hooks_set((dsd_rtl_stream_io_hooks){0});
    dsd_rtl_stream_metrics_hooks_set(NULL);
    dsd_rtl_stream_metrics_hook_symbol_cache_pending_reset();
    return 0;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
