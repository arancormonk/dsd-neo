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
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/dsp/frame_sync.h>
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
static int g_sps_hunt_restart_calls = 0;
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
static int g_rtl_tagged_tune_calls = 0;
static uint64_t g_rtl_last_request_id = 0U;
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
static int g_trunk_scan_active_gfsk_symbol_rate = 0;
static int g_trunk_scan_saved_autogain_is_set = 0;
static int g_trunk_scan_saved_autogain_on = 0;
static int g_trunk_scan_active_p25_cqpsk_is_set = 0;
static int g_trunk_scan_active_p25_cqpsk_enable = 0;
static int g_trunk_scan_active_p25_target = 0;
static int g_runtime_config_is_set = 0;

int
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_call_state_end_ex(dsd_state* state, uint8_t slot, double observed_m, dsd_call_end_reason reason) {
    (void)state;
    (void)slot;
    (void)observed_m;
    (void)reason;
    return 0;
}

int
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_call_state_end(dsd_state* state, uint8_t slot, double observed_m) {
    return dsd_call_state_end_ex(state, slot, observed_m, DSD_CALL_END_EXPLICIT);
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_event_sync_slot(dsd_opts* opts, dsd_state* state, uint8_t slot) {
    (void)opts;
    (void)state;
    (void)slot;
}

int
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_recent_activity_clear_all(dsd_state* state) {
    (void)state;
    return 0;
}

static int g_tune_generation_advance_calls = 0;
static uint64_t g_tune_request_next = 0U;
static uint64_t g_tune_request_pending = 0U;
static dsdneoRuntimeConfig g_runtime_config;

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
dmr_reset_blocks(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
dmr_enc_class_reset(dsd_state* state, uint8_t slot) {
    (void)state;
    (void)slot;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_frame_sync_reset_mod_state(void) {
    g_frame_sync_reset_calls++;
}

/* Mirrors the real helper in src/dsp/dsd_frame_sync.c so the assertions below read the
 * behavior rather than the stub. */
void
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_frame_sync_sps_hunt_restart_dwell(dsd_state* state) {
    g_sps_hunt_restart_calls++;
    if (!state) {
        return;
    }
    state->sps_hunt_counter = 0;
    state->sps_hunt_symbolcnt_mark = state->symbolcnt;
    state->sps_hunt_counter_at_entry = 0;
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
dsd_trunk_tuning_generation_advance(void) {
    g_tune_generation_advance_calls++;
}

uint64_t
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_trunk_tuning_request_begin(void) {
    g_tune_request_pending = ++g_tune_request_next;
    return g_tune_request_pending;
}

uint64_t
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_trunk_tuning_pending_request(void) {
    return g_tune_request_pending;
}

dsd_trunk_tune_result
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_trunk_tuning_request_status(uint64_t request_id, double* out_completed_m) {
    if (out_completed_m) {
        *out_completed_m = 0.0;
    }
    return request_id != 0U && request_id == g_tune_request_pending ? DSD_TRUNK_TUNE_RESULT_PENDING
                                                                    : DSD_TRUNK_TUNE_RESULT_FAILED;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_trunk_tuning_request_complete(uint64_t request_id, dsd_trunk_tune_result result) {
    if (request_id == 0U || request_id != g_tune_request_pending || result == DSD_TRUNK_TUNE_RESULT_PENDING) {
        return;
    }
    g_tune_request_pending = 0U;
    if (result == DSD_TRUNK_TUNE_RESULT_OK) {
        dsd_trunk_tuning_generation_advance();
    }
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_trunk_tuning_request_mark_ready(uint64_t request_id) {
    (void)request_id;
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
dsd_engine_trunk_scan_active_gfsk_symbol_rate(const dsd_state* state) {
    (void)state;
    return g_trunk_scan_active_gfsk_symbol_rate;
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

void*
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_engine_trunk_scan_active_p25_ctx(void) {
    static int p25_ctx_token;
    return g_trunk_scan_active_p25_target ? &p25_ctx_token : NULL;
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

int
rtl_stream_tune_tagged(RtlSdrContext* ctx, uint32_t center_freq_hz, uint64_t request_id) {
    g_rtl_tagged_tune_calls++;
    g_rtl_last_request_id = request_id;
    return rtl_stream_tune(ctx, center_freq_hz);
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

/* Model the queued profile request as an immediate apply (the real demod
 * thread consumes it between blocks with the same ordering). */
int
rtl_stream_request_demod_profile(int cqpsk_enable, int symbol_rate_hz, int levels, int channel_profile, int ted_sps,
                                 int ted_sps_is_override) {
    if (symbol_rate_hz > 0 && levels != 2 && levels != 4) {
        return -1;
    }
    if (ted_sps_is_override && ted_sps <= 0) {
        return -1;
    }
    if (cqpsk_enable >= 0) {
        rtl_stream_toggle_cqpsk(cqpsk_enable);
    }
    if (ted_sps >= 0) {
        rtl_stream_clear_ted_sps_override();
        if (ted_sps > 0) {
            if (ted_sps_is_override) {
                rtl_stream_set_ted_sps(ted_sps);
            } else {
                rtl_stream_set_ted_sps_no_override(ted_sps);
            }
        }
    }
    if (symbol_rate_hz > 0) {
        (void)rtl_stream_set_symbol_profile(symbol_rate_hz, levels, channel_profile);
    }
    return 0;
}

uint64_t
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_time_monotonic_ns(void) {
    return 1234500000000ULL;
}

void
dsd_neo_config_init(void) {}

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
    opts->trunk_is_tuned = 1;
    opts->audio_in_type = AUDIO_IN_PULSE; /* avoid RTL path in this regression */
    opts->use_rigctl = 1;
    opts->rigctl_sockfd = 1;

    state->trunk_cc_freq = 851000000;
    state->p25_cc_freq = 0;
    state->trunk_vc_freq[0] = 852000000;
    state->trunk_vc_freq[1] = 852000000;
    state->p25_p2_audio_allowed[0] = 1;
    state->p25_p2_audio_allowed[1] = 1;
    state->p25_crypto_state[0] = DSD_P25_CRYPTO_BLOCKED;
    state->p25_crypto_state[1] = DSD_P25_CRYPTO_BLOCKED;
    state->last_cc_sync_time = 0;
    state->last_cc_sync_time_m = 0.0;

    /* DMR/GFSK-ish demod settings should remain unchanged on DMR return. */
    state->samplesPerSymbol = 17;
    state->symbolCenter = 8;
    state->rf_mod = 2;

    g_setfreq_calls = 0;
    g_last_setfreq_hz = 0;

    dsd_engine_return_to_cc_request(opts, state, 0U);

    /* Core return semantics. */
    assert(opts->trunk_is_tuned == 0);
    assert(state->trunk_vc_freq[0] == 0);
    assert(state->trunk_vc_freq[1] == 0);
    assert(state->p25_p2_audio_allowed[0] == 0);
    assert(state->p25_p2_audio_allowed[1] == 0);
    assert(state->p25_crypto_state[0] == DSD_P25_CRYPTO_UNKNOWN);
    assert(state->p25_crypto_state[1] == DSD_P25_CRYPTO_UNKNOWN);

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

    /* NXDN trunking populates p25_cc_freq the same way P25 does, so a return to an NXDN control
     * channel must not be mistaken for a P25 one: rewriting rf_mod to C4FM/QPSK drops the GFSK
     * slicing an NXDN96 (or DMR-class) control channel needs. */
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    opts->trunk_enable = 1;
    opts->trunk_is_tuned = 1;
    opts->audio_in_type = AUDIO_IN_PULSE;
    opts->use_rigctl = 1;
    opts->rigctl_sockfd = 1;

    state->p25_cc_freq = 461000000;
    state->trunk_cc_freq = 461000000;
    state->p25_cc_is_tdma = 2; /* initState() sentinel: no P25 control channel seen */
    state->synctype = DSD_SYNC_NXDN_POS;
    state->lastsynctype = DSD_SYNC_NXDN_POS;
    state->samplesPerSymbol = 10;
    state->symbolCenter = 4;
    state->rf_mod = 2;
    state->sps_hunt_counter = 5;

    g_setfreq_calls = 0;
    g_last_setfreq_hz = 0;

    assert(dsd_engine_return_to_cc_request(opts, state, 0U) == DSD_TRUNK_TUNE_RESULT_OK);

    assert(g_setfreq_calls == 1);
    assert(g_last_setfreq_hz == 461000000);
    assert(state->rf_mod == 2);
    assert(state->samplesPerSymbol == 10);
    assert(state->symbolCenter == 4);
    assert(state->sps_hunt_counter == 5);

    /* EDACS also anchors p25_cc_freq and runs GFSK; same rule applies. */
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    opts->trunk_enable = 1;
    opts->trunk_is_tuned = 1;
    opts->audio_in_type = AUDIO_IN_PULSE;
    opts->use_rigctl = 1;
    opts->rigctl_sockfd = 1;

    state->p25_cc_freq = 856000000;
    state->trunk_cc_freq = 856000000;
    state->p25_cc_is_tdma = 2;
    state->synctype = DSD_SYNC_EDACS_POS;
    state->lastsynctype = DSD_SYNC_EDACS_POS;
    state->samplesPerSymbol = 5;
    state->symbolCenter = 2;
    state->rf_mod = 2;

    g_setfreq_calls = 0;
    assert(dsd_engine_return_to_cc_request(opts, state, 0U) == DSD_TRUNK_TUNE_RESULT_OK);
    assert(g_setfreq_calls == 1);
    assert(state->rf_mod == 2);
    assert(state->samplesPerSymbol == 5);
    assert(state->symbolCenter == 2);

    /* A parked non-P25 trunk-scan target that has not synced yet reads as P25 by synctype alone
     * (DSD_SYNC_P25P1_POS is 0), so the coordinator's own target type is the authority. */
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    opts->trunk_enable = 1;
    opts->trunk_is_tuned = 1;
    opts->trunk_scan_enabled = 1;
    opts->audio_in_type = AUDIO_IN_PULSE;
    opts->use_rigctl = 1;
    opts->rigctl_sockfd = 1;

    state->p25_cc_freq = 462000000;
    state->trunk_cc_freq = 462000000;
    state->p25_cc_is_tdma = 2;
    state->samplesPerSymbol = 10;
    state->symbolCenter = 4;
    state->rf_mod = 0; /* global -mc lock with an empty target modulation column */
    state->sps_hunt_counter = 5;
    g_trunk_scan_target_count = 2;
    g_trunk_scan_active_p25_target = 0;

    g_setfreq_calls = 0;
    assert(dsd_engine_return_to_cc_request(opts, state, 0U) == DSD_TRUNK_TUNE_RESULT_OK);
    assert(g_setfreq_calls == 1);
    assert(state->sps_hunt_counter == 5);
    g_trunk_scan_target_count = 0;

    /* A fixed input without rigctl has no tuner backend and must not fabricate
     * successful voice, control-channel, or scan retunes. */
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    opts->audio_in_type = AUDIO_IN_PULSE;
    opts->trunk_enable = 1;
    state->trunk_cc_freq = 851000000;
    g_frame_sync_reset_calls = 0;
    g_tune_generation_advance_calls = 0;
    assert(dsd_engine_trunk_tune_to_freq_request(opts, state, 853000000, 0, 0U) == DSD_TRUNK_TUNE_RESULT_FAILED);
    assert(opts->trunk_is_tuned == 0);
    assert(state->trunk_vc_freq[0] == 0);
    assert(dsd_engine_trunk_tune_to_cc_request(opts, state, 852000000, 0, 0U) == DSD_TRUNK_TUNE_RESULT_FAILED);
    assert(state->trunk_cc_freq == 851000000);
    assert(dsd_engine_scan_tune_to_freq(opts, state, 854000000, 0, NULL) == DSD_TRUNK_TUNE_RESULT_FAILED);
    assert(g_frame_sync_reset_calls == 0);
    assert(g_tune_generation_advance_calls == 0);

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
    assert(dsd_engine_trunk_tune_to_freq_request(opts, state, 853000000, 0, 0U) == DSD_TRUNK_TUNE_RESULT_OK);
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
    assert(dsd_engine_trunk_tune_to_freq_request(opts, state, 853500000, 0, 0U) == DSD_TRUNK_TUNE_RESULT_OK);
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
    opts->trunk_enable = 1;
    opts->trunk_is_tuned = 1;
    state->p25_cc_freq = 851000000;
    state->trunk_cc_freq = 851000000;
    state->p25_cc_is_tdma = 1;
    state->samplesPerSymbol = 8;
    state->symbolCenter = 3;
    g_setfreq_calls = 0;
    g_last_setfreq_hz = 0;
    g_setfreq_result = true;
    assert(dsd_engine_return_to_cc_request(opts, state, 0U) == DSD_TRUNK_TUNE_RESULT_OK);
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
    opts->trunk_enable = 1;
    g_frame_sync_reset_calls = 0;
    g_p25p2_frame_reset_calls = 0;
    g_setfreq_result = true;
    assert(dsd_engine_trunk_tune_to_freq_request(opts, state, 853600000, 16, 0U) == DSD_TRUNK_TUNE_RESULT_OK);
    assert(g_frame_sync_reset_calls == 1);
    assert(g_p25p2_frame_reset_calls == 1);

    /* NXDN trunking carries no P25 TED timing and must preserve the active
     * NXDN48 profile. */
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    opts->audio_in_type = AUDIO_IN_PULSE;
    opts->use_rigctl = 1;
    opts->trunk_enable = 1;
    opts->frame_nxdn48 = 1;
    state->p25_p2_active_slot = -1;
    state->sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_2400_4;
    state->sps_hunt_counter = 17;
    g_setfreq_result = true;
    assert(dsd_engine_trunk_tune_to_cc_request(opts, state, 451000000, 0, 0U) == DSD_TRUNK_TUNE_RESULT_OK);
    assert(state->sps_hunt_idx == DSD_FRAME_SYNC_SPS_PROFILE_2400_4);
    assert(state->sps_hunt_counter == 17);

    /* The voice-channel tune keeps the NXDN48 profile but starts its dwell over: the grant
     * put the decoder here, so the control channel's spend is not this channel's to inherit
     * (#392). The profile index is the invariant this case is about, and it is untouched. */
    state->sps_hunt_counter = 23;
    state->symbolcnt = 7000U;
    state->sps_hunt_symbolcnt_mark = 1234U;
    state->sps_hunt_counter_at_entry = 23;
    g_sps_hunt_restart_calls = 0;
    assert(dsd_engine_trunk_tune_to_freq_request(opts, state, 451500000, 0, 0U) == DSD_TRUNK_TUNE_RESULT_OK);
    assert(state->sps_hunt_idx == DSD_FRAME_SYNC_SPS_PROFILE_2400_4);
    assert(g_sps_hunt_restart_calls == 1);
    assert(state->sps_hunt_counter == 0);
    /* The anchor moves with the budget, so the fresh dwell is not immediately credited for
     * symbols spent before the tune (#394). */
    assert(state->sps_hunt_symbolcnt_mark == 7000U);
    assert(state->sps_hunt_counter_at_entry == 0);

    /* Direct conventional scan retunes publish a new generation only after
     * the backend completes the target. */
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    opts->audio_in_type = AUDIO_IN_PULSE;
    opts->use_rigctl = 1;
    g_tune_generation_advance_calls = 0;
    g_setfreq_result = true;
    assert(dsd_engine_scan_tune_to_freq(opts, state, 853700000, 0, NULL) == DSD_TRUNK_TUNE_RESULT_OK);
    assert(g_tune_generation_advance_calls == 1);
    g_setfreq_result = false;
    assert(dsd_engine_scan_tune_to_freq(opts, state, 853800000, 0, NULL) == DSD_TRUNK_TUNE_RESULT_FAILED);
    assert(g_tune_generation_advance_calls == 1);

#ifdef USE_RADIO
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    opts->audio_in_type = AUDIO_IN_RTL;
    state->rtl_ctx = (RtlSdrContext*)state;
    g_rtl_tune_result = RTL_STREAM_TUNE_TIMEOUT;
    g_rtl_tagged_tune_calls = 0;
    g_rtl_last_request_id = 0U;
    uint64_t scan_request_id = 0U;
    assert(dsd_engine_scan_tune_to_freq(opts, state, 853900000, 0, &scan_request_id) == DSD_TRUNK_TUNE_RESULT_PENDING);
    assert(g_tune_generation_advance_calls == 1);
    assert(scan_request_id != 0U && g_tune_request_pending == scan_request_id);
    assert(g_rtl_tagged_tune_calls == 1);
    assert(g_rtl_last_request_id == scan_request_id);
    dsd_trunk_tuning_request_complete(scan_request_id, DSD_TRUNK_TUNE_RESULT_FAILED);
    assert(g_tune_request_pending == 0U);
    g_rtl_tune_result = RTL_STREAM_TUNE_OK;

    /* RTL audio retuned by rigctl has no native RTL controller boundary, so the
     * queued P25 VC demod profile must be applied after SetFreq succeeds. */
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->use_rigctl = 1;
    opts->trunk_enable = 1;
    state->rtl_ctx = (RtlSdrContext*)state;
    state->rf_mod = 1;
    state->p25_p2_active_slot = 0;
    state->p25_vc_cqpsk_override = 1;
    state->sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_2;
    state->sps_hunt_counter = 11;
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
    assert(dsd_engine_trunk_tune_to_freq_request(opts, state, 853750000, 8, 0U) == DSD_TRUNK_TUNE_RESULT_OK);
    assert(g_setfreq_calls == 1);
    assert(g_last_setfreq_hz == 853750000);
    assert(g_rtl_tune_calls == 0);
    assert(g_rtl_pending_active == 0);
    assert(g_rtl_cqpsk_enable == 1);
    assert(g_rtl_symbol_rate_hz == 6000);
    assert(g_rtl_channel_profile == RTL_STREAM_CHANNEL_PROFILE_P25_CQPSK);
    assert(g_rtl_ted_sps == 8);
    assert(g_rtl_ted_sps_override == 8);
    assert(state->sps_hunt_idx == DSD_FRAME_SYNC_SPS_PROFILE_6000_4);
    assert(state->sps_hunt_counter == 0);
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
    assert(dsd_engine_trunk_tune_to_freq_request(opts, state, 854000000, 0, 0U) == DSD_TRUNK_TUNE_RESULT_FAILED);
    assert(opts->trunk_is_tuned == 0);
    assert(state->trunk_vc_freq[0] == 0);
    assert(g_frame_sync_reset_calls == 0);

#ifdef USE_RADIO
    /* Native RTL stream retunes keep relying on the RTL tune API for
     * hardware-side drain/clear behavior instead of also draining here. */
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    opts->audio_in_type = AUDIO_IN_RTL;
    state->rtl_ctx = (RtlSdrContext*)state;
    g_rtl_tune_result = RTL_STREAM_TUNE_OK;
    g_drain_audio_calls = 0;
    g_rtl_tune_calls = 0;
    assert(dsd_engine_trunk_tune_to_freq_request(opts, state, 854500000, 0, 0U) == DSD_TRUNK_TUNE_RESULT_OK);
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
    assert(dsd_engine_trunk_tune_to_cc_request(opts, state, 452000000, 10, 0U) == DSD_TRUNK_TUNE_RESULT_OK);
    assert(state->trunk_cc_freq == 452000000);
    assert(g_rtl_pending_active == 0);
    assert(g_rtl_cqpsk_enable == 0);
    assert(g_rtl_symbol_rate_hz == 4800);
    assert(g_rtl_symbol_levels == 4);
    assert(g_rtl_channel_profile == RTL_STREAM_CHANNEL_PROFILE_12K5);
    assert(g_rtl_ted_sps == 10);
    assert(g_rtl_ted_sps_override == 0);

    /* A parked nxdn48-conventional scan target runs 2400 sym/s in a 6.25 kHz channel. rf_mod == 2
     * is true for every GFSK-family target, so only the coordinator's own answer separates it from
     * the 4800 sym/s DMR/NXDN96 case above -- and a wrong filter here never self-corrects, because
     * a pinned SPS hunt stops re-applying the demod profile. */
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->trunk_scan_enabled = 1;
    state->rtl_ctx = (RtlSdrContext*)state;
    state->rf_mod = 2;
    g_trunk_scan_target_count = 2;
    g_trunk_scan_active_gfsk_symbol_rate = 2400;
    g_rtl_tune_result = RTL_STREAM_TUNE_OK;
    g_rtl_cqpsk_enable = 1;
    g_rtl_symbol_rate_hz = 4800;
    g_rtl_symbol_levels = 4;
    g_rtl_channel_profile = RTL_STREAM_CHANNEL_PROFILE_12K5;
    g_rtl_ted_sps = 10;
    g_rtl_ted_sps_override = 10;
    g_rtl_pending_active = 0;
    assert(dsd_engine_scan_tune_to_freq(opts, state, 461556250, 20, NULL) == DSD_TRUNK_TUNE_RESULT_OK);
    assert(g_rtl_pending_active == 0);
    assert(g_rtl_cqpsk_enable == 0);
    assert(g_rtl_symbol_rate_hz == 2400);
    assert(g_rtl_symbol_levels == 4);
    assert(g_rtl_channel_profile == RTL_STREAM_CHANNEL_PROFILE_6K25);
    assert(g_rtl_ted_sps == 20); /* 48000 / 2400 from the stubbed RTL output rate */
    assert(g_rtl_ted_sps_override == 0);

    /* Even stale scanner flags cannot change the trunk coordinator's backend
     * contract: rigctl owns the frequency and the target owns the profile. */
    opts->scanner_mode = 1;
    opts->use_rigctl = 1;
    g_setfreq_result = true;
    const int before_stale_scanner = g_rtl_tune_calls;
    assert(dsd_engine_scan_tune_to_freq(opts, state, 461556250, 20, NULL) == DSD_TRUNK_TUNE_RESULT_OK);
    assert(g_rtl_tune_calls == before_stale_scanner);
    assert(g_rtl_symbol_rate_hz == 2400 && g_rtl_channel_profile == RTL_STREAM_CHANNEL_PROFILE_6K25);
    opts->scanner_mode = 0;
    opts->use_rigctl = 0;

    /* A parked 4800-class scan target keeps the 12.5 kHz chain. */
    g_trunk_scan_active_gfsk_symbol_rate = 4800;
    g_rtl_symbol_rate_hz = 2400;
    g_rtl_channel_profile = RTL_STREAM_CHANNEL_PROFILE_6K25;
    g_rtl_ted_sps = 20;
    g_rtl_pending_active = 0;
    assert(dsd_engine_scan_tune_to_freq(opts, state, 461112500, 10, NULL) == DSD_TRUNK_TUNE_RESULT_OK);
    assert(g_rtl_symbol_rate_hz == 4800);
    assert(g_rtl_channel_profile == RTL_STREAM_CHANNEL_PROFILE_12K5);
    assert(g_rtl_ted_sps == 10);

    /* Outside trunk scan the coordinator answers 0 and the rf_mod == 2 gate still picks 4800. */
    g_trunk_scan_target_count = 0;
    g_trunk_scan_active_gfsk_symbol_rate = 0;
    g_rtl_symbol_rate_hz = 2400;
    g_rtl_channel_profile = RTL_STREAM_CHANNEL_PROFILE_6K25;
    g_rtl_ted_sps = 20;
    g_rtl_pending_active = 0;
    assert(dsd_engine_scan_tune_to_freq(opts, state, 461112500, 10, NULL) == DSD_TRUNK_TUNE_RESULT_OK);
    assert(g_rtl_symbol_rate_hz == 4800);
    assert(g_rtl_channel_profile == RTL_STREAM_CHANNEL_PROFILE_12K5);
    assert(g_rtl_ted_sps == 10);
    rtl_stream_clear_pending_retune_profile();

    /* A global -mc lock with an empty per-target modulation column leaves rf_mod at the locked C4FM
     * value, so the coordinator's answer is the only thing that can pick the chain for a parked
     * DMR/NXDN96 target. Without it the retune re-queues whatever the front end already had -- after
     * an nxdn48-conventional dwell, a 2400 sym/s 6.25 kHz chain on a 12.5 kHz channel -- and the
     * locked SPS hunt never re-applies over it, because it only rotates among equal-timing profiles. */
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->trunk_scan_enabled = 1;
    state->rtl_ctx = (RtlSdrContext*)state;
    state->rf_mod = 0;
    g_trunk_scan_target_count = 2;
    g_trunk_scan_active_p25_target = 0;
    g_trunk_scan_active_gfsk_symbol_rate = 4800;
    g_rtl_tune_result = RTL_STREAM_TUNE_OK;
    g_rtl_cqpsk_enable = 0;
    g_rtl_symbol_rate_hz = 2400;
    g_rtl_symbol_levels = 4;
    g_rtl_channel_profile = RTL_STREAM_CHANNEL_PROFILE_6K25;
    g_rtl_ted_sps = 20;
    g_rtl_ted_sps_override = 0;
    g_rtl_pending_active = 0;
    assert(dsd_engine_scan_tune_to_freq(opts, state, 461112500, 10, NULL) == DSD_TRUNK_TUNE_RESULT_OK);
    assert(g_rtl_cqpsk_enable == 0);
    assert(g_rtl_symbol_rate_hz == 4800);
    assert(g_rtl_symbol_levels == 4);
    assert(g_rtl_channel_profile == RTL_STREAM_CHANNEL_PROFILE_12K5);
    assert(g_rtl_ted_sps == 10);
    assert(g_rtl_ted_sps_override == 0);
    /* The chain follows the parked target; the lock keeps owning symbol slicing. */
    assert(state->rf_mod == 0);
    rtl_stream_clear_pending_retune_profile();

    /* Same on the trunk-CC path under -mq: a parked GFSK target must not inherit the previous P25
     * target's CQPSK demod and C4FM-family filter. */
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->trunk_enable = 1;
    opts->trunk_scan_enabled = 1;
    state->rtl_ctx = (RtlSdrContext*)state;
    state->rf_mod = 1;
    g_trunk_scan_target_count = 2;
    g_trunk_scan_active_p25_target = 0;
    g_trunk_scan_active_gfsk_symbol_rate = 4800;
    g_rtl_tune_result = RTL_STREAM_TUNE_OK;
    g_rtl_cqpsk_enable = 1;
    g_rtl_symbol_rate_hz = 4800;
    g_rtl_symbol_levels = 4;
    g_rtl_channel_profile = RTL_STREAM_CHANNEL_PROFILE_P25_CQPSK;
    g_rtl_ted_sps = 10;
    g_rtl_ted_sps_override = 0;
    g_rtl_pending_active = 0;
    assert(dsd_engine_trunk_tune_to_cc_request(opts, state, 452000000, 10, 0U) == DSD_TRUNK_TUNE_RESULT_OK);
    assert(g_rtl_cqpsk_enable == 0);
    assert(g_rtl_symbol_rate_hz == 4800);
    assert(g_rtl_symbol_levels == 4);
    assert(g_rtl_channel_profile == RTL_STREAM_CHANNEL_PROFILE_12K5);
    assert(g_rtl_ted_sps == 10);
    assert(state->rf_mod == 1);
    rtl_stream_clear_pending_retune_profile();

    /* Outside trunk scan the fall-through still owns a locked non-GFSK session: the coordinator
     * answers 0, rf_mod != 2, and the retune re-queues the front end's current chain unchanged. */
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    opts->audio_in_type = AUDIO_IN_RTL;
    state->rtl_ctx = (RtlSdrContext*)state;
    state->rf_mod = 0;
    g_trunk_scan_target_count = 0;
    g_trunk_scan_active_p25_target = 0;
    g_trunk_scan_active_gfsk_symbol_rate = 0;
    g_rtl_tune_result = RTL_STREAM_TUNE_OK;
    g_rtl_cqpsk_enable = 0;
    g_rtl_symbol_rate_hz = 4800;
    g_rtl_symbol_levels = 4;
    g_rtl_channel_profile = RTL_STREAM_CHANNEL_PROFILE_P25_C4FM;
    g_rtl_ted_sps = 8;
    g_rtl_ted_sps_override = 0;
    g_rtl_pending_active = 0;
    assert(dsd_engine_scan_tune_to_freq(opts, state, 461112500, 10, NULL) == DSD_TRUNK_TUNE_RESULT_OK);
    assert(g_rtl_cqpsk_enable == 0);
    assert(g_rtl_symbol_rate_hz == 4800);
    assert(g_rtl_symbol_levels == 4);
    assert(g_rtl_channel_profile == RTL_STREAM_CHANNEL_PROFILE_P25_C4FM);
    assert(g_rtl_ted_sps == 10);
    rtl_stream_clear_pending_retune_profile();

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
    assert(dsd_engine_trunk_tune_to_cc_request(opts, state, 453000000, 10, UINT64_C(0x10))
           == DSD_TRUNK_TUNE_RESULT_PENDING);
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
    assert(dsd_engine_trunk_tune_to_cc_request(opts, state, 453500000, 10, UINT64_C(0x11))
           == DSD_TRUNK_TUNE_RESULT_PENDING);
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
    opts->trunk_enable = 1;
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
    assert(dsd_engine_trunk_tune_to_freq_request(opts, state, 855000000, 4, 0U) == DSD_TRUNK_TUNE_RESULT_DEFERRED);
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

    /* Accepted RTL timeouts keep active demod settings unchanged until the
     * controller reaches the retune boundary, while preserving the requested
     * profile for that queued hardware request. */
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->trunk_enable = 1;
    state->rtl_ctx = (RtlSdrContext*)state;
    state->rf_mod = 1;
    state->p25_p2_active_slot = 0;
    state->p25_vc_cqpsk_override = 1;
    state->sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_2;
    state->sps_hunt_counter = 11;
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
    const uint64_t voice_request_id = UINT64_C(0x1122334455667788);
    g_rtl_tagged_tune_calls = 0;
    g_rtl_last_request_id = 0U;
    assert(dsd_engine_trunk_tune_to_freq_request(opts, state, 855000000, 8, voice_request_id)
           == DSD_TRUNK_TUNE_RESULT_PENDING);
    assert(g_rtl_tagged_tune_calls == 1);
    assert(g_rtl_last_request_id == voice_request_id);
    assert(opts->trunk_is_tuned == 1);
    assert(state->trunk_vc_freq[0] == 855000000);
    assert(state->p25_vc_cqpsk_override == -1);
    assert(state->sps_hunt_idx == DSD_FRAME_SYNC_SPS_PROFILE_6000_4);
    assert(state->sps_hunt_counter == 0);
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

    /* Accepted RTL CC timeouts likewise leave the active demod settings alone
     * until the controller applies the queued control-channel profile. */
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->trunk_enable = 1;
    state->rtl_ctx = (RtlSdrContext*)state;
    state->rf_mod = 0;
    state->p25_cc_is_tdma = 1;
    state->trunk_cc_freq = 851000000;
    state->sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_2;
    state->sps_hunt_counter = 13;
    g_rtl_tune_result = RTL_STREAM_TUNE_TIMEOUT;
    g_rtl_cqpsk_enable = 0;
    g_rtl_symbol_rate_hz = 4800;
    g_rtl_symbol_levels = 4;
    g_rtl_channel_profile = RTL_STREAM_CHANNEL_PROFILE_P25_C4FM;
    g_rtl_ted_sps = 5;
    g_rtl_ted_sps_override = 0;
    g_rtl_pending_active = 0;
    g_frame_sync_reset_calls = 0;
    const uint64_t cc_request_id = UINT64_C(0x8877665544332211);
    g_rtl_tagged_tune_calls = 0;
    g_rtl_last_request_id = 0U;
    assert(dsd_engine_trunk_tune_to_cc_request(opts, state, 852000000, 4, cc_request_id)
           == DSD_TRUNK_TUNE_RESULT_PENDING);
    assert(g_rtl_tagged_tune_calls == 1);
    assert(g_rtl_last_request_id == cc_request_id);
    assert(state->rf_mod == 1);
    assert(state->trunk_cc_freq == 852000000);
    assert(state->sps_hunt_idx == DSD_FRAME_SYNC_SPS_PROFILE_6000_4);
    assert(state->sps_hunt_counter == 0);
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
    opts->trunk_enable = 1;
    opts->trunk_scan_enabled = 1;
    state->rtl_ctx = (RtlSdrContext*)state;
    state->rf_mod = 0;
    state->synctype = DSD_SYNC_DMR_BS_DATA_POS;
    state->lastsynctype = DSD_SYNC_DMR_BS_VOICE_POS;
    state->p25_cc_is_tdma = 0;
    g_runtime_config_is_set = 1;
    g_runtime_config.cqpsk_is_set = 1;
    g_runtime_config.cqpsk_enable = 1;
    g_trunk_scan_active_p25_target = 1;
    g_trunk_scan_active_p25_cqpsk_is_set = 0;
    g_rtl_tune_result = RTL_STREAM_TUNE_OK;
    g_rtl_cqpsk_enable = 1;
    g_rtl_symbol_rate_hz = 6000;
    g_rtl_symbol_levels = 4;
    g_rtl_channel_profile = RTL_STREAM_CHANNEL_PROFILE_P25_CQPSK;
    g_rtl_pending_active = 0;
    assert(dsd_engine_trunk_tune_to_cc_request(opts, state, 852250000, 5, 0U) == DSD_TRUNK_TUNE_RESULT_OK);
    assert(g_rtl_cqpsk_enable == 1);

    /* Explicit target C4FM overrides a globally forced CQPSK runtime mode. */
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->trunk_enable = 1;
    opts->trunk_scan_enabled = 1;
    state->rtl_ctx = (RtlSdrContext*)state;
    state->rf_mod = 0;
    state->synctype = DSD_SYNC_DMR_BS_DATA_POS;
    state->lastsynctype = DSD_SYNC_DMR_BS_VOICE_POS;
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
    assert(dsd_engine_trunk_tune_to_cc_request(opts, state, 852500000, 5, 0U) == DSD_TRUNK_TUNE_RESULT_OK);
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
    opts->trunk_enable = 1;
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
    assert(dsd_engine_trunk_tune_to_freq_request(opts, state, 853000000, 8, 0U) == DSD_TRUNK_TUNE_RESULT_OK);
    assert(g_rtl_pending_active == 0);
    assert(g_rtl_cqpsk_enable == 1);
    assert(g_rtl_symbol_rate_hz == 6000);
    assert(g_rtl_channel_profile == RTL_STREAM_CHANNEL_PROFILE_P25_CQPSK);

    /* Explicit target CQPSK overrides a globally disabled CQPSK runtime mode. */
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    DSD_MEMSET(&g_runtime_config, 0, sizeof(g_runtime_config));
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->trunk_enable = 1;
    opts->trunk_scan_enabled = 1;
    state->rtl_ctx = (RtlSdrContext*)state;
    state->rf_mod = 0;
    state->synctype = DSD_SYNC_DMR_BS_DATA_POS;
    state->lastsynctype = DSD_SYNC_DMR_BS_VOICE_POS;
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
    assert(dsd_engine_trunk_tune_to_cc_request(opts, state, 852750000, 5, 0U) == DSD_TRUNK_TUNE_RESULT_OK);
    assert(state->rf_mod == 1);
    assert(g_rtl_cqpsk_enable == 1);
    assert(g_rtl_symbol_rate_hz == 4800);
    assert(g_rtl_channel_profile == RTL_STREAM_CHANNEL_PROFILE_P25_CQPSK);
    g_runtime_config_is_set = 0;
    g_trunk_scan_active_p25_target = 0;
    g_trunk_scan_active_p25_cqpsk_is_set = 0;

    /* Simulcast P25P2 voice return to a P25P1 CQPSK control channel applies
     * the 4800 sps CQPSK profile only after the RTL retune succeeds. */
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->trunk_enable = 1;
    opts->trunk_is_tuned = 1;
    opts->mod_qpsk = 1;
    state->rtl_ctx = (RtlSdrContext*)state;
    state->p25_cc_freq = 851000000;
    state->trunk_cc_freq = 851000000;
    state->p25_cc_is_tdma = 0;
    state->p25_p2_active_slot = 0;
    state->rf_mod = 1;
    state->sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_2;
    state->sps_hunt_counter = 19;
    g_rtl_tune_result = RTL_STREAM_TUNE_OK;
    g_rtl_cqpsk_enable = 1;
    g_rtl_symbol_rate_hz = 6000;
    g_rtl_symbol_levels = 4;
    g_rtl_channel_profile = RTL_STREAM_CHANNEL_PROFILE_P25_CQPSK;
    g_rtl_ted_sps = 8;
    g_rtl_ted_sps_override = 8;
    g_rtl_pending_active = 0;
    g_frame_sync_reset_calls = 0;
    assert(dsd_engine_return_to_cc_request(opts, state, 0U) == DSD_TRUNK_TUNE_RESULT_OK);
    assert(opts->trunk_is_tuned == 0);
    assert(state->rf_mod == 1);
    assert(state->samplesPerSymbol == 10);
    assert(state->sps_hunt_idx == DSD_FRAME_SYNC_SPS_PROFILE_4800_4);
    assert(state->sps_hunt_counter == 0);
    assert(g_rtl_pending_active == 0);
    assert(g_rtl_cqpsk_enable == 1);
    assert(g_rtl_symbol_rate_hz == 4800);
    assert(g_rtl_channel_profile == RTL_STREAM_CHANNEL_PROFILE_P25_CQPSK);
    assert(g_rtl_ted_sps == 10);
    assert(g_rtl_ted_sps_override == 0);
    assert(g_frame_sync_reset_calls == 1);

    /* Issue #423: under AUTO an FDMA control channel was pinned to C4FM on every tune, so a
     * P25p1 LSM site could never keep the CQPSK chain it had just decoded on. Once a NID has
     * validated through that chain the return-to-CC restores it without -mq. */
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->trunk_enable = 1;
    opts->trunk_is_tuned = 1;
    opts->mod_qpsk = 0;
    state->rtl_ctx = (RtlSdrContext*)state;
    state->p25_cc_freq = 851000000;
    state->trunk_cc_freq = 851000000;
    state->p25_cc_is_tdma = 0;
    state->p25_p2_active_slot = 0;
    state->p25_p1_validated_rf_mod = 1;
    state->rf_mod = 1;
    state->sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_2;
    g_rtl_tune_result = RTL_STREAM_TUNE_OK;
    g_rtl_cqpsk_enable = 1;
    g_rtl_symbol_rate_hz = 6000;
    g_rtl_symbol_levels = 4;
    g_rtl_channel_profile = RTL_STREAM_CHANNEL_PROFILE_P25_CQPSK;
    g_rtl_ted_sps = 8;
    g_rtl_ted_sps_override = 8;
    g_rtl_pending_active = 0;
    assert(dsd_engine_return_to_cc_request(opts, state, 0U) == DSD_TRUNK_TUNE_RESULT_OK);
    assert(state->rf_mod == 1);
    assert(state->samplesPerSymbol == 10);
    assert(state->sps_hunt_idx == DSD_FRAME_SYNC_SPS_PROFILE_4800_4);
    assert(g_rtl_cqpsk_enable == 1);
    assert(g_rtl_symbol_rate_hz == 4800);
    assert(g_rtl_channel_profile == RTL_STREAM_CHANNEL_PROFILE_P25_CQPSK);

    /* The counter-case that rules out simply preserving rf_mod: a C4FM control channel with
     * P25p2 TDMA voice channels leaves rf_mod at 1 when the grant ends, and the control channel
     * must still come back on C4FM. Nothing validated a P25p1 NID through CQPSK here. */
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->trunk_enable = 1;
    opts->trunk_is_tuned = 1;
    opts->mod_qpsk = 0;
    state->rtl_ctx = (RtlSdrContext*)state;
    state->p25_cc_freq = 851000000;
    state->trunk_cc_freq = 851000000;
    state->p25_cc_is_tdma = 0;
    state->p25_p2_active_slot = 0;
    state->p25_p1_validated_rf_mod = -1;
    state->rf_mod = 1;
    state->sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_2;
    g_rtl_tune_result = RTL_STREAM_TUNE_OK;
    g_rtl_cqpsk_enable = 1;
    g_rtl_symbol_rate_hz = 6000;
    g_rtl_channel_profile = RTL_STREAM_CHANNEL_PROFILE_P25_CQPSK;
    g_rtl_pending_active = 0;
    assert(dsd_engine_return_to_cc_request(opts, state, 0U) == DSD_TRUNK_TUNE_RESULT_OK);
    assert(state->rf_mod == 0);
    assert(g_rtl_cqpsk_enable == 0);
    assert(g_rtl_channel_profile == RTL_STREAM_CHANNEL_PROFILE_P25_C4FM);

    /* An explicit CLI modulation lock still owns the decision. */
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->trunk_enable = 1;
    opts->trunk_is_tuned = 1;
    opts->mod_qpsk = 0;
    opts->mod_cli_lock = 1;
    state->rtl_ctx = (RtlSdrContext*)state;
    state->p25_cc_freq = 851000000;
    state->trunk_cc_freq = 851000000;
    state->p25_cc_is_tdma = 0;
    state->p25_p1_validated_rf_mod = 1;
    state->rf_mod = 1;
    state->sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_2;
    g_rtl_tune_result = RTL_STREAM_TUNE_OK;
    g_rtl_cqpsk_enable = 1;
    g_rtl_channel_profile = RTL_STREAM_CHANNEL_PROFILE_P25_CQPSK;
    g_rtl_pending_active = 0;
    assert(dsd_engine_return_to_cc_request(opts, state, 0U) == DSD_TRUNK_TUNE_RESULT_OK);
    assert(state->rf_mod == 0);
    assert(g_rtl_channel_profile == RTL_STREAM_CHANNEL_PROFILE_P25_C4FM);

    /* The CC-tune path stages the same decision, so a CC hunt lands on the learned chain. */
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->trunk_enable = 1;
    state->rtl_ctx = (RtlSdrContext*)state;
    state->p25_cc_is_tdma = 0;
    state->p25_p1_validated_rf_mod = 1;
    state->lastsynctype = 0;
    g_rtl_tune_result = RTL_STREAM_TUNE_OK;
    g_rtl_pending_active = 0;
    g_rtl_pending_cqpsk = -1;
    g_rtl_pending_channel_profile = -1;
    assert(dsd_engine_trunk_tune_to_cc_request(opts, state, 852000000, 10, 0U) == DSD_TRUNK_TUNE_RESULT_OK);
    assert(state->rf_mod == 1);
    assert(g_rtl_cqpsk_enable == 1);
    assert(g_rtl_symbol_rate_hz == 4800);
    assert(g_rtl_channel_profile == RTL_STREAM_CHANNEL_PROFILE_P25_CQPSK);
    rtl_stream_clear_pending_retune_profile();

    /* A P25p1 FDMA voice grant carries no modulation of its own -- it inherits the control
     * channel's -- so on an LSM site the voice channel reaches CQPSK too. */
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->trunk_enable = 1;
    state->rtl_ctx = (RtlSdrContext*)state;
    state->p25_cc_is_tdma = 0;
    state->p25_p1_validated_rf_mod = 1;
    state->p25_p2_active_slot = -1;
    state->rf_mod = 1;
    state->lastsynctype = 0;
    g_rtl_tune_result = RTL_STREAM_TUNE_OK;
    g_rtl_pending_active = 0;
    g_rtl_pending_cqpsk = -1;
    g_rtl_pending_channel_profile = -1;
    assert(dsd_engine_trunk_tune_to_freq_request(opts, state, 853000000, 10, 0U) == DSD_TRUNK_TUNE_RESULT_OK);
    assert(g_rtl_cqpsk_enable == 1);
    assert(g_rtl_symbol_rate_hz == 4800);
    assert(g_rtl_channel_profile == RTL_STREAM_CHANNEL_PROFILE_P25_CQPSK);
    rtl_stream_clear_pending_retune_profile();
    /* Manual CC selection may interrupt P2 voice. CC type, including the
     * unknown/4800 fallback, must win over the active voice slot and rate. */
    for (int cc_type = -1; cc_type <= 1; cc_type++) {
        DSD_MEMSET(opts, 0, sizeof(*opts));
        DSD_MEMSET(state, 0, sizeof(*state));
        opts->audio_in_type = AUDIO_IN_RTL;
        opts->trunk_enable = 1;
        opts->trunk_is_tuned = 1;
        opts->frame_p25p1 = opts->frame_p25p2 = opts->frame_dmr = 1;
        state->rtl_ctx = (RtlSdrContext*)state;
        state->p25_cc_is_tdma = cc_type;
        state->p25_p2_active_slot = 1;
        state->synctype = state->lastsynctype = DSD_SYNC_P25P2_POS;
        state->rf_mod = 1;
        state->p25_p1_validated_rf_mod = -1;
        g_rtl_tune_result = RTL_STREAM_TUNE_OK;
        g_rtl_symbol_rate_hz = 6000;
        g_rtl_ted_sps = 8;
        g_rtl_pending_active = 0;
        const int sps = cc_type == 1 ? 8 : 10;
        assert(dsd_engine_trunk_tune_to_cc_request(opts, state, 852000000, sps, 0U) == DSD_TRUNK_TUNE_RESULT_OK);
        assert(g_rtl_symbol_rate_hz == (cc_type == 1 ? 6000 : 4800));
        assert(g_rtl_ted_sps == sps);
        assert(g_rtl_channel_profile
               == (cc_type == 1 ? RTL_STREAM_CHANNEL_PROFILE_P25_CQPSK : RTL_STREAM_CHANNEL_PROFILE_P25_C4FM));
        rtl_stream_clear_pending_retune_profile();
    }
#endif

    printf("ENGINE_TRUNK_RETUNE_REGRESSION: OK\n");
    free(state);
    free(opts);
    return 0;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif

void
dsd_trunk_tuning_request_publish(uint64_t request_id, dsd_trunk_tune_result result) {
    (void)request_id;
    (void)result;
}
