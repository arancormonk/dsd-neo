// SPDX-License-Identifier: GPL-3.0-or-later
// Coverage fixtures intentionally use private-source inclusion, synthetic sentinels,
// invalid-value negative vectors, or wrapper symbols to exercise guarded behavior.
// NOLINTBEGIN(misc-use-internal-linkage)
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <assert.h>
#include <dsd-neo/core/input_level.h>
#include <dsd-neo/core/power.h>
#include <dsd-neo/io/rtl_stream_c.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/rtl_stream_metrics_hooks.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>

#include "dsd-neo/io/rtl_stream_fwd.h"
#include "src/engine/engine_hooks_install.h"

unsigned int dsd_rtl_stream_output_rate(void);

static int g_output_rate_calls;
static int g_output_kind_calls;
static int g_symbol_profile_calls;
static int g_generation_calls;
static int g_request_profile_calls;
static int g_cqpsk_status_calls;
static int g_cqpsk_reacquire_calls;
static int g_cqpsk_timing_bias_calls;
static int g_snr_bias_calls;
static int g_snr_c4fm_calls;
static int g_snr_c4fm_eye_calls;
static int g_snr_cqpsk_calls;
static int g_snr_gfsk_calls;
static int g_snr_gfsk_eye_calls;
static int g_snr_qpsk_const_calls;
static int g_p25p1_ber_calls;
static int g_p25p2_err_calls;
static int g_stream_active_calls;
static int g_input_level_calls;
static int g_channel_squelch_calls;
static float g_last_channel_squelch;
static int g_channel_squelch_setting_calls;
static dsd_squelch_setting g_last_channel_squelch_setting;
static dsdneoRuntimeConfig g_runtime_config;

static int g_last_symbol_rate;
static int g_last_symbol_levels;
static int g_last_symbol_profile;
static int g_last_cqpsk_enable;
static int g_last_ted_sps;
static int g_last_ted_sps_is_override;
static int g_last_p25p1_ok;
static int g_last_p25p1_err;
static int g_last_p25p2_slot;
static int g_last_p25p2_facch_ok;
static int g_last_p25p2_facch_err;
static int g_last_p25p2_sacch_ok;
static int g_last_p25p2_sacch_err;
static int g_last_p25p2_voice_err;

const dsdneoRuntimeConfig*
dsd_neo_get_config(void) {
    return &g_runtime_config;
}

unsigned int
dsd_rtl_stream_output_rate(void) {
    ++g_output_rate_calls;
    return 48000U;
}

int
rtl_stream_get_output_kind(void) {
    ++g_output_kind_calls;
    return 2;
}

int
rtl_stream_get_symbol_profile_full(int* out_symbol_rate_hz, int* out_levels, int* out_channel_profile) {
    ++g_symbol_profile_calls;
    if (out_symbol_rate_hz) {
        *out_symbol_rate_hz = 6000;
    }
    if (out_levels) {
        *out_levels = 4;
    }
    if (out_channel_profile) {
        *out_channel_profile = 5;
    }
    return -6;
}

uint32_t
rtl_stream_output_generation(void) {
    ++g_generation_calls;
    return 123456U;
}

int
rtl_stream_request_demod_profile(int cqpsk_enable, int symbol_rate_hz, int levels, int channel_profile, int ted_sps,
                                 int ted_sps_is_override) {
    ++g_request_profile_calls;
    g_last_cqpsk_enable = cqpsk_enable;
    g_last_symbol_rate = symbol_rate_hz;
    g_last_symbol_levels = levels;
    g_last_symbol_profile = channel_profile;
    g_last_ted_sps = ted_sps;
    g_last_ted_sps_is_override = ted_sps_is_override;
    return -21;
}

static int g_request_analog_calls;
static int g_analog_profile_calls;
static int g_last_analog_family;
static int g_last_analog_kind;
static int g_last_analog_width_hz;

int
rtl_stream_request_analog_profile(int family, int kind, int width_hz) {
    ++g_request_analog_calls;
    g_last_analog_family = family;
    g_last_analog_kind = kind;
    g_last_analog_width_hz = width_hz;
    return -22;
}

