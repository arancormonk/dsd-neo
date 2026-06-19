// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Regression coverage for trunk retune edge cases:
 * - protocol-agnostic return-to-CC must retune when only trunk_enable is set
 * - non-P25 trunking must not apply P25-only CC symbol/modulation overrides
 * - RTL P25 voice/CC retunes must queue demod profile changes until the
 *   controller reaches the hardware retune boundary
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
#include <string.h>
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
static int g_rtl_pending_active = 0;
static int g_rtl_pending_cqpsk = -1;
static int g_rtl_pending_symbol_rate_hz = 0;
static int g_rtl_pending_symbol_levels = 0;
static int g_rtl_pending_channel_profile = -1;
static int g_rtl_pending_ted_sps = 0;
static int g_rtl_pending_ted_override = 0;
static int g_rtl_pending_tuner_gain_is_set = 0;
static int g_rtl_pending_tuner_gain_tenth_db = 0;
static int g_rtl_pending_tuner_gain_is_auto = 0;
static int g_rtl_pending_tuner_autogain_is_set = 0;
static int g_rtl_pending_tuner_autogain_on = 0;
static uint32_t g_rtl_pending_target_freq_hz = 0;
static size_t g_trunk_scan_target_count = 0;
static int g_trunk_scan_saved_autogain_is_set = 0;
static int g_trunk_scan_saved_autogain_on = 0;
static int g_trunk_scan_active_p25_cqpsk_is_set = 0;
static int g_trunk_scan_active_p25_cqpsk_enable = 0;
static int g_runtime_config_is_set = 0;
static dsdneoRuntimeConfig g_runtime_config;

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

size_t
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_engine_trunk_scan_target_count(const dsd_state* state) {
    (void)state;
    return g_trunk_scan_target_count;
}

int
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_engine_trunk_scan_saved_tuner_autogain(const dsd_state* state, int* out_on) {
    (void)state;
    if (out_on) {
        *out_on = g_trunk_scan_saved_autogain_on;
    }
    return g_trunk_scan_saved_autogain_is_set;
}

