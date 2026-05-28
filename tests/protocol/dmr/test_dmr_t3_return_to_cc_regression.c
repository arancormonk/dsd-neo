// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Regression: DMR Tier III return-to-CC must retune even when only
 * trunk_enable is set (p25_trunk disabled), and must not apply P25-only
 * CC symbol/modulation overrides when no P25 CC is active.
 */

#include <assert.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/engine/trunk_tuning.h>
#include <dsd-neo/io/rigctl_client.h>
#include <dsd-neo/io/rtl_stream_c.h>
#include <dsd-neo/runtime/config.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "dsd-neo/io/rtl_stream_fwd.h"
#include "dsd-neo/platform/sockets.h"
#include "dsd-neo/runtime/trunk_tuning_hooks.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

/*
 * Local stubs for trunk_tuning.c dependencies.
 * Keep behavior minimal and deterministic for regression coverage.
 */
static int g_setfreq_calls = 0;
static long int g_last_setfreq_hz = 0;
static bool g_setfreq_result = true;
static bool g_setmod_result = true;
static int g_frame_sync_reset_calls = 0;
static int g_p25p2_frame_reset_calls = 0;
static int g_rtl_tune_result = RTL_STREAM_TUNE_OK;
static int g_rtl_cqpsk_enable = 0;
static int g_rtl_symbol_rate_hz = 4800;
static int g_rtl_symbol_levels = 4;
static int g_rtl_channel_profile = RTL_STREAM_CHANNEL_PROFILE_P25_C4FM;
static int g_rtl_ted_sps = 5;
static int g_rtl_ted_sps_override = 0;
static int g_drain_audio_calls = 0;
static int g_rtl_tune_calls = 0;

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
dmr_reset_blocks(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_frame_sync_reset_mod_state(void) {
    g_frame_sync_reset_calls++;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
p25_p2_frame_reset(void) {
    g_p25p2_frame_reset_calls++;
}

int
// NOLINTNEXTLINE(misc-use-internal-linkage)
p25_sm_in_tick(void) {
    return 0;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_drain_audio_output(dsd_opts* opts) {
    (void)opts;
    g_drain_audio_calls++;
}

bool
SetFreq(dsd_socket_t sockfd, long int freq) {
    (void)sockfd;
    g_setfreq_calls++;
    g_last_setfreq_hz = freq;
    return g_setfreq_result;
}

bool
SetModulation(dsd_socket_t sockfd, int bandwidth) {
    (void)sockfd;
    (void)bandwidth;
    return g_setmod_result;
}

uint32_t
rtl_stream_output_rate(const RtlSdrContext* ctx) {
    (void)ctx;
    return 48000;
}

int
rtl_stream_tune(RtlSdrContext* ctx, uint32_t center_freq_hz) {
    (void)ctx;
    (void)center_freq_hz;
    g_rtl_tune_calls++;
    return g_rtl_tune_result;
}

void
rtl_stream_toggle_cqpsk(int onoff) {
    g_rtl_cqpsk_enable = onoff ? 1 : 0;
    if (g_rtl_cqpsk_enable) {
        g_rtl_channel_profile = RTL_STREAM_CHANNEL_PROFILE_P25_CQPSK;
    }
}

int
rtl_stream_dsp_get(int* cqpsk_enable, int* fll_enable, int* ted_enable) {
    if (cqpsk_enable) {
        *cqpsk_enable = g_rtl_cqpsk_enable;
    }
    if (fll_enable) {
        *fll_enable = 0;
    }
    if (ted_enable) {
        *ted_enable = 0;
    }
    return 0;
}

int
rtl_stream_get_symbol_profile_full(int* out_symbol_rate_hz, int* out_levels, int* out_channel_profile) {
    if (out_symbol_rate_hz) {
        *out_symbol_rate_hz = g_rtl_symbol_rate_hz;
    }
    if (out_levels) {
        *out_levels = g_rtl_symbol_levels;
    }
    if (out_channel_profile) {
        *out_channel_profile = g_rtl_channel_profile;
    }
    return 0;
}

int
rtl_stream_set_symbol_profile(int symbol_rate_hz, int levels, int channel_profile) {
    g_rtl_symbol_rate_hz = symbol_rate_hz;
    g_rtl_symbol_levels = levels;
    g_rtl_channel_profile = channel_profile;
    return 0;
}

int
rtl_stream_get_ted_sps(void) {
    return g_rtl_ted_sps;
}

int
rtl_stream_get_ted_sps_override(void) {
    return g_rtl_ted_sps_override;
}

void
rtl_stream_set_ted_sps(int sps) {
    g_rtl_ted_sps_override = sps;
}

void
rtl_stream_clear_ted_sps_override(void) {
    g_rtl_ted_sps_override = 0;
}

void
rtl_stream_set_ted_sps_no_override(int sps) {
    g_rtl_ted_sps = sps;
}

uint64_t
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_time_monotonic_ns(void) {
    return 1234500000000ULL;
}

void
dsd_neo_config_init(const dsd_opts* opts) {
    (void)opts;
}

const dsdneoRuntimeConfig*
dsd_neo_get_config(void) {
    return NULL;
}

int
main(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    if (!opts || !state) {
        DSD_FPRINTF(stderr, "allocation failed\n");
        free(state);
        free(opts);
        return 1;
    }

    /* DMR trunking active via protocol-agnostic flag only. */
    opts->trunk_enable = 1;
    opts->p25_trunk = 0;
    opts->trunk_is_tuned = 1;
    opts->p25_is_tuned = 1;
    opts->audio_in_type = AUDIO_IN_PULSE; /* avoid RTL path in this regression */
    opts->use_rigctl = 1;
    opts->rigctl_sockfd = 1;

    state->trunk_cc_freq = 851000000;
    state->p25_cc_freq = 0;
    state->trunk_vc_freq[0] = 852000000;
    state->trunk_vc_freq[1] = 852000000;
    state->last_cc_sync_time = 0;
    state->last_cc_sync_time_m = 0.0;

    /* DMR/GFSK-ish demod settings should remain unchanged on DMR return. */
    state->samplesPerSymbol = 17;
    state->symbolCenter = 8;
    state->rf_mod = 2;

    g_setfreq_calls = 0;
    g_last_setfreq_hz = 0;

    dsd_engine_return_to_cc(opts, state);

    /* Core return semantics. */
    assert(opts->trunk_is_tuned == 0);
    assert(opts->p25_is_tuned == 0);
    assert(state->trunk_vc_freq[0] == 0);
    assert(state->trunk_vc_freq[1] == 0);

    /* Critical regression check: DMR return must still issue a retune to CC. */
    assert(g_setfreq_calls == 1);
    assert(g_last_setfreq_hz == state->trunk_cc_freq);

    /* Critical regression check: DMR return still updates CC retune bookkeeping. */
    assert(state->last_cc_sync_time != 0);
    assert(state->last_cc_sync_time_m > 0.0);

    /* Critical regression check: no P25-specific modulation/timing override in DMR path. */
    assert(state->samplesPerSymbol == 17);
    assert(state->symbolCenter == 8);
    assert(state->rf_mod == 2);

    /* Rigctl modulation remains best-effort: a modulation failure must not
     * report tune failure after the frequency command succeeds. */
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    opts->audio_in_type = AUDIO_IN_PULSE;
    opts->use_rigctl = 1;
    opts->setmod_bw = 12500;
    g_setfreq_calls = 0;
    g_last_setfreq_hz = 0;
    g_setmod_result = false;
    g_setfreq_result = true;
    g_frame_sync_reset_calls = 0;
    assert(dsd_engine_trunk_tune_to_freq(opts, state, 853000000, 0) == DSD_TRUNK_TUNE_RESULT_OK);
    assert(g_setfreq_calls == 1);
    assert(g_last_setfreq_hz == 853000000);
    assert(opts->trunk_is_tuned == 1);
    assert(state->trunk_vc_freq[0] == 853000000);
    assert(g_frame_sync_reset_calls == 1);

    /* RTL input driven by rigctl still needs the generic output drain because
     * the RTL stream backend will not run its retune drain policy. */
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->use_rigctl = 1;
    g_setfreq_calls = 0;
    g_last_setfreq_hz = 0;
    g_setfreq_result = true;
    g_drain_audio_calls = 0;
    g_rtl_tune_calls = 0;
    assert(dsd_engine_trunk_tune_to_freq(opts, state, 853500000, 0) == DSD_TRUNK_TUNE_RESULT_OK);
    assert(g_drain_audio_calls == 1);
    assert(g_setfreq_calls == 1);
    assert(g_last_setfreq_hz == 853500000);
    assert(g_rtl_tune_calls == 0);

    /* If the frequency command itself fails, the decoder must not advance state
     * or reset DSP acquisition state for a channel it did not tune to. */
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    opts->audio_in_type = AUDIO_IN_PULSE;
    opts->use_rigctl = 1;
    opts->setmod_bw = 12500;
    g_setmod_result = false;
    g_setfreq_result = false;
    g_frame_sync_reset_calls = 0;
    assert(dsd_engine_trunk_tune_to_freq(opts, state, 854000000, 0) == DSD_TRUNK_TUNE_RESULT_FAILED);
    assert(opts->trunk_is_tuned == 0);
    assert(state->trunk_vc_freq[0] == 0);
    assert(g_frame_sync_reset_calls == 0);

#ifdef USE_RADIO
    /* Native RTL stream retunes keep relying on rtl_stream_tune() for
     * hardware-side drain/clear behavior instead of also draining here. */
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    opts->audio_in_type = AUDIO_IN_RTL;
    state->rtl_ctx = (RtlSdrContext*)state;
    g_rtl_tune_result = RTL_STREAM_TUNE_OK;
    g_drain_audio_calls = 0;
    g_rtl_tune_calls = 0;
    assert(dsd_engine_trunk_tune_to_freq(opts, state, 854500000, 0) == DSD_TRUNK_TUNE_RESULT_OK);
    assert(g_drain_audio_calls == 0);
    assert(g_rtl_tune_calls == 1);

    /* Deferred RTL voice retunes must roll back the demod profile/TED changes
     * prepared for the requested channel. */
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->p25_trunk = 1;
    state->rtl_ctx = (RtlSdrContext*)state;
    state->rf_mod = 1;
    state->p25_p2_active_slot = 0;
    state->p25_vc_cqpsk_override = 1;
    g_rtl_tune_result = RTL_STREAM_TUNE_DEFERRED;
    g_rtl_cqpsk_enable = 0;
    g_rtl_symbol_rate_hz = 4800;
    g_rtl_symbol_levels = 4;
    g_rtl_channel_profile = RTL_STREAM_CHANNEL_PROFILE_P25_C4FM;
    g_rtl_ted_sps = 5;
    g_rtl_ted_sps_override = 0;
    g_frame_sync_reset_calls = 0;
    g_p25p2_frame_reset_calls = 0;
    assert(dsd_engine_trunk_tune_to_freq(opts, state, 855000000, 4) == DSD_TRUNK_TUNE_RESULT_DEFERRED);
    assert(opts->trunk_is_tuned == 0);
    assert(state->trunk_vc_freq[0] == 0);
    assert(state->p25_vc_cqpsk_override == 1);
    assert(g_rtl_cqpsk_enable == 0);
    assert(g_rtl_symbol_rate_hz == 4800);
    assert(g_rtl_channel_profile == RTL_STREAM_CHANNEL_PROFILE_P25_C4FM);
    assert(g_rtl_ted_sps == 5);
    assert(g_rtl_ted_sps_override == 0);
    assert(g_frame_sync_reset_calls == 0);
    assert(g_p25p2_frame_reset_calls == 0);

    /* Pending RTL retunes keep the requested demod profile and trunk state
     * because the queued controller request may still complete after timeout. */
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->p25_trunk = 1;
    state->rtl_ctx = (RtlSdrContext*)state;
    state->rf_mod = 1;
    state->p25_p2_active_slot = 0;
    state->p25_vc_cqpsk_override = 1;
    g_rtl_tune_result = RTL_STREAM_TUNE_TIMEOUT;
    g_rtl_cqpsk_enable = 0;
    g_rtl_symbol_rate_hz = 4800;
    g_rtl_symbol_levels = 4;
    g_rtl_channel_profile = RTL_STREAM_CHANNEL_PROFILE_P25_C4FM;
    g_rtl_ted_sps = 5;
    g_rtl_ted_sps_override = 0;
    g_frame_sync_reset_calls = 0;
    g_p25p2_frame_reset_calls = 0;
    assert(dsd_engine_trunk_tune_to_freq(opts, state, 855000000, 8) == DSD_TRUNK_TUNE_RESULT_PENDING);
    assert(opts->trunk_is_tuned == 1);
    assert(state->trunk_vc_freq[0] == 855000000);
    assert(state->p25_vc_cqpsk_override == -1);
    assert(g_rtl_cqpsk_enable == 1);
    assert(g_rtl_symbol_rate_hz == 6000);
    assert(g_rtl_channel_profile == RTL_STREAM_CHANNEL_PROFILE_P25_CQPSK);
    assert(g_rtl_ted_sps == 5);
    assert(g_rtl_ted_sps_override == 8);
    assert(g_frame_sync_reset_calls == 1);
    assert(g_p25p2_frame_reset_calls == 1);

    /* Pending RTL CC retunes likewise keep the requested CC profile and tracked
     * CC state aligned with the queued hardware request. */
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->p25_trunk = 1;
    state->rtl_ctx = (RtlSdrContext*)state;
    state->rf_mod = 0;
    state->p25_cc_is_tdma = 1;
    state->trunk_cc_freq = 851000000;
    g_rtl_tune_result = RTL_STREAM_TUNE_TIMEOUT;
    g_rtl_cqpsk_enable = 0;
    g_rtl_symbol_rate_hz = 4800;
    g_rtl_symbol_levels = 4;
    g_rtl_channel_profile = RTL_STREAM_CHANNEL_PROFILE_P25_C4FM;
    g_rtl_ted_sps = 5;
    g_rtl_ted_sps_override = 0;
    g_frame_sync_reset_calls = 0;
    assert(dsd_engine_trunk_tune_to_cc(opts, state, 852000000, 4) == DSD_TRUNK_TUNE_RESULT_PENDING);
    assert(state->rf_mod == 1);
    assert(state->trunk_cc_freq == 852000000);
    assert(g_rtl_cqpsk_enable == 1);
    assert(g_rtl_symbol_rate_hz == 6000);
    assert(g_rtl_channel_profile == RTL_STREAM_CHANNEL_PROFILE_P25_CQPSK);
    assert(g_rtl_ted_sps == 4);
    assert(g_rtl_ted_sps_override == 0);
    assert(g_frame_sync_reset_calls == 1);
#endif

    printf("DMR_T3_RETURN_TO_CC_REGRESSION: OK\n");
    free(state);
    free(opts);
    return 0;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