static int g_digital_landing_calls;
static int g_last_landing_explicit;

int
rtl_stream_request_digital_family_landing(int cqpsk_explicit) {
    ++g_digital_landing_calls;
    g_last_landing_explicit = cqpsk_explicit;
    return -23;
}

int
rtl_stream_get_analog_profile(int* out_kind, int* out_width_hz, int* out_lpf_on) {
    ++g_analog_profile_calls;
    if (out_kind) {
        *out_kind = 0;
    }
    if (out_width_hz) {
        *out_width_hz = 12500;
    }
    if (out_lpf_on) {
        *out_lpf_on = 1;
    }
    return 1;
}

static int g_analog_family_active_calls;
static int g_output_rate_for_family_calls;
static int g_last_rate_family;
static int g_last_rate_cqpsk;
static int g_last_rate_symbol_rate;
static int g_last_rate_explicit;

int
rtl_stream_analog_family_active(void) {
    ++g_analog_family_active_calls;
    return 1;
}

static int g_family_landing_after_pending_calls;

/* Answers unlike rtl_stream_analog_family_active() above, so the wrapper is seen to reach this one. */
int
rtl_stream_family_landing_after_pending(void) {
    ++g_family_landing_after_pending_calls;
    return 0;
}

unsigned int
rtl_stream_output_rate_for_family(int family, int cqpsk_enable, int symbol_rate_hz, int cqpsk_explicit) {
    ++g_output_rate_for_family_calls;
    g_last_rate_family = family;
    g_last_rate_cqpsk = cqpsk_enable;
    g_last_rate_symbol_rate = symbol_rate_hz;
    g_last_rate_explicit = cqpsk_explicit;
    return 24000U;
}

int
rtl_stream_get_cqpsk_status(int* cqpsk_enable, int* cqpsk_timing_active) {
    ++g_cqpsk_status_calls;
    if (cqpsk_enable) {
        *cqpsk_enable = 1;
    }
    if (cqpsk_timing_active) {
        *cqpsk_timing_active = 0;
    }
    return -8;
}

int
rtl_stream_request_cqpsk_reacquire(void) {
    ++g_cqpsk_reacquire_calls;
    return -14;
}

int
rtl_stream_cqpsk_timing_bias(const RtlSdrContext* ctx) {
    assert(ctx == NULL);
    ++g_cqpsk_timing_bias_calls;
    return -13;
}

double
rtl_stream_get_snr_bias_evm(void) {
    ++g_snr_bias_calls;
    return 1.25;
}

double
rtl_stream_get_snr_c4fm(void) {
    ++g_snr_c4fm_calls;
    return 2.5;
}

double
rtl_stream_estimate_snr_c4fm_eye(void) {
    ++g_snr_c4fm_eye_calls;
    return 3.75;
}

double
rtl_stream_get_snr_cqpsk(void) {
    ++g_snr_cqpsk_calls;
    return 4.5;
}

double
rtl_stream_get_snr_gfsk(void) {
    ++g_snr_gfsk_calls;
    return 5.5;
}

double
rtl_stream_estimate_snr_gfsk_eye(void) {
    ++g_snr_gfsk_eye_calls;
    return 6.25;
}

double
rtl_stream_estimate_snr_qpsk_const(void) {
    ++g_snr_qpsk_const_calls;
    return 6.5;
}

void
rtl_stream_p25p1_ber_update(int fec_ok_delta, int fec_err_delta) {
    ++g_p25p1_ber_calls;
    g_last_p25p1_ok = fec_ok_delta;
    g_last_p25p1_err = fec_err_delta;
}

void
rtl_stream_p25p2_err_update(int slot, int facch_ok_delta, int facch_err_delta, int sacch_ok_delta, int sacch_err_delta,
                            int voice_err_delta) {
    ++g_p25p2_err_calls;
    g_last_p25p2_slot = slot;
    g_last_p25p2_facch_ok = facch_ok_delta;
    g_last_p25p2_facch_err = facch_err_delta;
    g_last_p25p2_sacch_ok = sacch_ok_delta;
    g_last_p25p2_sacch_err = sacch_err_delta;
    g_last_p25p2_voice_err = voice_err_delta;
}

