// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <assert.h>
#include <dsd-neo/core/input_level.h>
#include <stddef.h>
#include <stdint.h>

#include <dsd-neo/runtime/rtl_stream_metrics_hooks.h>

static int g_output_rate_calls = 0;
static int g_output_kind_calls = 0;
static int g_symbol_profile_calls = 0;
static int g_stream_generation_calls = 0;
static int g_stream_active_calls = 0;
static int g_apply_demod_profile_calls = 0;
static int g_cqpsk_status_calls = 0;
static int g_cqpsk_reacquire_calls = 0;
static int g_cqpsk_timing_bias_calls = 0;
static int g_snr_bias_calls = 0;
static int g_snr_c4fm_calls = 0;
static int g_snr_c4fm_eye_calls = 0;
static int g_snr_cqpsk_calls = 0;
static int g_snr_gfsk_calls = 0;
static int g_snr_gfsk_eye_calls = 0;
static int g_snr_qpsk_const_calls = 0;
static int g_p25p1_ber_calls = 0;
static int g_p25p2_err_calls = 0;
static int g_input_level_calls = 0;

static int g_p25p1_ok_delta = 0;
static int g_p25p1_err_delta = 0;

static int g_apply_cqpsk_enable = 0;
static int g_apply_symbol_rate_hz = 0;
static int g_apply_symbol_levels = 0;
static int g_apply_symbol_channel_profile = 0;
static int g_apply_ted_sps = 0;

static int g_p25p2_slot = 0;
static int g_p25p2_facch_ok_delta = 0;
static int g_p25p2_facch_err_delta = 0;
static int g_p25p2_sacch_ok_delta = 0;
static int g_p25p2_sacch_err_delta = 0;
static int g_p25p2_voice_err_delta = 0;

static unsigned int
fake_output_rate_hz(void) {
    g_output_rate_calls++;
    return 24000U;
}

static int
fake_output_kind(void) {
    g_output_kind_calls++;
    return 1;
}

static int
fake_symbol_profile(int* out_symbol_rate_hz, int* out_levels, int* out_channel_profile) {
    g_symbol_profile_calls++;
    if (out_symbol_rate_hz) {
        *out_symbol_rate_hz = 4800;
    }
    if (out_levels) {
        *out_levels = 4;
    }
    if (out_channel_profile) {
        *out_channel_profile = 5;
    }
    return -5;
}

static uint32_t
fake_stream_generation(void) {
    g_stream_generation_calls++;
    return 1234U;
}

static int
fake_stream_active(void) {
    g_stream_active_calls++;
    return 1;
}

static int
fake_apply_demod_profile(int cqpsk_enable, int symbol_rate_hz, int levels, int channel_profile, int ted_sps) {
    g_apply_demod_profile_calls++;
    g_apply_cqpsk_enable = cqpsk_enable;
    g_apply_symbol_rate_hz = symbol_rate_hz;
    g_apply_symbol_levels = levels;
    g_apply_symbol_channel_profile = channel_profile;
    g_apply_ted_sps = ted_sps;
    return 8;
}

static int
fake_cqpsk_status(int* out_cqpsk_enable, int* out_cqpsk_timing_active) {
    g_cqpsk_status_calls++;
    if (out_cqpsk_enable) {
        *out_cqpsk_enable = 1;
    }
    if (out_cqpsk_timing_active) {
        *out_cqpsk_timing_active = 3;
    }
    return -7;
}

static int
fake_cqpsk_reacquire(void) {
    g_cqpsk_reacquire_calls++;
    return 17;
}

static int
fake_cqpsk_timing_bias(void) {
    g_cqpsk_timing_bias_calls++;
    return 123;
}

static double
fake_snr_bias_evm(void) {
    g_snr_bias_calls++;
    return 9.87;
}

static double
fake_snr_c4fm_db(void) {
    g_snr_c4fm_calls++;
    return 12.34;
}

static double
fake_snr_c4fm_eye_db(void) {
    g_snr_c4fm_eye_calls++;
    return 56.78;
}

static double
fake_snr_cqpsk_db(void) {
    g_snr_cqpsk_calls++;
    return 23.45;
}

static double
fake_snr_gfsk_db(void) {
    g_snr_gfsk_calls++;
    return 34.56;
}

