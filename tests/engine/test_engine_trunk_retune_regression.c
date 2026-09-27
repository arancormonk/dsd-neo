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
 * - a -Y row that runs the analog family queues the configured analog
 *   profile for its retune, never a symbol profile
 * - a rigctl scan tune asks the peer for the row's demodulator and passband
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
#include <dsd-neo/runtime/analog_channel.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/scan_mode.h>
#include <dsd-neo/runtime/scan_options.h>
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
#include "scan_analog_internal.h"

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
static int g_setmod_calls = 0;
static int g_setmod_kind = -1;
static int g_setmod_bw = 0;

/* Which rigctl modulation call a tune or restore made: a session request, a scan row's own, or the scan's restore. */
enum { SETMOD_SESSION = 0, SETMOD_ROW = 1, SETMOD_RESTORE = 2 };

static int g_setmod_call = -1;
/* The demodulator the fake peer last accepted (CachedModulationKind()). */
static int g_peer_kind = DSD_ANALOG_DEMOD_FM;
/* RevertModulation(): how often a failed tune asked, what it asked to put back, and whether the peer accepts. */
static int g_revert_calls = 0;
static int g_revert_kind = -2;
static bool g_revert_result = true;
static const dsd_scan_option_values* g_tuning_row_options = NULL;
static int g_frame_sync_reset_calls = 0;
static int g_sps_hunt_restart_calls = 0;
static int g_p25p2_frame_reset_calls = 0;
static int g_rtl_tune_result = RTL_STREAM_TUNE_OK;
static uint32_t g_rtl_last_applied_freq = 0U;
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
static int g_rtl_analog_prepare_calls = 0;
static int g_rtl_analog_prepare_rc = 0;
static rtl_stream_retune_analog_profile g_rtl_analog_prepared;
static uint32_t g_rtl_analog_prepared_target_hz = 0U;
static size_t g_trunk_scan_target_count = 0;
static int g_trunk_scan_active_gfsk_symbol_rate = 0;
static int g_trunk_scan_saved_autogain_is_set = 0;
static int g_trunk_scan_saved_autogain_on = 0;
static int g_trunk_scan_active_p25_cqpsk_is_set = 0;
static int g_trunk_scan_active_p25_cqpsk_enable = 0;
static int g_trunk_scan_active_p25_target = 0;
static int g_trunk_scan_active_p25_class = 0;
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

int
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_engine_trunk_scan_active_is_p25_class(const dsd_state* state) {
    (void)state;
    return g_trunk_scan_active_p25_class || g_trunk_scan_active_p25_target;
}

bool
SetFreq(dsd_socket_t sockfd, long int freq) {
    (void)sockfd;
    g_setfreq_calls++;
    g_last_setfreq_hz = freq;
    return g_setfreq_result;
}

/* Records the demodulator and passband each tune asks the rigctl peer for, and through which call (issue #526). */
static bool
record_setmod(int call, int kind, int bandwidth) {
    g_setmod_calls++;
    g_setmod_call = call;
    g_setmod_kind = kind;
    g_setmod_bw = bandwidth;
    if (g_setmod_result) {
        g_peer_kind = kind;
    }
    return g_setmod_result;
}

bool
SetModulationKind(dsd_socket_t sockfd, int kind, int bandwidth) {
    (void)sockfd;
    return record_setmod(SETMOD_SESSION, kind, bandwidth);
}

bool
SetScanRowModulation(dsd_socket_t sockfd, int kind, int bandwidth) {
    (void)sockfd;
    return record_setmod(SETMOD_ROW, kind, bandwidth);
}

bool
RestoreScanModulation(dsd_socket_t sockfd, int kind, int bandwidth) {
    (void)sockfd;
    return record_setmod(SETMOD_RESTORE, kind, bandwidth);
}

int
CachedModulationKind(dsd_socket_t sockfd) {
    (void)sockfd;
    return g_peer_kind;
}

dsd_rigctl_modulation
CachedModulation(dsd_socket_t sockfd) {
    (void)sockfd;
    const dsd_rigctl_modulation now = {g_peer_kind, 0};
    return now;
}

bool
RevertModulation(dsd_socket_t sockfd, dsd_rigctl_modulation before) {
    (void)sockfd;
    g_revert_calls++;
    g_revert_kind = before.kind;
    if (g_revert_result) {
        g_peer_kind = before.kind;
    }
    return g_revert_result;
}

/* The row options a rigctl tune reads a row's own passband from (issue #526): none unless a test installs some. */
const dsd_scan_option_values*
dsd_engine_scan_tuning_row_options(const dsd_opts* opts, const dsd_state* state) {
    (void)opts;
    (void)state;
    return g_tuning_row_options;
}

int
dsd_analog_width_effective_hz(int kind, int configured_hz) {
    if (configured_hz > 0) {
        return configured_hz;
    }
    return kind == DSD_ANALOG_DEMOD_AM ? DSD_ANALOG_AM_WIDTH_DEFAULT_HZ : DSD_ANALOG_NFM_WIDTH_DEFAULT_HZ;
}