int
rtl_stream_is_active(void) {
    ++g_stream_active_calls;
    return 1;
}

int
rtl_stream_get_input_level(dsd_input_level_snapshot* out) {
    ++g_input_level_calls;
    if (out) {
        out->status = DSD_INPUT_LEVEL_OK;
        out->source = DSD_INPUT_LEVEL_SOURCE_RTL_CU8;
        out->sample_count = 1024U;
    }
    return -9;
}

void
rtl_stream_set_channel_squelch(float level) {
    ++g_channel_squelch_calls;
    g_last_channel_squelch = level;
}

void
rtl_stream_set_channel_squelch_setting(const dsd_squelch_setting* setting) {
    ++g_channel_squelch_setting_calls;
    g_last_channel_squelch_setting = *setting;
}

static int g_replay_batch_calls;
static int g_replay_batch_result;

/* A tag whose fields all differ, so each is seen to reach its own field of the runtime batch. */
int
rtl_stream_get_replay_batch(rtl_stream_replay_batch* out) {
    ++g_replay_batch_calls;
    assert(out != NULL);
    *out = (rtl_stream_replay_batch){0};
    out->chunk_sequence = 7U;
    out->output_generation = 654321U;
    out->output_kind = 2;
    out->channel_profile = 5;
    out->symbol_rate_hz = 6000;
    out->symbol_levels = 2;
    out->output_rate_hz = 24000;
    out->media_start_ns = 1234567890123ULL;
    out->media_duration_ns = 21333333ULL;
    out->output_count = 99U;
    out->first_index = 17U;
    out->center_frequency_hz = 851012500U;
    return g_replay_batch_result;
}

/* The replay batch the decoder's last read took its samples from reaches DSP with the fields it labels a symbol cache
 * with, the media span it runs the decode clock on and the centre it was captured on (issue #575); no replay batch is
 * none. Called with the engine's table installed. */
static void
test_replay_batch_hook(void) {
    dsd_rtl_stream_replay_batch batch = {0};
    g_replay_batch_result = 0;
    assert(dsd_rtl_stream_metrics_hook_replay_batch(&batch) == 1);
    assert(g_replay_batch_calls == 1);
    assert(batch.generation == 654321U);
    assert(batch.output_kind == 2);
    assert(batch.channel_profile == 5);
    assert(batch.symbol_rate_hz == 6000);
    assert(batch.levels == 2);
    assert(batch.media_start_ns == 1234567890123ULL);
    assert(batch.media_duration_ns == 21333333ULL);
    assert(batch.output_count == 99U);
    assert(batch.first_index == 17U);
    assert(batch.center_frequency_hz == 851012500U);
    g_replay_batch_result = -1;
    assert(dsd_rtl_stream_metrics_hook_replay_batch(&batch) == 0);
    assert(g_replay_batch_calls == 2);
    assert(batch.generation == 0U && batch.output_kind == 0 && batch.channel_profile == 0);
    assert(batch.symbol_rate_hz == 0 && batch.levels == 0);
    assert(batch.media_start_ns == 0U && batch.media_duration_ns == 0U && batch.output_count == 0U);
    assert(batch.first_index == 0U && batch.center_frequency_hz == 0U);
}