static double
fake_snr_gfsk_eye_db(void) {
    g_snr_gfsk_eye_calls++;
    return 43.21;
}

static double
fake_snr_qpsk_const_db(void) {
    g_snr_qpsk_const_calls++;
    return 45.67;
}

static void
fake_p25p1_ber_update(int ok_delta, int err_delta) {
    g_p25p1_ber_calls++;
    g_p25p1_ok_delta = ok_delta;
    g_p25p1_err_delta = err_delta;
}

static void
fake_p25p2_err_update(int slot, int facch_ok_delta, int facch_err_delta, int sacch_ok_delta, int sacch_err_delta,
                      int voice_err_delta) {
    g_p25p2_err_calls++;
    g_p25p2_slot = slot;
    g_p25p2_facch_ok_delta = facch_ok_delta;
    g_p25p2_facch_err_delta = facch_err_delta;
    g_p25p2_sacch_ok_delta = sacch_ok_delta;
    g_p25p2_sacch_err_delta = sacch_err_delta;
    g_p25p2_voice_err_delta = voice_err_delta;
}

static int
fake_input_level(dsd_input_level_snapshot* out) {
    g_input_level_calls++;
    if (out) {
        out->status = DSD_INPUT_LEVEL_HOT;
        out->source = DSD_INPUT_LEVEL_SOURCE_RTL_CU8;
        out->rms_dbfs = -18.0;
        out->peak_dbfs = -0.5;
        out->clip_pct = 0.0;
        out->sample_count = 4096U;
        out->updated = 12345;
    }
    return -9;
}