const char*
dsd_analog_demod_label(int kind) {
    return kind == DSD_ANALOG_DEMOD_AM ? "AM" : "NFM";
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

int
rtl_stream_get_last_applied_freq(uint32_t* out_freq_hz) {
    if (g_rtl_last_applied_freq == 0U) {
        return -1;
    }
    *out_freq_hz = g_rtl_last_applied_freq;
    return 0;
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

/* Records the analog profile attached to a target's retune; the receive family is the stream's, which this regression
   does not model, so nothing else changes. */
int
rtl_stream_prepare_retune_analog_profile_for_target(uint32_t target_freq_hz,
                                                    const rtl_stream_retune_analog_profile* analog) {
    g_rtl_analog_prepare_calls++;
    if (!analog || g_rtl_analog_prepare_rc != 0) {
        return -1;
    }
    g_rtl_analog_prepared = *analog;
    g_rtl_analog_prepared_target_hz = target_freq_hz;
    return 0;
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

/* The scanner's receive-family side (issue #526): these fixtures tune on a digital front end, which never reads the
 * analog profile back; an analog row's retune attaches its family through the recording stub above. */
int
rtl_stream_get_analog_profile(int* out_kind, int* out_width_hz, int* out_lpf_on) {
    if (out_kind) {
        *out_kind = 0;
    }
    if (out_width_hz) {
        *out_width_hz = 0;
    }
    if (out_lpf_on) {
        *out_lpf_on = 0;
    }
    return 0;
}

int
rtl_stream_analog_family_active(void) {
    return 0;
}

unsigned int
rtl_stream_output_rate_for_family(int family, int cqpsk_enable, int symbol_rate_hz) {
    (void)family;
    (void)cqpsk_enable;
    (void)symbol_rate_hz;
    return 48000U;
}

int
rtl_stream_get_request_rate_hz(void) {
    return 48000;
}

uint32_t
rtl_stream_live_family_request_count(void) {
    return 0U;
}

int
dsd_analog_width_check(int kind, int width_hz, int rate_hz, char* err, size_t err_size) {
    (void)kind;
    (void)width_hz;
    (void)rate_hz;
    if (err && err_size > 0U) {
        err[0] = '\0';
    }
    return 0;
}

int
dsd_scan_mode_configured_digital(const dsd_opts* opts, const dsd_state* state) {
    (void)state;
    return opts && !(opts->analog_only == 1 && opts->m17encoder != 1);
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

static void
test_backend_tune_updates_center_freq_cache(void) {
    dsd_opts* opts = calloc(1, sizeof(*opts));
    dsd_state* state = calloc(1, sizeof(*state));
    assert(opts && state);
#ifdef USE_RADIO
    opts->audio_in_type = AUDIO_IN_RTL;
    state->rtl_ctx = (RtlSdrContext*)state;
    opts->rtlsdr_center_freq = 111111100U;
    g_rtl_tune_result = RTL_STREAM_TUNE_OK;
    assert(dsd_engine_trunk_tune_to_cc_request(opts, state, 456318750, 10, 0U) == DSD_TRUNK_TUNE_RESULT_OK);
    assert(opts->rtlsdr_center_freq == 456318750U);

    opts->rtlsdr_center_freq = 111111100U;
    g_rtl_last_applied_freq = 456331250U;
    assert(dsd_engine_trunk_tune_to_cc_request(opts, state, 456318750, 10, 0U) == DSD_TRUNK_TUNE_RESULT_OK);
    assert(opts->rtlsdr_center_freq == 456331250U);
    g_rtl_last_applied_freq = 0U;

    opts->rtlsdr_center_freq = 111111100U;
    g_rtl_tune_result = RTL_STREAM_TUNE_FAILED;
    assert(dsd_engine_trunk_tune_to_cc_request(opts, state, 456318750, 10, 0U) == DSD_TRUNK_TUNE_RESULT_FAILED);
    assert(opts->rtlsdr_center_freq == 111111100U);

    g_rtl_tune_result = RTL_STREAM_TUNE_DEFERRED;
    assert(dsd_engine_trunk_tune_to_cc_request(opts, state, 456318750, 10, 0U) == DSD_TRUNK_TUNE_RESULT_DEFERRED);
    assert(opts->rtlsdr_center_freq == 111111100U);

    g_rtl_tune_result = RTL_STREAM_TUNE_TIMEOUT;
    assert(dsd_engine_trunk_tune_to_cc_request(opts, state, 456318750, 10, UINT64_C(0x10))
           == DSD_TRUNK_TUNE_RESULT_PENDING);
    assert(opts->rtlsdr_center_freq == 456318750U);
    rtl_stream_clear_pending_retune_profile();

    opts->rtlsdr_center_freq = 111111100U;
    g_rtl_tune_result = RTL_STREAM_TUNE_OK;
    assert(dsd_engine_scan_tune_to_freq(opts, state, 461556250, 20, NULL) == DSD_TRUNK_TUNE_RESULT_OK);
    assert(opts->rtlsdr_center_freq == 461556250U);

    /* A conventional P25 target selects the P25 chain without enabling a trunk SM. */
    g_trunk_scan_active_p25_class = 1;
    g_trunk_scan_target_count = 1;
    opts->trunk_enable = 0;
    state->rf_mod = 0;
    g_rtl_cqpsk_enable = 0;
    g_rtl_symbol_rate_hz = 2400;
    g_rtl_symbol_levels = 2;
    g_rtl_channel_profile = RTL_STREAM_CHANNEL_PROFILE_6K25;
    g_rtl_pending_active = 0;
    assert(dsd_engine_scan_tune_to_freq(opts, state, 853000000, 10, NULL) == DSD_TRUNK_TUNE_RESULT_OK);
    assert(g_rtl_symbol_rate_hz == 4800);
    assert(g_rtl_symbol_levels == 4);
    assert(g_rtl_channel_profile == RTL_STREAM_CHANNEL_PROFILE_P25_C4FM);
    g_trunk_scan_active_p25_class = 0;
    g_trunk_scan_target_count = 0;
#endif
    opts->audio_in_type = AUDIO_IN_PULSE;
    opts->use_rigctl = 1;
    opts->rtlsdr_center_freq = 111111100U;
    g_setfreq_result = true;
    assert(dsd_engine_trunk_tune_to_cc_request(opts, state, 451000000, 0, 0U) == DSD_TRUNK_TUNE_RESULT_OK);
    assert(opts->rtlsdr_center_freq == 451000000U);
    opts->rtlsdr_center_freq = 111111100U;
    g_setfreq_result = false;
    assert(dsd_engine_trunk_tune_to_cc_request(opts, state, 451000000, 0, 0U) == DSD_TRUNK_TUNE_RESULT_FAILED);
    assert(opts->rtlsdr_center_freq == 111111100U);
    g_setfreq_result = true;
    free(state);
    free(opts);
}

#ifdef USE_RADIO
/* A -Y row tuned with the RTL front end set up for @p freq_hz from the AM monitor's WIDE channel, with nothing queued
   for its retune unless @p stale_symbol_profile queues a P25 C4FM symbol profile for the same target first. Returns the
   tune's result; @p tune_calls receives how many times the backend was asked to tune. */
static dsd_trunk_tune_result
tune_scan_row(dsd_opts* opts, dsd_state* state, long int freq_hz, int stale_symbol_profile, int* tune_calls) {
    g_rtl_tune_result = RTL_STREAM_TUNE_OK;
    g_rtl_cqpsk_enable = 0;
    g_rtl_symbol_rate_hz = 4800;
    g_rtl_symbol_levels = 4;
    g_rtl_channel_profile = RTL_STREAM_CHANNEL_PROFILE_WIDE;
    rtl_stream_clear_pending_retune_profile();
    if (stale_symbol_profile) {
        rtl_stream_prepare_retune_profile_for_target_with_gain((uint32_t)freq_hz, 0, 4800, 4,
                                                               RTL_STREAM_CHANNEL_PROFILE_P25_C4FM, 10, 0, NULL);
    }
    g_rtl_analog_prepare_calls = 0;
    DSD_MEMSET(&g_rtl_analog_prepared, 0, sizeof(g_rtl_analog_prepared));
    g_rtl_analog_prepared_target_hz = 0U;
    const int calls_before = g_rtl_tune_calls;
    const dsd_trunk_tune_result result = dsd_engine_scan_tune_to_freq(opts, state, freq_hz, 10, NULL);
    *tune_calls = g_rtl_tune_calls - calls_before;
    return result;
}

/*
 * A -Y row whose settings run the analog family (a blank row on an -fM or -fA session keeps the configured mode) has
 * no symbol clock (issue #524). Its retune queues the configured analog profile for the target, the kind and width
 * with 0 for the default, and no symbol profile: one would put a digital channel filter on the monitor, and on AM read
 * the carrier with the FM discriminator. A symbol profile already queued for the target does not ride along. A
 * refused analog profile fails the tune before the backend moves (the scan skips the row, issue #526), dropping what
 * was queued for it. A typed digital row on the same session queues its symbol profile as before.
 */
static void
test_analog_scan_row_queues_analog_profile(void) {
    dsd_opts* opts = calloc(1, sizeof(*opts));
    dsd_state* state = calloc(1, sizeof(*state));
    assert(opts && state);
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->scanner_mode = 1;
    opts->analog_only = 1;
    opts->monitor_input_audio = 1;
    opts->analog_demod = DSD_ANALOG_DEMOD_AM;
    opts->analog_am_bandwidth_hz = 10000;
    state->rtl_ctx = (RtlSdrContext*)state;
    int tuned = 0;

    assert(tune_scan_row(opts, state, 118100000, 0, &tuned) == DSD_TRUNK_TUNE_RESULT_OK && tuned == 1);
    assert(g_rtl_analog_prepare_calls == 1);
    assert(g_rtl_analog_prepared_target_hz == 118100000U);
    assert(g_rtl_analog_prepared.family == DSD_RX_FAMILY_ANALOG);
    assert(g_rtl_analog_prepared.kind == DSD_ANALOG_DEMOD_AM);
    assert(g_rtl_analog_prepared.width_hz == 10000);
    assert(g_rtl_channel_profile == RTL_STREAM_CHANNEL_PROFILE_WIDE);
    assert(g_rtl_symbol_rate_hz == 4800 && g_rtl_cqpsk_enable == 0);

    assert(tune_scan_row(opts, state, 118100000, 1, &tuned) == DSD_TRUNK_TUNE_RESULT_OK && tuned == 1);
    assert(g_rtl_analog_prepare_calls == 1);
    assert(g_rtl_channel_profile == RTL_STREAM_CHANNEL_PROFILE_WIDE);

    g_rtl_analog_prepare_rc = -1;
    assert(tune_scan_row(opts, state, 118100000, 1, &tuned) == DSD_TRUNK_TUNE_RESULT_FAILED && tuned == 0);
    assert(g_rtl_analog_prepare_calls == 1);
    assert(g_rtl_pending_active == 0);
    assert(g_rtl_channel_profile == RTL_STREAM_CHANNEL_PROFILE_WIDE);
    g_rtl_analog_prepare_rc = 0;

    /* -fA at the unset NFM default. */
    opts->analog_demod = DSD_ANALOG_DEMOD_FM;
    assert(tune_scan_row(opts, state, 146520000, 0, &tuned) == DSD_TRUNK_TUNE_RESULT_OK && tuned == 1);
    assert(g_rtl_analog_prepare_calls == 1);
    assert(g_rtl_analog_prepared.kind == DSD_ANALOG_DEMOD_FM);
    assert(g_rtl_analog_prepared.width_hz == 0);
    assert(g_rtl_channel_profile == RTL_STREAM_CHANNEL_PROFILE_WIDE);

    /* A typed DMR row: the row's settings are digital. */
    opts->analog_only = 0;
    opts->monitor_input_audio = 0;
    opts->frame_dmr = 1;
    assert(tune_scan_row(opts, state, 146520000, 0, &tuned) == DSD_TRUNK_TUNE_RESULT_OK && tuned == 1);
    assert(g_rtl_analog_prepare_calls == 0);
    assert(g_rtl_channel_profile != RTL_STREAM_CHANNEL_PROFILE_WIDE);

    rtl_stream_clear_pending_retune_profile();
    free(state);
    free(opts);
}
#endif

/* A rigctl scan tune of @p freq_hz with the peer answering @p peer_accepts; the frequency is asked for only after the
   modulation, so @p moved receives whether it was. */
static dsd_trunk_tune_result
tune_rigctl_row(dsd_opts* opts, dsd_state* state, long int freq_hz, bool peer_accepts, int* moved) {
    g_setmod_result = peer_accepts;
    g_setmod_calls = 0;
    g_setmod_kind = -1;
    g_setmod_bw = -1;
    g_setmod_call = -1;
    g_revert_calls = 0;
    g_revert_kind = -2;
    const int setfreq_before = g_setfreq_calls;
    const dsd_trunk_tune_result result = dsd_engine_scan_tune_to_freq(opts, state, freq_hz, 0, NULL);
    *moved = g_setfreq_calls - setfreq_before;
    g_setmod_result = true;
    return result;
}

/*
 * Issue #526: a rigctl peer demodulates PCM-input scans, so each scan tune asks it for the demodulator and passband the
 * row runs. An am row (the AM monitor) asks for AM at the AM width in force -- its own or the configured one, the 6 kHz
 * default when none is set -- and an nfm row that sets its own --nfm-bandwidth-hz asks for FM at that width. Both are
 * the row's own request (SetScanRowModulation(), which reads the peer's own passband first so the scan can put it
 * back): a peer that refuses fails the tune before the frequency moves, so the scanner takes its usual row-tune failure
 * path. Any other row asks for FM at -B (0 without it: the peer's own passband) as a session request, and stays
 * best-effort, as -B always was: a refusal still tunes.
 */
static void
test_rigctl_scan_rows_set_the_peer_demodulator(void) {
    dsd_opts* opts = calloc(1, sizeof(*opts));
    dsd_state* state = calloc(1, sizeof(*state));
    assert(opts && state);
    opts->audio_in_type = AUDIO_IN_PULSE;
    opts->use_rigctl = 1;
    opts->rigctl_sockfd = 5;
    opts->scanner_mode = 1;
    opts->analog_only = 1;
    opts->monitor_input_audio = 1;
    opts->analog_demod = DSD_ANALOG_DEMOD_AM;
    opts->analog_am_bandwidth_hz = 8333;
    opts->analog_nfm_bandwidth_hz = 20000;
    opts->setmod_bw = 0;
    int moved = 0;

    /* An am row: AM at the width it runs, else the AM default. */
    assert(tune_rigctl_row(opts, state, 118300000, true, &moved) == DSD_TRUNK_TUNE_RESULT_OK && moved == 1);
    assert(g_setmod_calls == 1 && g_setmod_kind == DSD_ANALOG_DEMOD_AM && g_setmod_bw == 8333);
    assert(g_setmod_call == SETMOD_ROW);
    assert(g_last_setfreq_hz == 118300000 && opts->rtlsdr_center_freq == 118300000U);
    opts->analog_am_bandwidth_hz = 0;
    assert(tune_rigctl_row(opts, state, 121500000, true, &moved) == DSD_TRUNK_TUNE_RESULT_OK && moved == 1);
    assert(g_setmod_kind == DSD_ANALOG_DEMOD_AM && g_setmod_bw == DSD_ANALOG_AM_WIDTH_DEFAULT_HZ);
    /* A peer that cannot demodulate AM fails the row's tune, and the frequency never moves. */
    assert(tune_rigctl_row(opts, state, 119100000, false, &moved) == DSD_TRUNK_TUNE_RESULT_FAILED && moved == 0);
    assert(g_setmod_kind == DSD_ANALOG_DEMOD_AM && opts->rtlsdr_center_freq == 121500000U);

    /* An nfm row with its own width hands the peer that width as the FM passband, and fails the same way. */
    static dsd_scan_option_values row;
    DSD_MEMSET(&row, 0, sizeof row);
    row.present = DSD_SCAN_OPT_BANDWIDTH;
    row.channel_bw_hz = 12500;
    row.channel_bw_kind = DSD_ANALOG_DEMOD_FM;
    g_tuning_row_options = &row;
    opts->analog_demod = DSD_ANALOG_DEMOD_FM;
    opts->analog_nfm_bandwidth_hz = 12500;
    assert(tune_rigctl_row(opts, state, 154430000, true, &moved) == DSD_TRUNK_TUNE_RESULT_OK && moved == 1);
    assert(g_setmod_kind == DSD_ANALOG_DEMOD_FM && g_setmod_bw == 12500 && g_setmod_call == SETMOD_ROW);
    assert(tune_rigctl_row(opts, state, 155475000, false, &moved) == DSD_TRUNK_TUNE_RESULT_FAILED && moved == 0);

    /* An nfm row without one runs the peer's own passband -- the configured NFM width is a channel filter DSD-neo
       applies to I/Q, not the peer's -- and a refusal does not keep it from tuning. */
    g_tuning_row_options = NULL;
    opts->analog_nfm_bandwidth_hz = 20000;
    assert(tune_rigctl_row(opts, state, 155475000, false, &moved) == DSD_TRUNK_TUNE_RESULT_OK && moved == 1);
    assert(g_setmod_kind == DSD_ANALOG_DEMOD_FM && g_setmod_bw == 0 && g_setmod_call == SETMOD_SESSION);

    /* A digital row asks for FM at -B, best-effort. */
    opts->analog_only = 0;
    opts->monitor_input_audio = 0;
    opts->frame_dmr = 1;
    opts->setmod_bw = 7000;
    assert(tune_rigctl_row(opts, state, 461000000, false, &moved) == DSD_TRUNK_TUNE_RESULT_OK && moved == 1);
    assert(g_setmod_kind == DSD_ANALOG_DEMOD_FM && g_setmod_bw == 7000 && g_setmod_call == SETMOD_SESSION);
    /* A row's width only means something on its own analog row. */
    g_tuning_row_options = &row;
    assert(tune_rigctl_row(opts, state, 461012500, true, &moved) == DSD_TRUNK_TUNE_RESULT_OK && moved == 1);
    assert(g_setmod_kind == DSD_ANALOG_DEMOD_FM && g_setmod_bw == 7000 && g_setmod_call == SETMOD_SESSION);
    g_tuning_row_options = NULL;
    free(state);
    free(opts);
}

/*
 * Issue #526: a row that sets nothing of its own asks the peer for FM at -B best-effort, unless the peer is still on
 * the AM an am row put it on (CachedModulationKind()). A peer that then refuses FM would give the next row AM audio,
 * so that tune fails before the frequency moves, as a row's own refused request does, and the scanner takes its usual
 * row-tune failure path. Once the peer is back on FM, a refused passband tunes again.
 */
static void
test_rigctl_refused_return_from_am_fails_the_tune(void) {
    dsd_opts* opts = calloc(1, sizeof(*opts));
    dsd_state* state = calloc(1, sizeof(*state));
    assert(opts && state);
    opts->audio_in_type = AUDIO_IN_PULSE;
    opts->use_rigctl = 1;
    opts->rigctl_sockfd = 5;
    opts->scanner_mode = 1;
    opts->analog_only = 1;
    opts->monitor_input_audio = 1;
    opts->analog_demod = DSD_ANALOG_DEMOD_AM;
    opts->setmod_bw = 0;
    g_peer_kind = DSD_ANALOG_DEMOD_FM;
    int moved = 0;

    assert(tune_rigctl_row(opts, state, 118300000, true, &moved) == DSD_TRUNK_TUNE_RESULT_OK && moved == 1);
    assert(g_peer_kind == DSD_ANALOG_DEMOD_AM);

    /* A digital row after it: the peer refuses FM, so the row is not tuned. */
    opts->analog_only = 0;
    opts->monitor_input_audio = 0;
    opts->frame_dmr = 1;
    opts->analog_demod = DSD_ANALOG_DEMOD_FM;
    assert(tune_rigctl_row(opts, state, 461000000, false, &moved) == DSD_TRUNK_TUNE_RESULT_FAILED && moved == 0);
    assert(g_setmod_kind == DSD_ANALOG_DEMOD_FM && g_setmod_bw == 0 && g_setmod_call == SETMOD_SESSION);
    assert(opts->rtlsdr_center_freq == 118300000U);
    /* ...nor an nfm row without a width of its own, and -B changes nothing. */
    opts->analog_only = 1;
    opts->monitor_input_audio = 1;
    opts->frame_dmr = 0;
    opts->setmod_bw = 12500;
    assert(tune_rigctl_row(opts, state, 155475000, false, &moved) == DSD_TRUNK_TUNE_RESULT_FAILED && moved == 0);
    assert(g_setmod_kind == DSD_ANALOG_DEMOD_FM && g_setmod_bw == 12500 && g_setmod_call == SETMOD_SESSION);

    /* Back on FM, a refused passband stays best-effort. */
    assert(tune_rigctl_row(opts, state, 155475000, true, &moved) == DSD_TRUNK_TUNE_RESULT_OK && moved == 1);
    assert(g_peer_kind == DSD_ANALOG_DEMOD_FM);
    assert(tune_rigctl_row(opts, state, 155500000, false, &moved) == DSD_TRUNK_TUNE_RESULT_OK && moved == 1);
    assert(opts->rtlsdr_center_freq == 155500000U);

    /* A peer that may run either demodulator (a request for AM whose reply was lost) and refuses FM fails the tune the
       same way, digital row or nfm row, until it accepts FM again. */
    g_peer_kind = DSD_RIGCTL_KIND_UNKNOWN;
    assert(tune_rigctl_row(opts, state, 155525000, false, &moved) == DSD_TRUNK_TUNE_RESULT_FAILED && moved == 0);
    opts->analog_only = 0;
    opts->monitor_input_audio = 0;
    opts->frame_dmr = 1;
    assert(tune_rigctl_row(opts, state, 461000000, false, &moved) == DSD_TRUNK_TUNE_RESULT_FAILED && moved == 0);
    assert(opts->rtlsdr_center_freq == 155500000U);
    assert(tune_rigctl_row(opts, state, 461000000, true, &moved) == DSD_TRUNK_TUNE_RESULT_OK && moved == 1);
    assert(g_peer_kind == DSD_ANALOG_DEMOD_FM);
    free(state);
    free(opts);
}

/*
 * Issue #526: a rigctl tune that fails leaves the row on air where it was, so what its modulation request changed on
 * the peer is put back (RevertModulation(), with what CachedModulation() read before the request): an am row's AM
 * accepted before the peer refused its frequency would otherwise have the nfm row still on air heard through AM. A
 * refused row request asks for the same, since the own-passband read before it may have switched the peer. A tune that
 * lands puts nothing back.
 */
static void
test_rigctl_failed_tune_puts_back_the_peer_modulation(void) {
    dsd_opts* opts = calloc(1, sizeof(*opts));
    dsd_state* state = calloc(1, sizeof(*state));
    assert(opts && state);
    opts->audio_in_type = AUDIO_IN_PULSE;
    opts->use_rigctl = 1;
    opts->rigctl_sockfd = 5;
    opts->scanner_mode = 1;
    opts->analog_only = 1;
    opts->monitor_input_audio = 1;
    opts->analog_demod = DSD_ANALOG_DEMOD_FM;
    opts->setmod_bw = 0;
    g_peer_kind = DSD_ANALOG_DEMOD_FM;
    int moved = 0;

    /* An nfm row on air. */
    assert(tune_rigctl_row(opts, state, 155475000, true, &moved) == DSD_TRUNK_TUNE_RESULT_OK && moved == 1);
    assert(g_revert_calls == 0);

    /* An am row whose AM the peer takes and whose frequency it refuses: the peer goes back to FM. */
    opts->analog_demod = DSD_ANALOG_DEMOD_AM;
    g_setfreq_result = false;
    assert(tune_rigctl_row(opts, state, 118300000, true, &moved) == DSD_TRUNK_TUNE_RESULT_FAILED && moved == 1);
    g_setfreq_result = true;
    assert(g_setmod_kind == DSD_ANALOG_DEMOD_AM && g_setmod_call == SETMOD_ROW);
    assert(g_revert_calls == 1 && g_revert_kind == DSD_ANALOG_DEMOD_FM);
    assert(g_peer_kind == DSD_ANALOG_DEMOD_FM && opts->rtlsdr_center_freq == 155475000U);

    /* An am row whose width the peer refuses asks for the same, and the frequency never moves. */
    assert(tune_rigctl_row(opts, state, 118300000, false, &moved) == DSD_TRUNK_TUNE_RESULT_FAILED && moved == 0);
    assert(g_revert_calls == 1 && g_revert_kind == DSD_ANALOG_DEMOD_FM);

    /* A peer that cannot be put back fails the tune all the same. */
    g_setfreq_result = false;
    g_revert_result = false;
    assert(tune_rigctl_row(opts, state, 118300000, true, &moved) == DSD_TRUNK_TUNE_RESULT_FAILED && moved == 1);
    g_setfreq_result = true;
    g_revert_result = true;
    assert(g_revert_calls == 1 && g_peer_kind == DSD_ANALOG_DEMOD_AM);

    /* The am row that lands puts nothing back. */
    assert(tune_rigctl_row(opts, state, 118300000, true, &moved) == DSD_TRUNK_TUNE_RESULT_OK && moved == 1);
    assert(g_revert_calls == 0 && g_peer_kind == DSD_ANALOG_DEMOD_AM);
    free(state);
    free(opts);
}

/*
 * Issue #526: once a scanner has left its rows, the rigctl peer goes back to what the restored settings ask for -- FM
 * at -B, the peer's own passband without it -- through RestoreScanModulation(), which first sends back each passband a
 * row changed, since no later tune outside a scan would undo an am row's AM. The restore is best-effort, and a session
 * without rigctl asks nothing.
 */
static void
test_rigctl_restore_after_a_scan(void) {
    dsd_opts* opts = calloc(1, sizeof(*opts));
    dsd_state* state = calloc(1, sizeof(*state));
    assert(opts && state);
    opts->audio_in_type = AUDIO_IN_PULSE;
    opts->use_rigctl = 1;
    opts->rigctl_sockfd = 5;
    opts->frame_dmr = 1;
    opts->setmod_bw = 0;
    g_setmod_calls = 0;
    g_setmod_result = false;
    dsd_engine_scan_rigctl_restore(opts, state);
    assert(g_setmod_calls == 1 && g_setmod_kind == DSD_ANALOG_DEMOD_FM && g_setmod_bw == 0);
    assert(g_setmod_call == SETMOD_RESTORE);
    opts->setmod_bw = 12500;
    dsd_engine_scan_rigctl_restore(opts, state);
    assert(g_setmod_calls == 2 && g_setmod_kind == DSD_ANALOG_DEMOD_FM && g_setmod_bw == 12500);
    assert(g_setmod_call == SETMOD_RESTORE);
    g_setmod_result = true;
    opts->use_rigctl = 0;
    dsd_engine_scan_rigctl_restore(opts, state);
    opts->use_rigctl = 1;
    opts->rigctl_sockfd = DSD_INVALID_SOCKET;
    dsd_engine_scan_rigctl_restore(opts, state);
    dsd_engine_scan_rigctl_restore(NULL, state);
    assert(g_setmod_calls == 2);
    free(state);
    free(opts);
}

#ifdef USE_RADIO
/*
 * Issue #526: on an RTL-family input DSD-neo demodulates the I/Q itself, and a rigctl peer beside it only follows the
 * frequency. An am row, or an nfm row with its own width, therefore asks the peer for FM at -B, best-effort, as every
 * tune did before: a peer that refuses AM or a passband never fails a row DSD-neo can receive.
 */
static void
test_rigctl_on_rtl_input_follows_the_frequency_only(void) {
    dsd_opts* opts = calloc(1, sizeof(*opts));
    dsd_state* state = calloc(1, sizeof(*state));
    assert(opts && state);
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->use_rigctl = 1;
    opts->rigctl_sockfd = 5;
    opts->scanner_mode = 1;
    opts->analog_only = 1;
    opts->monitor_input_audio = 1;
    opts->analog_demod = DSD_ANALOG_DEMOD_AM;
    opts->analog_am_bandwidth_hz = 8333;
    opts->setmod_bw = 0;
    state->rtl_ctx = (RtlSdrContext*)state;
    int tuned = 0;

    g_setmod_calls = 0;
    g_setmod_result = false;
    const int setfreq_before = g_setfreq_calls;
    assert(tune_scan_row(opts, state, 118300000, 0, &tuned) == DSD_TRUNK_TUNE_RESULT_OK && tuned == 1);
    assert(g_setmod_calls == 1 && g_setmod_kind == DSD_ANALOG_DEMOD_FM && g_setmod_bw == 0);
    assert(g_setmod_call == SETMOD_SESSION);
    assert(g_setfreq_calls == setfreq_before + 1);
    assert(g_rtl_analog_prepared.kind == DSD_ANALOG_DEMOD_AM && g_rtl_analog_prepared.width_hz == 8333);

    /* A peer an earlier audio-input scan left on AM, or on a demodulator not known, is never heard here: its refusal
       of FM still tunes both the peer's frequency and the RTL front end. */
    const int peer_kinds[] = {DSD_ANALOG_DEMOD_AM, DSD_RIGCTL_KIND_UNKNOWN};
    for (size_t i = 0; i < sizeof peer_kinds / sizeof peer_kinds[0]; i++) {
        g_peer_kind = peer_kinds[i];
        const int setfreq_at = g_setfreq_calls;
        assert(tune_scan_row(opts, state, 119100000, 0, &tuned) == DSD_TRUNK_TUNE_RESULT_OK && tuned == 1);
        assert(g_setfreq_calls == setfreq_at + 1 && g_setmod_kind == DSD_ANALOG_DEMOD_FM);
        assert(g_peer_kind == peer_kinds[i]);
    }
    g_peer_kind = DSD_ANALOG_DEMOD_FM;

    static dsd_scan_option_values row;
    DSD_MEMSET(&row, 0, sizeof row);
    row.present = DSD_SCAN_OPT_BANDWIDTH;
    row.channel_bw_hz = 12500;
    row.channel_bw_kind = DSD_ANALOG_DEMOD_FM;
    g_tuning_row_options = &row;
    opts->analog_demod = DSD_ANALOG_DEMOD_FM;
    opts->analog_nfm_bandwidth_hz = 12500;
    opts->setmod_bw = 7000;
    assert(tune_scan_row(opts, state, 154430000, 0, &tuned) == DSD_TRUNK_TUNE_RESULT_OK && tuned == 1);
    assert(g_setmod_kind == DSD_ANALOG_DEMOD_FM && g_setmod_bw == 7000 && g_setmod_call == SETMOD_SESSION);
    g_tuning_row_options = NULL;
    g_setmod_result = true;
    rtl_stream_clear_pending_retune_profile();
    free(state);
    free(opts);
}
#endif

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

    test_backend_tune_updates_center_freq_cache();
#ifdef USE_RADIO
    test_analog_scan_row_queues_analog_profile();
#endif
    test_rigctl_scan_rows_set_the_peer_demodulator();
    test_rigctl_refused_return_from_am_fails_the_tune();
    test_rigctl_failed_tune_puts_back_the_peer_modulation();
    test_rigctl_restore_after_a_scan();
#ifdef USE_RADIO
    test_rigctl_on_rtl_input_follows_the_frequency_only();
#endif

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

    /* An nxdn48-trunk target anchors p25_cc_freq like nxdn-trunk, so its return to the control channel goes
     * through dsd_engine_compute_cc_sps(), which only knows P25 rates. The RTL chain must still come back as
     * 2400 sym/s in 6.25 kHz with TED 20 from the coordinator's rate, not the P25-derived 10. */
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->trunk_scan_enabled = 1;
    opts->trunk_enable = 1;
    opts->trunk_is_tuned = 1;
    state->rtl_ctx = (RtlSdrContext*)state;
    state->rf_mod = 2;
    state->p25_cc_freq = 461556250;
    state->trunk_cc_freq = 461556250;
    state->p25_cc_is_tdma = 2;
    state->synctype = DSD_SYNC_NXDN_POS;
    state->lastsynctype = DSD_SYNC_NXDN_POS;
    state->samplesPerSymbol = 20;
    state->symbolCenter = 9;
    state->sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_2400_4;
    g_trunk_scan_target_count = 2;
    g_trunk_scan_active_p25_target = 0;
    g_trunk_scan_active_gfsk_symbol_rate = 2400;
    g_rtl_tune_result = RTL_STREAM_TUNE_OK;
    g_rtl_symbol_rate_hz = 4800;
    g_rtl_channel_profile = RTL_STREAM_CHANNEL_PROFILE_12K5;
    g_rtl_ted_sps = 10;
    g_rtl_pending_active = 0;
    assert(dsd_engine_return_to_cc_request(opts, state, 0U) == DSD_TRUNK_TUNE_RESULT_OK);
    assert(g_rtl_symbol_rate_hz == 2400);
    assert(g_rtl_channel_profile == RTL_STREAM_CHANNEL_PROFILE_6K25);
    assert(g_rtl_ted_sps == 20);
    assert(state->rf_mod == 2 && state->samplesPerSymbol == 20 && state->symbolCenter == 9);
    assert(state->sps_hunt_idx == DSD_FRAME_SYNC_SPS_PROFILE_2400_4);
    assert(opts->trunk_is_tuned == 0);

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