int
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_engine_trunk_scan_active_p25_cqpsk_request(const dsd_state* state, int* out_enable) {
    (void)state;
    if (!out_enable || !g_trunk_scan_active_p25_cqpsk_is_set) {
        return 0;
    }
    *out_enable = g_trunk_scan_active_p25_cqpsk_enable ? 1 : 0;
    return 1;
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

static void
apply_pending_retune_profile(uint32_t target_freq_hz) {
    if (!g_rtl_pending_active) {
        return;
    }
    if (g_rtl_pending_target_freq_hz != 0) {
        if (target_freq_hz == 0 || g_rtl_pending_target_freq_hz != target_freq_hz) {
            return;
        }
    }
    if (g_rtl_pending_cqpsk >= 0) {
        g_rtl_cqpsk_enable = g_rtl_pending_cqpsk ? 1 : 0;
    }
    if (g_rtl_pending_symbol_rate_hz > 0 && (g_rtl_pending_symbol_levels == 2 || g_rtl_pending_symbol_levels == 4)) {
        g_rtl_symbol_rate_hz = g_rtl_pending_symbol_rate_hz;
        g_rtl_symbol_levels = g_rtl_pending_symbol_levels;
        g_rtl_channel_profile = g_rtl_pending_channel_profile;
    }
    if (g_rtl_pending_ted_sps > 0) {
        g_rtl_ted_sps = g_rtl_pending_ted_sps;
        g_rtl_ted_sps_override = g_rtl_pending_ted_override ? g_rtl_pending_ted_sps : 0;
    }
    g_rtl_pending_active = 0;
    g_rtl_pending_target_freq_hz = 0;
}

int
rtl_stream_tune(RtlSdrContext* ctx, uint32_t center_freq_hz) {
    (void)ctx;
    g_rtl_tune_calls++;
    if (g_rtl_tune_result == RTL_STREAM_TUNE_OK) {
        apply_pending_retune_profile(center_freq_hz);
    }
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
rtl_stream_get_cqpsk_status(int* cqpsk_enable, int* cqpsk_timing_active) {
    if (cqpsk_enable) {
        *cqpsk_enable = g_rtl_cqpsk_enable;
    }
    if (cqpsk_timing_active) {
        *cqpsk_timing_active = g_rtl_cqpsk_enable ? 1 : 0;
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

void
rtl_stream_prepare_retune_profile_for_target_with_gain(uint32_t target_freq_hz, int cqpsk_enable, int symbol_rate_hz,
                                                       int levels, int channel_profile, int ted_sps,
                                                       int persist_ted_override,
                                                       const rtl_stream_retune_gain_profile* gain_profile) {
    g_rtl_pending_cqpsk = cqpsk_enable;
    g_rtl_pending_symbol_rate_hz = symbol_rate_hz;
    g_rtl_pending_symbol_levels = levels;
    g_rtl_pending_channel_profile = channel_profile;
    g_rtl_pending_ted_sps = ted_sps;
    g_rtl_pending_ted_override = persist_ted_override ? 1 : 0;
    g_rtl_pending_tuner_gain_is_set = gain_profile ? gain_profile->tuner_gain_is_set : 0;
    g_rtl_pending_tuner_gain_tenth_db = gain_profile ? gain_profile->tuner_gain_tenth_db : 0;
    g_rtl_pending_tuner_gain_is_auto = gain_profile ? gain_profile->tuner_gain_is_auto : 0;
    g_rtl_pending_tuner_autogain_is_set = gain_profile ? gain_profile->tuner_autogain_is_set : 0;
    g_rtl_pending_tuner_autogain_on = gain_profile ? gain_profile->tuner_autogain_on : 0;
    g_rtl_pending_target_freq_hz = target_freq_hz;
    g_rtl_pending_active = 1;
}

void
rtl_stream_prepare_retune_profile_for_target(uint32_t target_freq_hz, int cqpsk_enable, int symbol_rate_hz, int levels,
                                             int channel_profile, int ted_sps, int persist_ted_override) {
    rtl_stream_prepare_retune_profile_for_target_with_gain(target_freq_hz, cqpsk_enable, symbol_rate_hz, levels,
                                                           channel_profile, ted_sps, persist_ted_override, NULL);
}

void
rtl_stream_prepare_retune_profile(int cqpsk_enable, int symbol_rate_hz, int levels, int channel_profile, int ted_sps,
                                  int persist_ted_override) {
    rtl_stream_prepare_retune_profile_for_target(0, cqpsk_enable, symbol_rate_hz, levels, channel_profile, ted_sps,
                                                 persist_ted_override);
}

void
rtl_stream_apply_pending_retune_profile(void) {
    apply_pending_retune_profile(0);
}

void
rtl_stream_apply_pending_retune_profile_for_target(uint32_t target_freq_hz) {
    apply_pending_retune_profile(target_freq_hz);
}

void
rtl_stream_clear_pending_retune_profile(void) {
    g_rtl_pending_active = 0;
    g_rtl_pending_target_freq_hz = 0;
    g_rtl_pending_tuner_gain_is_set = 0;
    g_rtl_pending_tuner_gain_tenth_db = 0;
    g_rtl_pending_tuner_gain_is_auto = 0;
    g_rtl_pending_tuner_autogain_is_set = 0;
    g_rtl_pending_tuner_autogain_on = 0;
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
    return g_runtime_config_is_set ? &g_runtime_config : NULL;
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
    state->p25_p2_audio_allowed[0] = 1;
    state->p25_p2_audio_allowed[1] = 1;
    state->p25_p2_enc_lockout_muted[0] = 1;
    state->p25_p2_enc_lockout_muted[1] = 1;
    state->last_cc_sync_time = 0;
    state->last_cc_sync_time_m = 0.0;
    DSD_SNPRINTF(state->call_string[0], sizeof(state->call_string[0]), "%s", "left active");
    DSD_SNPRINTF(state->call_string[1], sizeof(state->call_string[1]), "%s", "right active");
    DSD_SNPRINTF(state->active_channel[0], sizeof(state->active_channel[0]), "%s", "Active Ch: 1234 TG: 56;");
    DSD_SNPRINTF(state->active_channel[1], sizeof(state->active_channel[1]), "%s", "Active Ch: 5678 TG: 90;");

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
    assert(state->p25_p2_audio_allowed[0] == 0);
    assert(state->p25_p2_audio_allowed[1] == 0);
    assert(state->p25_p2_enc_lockout_muted[0] == 0);
    assert(state->p25_p2_enc_lockout_muted[1] == 0);
    assert(strcmp(state->call_string[0], "                     ") == 0);
    assert(strcmp(state->call_string[1], "                     ") == 0);
    assert(state->active_channel[0][0] == '\0');
    assert(state->active_channel[1][0] == '\0');

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

    /* Non-radio P25 return-to-CC timing must follow the active PCM input rate,
     * not the RTL bandwidth fallback. */
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    opts->audio_in_type = AUDIO_IN_WAV;
    opts->wav_sample_rate = 96000;
    opts->wav_decimator = 48000;
    opts->use_rigctl = 1;
    opts->p25_trunk = 1;
    opts->trunk_enable = 1;
    opts->p25_is_tuned = 1;
    opts->trunk_is_tuned = 1;
    state->p25_cc_freq = 851000000;
    state->trunk_cc_freq = 851000000;
    state->p25_cc_is_tdma = 1;
    state->samplesPerSymbol = 8;
    state->symbolCenter = 3;
    g_setfreq_calls = 0;
    g_last_setfreq_hz = 0;
    g_setfreq_result = true;
    assert(dsd_engine_return_to_cc(opts, state) == DSD_TRUNK_TUNE_RESULT_OK);
    assert(g_setfreq_calls == 1);
    assert(g_last_setfreq_hz == 851000000);
    assert(state->samplesPerSymbol == 16);
    assert(state->symbolCenter == 7);
    assert(state->rf_mod == 1);

    /* P25P2 reset detection uses the same non-radio timing rate on direct
     * voice-channel tunes. */
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    opts->audio_in_type = AUDIO_IN_WAV;
    opts->wav_sample_rate = 96000;
    opts->wav_decimator = 48000;
    opts->use_rigctl = 1;
    opts->p25_trunk = 1;
    g_frame_sync_reset_calls = 0;
    g_p25p2_frame_reset_calls = 0;
    g_setfreq_result = true;
    assert(dsd_engine_trunk_tune_to_freq(opts, state, 853600000, 16) == DSD_TRUNK_TUNE_RESULT_OK);
    assert(g_frame_sync_reset_calls == 1);
    assert(g_p25p2_frame_reset_calls == 1);

#ifdef USE_RADIO
    /* RTL audio retuned by rigctl has no native RTL controller boundary, so the
     * queued P25 VC demod profile must be applied after SetFreq succeeds. */
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->use_rigctl = 1;
    opts->p25_trunk = 1;
    state->rtl_ctx = (RtlSdrContext*)state;
    state->rf_mod = 1;
    state->p25_p2_active_slot = 0;
    state->p25_vc_cqpsk_override = 1;
    g_setfreq_calls = 0;
    g_last_setfreq_hz = 0;
    g_setfreq_result = true;
    g_rtl_tune_calls = 0;
    g_rtl_cqpsk_enable = 0;
    g_rtl_symbol_rate_hz = 4800;
    g_rtl_symbol_levels = 4;
    g_rtl_channel_profile = RTL_STREAM_CHANNEL_PROFILE_P25_C4FM;
    g_rtl_ted_sps = 5;
    g_rtl_ted_sps_override = 0;
    g_rtl_pending_active = 0;
    assert(dsd_engine_trunk_tune_to_freq(opts, state, 853750000, 8) == DSD_TRUNK_TUNE_RESULT_OK);
    assert(g_setfreq_calls == 1);
    assert(g_last_setfreq_hz == 853750000);
    assert(g_rtl_tune_calls == 0);
    assert(g_rtl_pending_active == 0);
    assert(g_rtl_cqpsk_enable == 1);
    assert(g_rtl_symbol_rate_hz == 6000);
    assert(g_rtl_channel_profile == RTL_STREAM_CHANNEL_PROFILE_P25_CQPSK);
    assert(g_rtl_ted_sps == 8);
    assert(g_rtl_ted_sps_override == 8);
#endif

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

    /* DMR/GFSK control-channel retunes must replace any previous P25 CQPSK
     * profile at the RTL retune boundary. */
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->trunk_enable = 1;
    state->rtl_ctx = (RtlSdrContext*)state;
    state->rf_mod = 2;
    g_rtl_tune_result = RTL_STREAM_TUNE_OK;
    g_rtl_cqpsk_enable = 1;
    g_rtl_symbol_rate_hz = 6000;
    g_rtl_symbol_levels = 4;
    g_rtl_channel_profile = RTL_STREAM_CHANNEL_PROFILE_P25_CQPSK;
    g_rtl_ted_sps = 8;
    g_rtl_ted_sps_override = 8;
    g_rtl_pending_active = 0;
    assert(dsd_engine_trunk_tune_to_cc(opts, state, 452000000, 10) == DSD_TRUNK_TUNE_RESULT_OK);
    assert(state->trunk_cc_freq == 452000000);
    assert(g_rtl_pending_active == 0);
    assert(g_rtl_cqpsk_enable == 0);
    assert(g_rtl_symbol_rate_hz == 4800);
    assert(g_rtl_symbol_levels == 4);
    assert(g_rtl_channel_profile == RTL_STREAM_CHANNEL_PROFILE_12K5);
    assert(g_rtl_ted_sps == 10);
    assert(g_rtl_ted_sps_override == 0);

    /* Trunk-scan RTL retunes queue the active target/global gain with the
     * demod profile so gain changes happen at the retune boundary. */
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->trunk_enable = 1;
    opts->trunk_scan_enabled = 1;
    opts->rtl_gain_value = 27;
    state->rtl_ctx = (RtlSdrContext*)state;
    state->rf_mod = 2;
    g_trunk_scan_target_count = 2;
    g_trunk_scan_saved_autogain_is_set = 1;
    g_trunk_scan_saved_autogain_on = 1;
    g_rtl_tune_result = RTL_STREAM_TUNE_TIMEOUT;
    rtl_stream_clear_pending_retune_profile();
    assert(dsd_engine_trunk_tune_to_cc(opts, state, 453000000, 10) == DSD_TRUNK_TUNE_RESULT_PENDING);
    assert(g_rtl_pending_active == 1);
    assert(g_rtl_pending_tuner_gain_is_set == 1);
    assert(g_rtl_pending_tuner_gain_tenth_db == 270);
    assert(g_rtl_pending_tuner_gain_is_auto == 0);
    assert(g_rtl_pending_tuner_autogain_is_set == 1);
    assert(g_rtl_pending_tuner_autogain_on == 0);

    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->trunk_enable = 1;
    opts->trunk_scan_enabled = 1;
    opts->rtl_gain_value = 0;
    state->rtl_ctx = (RtlSdrContext*)state;
    state->rf_mod = 2;
    g_trunk_scan_target_count = 2;
    g_trunk_scan_saved_autogain_is_set = 1;
    g_trunk_scan_saved_autogain_on = 1;
    g_rtl_tune_result = RTL_STREAM_TUNE_TIMEOUT;
    rtl_stream_clear_pending_retune_profile();
    assert(dsd_engine_trunk_tune_to_cc(opts, state, 453500000, 10) == DSD_TRUNK_TUNE_RESULT_PENDING);
    assert(g_rtl_pending_active == 1);
    assert(g_rtl_pending_tuner_gain_is_set == 1);
    assert(g_rtl_pending_tuner_gain_is_auto == 1);
    assert(g_rtl_pending_tuner_autogain_is_set == 1);
    assert(g_rtl_pending_tuner_autogain_on == 1);
    g_trunk_scan_target_count = 0;
    g_trunk_scan_saved_autogain_is_set = 0;
    g_trunk_scan_saved_autogain_on = 0;
    rtl_stream_clear_pending_retune_profile();

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
    g_rtl_pending_active = 0;
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
    assert(g_rtl_pending_active == 0);
    assert(g_frame_sync_reset_calls == 0);
    assert(g_p25p2_frame_reset_calls == 0);

    /* Pending RTL retunes keep active demod settings unchanged until the
     * controller reaches the retune boundary, while preserving the requested
     * profile for that queued hardware request. */
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
    g_rtl_pending_active = 0;
    g_frame_sync_reset_calls = 0;
    g_p25p2_frame_reset_calls = 0;
    assert(dsd_engine_trunk_tune_to_freq(opts, state, 855000000, 8) == DSD_TRUNK_TUNE_RESULT_PENDING);
    assert(opts->trunk_is_tuned == 1);
    assert(state->trunk_vc_freq[0] == 855000000);
    assert(state->p25_vc_cqpsk_override == -1);
    assert(g_rtl_cqpsk_enable == 0);
    assert(g_rtl_symbol_rate_hz == 4800);
    assert(g_rtl_channel_profile == RTL_STREAM_CHANNEL_PROFILE_P25_C4FM);
    assert(g_rtl_ted_sps == 5);
    assert(g_rtl_ted_sps_override == 0);
    assert(g_rtl_pending_active == 1);
    assert(g_rtl_pending_cqpsk == 1);
    assert(g_rtl_pending_symbol_rate_hz == 6000);
    assert(g_rtl_pending_channel_profile == RTL_STREAM_CHANNEL_PROFILE_P25_CQPSK);
    assert(g_rtl_pending_ted_sps == 8);
    assert(g_rtl_pending_ted_override == 1);
    assert(g_frame_sync_reset_calls == 1);
    assert(g_p25p2_frame_reset_calls == 1);

    /* Pending RTL CC retunes likewise leave the active demod settings alone
     * until the controller applies the queued control-channel profile. */
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
    g_rtl_pending_active = 0;
    g_frame_sync_reset_calls = 0;
    assert(dsd_engine_trunk_tune_to_cc(opts, state, 852000000, 4) == DSD_TRUNK_TUNE_RESULT_PENDING);
    assert(state->rf_mod == 1);
    assert(state->trunk_cc_freq == 852000000);
    assert(g_rtl_cqpsk_enable == 0);
    assert(g_rtl_symbol_rate_hz == 4800);
    assert(g_rtl_channel_profile == RTL_STREAM_CHANNEL_PROFILE_P25_C4FM);
    assert(g_rtl_ted_sps == 5);
    assert(g_rtl_ted_sps_override == 0);
    assert(g_rtl_pending_active == 1);
    assert(g_rtl_pending_cqpsk == 1);
    assert(g_rtl_pending_symbol_rate_hz == 6000);
    assert(g_rtl_pending_channel_profile == RTL_STREAM_CHANNEL_PROFILE_P25_CQPSK);
    assert(g_rtl_pending_ted_sps == 4);
    assert(g_rtl_pending_ted_override == 0);
    assert(g_frame_sync_reset_calls == 1);

    /* Empty trunk-scan modulation preserves the explicit runtime CQPSK mode. */
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    DSD_MEMSET(&g_runtime_config, 0, sizeof(g_runtime_config));
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->p25_trunk = 1;
    opts->trunk_scan_enabled = 1;
    state->rtl_ctx = (RtlSdrContext*)state;
    state->rf_mod = 0;
    state->p25_cc_is_tdma = 0;
    g_runtime_config_is_set = 1;
    g_runtime_config.cqpsk_is_set = 1;
    g_runtime_config.cqpsk_enable = 1;
    g_trunk_scan_active_p25_cqpsk_is_set = 0;
    g_rtl_tune_result = RTL_STREAM_TUNE_OK;
    g_rtl_cqpsk_enable = 1;
    g_rtl_symbol_rate_hz = 6000;
    g_rtl_symbol_levels = 4;
    g_rtl_channel_profile = RTL_STREAM_CHANNEL_PROFILE_P25_CQPSK;
    g_rtl_pending_active = 0;
    assert(dsd_engine_trunk_tune_to_cc(opts, state, 852250000, 5) == DSD_TRUNK_TUNE_RESULT_OK);
    assert(g_rtl_cqpsk_enable == 1);

    /* Explicit target C4FM overrides a globally forced CQPSK runtime mode. */
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->p25_trunk = 1;
    opts->trunk_scan_enabled = 1;
    state->rtl_ctx = (RtlSdrContext*)state;
    state->rf_mod = 0;
    state->p25_cc_is_tdma = 0;
    g_runtime_config_is_set = 1;
    g_runtime_config.cqpsk_is_set = 1;
    g_runtime_config.cqpsk_enable = 1;
    g_trunk_scan_active_p25_cqpsk_is_set = 1;
    g_trunk_scan_active_p25_cqpsk_enable = 0;
    g_rtl_tune_result = RTL_STREAM_TUNE_OK;
    g_rtl_cqpsk_enable = 1;
    g_rtl_symbol_rate_hz = 6000;
    g_rtl_symbol_levels = 4;
    g_rtl_channel_profile = RTL_STREAM_CHANNEL_PROFILE_P25_CQPSK;
    g_rtl_pending_active = 0;
    assert(dsd_engine_trunk_tune_to_cc(opts, state, 852500000, 5) == DSD_TRUNK_TUNE_RESULT_OK);
    assert(state->rf_mod == 0);
    assert(g_rtl_cqpsk_enable == 0);
    assert(g_rtl_symbol_rate_hz == 4800);
    assert(g_rtl_channel_profile == RTL_STREAM_CHANNEL_PROFILE_P25_C4FM);

    /* Target modulation overrides must also apply to P25P2 voice retunes.
     * A C4FM/auto CC target can still grant TDMA voice, which must switch the
     * RTL demod chain to CQPSK even when runtime config explicitly set CQPSK. */
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    DSD_MEMSET(&g_runtime_config, 0, sizeof(g_runtime_config));
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->p25_trunk = 1;
    opts->trunk_scan_enabled = 1;
    state->rtl_ctx = (RtlSdrContext*)state;
    state->rf_mod = 1;
    state->p25_p2_active_slot = 0;
    state->p25_vc_cqpsk_pref = -1;
    state->p25_vc_cqpsk_override = -1;
    g_runtime_config_is_set = 1;
    g_runtime_config.cqpsk_is_set = 1;
    g_runtime_config.cqpsk_enable = 1;
    g_trunk_scan_active_p25_cqpsk_is_set = 1;
    g_trunk_scan_active_p25_cqpsk_enable = 0;
    g_rtl_tune_result = RTL_STREAM_TUNE_OK;
    g_rtl_cqpsk_enable = 0;
    g_rtl_symbol_rate_hz = 4800;
    g_rtl_symbol_levels = 4;
    g_rtl_channel_profile = RTL_STREAM_CHANNEL_PROFILE_P25_C4FM;
    g_rtl_pending_active = 0;
    assert(dsd_engine_trunk_tune_to_freq(opts, state, 853000000, 8) == DSD_TRUNK_TUNE_RESULT_OK);
    assert(g_rtl_pending_active == 0);
    assert(g_rtl_cqpsk_enable == 1);
    assert(g_rtl_symbol_rate_hz == 6000);
    assert(g_rtl_channel_profile == RTL_STREAM_CHANNEL_PROFILE_P25_CQPSK);

    /* Explicit target CQPSK overrides a globally disabled CQPSK runtime mode. */
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    DSD_MEMSET(&g_runtime_config, 0, sizeof(g_runtime_config));
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->p25_trunk = 1;
    opts->trunk_scan_enabled = 1;
    state->rtl_ctx = (RtlSdrContext*)state;
    state->rf_mod = 0;
    state->p25_cc_is_tdma = 0;
    g_runtime_config_is_set = 1;
    g_runtime_config.cqpsk_is_set = 1;
    g_runtime_config.cqpsk_enable = 0;
    g_trunk_scan_active_p25_cqpsk_is_set = 1;
    g_trunk_scan_active_p25_cqpsk_enable = 1;
    g_rtl_tune_result = RTL_STREAM_TUNE_OK;
    g_rtl_cqpsk_enable = 0;
    g_rtl_symbol_rate_hz = 4800;
    g_rtl_symbol_levels = 4;
    g_rtl_channel_profile = RTL_STREAM_CHANNEL_PROFILE_P25_C4FM;
    g_rtl_pending_active = 0;
    assert(dsd_engine_trunk_tune_to_cc(opts, state, 852750000, 5) == DSD_TRUNK_TUNE_RESULT_OK);
    assert(state->rf_mod == 1);
    assert(g_rtl_cqpsk_enable == 1);
    assert(g_rtl_symbol_rate_hz == 4800);
    assert(g_rtl_channel_profile == RTL_STREAM_CHANNEL_PROFILE_P25_CQPSK);
    g_runtime_config_is_set = 0;
    g_trunk_scan_active_p25_cqpsk_is_set = 0;

    /* Simulcast P25P2 voice return to a P25P1 CQPSK control channel applies
     * the 4800 sps CQPSK profile only after the RTL retune succeeds. */
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->p25_trunk = 1;
    opts->trunk_enable = 1;
    opts->p25_is_tuned = 1;
    opts->trunk_is_tuned = 1;
    opts->mod_qpsk = 1;
    state->rtl_ctx = (RtlSdrContext*)state;
    state->p25_cc_freq = 851000000;
    state->trunk_cc_freq = 851000000;
    state->p25_cc_is_tdma = 0;
    state->p25_p2_active_slot = 0;
    state->rf_mod = 1;
    g_rtl_tune_result = RTL_STREAM_TUNE_OK;
    g_rtl_cqpsk_enable = 1;
    g_rtl_symbol_rate_hz = 6000;
    g_rtl_symbol_levels = 4;
    g_rtl_channel_profile = RTL_STREAM_CHANNEL_PROFILE_P25_CQPSK;
    g_rtl_ted_sps = 8;
    g_rtl_ted_sps_override = 8;
    g_rtl_pending_active = 0;
    g_frame_sync_reset_calls = 0;
    assert(dsd_engine_return_to_cc(opts, state) == DSD_TRUNK_TUNE_RESULT_OK);
    assert(opts->trunk_is_tuned == 0);
    assert(opts->p25_is_tuned == 0);
    assert(state->rf_mod == 1);
    assert(state->samplesPerSymbol == 10);
    assert(g_rtl_pending_active == 0);
    assert(g_rtl_cqpsk_enable == 1);
    assert(g_rtl_symbol_rate_hz == 4800);
    assert(g_rtl_channel_profile == RTL_STREAM_CHANNEL_PROFILE_P25_CQPSK);
    assert(g_rtl_ted_sps == 10);
    assert(g_rtl_ted_sps_override == 0);
    assert(g_frame_sync_reset_calls == 1);
#endif

    printf("ENGINE_TRUNK_RETUNE_REGRESSION: OK\n");
    free(state);
    free(opts);
    return 0;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