int
main(void) {
    /*
     * First verify wrapper defaults with no hook table installed, including the
     * built-in symbol-cache counter. Then install a full fake hook table and
     * assert that every wrapper forwards calls, return values, and out-params.
     */

    // Default behavior with hooks unset.
    dsd_rtl_stream_metrics_hooks_set(NULL);

    assert(dsd_rtl_stream_metrics_hook_output_rate_hz() == 0U);
    assert(dsd_rtl_stream_metrics_hook_output_kind() == 0);

    int symbol_rate_hz = -1;
    int symbol_levels = -1;
    int channel_profile = -1;
    assert(dsd_rtl_stream_metrics_hook_symbol_profile(&symbol_rate_hz, &symbol_levels, &channel_profile) == 0);
    assert(symbol_rate_hz == 0);
    assert(symbol_levels == 0);
    assert(channel_profile == 0);
    assert(dsd_rtl_stream_metrics_hook_stream_generation() == 0U);
    assert(dsd_rtl_stream_metrics_hook_stream_active() == 0);
    dsd_input_level_snapshot input_level = {
        .status = DSD_INPUT_LEVEL_CLIPPING,
        .source = DSD_INPUT_LEVEL_SOURCE_RTL_CU8,
        .rms_dbfs = -1.0,
        .peak_dbfs = 0.0,
        .clip_pct = 50.0,
        .sample_count = 99U,
        .updated = 77,
    };
    assert(dsd_rtl_stream_metrics_hook_input_level(&input_level) == 0);
    assert(input_level.status == DSD_INPUT_LEVEL_UNKNOWN);
    assert(input_level.source == DSD_INPUT_LEVEL_SOURCE_UNKNOWN);
    assert(input_level.sample_count == 0U);
    assert(dsd_rtl_stream_metrics_hook_apply_demod_profile(1, 6000, 4, 5, 8) == -1);

    int cqpsk = -1;
    int timing = -1;
    assert(dsd_rtl_stream_metrics_hook_cqpsk_status(&cqpsk, &timing) == 0);
    assert(cqpsk == 0);
    assert(timing == 0);
    assert(dsd_rtl_stream_metrics_hook_request_cqpsk_reacquire() == -1);

    assert(dsd_rtl_stream_metrics_hook_cqpsk_timing_bias() == 0);

    assert(dsd_rtl_stream_metrics_hook_snr_bias_evm() == 2.43);

    assert(dsd_rtl_stream_metrics_hook_snr_c4fm_db() == -100.0);
    assert(dsd_rtl_stream_metrics_hook_snr_c4fm_eye_db() == -100.0);
    assert(dsd_rtl_stream_metrics_hook_snr_cqpsk_db() == -100.0);
    assert(dsd_rtl_stream_metrics_hook_snr_gfsk_db() == -100.0);
    assert(dsd_rtl_stream_metrics_hook_snr_gfsk_eye_db() == -100.0);
    assert(dsd_rtl_stream_metrics_hook_snr_qpsk_const_db() == -100.0);

    dsd_rtl_stream_metrics_hook_p25p1_ber_update(1, 0);
    dsd_rtl_stream_metrics_hook_p25p2_err_update(0, 1, 0, 0, 0, 0);

    dsd_rtl_stream_metrics_hook_symbol_cache_pending_reset();
    assert(dsd_rtl_stream_metrics_hook_symbol_cache_pending() == 0);
    dsd_rtl_stream_metrics_hook_symbol_cache_pending_delta(3);
    assert(dsd_rtl_stream_metrics_hook_symbol_cache_pending() == 3);
    dsd_rtl_stream_metrics_hook_symbol_cache_pending_delta(-1);
    assert(dsd_rtl_stream_metrics_hook_symbol_cache_pending() == 2);
    dsd_rtl_stream_metrics_hook_symbol_cache_pending_delta(-5);
    assert(dsd_rtl_stream_metrics_hook_symbol_cache_pending() == 0);

    // Installed hooks should be invoked through wrappers.
    g_output_rate_calls = 0;
    g_output_kind_calls = 0;
    g_symbol_profile_calls = 0;
    g_stream_generation_calls = 0;
    g_stream_active_calls = 0;
    g_apply_demod_profile_calls = 0;
    g_cqpsk_status_calls = 0;
    g_cqpsk_reacquire_calls = 0;
    g_cqpsk_timing_bias_calls = 0;
    g_snr_bias_calls = 0;
    g_snr_c4fm_calls = 0;
    g_snr_c4fm_eye_calls = 0;
    g_snr_cqpsk_calls = 0;
    g_snr_gfsk_calls = 0;
    g_snr_gfsk_eye_calls = 0;
    g_snr_qpsk_const_calls = 0;
    g_p25p1_ber_calls = 0;
    g_p25p2_err_calls = 0;
    g_input_level_calls = 0;
    g_apply_cqpsk_enable = 0;
    g_apply_symbol_rate_hz = 0;
    g_apply_symbol_levels = 0;
    g_apply_symbol_channel_profile = 0;
    g_apply_ted_sps = 0;

    dsd_rtl_stream_metrics_hooks hooks = {0};
    hooks.output_rate_hz = fake_output_rate_hz;
    hooks.output_kind = fake_output_kind;
    hooks.symbol_profile = fake_symbol_profile;
    hooks.stream_generation = fake_stream_generation;
    hooks.stream_active = fake_stream_active;
    hooks.apply_demod_profile = fake_apply_demod_profile;
    hooks.cqpsk_status = fake_cqpsk_status;
    hooks.request_cqpsk_reacquire = fake_cqpsk_reacquire;
    hooks.cqpsk_timing_bias = fake_cqpsk_timing_bias;
    hooks.snr_bias_evm = fake_snr_bias_evm;
    hooks.snr_c4fm_db = fake_snr_c4fm_db;
    hooks.snr_c4fm_eye_db = fake_snr_c4fm_eye_db;
    hooks.snr_cqpsk_db = fake_snr_cqpsk_db;
    hooks.snr_gfsk_db = fake_snr_gfsk_db;
    hooks.snr_gfsk_eye_db = fake_snr_gfsk_eye_db;
    hooks.snr_qpsk_const_db = fake_snr_qpsk_const_db;
    hooks.p25p1_ber_update = fake_p25p1_ber_update;
    hooks.p25p2_err_update = fake_p25p2_err_update;
    hooks.input_level = fake_input_level;
    dsd_rtl_stream_metrics_hooks_set(&hooks);

    assert(dsd_rtl_stream_metrics_hook_output_rate_hz() == 24000U);
    assert(g_output_rate_calls == 1);
    assert(dsd_rtl_stream_metrics_hook_output_kind() == 1);
    assert(g_output_kind_calls == 1);

    symbol_rate_hz = 0;
    symbol_levels = 0;
    channel_profile = 0;
    assert(dsd_rtl_stream_metrics_hook_symbol_profile(&symbol_rate_hz, &symbol_levels, &channel_profile) == -5);
    assert(g_symbol_profile_calls == 1);
    assert(symbol_rate_hz == 4800);
    assert(symbol_levels == 4);
    assert(channel_profile == 5);
    assert(dsd_rtl_stream_metrics_hook_stream_generation() == 1234U);
    assert(g_stream_generation_calls == 1);
    assert(dsd_rtl_stream_metrics_hook_stream_active() == 1);
    assert(g_stream_active_calls == 1);
    input_level = (dsd_input_level_snapshot){0};
    assert(dsd_rtl_stream_metrics_hook_input_level(&input_level) == -9);
    assert(g_input_level_calls == 1);
    assert(input_level.status == DSD_INPUT_LEVEL_HOT);
    assert(input_level.source == DSD_INPUT_LEVEL_SOURCE_RTL_CU8);
    assert(input_level.sample_count == 4096U);
    assert(input_level.updated == 12345);

    assert(dsd_rtl_stream_metrics_hook_apply_demod_profile(1, 6000, 4, 5, 8) == 8);
    assert(g_apply_demod_profile_calls == 1);
    assert(g_apply_cqpsk_enable == 1);
    assert(g_apply_symbol_rate_hz == 6000);
    assert(g_apply_symbol_levels == 4);
    assert(g_apply_symbol_channel_profile == 5);
    assert(g_apply_ted_sps == 8);

    // Out-parameter hooks must report both call counts and returned values.
    cqpsk = timing = 0;
    assert(dsd_rtl_stream_metrics_hook_cqpsk_status(&cqpsk, &timing) == -7);
    assert(g_cqpsk_status_calls == 1);
    assert(cqpsk == 1);
    assert(timing == 3);
    assert(dsd_rtl_stream_metrics_hook_request_cqpsk_reacquire() == 17);
    assert(g_cqpsk_reacquire_calls == 1);

    assert(dsd_rtl_stream_metrics_hook_cqpsk_timing_bias() == 123);
    assert(g_cqpsk_timing_bias_calls == 1);

    assert(dsd_rtl_stream_metrics_hook_snr_bias_evm() == 9.87);
    assert(g_snr_bias_calls == 1);

    assert(dsd_rtl_stream_metrics_hook_snr_c4fm_db() == 12.34);
    assert(g_snr_c4fm_calls == 1);

    assert(dsd_rtl_stream_metrics_hook_snr_c4fm_eye_db() == 56.78);
    assert(g_snr_c4fm_eye_calls == 1);

    assert(dsd_rtl_stream_metrics_hook_snr_cqpsk_db() == 23.45);
    assert(g_snr_cqpsk_calls == 1);

    assert(dsd_rtl_stream_metrics_hook_snr_gfsk_db() == 34.56);
    assert(g_snr_gfsk_calls == 1);

    assert(dsd_rtl_stream_metrics_hook_snr_gfsk_eye_db() == 43.21);
    assert(g_snr_gfsk_eye_calls == 1);

    assert(dsd_rtl_stream_metrics_hook_snr_qpsk_const_db() == 45.67);
    assert(g_snr_qpsk_const_calls == 1);

    g_p25p1_ok_delta = 0;
    g_p25p1_err_delta = 0;
    dsd_rtl_stream_metrics_hook_p25p1_ber_update(7, 9);
    assert(g_p25p1_ber_calls == 1);
    assert(g_p25p1_ok_delta == 7);
    assert(g_p25p1_err_delta == 9);

    g_p25p2_slot = 0;
    g_p25p2_facch_ok_delta = 0;
    g_p25p2_facch_err_delta = 0;
    g_p25p2_sacch_ok_delta = 0;
    g_p25p2_sacch_err_delta = 0;
    g_p25p2_voice_err_delta = 0;
    dsd_rtl_stream_metrics_hook_p25p2_err_update(1, 2, 3, 4, 5, 6);
    assert(g_p25p2_err_calls == 1);
    assert(g_p25p2_slot == 1);
    assert(g_p25p2_facch_ok_delta == 2);
    assert(g_p25p2_facch_err_delta == 3);
    assert(g_p25p2_sacch_ok_delta == 4);
    assert(g_p25p2_sacch_err_delta == 5);
    assert(g_p25p2_voice_err_delta == 6);

    return 0;
}
