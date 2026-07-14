// SPDX-License-Identifier: GPL-3.0-or-later
// Coverage fixtures intentionally use private-source inclusion, synthetic sentinels,
// invalid-value negative vectors, or wrapper symbols to exercise guarded behavior.
// NOLINTBEGIN(misc-use-internal-linkage)
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <assert.h>
#include <dsd-neo/core/input_level.h>
#include <dsd-neo/io/rtl_stream_c.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/rtl_stream_metrics_hooks.h>
#include <stddef.h>
#include <stdint.h>

#include "dsd-neo/io/rtl_stream_fwd.h"
#include "src/engine/engine_hooks_install.h"

unsigned int dsd_rtl_stream_output_rate(void);

static int g_output_rate_calls;
static int g_output_kind_calls;
static int g_symbol_profile_calls;
static int g_generation_calls;
static int g_set_symbol_profile_calls;
static int g_family_calls;
static int g_ted_clear_calls;
static int g_ted_set_calls;
static int g_cqpsk_status_calls;
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
static dsdneoRuntimeConfig g_runtime_config;

static int g_last_symbol_rate;
static int g_last_symbol_levels;
static int g_last_symbol_profile;
static int g_last_cqpsk_enable;
static int g_last_ted_sps;
static int g_apply_order;
static int g_family_order;
static int g_ted_clear_order;
static int g_ted_set_order;
static int g_symbol_profile_order;
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
rtl_stream_set_symbol_profile(int symbol_rate_hz, int levels, int channel_profile) {
    ++g_set_symbol_profile_calls;
    g_symbol_profile_order = ++g_apply_order;
    g_last_symbol_rate = symbol_rate_hz;
    g_last_symbol_levels = levels;
    g_last_symbol_profile = channel_profile;
    return 7;
}

void
rtl_stream_toggle_cqpsk(int onoff) {
    ++g_family_calls;
    g_family_order = ++g_apply_order;
    g_last_cqpsk_enable = onoff;
}

void
rtl_stream_clear_ted_sps_override(void) {
    ++g_ted_clear_calls;
    g_ted_clear_order = ++g_apply_order;
}

void
rtl_stream_set_ted_sps_no_override(int sps) {
    ++g_ted_set_calls;
    g_ted_set_order = ++g_apply_order;
    g_last_ted_sps = sps;
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
    g_family_calls = 0;
    g_ted_clear_calls = 0;
    g_ted_set_calls = 0;
    g_apply_order = 0;
    g_family_order = 0;
    g_ted_clear_order = 0;
    g_ted_set_order = 0;
    g_symbol_profile_order = 0;
    assert(dsd_rtl_stream_metrics_hook_apply_demod_profile(1, 6000, 4, 5, 8) == 7);
    assert(g_family_calls == 1);
    assert(g_last_cqpsk_enable == 1);
    assert(g_family_order == 1);
    assert(g_ted_clear_calls == 1);
    assert(g_ted_clear_order == 2);
    assert(g_ted_set_calls == 1);
    assert(g_last_ted_sps == 8);
    assert(g_ted_set_order == 3);
    assert(g_set_symbol_profile_calls == 1);
    assert(g_last_symbol_rate == 6000);
    assert(g_last_symbol_levels == 4);
    assert(g_last_symbol_profile == 5);
    assert(g_symbol_profile_order == 4);

    g_runtime_config.cqpsk_is_set = 1;
    g_runtime_config.cqpsk_enable = 0;
    g_family_calls = 0;
    assert(dsd_rtl_stream_metrics_hook_apply_demod_profile(1, 6000, 4, 5, 8) == 7);
    assert(g_family_calls == 0);
    assert(g_ted_clear_calls == 2);
    assert(g_ted_set_calls == 2);
    assert(g_set_symbol_profile_calls == 2);

    g_runtime_config.cqpsk_enable = 1;
    assert(dsd_rtl_stream_metrics_hook_apply_demod_profile(0, 4800, 4, 3, 10) == 7);
    assert(g_family_calls == 0);
    assert(g_ted_clear_calls == 3);
    assert(g_ted_set_calls == 3);
    assert(g_set_symbol_profile_calls == 3);

    int cqpsk_enable = -1;
    int cqpsk_timing = -1;
    assert(dsd_rtl_stream_metrics_hook_cqpsk_status(&cqpsk_enable, &cqpsk_timing) == -8);
    assert(g_cqpsk_status_calls == 1);
    assert(cqpsk_enable == 1);
    assert(cqpsk_timing == 0);
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

    dsd_rtl_stream_metrics_hooks_set(NULL);
    return 0;
}

// NOLINTEND(misc-use-internal-linkage)