int
main(void) {
    dsd_rtl_stream_metrics_hooks_set(NULL);
    dsd_engine_rtl_stream_metrics_hooks_install();

    assert(dsd_rtl_stream_metrics_hook_output_rate_hz() == 48000U);
    assert(g_output_rate_calls == 1);
    assert(dsd_rtl_stream_metrics_hook_output_kind() == 2);
    assert(g_output_kind_calls == 1);

    int rate = 0;
    int levels = 0;
    int profile = 0;
    assert(dsd_rtl_stream_metrics_hook_symbol_profile(&rate, &levels, &profile) == -6);
    assert(g_symbol_profile_calls == 1);
    assert(rate == 6000);
    assert(levels == 4);
    assert(profile == 5);

    assert(dsd_rtl_stream_metrics_hook_stream_generation() == 123456U);
    assert(g_generation_calls == 1);
    g_request_profile_calls = 0;
    assert(dsd_rtl_stream_metrics_hook_apply_demod_profile(1, 6000, 4, 5, 8) == -21);
    assert(g_request_profile_calls == 1);
    assert(g_last_cqpsk_enable == 1);
    assert(g_last_symbol_rate == 6000);
    assert(g_last_symbol_levels == 4);
    assert(g_last_symbol_profile == 5);
    assert(g_last_ted_sps == 8);
    assert(g_last_ted_sps_is_override == 0);

    /* A user cqpsk override leaves the demod family unchanged (-1). */
    g_runtime_config.cqpsk_is_set = 1;
    g_runtime_config.cqpsk_enable = 0;
    assert(dsd_rtl_stream_metrics_hook_apply_demod_profile(1, 6000, 4, 5, 8) == -21);
    assert(g_request_profile_calls == 2);
    assert(g_last_cqpsk_enable == -1);

    g_runtime_config.cqpsk_enable = 1;
    assert(dsd_rtl_stream_metrics_hook_apply_demod_profile(0, 4800, 4, 3, 10) == -21);
    assert(g_request_profile_calls == 3);
    assert(g_last_cqpsk_enable == -1);
    assert(g_last_symbol_rate == 4800);
    assert(g_last_symbol_profile == 3);
    assert(g_last_ted_sps == 10);

    /* ted_sps<=0 maps to 0: clear the override without applying a value. */
    assert(dsd_rtl_stream_metrics_hook_apply_demod_profile(0, 4800, 4, 3, -5) == -21);
    assert(g_request_profile_calls == 4);
    assert(g_last_ted_sps == 0);
    assert(g_last_ted_sps_is_override == 0);

    /* The analog profile request and readback go straight to the stream. */
    assert(dsd_rtl_stream_metrics_hook_apply_analog_profile(1, 0, 12500) == -22);
    assert(g_request_analog_calls == 1);
    assert(g_last_analog_family == 1);
    assert(g_last_analog_kind == 0);
    assert(g_last_analog_width_hz == 12500);
    /* So does the digital family request that lands the digital family's landing (issue #583), not the plain one. */
    assert(dsd_rtl_stream_metrics_hook_request_digital_family_landing(1) == -23);
    assert(g_digital_landing_calls == 1 && g_last_landing_explicit == 1);
    assert(g_request_analog_calls == 1);
    int analog_kind = -1;
    int analog_width = -1;
    int analog_lpf_on = -1;
    assert(dsd_rtl_stream_metrics_hook_analog_profile(&analog_kind, &analog_width, &analog_lpf_on) == 1);
    assert(g_analog_profile_calls == 1);
    assert(analog_kind == 0);
    assert(analog_width == 12500);
    assert(analog_lpf_on == 1);
    /* So do the family readback and the output rate a family switch lands on. */
    assert(dsd_rtl_stream_metrics_hook_analog_family_active() == 1);
    assert(g_analog_family_active_calls == 1);
    /* Whether outstanding work lands a family is the stream's own query, not the live family (issue #583). */
    assert(dsd_rtl_stream_metrics_hook_family_landing_after_pending() == 0);
    assert(g_family_landing_after_pending_calls == 1);
    assert(g_analog_family_active_calls == 1);
    /* The target's own CQPSK choice reaches the stream with it (issue #583). */
    assert(dsd_rtl_stream_metrics_hook_output_rate_for_family(0, 1, 6000, 1) == 24000U);
    assert(g_output_rate_for_family_calls == 1);
    assert(g_last_rate_family == 0 && g_last_rate_cqpsk == 1 && g_last_rate_symbol_rate == 6000);
    assert(g_last_rate_explicit == 1);

    int cqpsk_enable = -1;
    int cqpsk_timing = -1;
    assert(dsd_rtl_stream_metrics_hook_cqpsk_status(&cqpsk_enable, &cqpsk_timing) == -8);
    assert(g_cqpsk_status_calls == 1);
    assert(cqpsk_enable == 1);
    assert(cqpsk_timing == 0);
    assert(dsd_rtl_stream_metrics_hook_request_cqpsk_reacquire() == -14);
    assert(g_cqpsk_reacquire_calls == 1);
    assert(dsd_rtl_stream_metrics_hook_cqpsk_timing_bias() == -13);
    assert(g_cqpsk_timing_bias_calls == 1);

    assert(dsd_rtl_stream_metrics_hook_snr_bias_evm() == 1.25);
    assert(dsd_rtl_stream_metrics_hook_snr_c4fm_db() == 2.5);
    assert(dsd_rtl_stream_metrics_hook_snr_c4fm_eye_db() == 3.75);
    assert(dsd_rtl_stream_metrics_hook_snr_cqpsk_db() == 4.5);
    assert(dsd_rtl_stream_metrics_hook_snr_gfsk_db() == 5.5);
    assert(dsd_rtl_stream_metrics_hook_snr_gfsk_eye_db() == 6.25);
    assert(dsd_rtl_stream_metrics_hook_snr_qpsk_const_db() == 6.5);
    assert(g_snr_bias_calls == 1);
    assert(g_snr_c4fm_calls == 1);
    assert(g_snr_c4fm_eye_calls == 1);
    assert(g_snr_cqpsk_calls == 1);
    assert(g_snr_gfsk_calls == 1);
    assert(g_snr_gfsk_eye_calls == 1);
    assert(g_snr_qpsk_const_calls == 1);

    dsd_rtl_stream_metrics_hook_p25p1_ber_update(11, 12);
    assert(g_p25p1_ber_calls == 1);
    assert(g_last_p25p1_ok == 11);
    assert(g_last_p25p1_err == 12);
    dsd_rtl_stream_metrics_hook_p25p2_err_update(1, 2, 3, 4, 5, 6);
    assert(g_p25p2_err_calls == 1);
    assert(g_last_p25p2_slot == 1);
    assert(g_last_p25p2_facch_ok == 2);
    assert(g_last_p25p2_facch_err == 3);
    assert(g_last_p25p2_sacch_ok == 4);
    assert(g_last_p25p2_sacch_err == 5);
    assert(g_last_p25p2_voice_err == 6);

    assert(dsd_rtl_stream_metrics_hook_stream_active() == 1);
    assert(g_stream_active_calls == 1);
    dsd_input_level_snapshot level = {0};
    assert(dsd_rtl_stream_metrics_hook_input_level(&level) == -9);
    assert(g_input_level_calls == 1);
    assert(level.status == DSD_INPUT_LEVEL_OK);
    assert(level.source == DSD_INPUT_LEVEL_SOURCE_RTL_CU8);
    assert(level.sample_count == 1024U);

    /* Scan rows push their squelch through the runtime table to the same demod setter the
     * operator's squelch command uses (issue #521). */
    assert(dsd_rtl_stream_metrics_hook_set_channel_squelch(1e-6) == 0);
    assert(g_channel_squelch_calls == 1);
    assert(fabsf(g_last_channel_squelch - 1e-6f) < 1e-12f);
    /* A whole setting, the auto squelch's margin included, reaches its own setter (issue #518 follow-up). */
    const dsd_squelch_setting auto6 = {DSD_SQUELCH_MODE_AUTO, 0.0, 6};
    assert(dsd_rtl_stream_metrics_hook_set_channel_squelch_setting(&auto6) == 0);
    assert(g_channel_squelch_setting_calls == 1 && g_channel_squelch_calls == 1);
    assert(g_last_channel_squelch_setting.mode == DSD_SQUELCH_MODE_AUTO
           && g_last_channel_squelch_setting.margin_db == 6);
    assert(dsd_rtl_stream_metrics_hook_set_channel_squelch_setting(NULL) == -1);

    test_replay_batch_hook();

    dsd_rtl_stream_metrics_hooks_set(NULL);
    return 0;
}

// NOLINTEND(misc-use-internal-linkage)
