// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/dsp/demod_state.h>
#include <dsd-neo/io/rtl_demod_config.h>
#include <dsd-neo/io/rtl_metrics.h>
#include <dsd-neo/io/rtl_stream_c.h>
#include <dsd-neo/runtime/ring.h>
#include <stdint.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"

extern demod_state demod;
extern std::atomic<double> g_snr_c4fm_db;
extern std::atomic<double> g_snr_gfsk_db;
extern std::atomic<double> g_snr_qpsk_db;

static int
is_fsk_output_kind(int kind) {
    return kind == DSD_DEMOD_OUTPUT_FSK_DISCRIMINATOR;
}

static int
expect_int_eq(const char* label, int got, int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got=%d want=%d\n", label, got, want);
        return 1;
    }
    return 0;
}

static int
expect_double_near(const char* label, double got, double want, double tolerance) {
    if (fabs(got - want) > tolerance) {
        DSD_FPRINTF(stderr, "%s: got=%f want=%f tolerance=%f\n", label, got, want, tolerance);
        return 1;
    }
    return 0;
}

static int
expect_size_eq(const char* label, size_t got, size_t want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got=%zu want=%zu\n", label, got, want);
        return 1;
    }
    return 0;
}

static int
expect_generation_eq(const char* label, uint32_t before, uint32_t after) {
    if (before != after) {
        DSD_FPRINTF(stderr, "%s: before=%u after=%u\n", label, before, after);
        return 1;
    }
    return 0;
}

static int
expect_generation_changed(const char* label, uint32_t before, uint32_t after) {
    if (before == after) {
        DSD_FPRINTF(stderr, "%s: before=%u after=%u\n", label, before, after);
        return 1;
    }
    return 0;
}

static int
expect_sps(const char* label, const dsd_opts& opts, int rate_hz, int override_sps, int want_sps, int want_profile) {
    static demod_state demod;
    output_state output;
    DSD_MEMSET(&demod, 0, sizeof(demod));
    DSD_MEMSET(&output, 0, sizeof(output));
    demod.cqpsk_enable = opts.mod_qpsk ? 1 : 0;
    demod.rate_out = rate_hz;
    demod.ted_sps_override = override_sps;
    output.rate = static_cast<unsigned int>(rate_hz);

    rtl_demod_maybe_refresh_ted_sps_after_rate_change(&demod, &opts, &output);
    if (demod.ted_sps != want_sps) {
        DSD_FPRINTF(stderr, "%s: got ted_sps=%d want=%d\n", label, demod.ted_sps, want_sps);
        return 1;
    }
    if (want_profile >= 0 && demod.channel_lpf_profile != want_profile) {
        DSD_FPRINTF(stderr, "%s: got channel_lpf_profile=%d want=%d\n", label, demod.channel_lpf_profile, want_profile);
        return 1;
    }
    return 0;
}

static int
expect_output_kind(const char* label, const dsd_opts& opts, int want_kind, int want_sym_rate, int want_levels) {
    demod_state* demod = static_cast<demod_state*>(std::calloc(1, sizeof(*demod)));
    output_state output;
    DSD_MEMSET(&output, 0, sizeof(output));
    output.rate = 48000U;
    if (!demod) {
        DSD_FPRINTF(stderr, "%s: allocation failed\n", label);
        return 1;
    }

    rtl_demod_init_for_mode(demod, &output, &opts, 48000);
    int rc = 0;
    if (demod->output_kind != want_kind) {
        DSD_FPRINTF(stderr, "%s: got output_kind=%d want=%d\n", label, demod->output_kind, want_kind);
        rc = 1;
    }
    if (demod->symbol_rate_hz != want_sym_rate) {
        DSD_FPRINTF(stderr, "%s: got symbol_rate_hz=%d want=%d\n", label, demod->symbol_rate_hz, want_sym_rate);
        rc = 1;
    }
    if (demod->symbol_levels != want_levels) {
        DSD_FPRINTF(stderr, "%s: got symbol_levels=%d want=%d\n", label, demod->symbol_levels, want_levels);
        rc = 1;
    }
    if (is_fsk_output_kind(want_kind)) {
        if (demod->cqpsk_enable != 0 || demod->ted_enabled != 0) {
            DSD_FPRINTF(stderr, "%s: FSK path left CQPSK timing enabled\n", label);
            rc = 1;
        }
    }
    if (want_kind == DSD_DEMOD_OUTPUT_SYMBOL_CQPSK && demod->ted_enabled != 1) {
        DSD_FPRINTF(stderr, "%s: CQPSK symbol path did not force TED on\n", label);
        rc = 1;
    }

    rtl_demod_maybe_update_resampler_after_rate_change(demod, &output, 48000);
    if ((is_fsk_output_kind(want_kind) || want_kind == DSD_DEMOD_OUTPUT_SYMBOL_CQPSK) && output.rate != 48000U) {
        DSD_FPRINTF(stderr, "%s: symbol output changed public output rate to %u\n", label, output.rate);
        rc = 1;
    }

    rtl_demod_cleanup(demod);
    std::free(demod);
    return rc;
}

static int
expect_configured_channel_profile(const char* label, const dsd_opts& opts, int rtl_dsp_bw_hz, int want_profile) {
    demod_state* demod = static_cast<demod_state*>(std::calloc(1, sizeof(*demod)));
    output_state output;
    DSD_MEMSET(&output, 0, sizeof(output));
    output.rate = static_cast<unsigned int>(rtl_dsp_bw_hz);
    if (!demod) {
        DSD_FPRINTF(stderr, "%s: allocation failed\n", label);
        return 1;
    }

    static dsd_opts mutable_opts;
    mutable_opts = opts;
    rtl_demod_init_for_mode(demod, &output, &mutable_opts, rtl_dsp_bw_hz);
    rtl_demod_config_from_env_and_opts(demod, &mutable_opts);
    rtl_demod_select_defaults_for_mode(demod, &mutable_opts, &output);

    int rc = 0;
    if (demod->channel_lpf_enable != 1) {
        DSD_FPRINTF(stderr, "%s: channel_lpf_enable=%d want=1\n", label, demod->channel_lpf_enable);
        rc = 1;
    }
    if (demod->channel_lpf_profile != want_profile) {
        DSD_FPRINTF(stderr, "%s: got channel_lpf_profile=%d want=%d\n", label, demod->channel_lpf_profile,
                    want_profile);
        rc = 1;
    }

    rtl_demod_cleanup(demod);
    std::free(demod);
    return rc;
}

static int
expect_configured_mode(const char* label, const dsd_opts& opts, int rtl_dsp_bw_hz, int want_kind, int want_sym_rate,
                       int want_levels, int want_profile) {
    demod_state* demod = static_cast<demod_state*>(std::calloc(1, sizeof(*demod)));
    output_state output;
    DSD_MEMSET(&output, 0, sizeof(output));
    output.rate = static_cast<unsigned int>(rtl_dsp_bw_hz);
    if (!demod) {
        DSD_FPRINTF(stderr, "%s: allocation failed\n", label);
        return 1;
    }

    static dsd_opts mutable_opts;
    mutable_opts = opts;
    rtl_demod_init_for_mode(demod, &output, &mutable_opts, rtl_dsp_bw_hz);
    rtl_demod_config_from_env_and_opts(demod, &mutable_opts);
    rtl_demod_select_defaults_for_mode(demod, &mutable_opts, &output);
    rtl_demod_maybe_update_resampler_after_rate_change(demod, &output, rtl_dsp_bw_hz);

    int rc = 0;
    if (demod->output_kind != want_kind) {
        DSD_FPRINTF(stderr, "%s: got output_kind=%d want=%d\n", label, demod->output_kind, want_kind);
        rc = 1;
    }
    if (demod->symbol_rate_hz != want_sym_rate) {
        DSD_FPRINTF(stderr, "%s: got symbol_rate_hz=%d want=%d\n", label, demod->symbol_rate_hz, want_sym_rate);
        rc = 1;
    }
    if (demod->symbol_levels != want_levels) {
        DSD_FPRINTF(stderr, "%s: got symbol_levels=%d want=%d\n", label, demod->symbol_levels, want_levels);
        rc = 1;
    }
    if (demod->channel_lpf_enable != 1) {
        DSD_FPRINTF(stderr, "%s: channel_lpf_enable=%d want=1\n", label, demod->channel_lpf_enable);
        rc = 1;
    }
    if (demod->channel_lpf_profile != want_profile) {
        DSD_FPRINTF(stderr, "%s: got channel_lpf_profile=%d want=%d\n", label, demod->channel_lpf_profile,
                    want_profile);
        rc = 1;
    }
    if (is_fsk_output_kind(want_kind)) {
        if (demod->cqpsk_enable != 0 || demod->ted_enabled != 0) {
            DSD_FPRINTF(stderr, "%s: FSK path left CQPSK timing enabled\n", label);
            rc = 1;
        }
    }
    if (want_kind == DSD_DEMOD_OUTPUT_SYMBOL_CQPSK && demod->ted_enabled != 1) {
        DSD_FPRINTF(stderr, "%s: CQPSK symbol path did not force TED on\n", label);
        rc = 1;
    }
    if ((is_fsk_output_kind(want_kind) || want_kind == DSD_DEMOD_OUTPUT_SYMBOL_CQPSK) && output.rate != rtl_dsp_bw_hz) {
        DSD_FPRINTF(stderr, "%s: symbol output changed public output rate to %u\n", label, output.rate);
        rc = 1;
    }

    rtl_demod_cleanup(demod);
    std::free(demod);
    return rc;
}

static int
expect_live_symbol_status(void) {
    int rc = 0;

    DSD_MEMSET(&demod, 0, sizeof(demod));
    demod.output_kind = DSD_DEMOD_OUTPUT_FSK_DISCRIMINATOR;
    demod.symbol_rate_hz = 4800;
    demod.symbol_levels = 4;

    int cq = -1;
    int timing = -1;
    rtl_stream_get_cqpsk_status(&cq, &timing);
    if (cq != 0 || timing != 0 || demod.ted_enabled != 0) {
        DSD_FPRINTF(stderr, "FSK discriminator output reported CQPSK timing active\n");
        rc = 1;
    }

    DSD_MEMSET(&demod, 0, sizeof(demod));
    rtl_stream_toggle_cqpsk(1);

    cq = -1;
    timing = -1;
    rtl_stream_get_cqpsk_status(&cq, &timing);
    if (cq != 1 || timing != 1 || demod.ted_enabled != 1) {
        DSD_FPRINTF(stderr, "CQPSK symbol output did not force CQPSK timing active\n");
        rc = 1;
    }

    DSD_MEMSET(&demod, 0, sizeof(demod));
    demod.output_kind = DSD_DEMOD_OUTPUT_AUDIO_MONITOR;

    cq = -1;
    timing = -1;
    rtl_stream_get_cqpsk_status(&cq, &timing);
    if (cq != 0 || timing != 0) {
        DSD_FPRINTF(stderr, "Audio monitor output reported CQPSK timing active\n");
        rc = 1;
    }

    return rc;
}

static int
expect_cqpsk_toggle_clears_output_contract_backlog(void) {
    int failed = 0;
    rtl_stream_test_cqpsk_toggle_result result = {};

    int rc = rtl_stream_test_cqpsk_toggle_output_clear(0, 1, 1, 11U, 5, &result);
    failed |= expect_int_eq("FSK to CQPSK output clear helper rc", rc, 0);
    failed |= expect_generation_changed("FSK to CQPSK bumps output generation", result.generation_before,
                                        result.generation_after);
    failed |= expect_size_eq("FSK to CQPSK clears queued output", result.used_after, 0U);
    failed |= expect_int_eq("FSK to CQPSK clears cached symbols", result.cache_pending_after, 0);
    failed |=
        expect_int_eq("FSK to CQPSK selects CQPSK symbols", result.output_kind_after, RTL_STREAM_OUTPUT_SYMBOL_CQPSK);
    failed |= expect_int_eq("FSK to CQPSK does not queue FSK reset", result.fsk_reset_pending_after_toggle, 0);
    failed |= expect_int_eq("FSK to CQPSK reset not consumed", result.reset_consumed, 0);
    failed |= expect_int_eq("FSK to CQPSK leaves FSK modem history untouched", result.have_prev_after_consume, 1);

    result = {};
    rc = rtl_stream_test_cqpsk_toggle_output_clear(1, 0, 1, 13U, 6, &result);
    failed |= expect_int_eq("CQPSK to FSK output clear helper rc", rc, 0);
    failed |= expect_generation_changed("CQPSK to FSK bumps output generation", result.generation_before,
                                        result.generation_after);
    failed |= expect_size_eq("CQPSK to FSK clears queued output", result.used_after, 0U);
    failed |= expect_int_eq("CQPSK to FSK clears cached symbols", result.cache_pending_after, 0);
    failed |= expect_int_eq("CQPSK to FSK selects FSK discriminator", result.output_kind_after,
                            RTL_STREAM_OUTPUT_FSK_DISCRIMINATOR);
    failed |= expect_int_eq("CQPSK to FSK queues FSK reset", result.fsk_reset_pending_after_toggle, 1);
    failed |= expect_int_eq("CQPSK to FSK reset consumed", result.reset_consumed, 1);
    failed |= expect_int_eq("CQPSK to FSK clears FSK modem history", result.have_prev_after_consume, 0);

    result = {};
    rc = rtl_stream_test_cqpsk_toggle_output_clear(0, 0, 1, 7U, 3, &result);
    failed |= expect_int_eq("FSK no-op CQPSK toggle helper rc", rc, 0);
    failed |= expect_generation_eq("FSK no-op CQPSK toggle keeps output generation", result.generation_before,
                                   result.generation_after);
    failed |= expect_size_eq("FSK no-op CQPSK toggle leaves queued output", result.used_after, 7U);
    failed |= expect_int_eq("FSK no-op CQPSK toggle leaves cached symbols", result.cache_pending_after, 3);
    failed |= expect_int_eq("FSK no-op CQPSK toggle keeps FSK discriminator", result.output_kind_after,
                            RTL_STREAM_OUTPUT_FSK_DISCRIMINATOR);
    failed |= expect_int_eq("FSK no-op CQPSK toggle leaves reset unqueued", result.fsk_reset_pending_after_toggle, 0);
    failed |= expect_int_eq("FSK no-op CQPSK toggle reset not consumed", result.reset_consumed, 0);
    failed |= expect_int_eq("FSK no-op CQPSK toggle keeps FSK modem history", result.have_prev_after_consume, 1);

    return failed;
}

static int
expect_steady_state_watermark_disabled(const char* label, const char* audio_in_dev) {
    int enabled = rtl_stream_test_steady_state_watermark_enabled(audio_in_dev);
    if (enabled != 0) {
        DSD_FPRINTF(stderr, "%s: steady-state watermark enabled=%d want=0\n", label, enabled);
        return 1;
    }
    return 0;
}

static int
expect_cqpsk_toggle_restores_fsk_channel_profile(void) {
    int rc = 0;

    DSD_MEMSET(&demod, 0, sizeof(demod));
    demod.output_kind = DSD_DEMOD_OUTPUT_SYMBOL_CQPSK;
    demod.cqpsk_enable = 1;
    demod.channel_lpf_profile = DSD_CH_LPF_PROFILE_P25_CQPSK;
    demod.symbol_rate_hz = 4800;
    demod.symbol_levels = 4;
    rtl_stream_toggle_cqpsk(0);
    if (demod.channel_lpf_profile != DSD_CH_LPF_PROFILE_12K5) {
        DSD_FPRINTF(stderr, "CQPSK off for 4.8 ksps 4FSK restored profile=%d want 12K5\n", demod.channel_lpf_profile);
        rc = 1;
    }

    DSD_MEMSET(&demod, 0, sizeof(demod));
    demod.output_kind = DSD_DEMOD_OUTPUT_SYMBOL_CQPSK;
    demod.cqpsk_enable = 1;
    demod.channel_lpf_profile = DSD_CH_LPF_PROFILE_P25_CQPSK;
    demod.symbol_rate_hz = 2400;
    demod.symbol_levels = 4;
    rtl_stream_toggle_cqpsk(0);
    if (demod.channel_lpf_profile != DSD_CH_LPF_PROFILE_6K25) {
        DSD_FPRINTF(stderr, "CQPSK off for 2.4 ksps 4FSK restored profile=%d want 6K25\n", demod.channel_lpf_profile);
        rc = 1;
    }

    DSD_MEMSET(&demod, 0, sizeof(demod));
    demod.output_kind = DSD_DEMOD_OUTPUT_SYMBOL_CQPSK;
    demod.cqpsk_enable = 1;
    demod.channel_lpf_profile = DSD_CH_LPF_PROFILE_P25_CQPSK;
    demod.symbol_rate_hz = 9600;
    demod.symbol_levels = 2;
    rtl_stream_toggle_cqpsk(0);
    if (demod.channel_lpf_profile != DSD_CH_LPF_PROFILE_PROVOICE) {
        DSD_FPRINTF(stderr, "CQPSK off for 9.6 ksps binary FSK restored profile=%d want ProVoice\n",
                    demod.channel_lpf_profile);
        rc = 1;
    }

    return rc;
}

static int
expect_rtl_metrics_do_not_nudge_cqpsk_bandedge(void) {
    DSD_MEMSET(&demod, 0, sizeof(demod));
    demod.cqpsk_enable = 1;
    demod.rate_out = 48000;
    demod.ted_sps = 10;
    demod.fll_band_edge_state.initialized = 1;
    demod.fll_band_edge_state.min_freq = -1.0f;
    demod.fll_band_edge_state.max_freq = 1.0f;
    demod.fll_band_edge_state.freq = 0.12345f;
    demod.costas_state.initialized = 1;
    demod.ted_state.lock_count = 32;
    demod.ted_state.lock_accum = 32.0f;

    const float want = demod.fll_band_edge_state.freq;
    g_snr_qpsk_db.store(30.0, std::memory_order_relaxed);

    static float iq[2048];
    const float kTwoPi = 6.28318530717958647692f;
    const float tone_hz = 1000.0f;
    const float rate_hz = 48000.0f;
    for (int n = 0; n < 1024; n++) {
        float phase = kTwoPi * tone_hz * (float)n / rate_hz;
        iq[(size_t)(n << 1)] = cosf(phase);
        iq[(size_t)(n << 1) + 1] = sinf(phase);
    }

    rtl_metrics_update_spectrum_from_iq(iq, 2048, 48000);
    g_snr_qpsk_db.store(-100.0, std::memory_order_relaxed);

    if (fabsf(demod.fll_band_edge_state.freq - want) > 1e-7f) {
        DSD_FPRINTF(stderr, "RTL metrics nudged CQPSK band-edge freq=%f want=%f\n", demod.fll_band_edge_state.freq,
                    want);
        return 1;
    }
    return 0;
}

static int
expect_rtl_metrics_exports_and_toggles(void) {
    int rc = 0;
    const double kTwoPi = 6.28318530717958647692;
    const int rate_hz = 48000;

    // Spectrum sizing clamps and rejects malformed snapshots before publishing data.
    rc |= expect_int_eq("RTL spectrum clamps below minimum", rtl_stream_spectrum_set_size(1), 64);
    rc |= expect_int_eq("RTL spectrum reports clamped minimum", rtl_stream_spectrum_get_size(), 64);
    rc |= expect_int_eq("RTL spectrum rounds up to power-of-two", rtl_stream_spectrum_set_size(65), 128);
    rc |= expect_int_eq("RTL spectrum clamps above maximum", rtl_stream_spectrum_set_size(2048), 1024);
    rc |= expect_int_eq("RTL spectrum clamps get size", rtl_stream_spectrum_get_size(), 1024);

    float bins[8] = {};
    int out_rate = -1;
    rc |= expect_int_eq("RTL spectrum rejects null output", rtl_stream_spectrum_get(nullptr, 8, &out_rate), 0);
    rc |= expect_int_eq("RTL spectrum rejects zero bins", rtl_stream_spectrum_get(bins, 0, &out_rate), 0);
    rc |= expect_int_eq("RTL spectrum test size", rtl_stream_spectrum_set_size(64), 64);

    DSD_MEMSET(&demod, 0, sizeof(demod));
    demod.cqpsk_enable = 1;
    demod.rate_out = rate_hz;
    demod.ted_sps = 5;
    demod.fll_band_edge_state.initialized = 1;
    demod.fll_band_edge_state.max_freq = 1.0f;
    demod.fll_band_edge_state.freq = 0.010f;
    demod.costas_state.initialized = 1;
    demod.costas_state.freq = 0.020f;
    demod.costas_state.phase = 0.75f;
    demod.costas_state.error = -0.25f;
    demod.costas_state.error_smooth = 0.125f;
    demod.ted_state.lock_count = 32;
    demod.ted_state.lock_accum = 32.0f;
    demod.costas_err_avg_q14 = 1234;
    demod.costas_err_raw_avg_q14 = 2345;
    demod.costas_conf_avg_q14 = 12000;
    demod.costas_zero_conf_pct = 7;

    // Populate a stable spectrum and verify demodulator/costas metrics exported from it.
    static float iq[128];
    for (int n = 0; n < 64; n++) {
        double phase = kTwoPi * 1000.0 * (double)n / (double)rate_hz;
        iq[(size_t)(n << 1)] = (float)cos(phase);
        iq[(size_t)(n << 1) + 1] = (float)sin(phase);
    }
    rtl_metrics_update_spectrum_from_iq(iq, 128, rate_hz);

    out_rate = -1;
    rc |= expect_int_eq("RTL spectrum copies requested bins", rtl_stream_spectrum_get(bins, 8, &out_rate), 8);
    rc |= expect_int_eq("RTL spectrum publishes rate", out_rate, rate_hz);
    rc |= expect_int_eq("RTL metrics publishes demod rate", rtl_stream_get_demod_rate_hz(), rate_hz);
    rc |= expect_int_eq("RTL metrics publishes Costas error", rtl_stream_get_costas_err_q14(), 1234);

    rtl_stream_costas_metrics metrics = {};
    rc |= expect_int_eq("RTL Costas metrics reject null", rtl_stream_get_costas_metrics(nullptr), -1);
    rc |= expect_int_eq("RTL Costas metrics snapshot", rtl_stream_get_costas_metrics(&metrics), 0);
    rc |= expect_int_eq("RTL Costas metrics smooth", metrics.err_smooth_avg_q14, 1234);
    rc |= expect_int_eq("RTL Costas metrics raw", metrics.err_raw_avg_q14, 2345);
    rc |= expect_int_eq("RTL Costas metrics confidence", metrics.confidence_avg_q14, 12000);
    rc |= expect_int_eq("RTL Costas metrics zero confidence", metrics.zero_conf_pct, 7);

    const double total_rad = 0.010 + (0.020 / 5.0);
    rc |=
        expect_double_near("RTL metrics NCO CFO", rtl_stream_get_cfo_hz(), total_rad * (double)rate_hz / kTwoPi, 0.05);
    rc |= expect_double_near("RTL metrics FLL band-edge CFO", rtl_stream_get_fll_band_edge_freq_hz(),
                             0.010 * (double)rate_hz / kTwoPi, 0.05);
    rc |= expect_int_eq("RTL metrics NCO q15", rtl_stream_get_nco_q15(), (int)lrint(total_rad * (32768.0 / kTwoPi)));
    int carrier_lock = rtl_stream_get_carrier_lock();
    if (carrier_lock != 0 && carrier_lock != 1) {
        DSD_FPRINTF(stderr, "RTL carrier lock returned non-boolean value=%d\n", carrier_lock);
        rc = 1;
    }
    (void)rtl_stream_get_residual_cfo_hz();

    // Resetting Costas state clears phase/error accumulators but preserves tuned frequency.
    rtl_stream_reset_costas();
    rc |= expect_double_near("RTL reset Costas preserves frequency", demod.costas_state.freq, 0.020, 1e-6);
    rc |= expect_double_near("RTL reset Costas clears phase", demod.costas_state.phase, 0.0, 1e-6);
    rc |= expect_double_near("RTL reset Costas clears error", demod.costas_state.error, 0.0, 1e-6);
    rc |= expect_double_near("RTL reset Costas clears smoothed error", demod.costas_state.error_smooth, 0.0, 1e-6);
    rc |= expect_double_near("RTL reset Costas diff prev real", demod.cqpsk_diff_prev_r, 1.0, 1e-6);
    rc |= expect_double_near("RTL reset Costas diff prev imag", demod.cqpsk_diff_prev_j, 0.0, 1e-6);
    rc |= expect_int_eq("RTL reset Costas clears exported error", rtl_stream_get_costas_err_q14(), 0);
    rc |= expect_int_eq("RTL reset Costas metrics snapshot", rtl_stream_get_costas_metrics(&metrics), 0);
    rc |= expect_int_eq("RTL reset Costas smooth metric", metrics.err_smooth_avg_q14, 0);
    rc |= expect_int_eq("RTL reset Costas raw metric", metrics.err_raw_avg_q14, 0);
    rc |= expect_int_eq("RTL reset Costas confidence metric", metrics.confidence_avg_q14, 0);
    rc |= expect_int_eq("RTL reset Costas zero confidence metric", metrics.zero_conf_pct, 0);

    g_snr_c4fm_db.store(11.25, std::memory_order_relaxed);
    g_snr_qpsk_db.store(12.50, std::memory_order_relaxed);
    g_snr_gfsk_db.store(13.75, std::memory_order_relaxed);
    rc |= expect_double_near("RTL C4FM SNR export", rtl_stream_get_snr_c4fm(), 11.25, 1e-9);
    rc |= expect_double_near("RTL CQPSK SNR export", rtl_stream_get_snr_cqpsk(), 12.50, 1e-9);
    rc |= expect_double_near("RTL GFSK SNR export", rtl_stream_get_snr_gfsk(), 13.75, 1e-9);
    g_snr_c4fm_db.store(-100.0, std::memory_order_relaxed);
    g_snr_qpsk_db.store(-100.0, std::memory_order_relaxed);
    g_snr_gfsk_db.store(-100.0, std::memory_order_relaxed);

    // User-facing tuner toggles should round-trip through the public control wrappers.
    rtl_stream_set_tuner_autogain(1);
    rc |= expect_int_eq("RTL tuner autogain on", rtl_stream_get_tuner_autogain(), 1);
    rtl_stream_set_tuner_autogain(0);
    rc |= expect_int_eq("RTL tuner autogain off", rtl_stream_get_tuner_autogain(), 0);

    rtl_stream_set_auto_ppm(1);
    rc |= expect_int_eq("RTL auto PPM user enable", rtl_stream_get_auto_ppm(), 1);
    rtl_stream_set_auto_ppm(0);
    rc |= expect_int_eq("RTL auto PPM user disable", rtl_stream_get_auto_ppm(), 0);

    int enabled = -1;
    double snr_db = 0.0;
    double df_hz = 0.0;
    double est_ppm = 0.0;
    int last_dir = 99;
    int cooldown = 99;
    int locked = 99;
    // Auto-PPM status and lock snapshots accept optional output pointers.
    rc |=
        expect_int_eq("RTL auto PPM accepts null status outputs",
                      rtl_stream_auto_ppm_get_status(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr), 0);
    rc |= expect_int_eq(
        "RTL auto PPM status snapshot",
        rtl_stream_auto_ppm_get_status(&enabled, &snr_db, &df_hz, &est_ppm, &last_dir, &cooldown, &locked), 0);
    int training = rtl_stream_auto_ppm_training_active();
    if (training != 0 && training != 1) {
        DSD_FPRINTF(stderr, "RTL auto PPM training returned non-boolean value=%d\n", training);
        rc = 1;
    }
    int ppm = 123;
    rc |= expect_int_eq("RTL auto PPM lock snapshot", rtl_stream_auto_ppm_get_lock(&ppm, &snr_db, &df_hz), 0);
    rc |= expect_int_eq("RTL auto PPM accepts null lock outputs",
                        rtl_stream_auto_ppm_get_lock(nullptr, nullptr, nullptr), 0);
    return rc;
}

static int
expect_public_control_wrapper_contracts(void) {
    int rc = 0;

    // Null/default wrapper calls must be safe before any demodulator state is active.
    rc |= expect_int_eq("RTL output rate rejects null context", (int)rtl_stream_output_rate(nullptr), 0);
    rc |= expect_int_eq("RTL monitor rate rejects null context", (int)rtl_stream_monitor_rate(nullptr), 0);
    rc |= expect_int_eq("RTL active state defaults inactive", rtl_stream_is_active(), 0);

    DSD_MEMSET(&demod, 0, sizeof(demod));
    demod.ted_state.e_ema = 0.25f;
    rc |= expect_int_eq("RTL timing bias exports Q14", rtl_stream_cqpsk_timing_bias(nullptr), 4096);
    demod.ted_state.e_ema = -0.5f;
    rc |= expect_int_eq("RTL timing bias preserves sign", rtl_stream_cqpsk_timing_bias(nullptr), -8192);

    // Symbol profile setters validate input and refresh the dependent demodulator fields.
    DSD_MEMSET(&demod, 0, sizeof(demod));
    demod.output_kind = DSD_DEMOD_OUTPUT_FSK_DISCRIMINATOR;
    demod.rate_out = 48000;
    demod.symbol_rate_hz = 4800;
    demod.symbol_levels = 4;
    demod.channel_lpf_profile = DSD_CH_LPF_PROFILE_12K5;
    demod.ted_sps = 10;

    int symbol_rate = -1;
    int levels = -1;
    int channel_profile = -1;
    rc |= expect_int_eq("RTL symbol profile rejects zero rate", rtl_stream_set_symbol_profile(0, 4, 0), -1);
    rc |= expect_int_eq("RTL symbol profile rejects unsupported levels", rtl_stream_set_symbol_profile(4800, 3, 0), -1);
    rc |= expect_int_eq("RTL symbol profile accepts null outputs",
                        rtl_stream_get_symbol_profile_full(nullptr, nullptr, nullptr), 0);
    rc |= expect_int_eq("RTL symbol profile accepts 2.4 ksps",
                        rtl_stream_set_symbol_profile(2400, 4, DSD_CH_LPF_PROFILE_6K25), 0);
    rc |= expect_int_eq("RTL symbol profile snapshot full",
                        rtl_stream_get_symbol_profile_full(&symbol_rate, &levels, &channel_profile), 0);
    rc |= expect_int_eq("RTL symbol profile rate", symbol_rate, 2400);
    rc |= expect_int_eq("RTL symbol profile levels", levels, 4);
    rc |= expect_int_eq("RTL symbol profile channel", channel_profile, DSD_CH_LPF_PROFILE_6K25);
    rc |= expect_int_eq("RTL symbol profile recalculates FSK SPS", demod.ted_sps, 20);
    rc |= expect_int_eq("RTL symbol profile marks Costas reset", demod.costas_reset_pending, 1);

    channel_profile = -1;
    rc |= expect_int_eq("RTL symbol profile ignores invalid channel profile",
                        rtl_stream_set_symbol_profile(9600, 2, 999), 0);
    rc |= expect_int_eq("RTL symbol profile snapshot compact", rtl_stream_get_symbol_profile(&symbol_rate, &levels), 0);
    rc |= expect_int_eq("RTL compact profile rate", symbol_rate, 9600);
    rc |= expect_int_eq("RTL compact profile levels", levels, 2);
    rc |= expect_int_eq("RTL invalid channel profile leaves previous channel",
                        rtl_stream_get_symbol_profile_full(nullptr, nullptr, &channel_profile), 0);
    rc |= expect_int_eq("RTL retained channel profile", channel_profile, DSD_CH_LPF_PROFILE_6K25);

    // TED controls clamp override and active SPS paths independently.
    DSD_MEMSET(&demod, 0, sizeof(demod));
    demod.rate_out = 48000;
    demod.symbol_levels = 4;
    demod.channel_lpf_profile = DSD_CH_LPF_PROFILE_12K5;
    demod.ted_sps = 10;
    demod.cqpsk_enable = 1;
    rtl_stream_set_ted_sps(1);
    rc |= expect_int_eq("RTL TED SPS clamps low override", rtl_stream_get_ted_sps_override(), 2);
    rc |= expect_int_eq("RTL TED SPS selects CQPSK LPF", demod.channel_lpf_profile, DSD_CH_LPF_PROFILE_P25_CQPSK);
    rtl_stream_clear_ted_sps_override();
    rc |= expect_int_eq("RTL TED SPS override clears", rtl_stream_get_ted_sps_override(), 0);
    rtl_stream_set_ted_sps(99);
    rc |= expect_int_eq("RTL TED SPS clamps high override", rtl_stream_get_ted_sps_override(), 64);
    rtl_stream_set_ted_sps_no_override(99);
    rc |= expect_int_eq("RTL TED SPS no-override clamps active SPS", rtl_stream_get_ted_sps(), 64);
    rc |= expect_int_eq("RTL TED SPS no-override leaves override", rtl_stream_get_ted_sps_override(), 64);
    rtl_stream_clear_ted_sps_override();

    rtl_stream_set_ted_gain(-1.0f);
    rc |= expect_double_near("RTL TED gain clamps low", rtl_stream_get_ted_gain(), 0.01, 1e-6);
    rtl_stream_set_ted_gain(1.0f);
    rc |= expect_double_near("RTL TED gain clamps high", rtl_stream_get_ted_gain(), 0.50, 1e-6);

    // IQ DC setup precharges from buffered samples and clamps shift configuration.
    DSD_MEMSET(&demod, 0, sizeof(demod));
    static float lowpassed[] = {1.0f, -3.0f, 5.0f, 7.0f};
    demod.lowpassed = lowpassed;
    demod.lp_len = 4;
    int shift = -1;
    rtl_stream_set_iq_dc(1, 3);
    rc |= expect_int_eq("RTL IQ DC enables", rtl_stream_get_iq_dc(&shift), 1);
    rc |= expect_int_eq("RTL IQ DC shift clamps low", shift, 6);
    rc |= expect_double_near("RTL IQ DC precharges I average", demod.iq_dc_avg_r, 3.0, 1e-6);
    rc |= expect_double_near("RTL IQ DC precharges Q average", demod.iq_dc_avg_i, 2.0, 1e-6);
    rtl_stream_set_iq_dc(-1, 99);
    rc |= expect_int_eq("RTL IQ DC negative enable keeps state", rtl_stream_get_iq_dc(&shift), 1);
    rc |= expect_int_eq("RTL IQ DC shift clamps high", shift, 15);
    rtl_stream_set_iq_dc(0, -1);
    rc |= expect_int_eq("RTL IQ DC disables without changing shift", rtl_stream_get_iq_dc(&shift), 0);
    rc |= expect_int_eq("RTL IQ DC disabled shift snapshot", shift, 15);

    rtl_stream_toggle_iq_balance(1);
    rc |= expect_int_eq("RTL IQ balance enables", rtl_stream_get_iq_balance(), 1);
    rtl_stream_toggle_iq_balance(0);
    rc |= expect_int_eq("RTL IQ balance disables", rtl_stream_get_iq_balance(), 0);

    // Decode health stays invalid while the stream is inactive even after error updates.
    rtl_stream_decode_health health = {};
    rc |= expect_int_eq("RTL decode health rejects null", rtl_stream_get_decode_health(nullptr), -1);
    rtl_stream_p25p1_ber_update(10, 5);
    rtl_stream_p25p2_err_update(1, 2, 3, 4, 5, 6);
    rc |= expect_int_eq("RTL inactive decode health snapshot", rtl_stream_get_decode_health(&health), 0);
    rc |= expect_int_eq("RTL inactive decode health invalid", health.valid, 0);
    rc |= expect_int_eq("RTL inactive P25P1 OK stays zero", (int)health.p25p1_fec_ok, 0);
    rc |= expect_int_eq("RTL inactive P25P2 FACCH OK stays zero", (int)health.p25p2_facch_ok, 0);

    return rc;
}

static int
expect_fsk_snr_sps_uses_active_profile(void) {
    int rc = 0;

    rc |= expect_int_eq("FSK SNR ignores stale TED SPS for ProVoice", rtl_stream_test_fsk_snr_sps(24000, 9600, 10), 3);
    rc |= expect_int_eq("FSK SNR ignores stale low TED SPS for 4.8k", rtl_stream_test_fsk_snr_sps(48000, 4800, 2), 10);
    return rc;
}

static int
expect_direct_output_open_rate_uses_demod_rate(void) {
    int rc = 0;
    unsigned int output_rate_hz = 0U;
    int resamp_enabled = -1;

    int helper_rc = rtl_stream_test_direct_output_rate_after_open_update(DSD_DEMOD_OUTPUT_FSK_DISCRIMINATOR, 24000,
                                                                         48000, &output_rate_hz, &resamp_enabled);
    rc |= expect_int_eq("FSK direct output rate helper rc", helper_rc, 0);
    rc |= expect_int_eq("FSK direct output publishes demod rate", (int)output_rate_hz, 24000);
    rc |= expect_int_eq("FSK direct output disables resampler", resamp_enabled, 0);

    output_rate_hz = 0U;
    resamp_enabled = -1;
    helper_rc = rtl_stream_test_direct_output_rate_after_open_update(DSD_DEMOD_OUTPUT_SYMBOL_CQPSK, 24000, 48000,
                                                                     &output_rate_hz, &resamp_enabled);
    rc |= expect_int_eq("CQPSK direct output rate helper rc", helper_rc, 0);
    rc |= expect_int_eq("CQPSK direct output publishes demod rate", (int)output_rate_hz, 24000);
    rc |= expect_int_eq("CQPSK direct output disables resampler", resamp_enabled, 0);
    return rc;
}

int
main(void) {
    int rc = 0;
    /*
     * Walk protocol families through the same demod configuration helper.
     * The assertions check symbol rate, output kind, and channel filter profile
     * so a mode-specific regression is visible without needing live SDR input.
     */
    static dsd_opts p25p2_qpsk;
    DSD_MEMSET(&p25p2_qpsk, 0, sizeof(p25p2_qpsk));
    p25p2_qpsk.frame_p25p2 = 1;
    p25p2_qpsk.mod_qpsk = 1;
    rc |= expect_sps("P25P2-only QPSK uses 6 ksps", p25p2_qpsk, 48000, 0, 8, DSD_CH_LPF_PROFILE_P25_CQPSK);
    rc |= expect_sps("P25P2-only QPSK uses 6 ksps at 24 kHz", p25p2_qpsk, 24000, 0, 4, DSD_CH_LPF_PROFILE_P25_CQPSK);

    static dsd_opts p25p1_qpsk;
    DSD_MEMSET(&p25p1_qpsk, 0, sizeof(p25p1_qpsk));
    p25p1_qpsk.frame_p25p1 = 1;
    p25p1_qpsk.mod_qpsk = 1;
    rc |= expect_sps("P25P1 QPSK uses 4.8 ksps", p25p1_qpsk, 48000, 0, 10, DSD_CH_LPF_PROFILE_P25_CQPSK);
    rc |= expect_sps("P25P1 QPSK uses 4.8 ksps at 24 kHz", p25p1_qpsk, 24000, 0, 5, DSD_CH_LPF_PROFILE_P25_CQPSK);

    static dsd_opts p25_trunk_qpsk;
    DSD_MEMSET(&p25_trunk_qpsk, 0, sizeof(p25_trunk_qpsk));
    p25_trunk_qpsk.frame_p25p1 = 1;
    p25_trunk_qpsk.frame_p25p2 = 1;
    p25_trunk_qpsk.mod_qpsk = 1;
    rc |= expect_sps("P25 trunk QPSK defaults to CC rate", p25_trunk_qpsk, 48000, 0, 10, DSD_CH_LPF_PROFILE_P25_CQPSK);
    rc |= expect_sps("P25 trunk TDMA override wins", p25_trunk_qpsk, 48000, 8, 8, DSD_CH_LPF_PROFILE_P25_CQPSK);

    static dsd_opts p25_c4fm;
    DSD_MEMSET(&p25_c4fm, 0, sizeof(p25_c4fm));
    p25_c4fm.frame_p25p1 = 1;
    rc |=
        expect_output_kind("P25 C4FM selects FSK discriminator", p25_c4fm, DSD_DEMOD_OUTPUT_FSK_DISCRIMINATOR, 4800, 4);
    rc |= expect_configured_mode("P25 C4FM uses P25 C4FM LPF", p25_c4fm, 48000, DSD_DEMOD_OUTPUT_FSK_DISCRIMINATOR,
                                 4800, 4, DSD_CH_LPF_PROFILE_P25_C4FM);
    rc |= expect_configured_mode("P25 C4FM keeps profile at 24 kHz", p25_c4fm, 24000,
                                 DSD_DEMOD_OUTPUT_FSK_DISCRIMINATOR, 4800, 4, DSD_CH_LPF_PROFILE_P25_C4FM);

    rc |= expect_output_kind("P25 QPSK selects CQPSK symbols", p25p1_qpsk, DSD_DEMOD_OUTPUT_SYMBOL_CQPSK, 4800, 4);
    rc |= expect_configured_mode("P25 QPSK uses P25 CQPSK LPF", p25p1_qpsk, 48000, DSD_DEMOD_OUTPUT_SYMBOL_CQPSK, 4800,
                                 4, DSD_CH_LPF_PROFILE_P25_CQPSK);
    rc |= expect_configured_mode("P25 QPSK keeps CQPSK LPF at 24 kHz", p25p1_qpsk, 24000, DSD_DEMOD_OUTPUT_SYMBOL_CQPSK,
                                 4800, 4, DSD_CH_LPF_PROFILE_P25_CQPSK);
    rc |= expect_configured_mode("P25P2 QPSK uses 6 ksps CQPSK LPF", p25p2_qpsk, 48000, DSD_DEMOD_OUTPUT_SYMBOL_CQPSK,
                                 6000, 4, DSD_CH_LPF_PROFILE_P25_CQPSK);
    rc |= expect_configured_mode("P25P2 QPSK keeps 6 ksps CQPSK LPF at 24 kHz", p25p2_qpsk, 24000,
                                 DSD_DEMOD_OUTPUT_SYMBOL_CQPSK, 6000, 4, DSD_CH_LPF_PROFILE_P25_CQPSK);

    // Narrowband and wideband FSK modes choose different channel LPF profiles.
    static dsd_opts nxdn48;
    DSD_MEMSET(&nxdn48, 0, sizeof(nxdn48));
    nxdn48.frame_nxdn48 = 1;
    rc |= expect_output_kind("NXDN48 selects 2400 FSK discriminator", nxdn48, DSD_DEMOD_OUTPUT_FSK_DISCRIMINATOR, 2400,
                             4);
    rc |= expect_configured_mode("NXDN48 uses 6.25 kHz LPF", nxdn48, 48000, DSD_DEMOD_OUTPUT_FSK_DISCRIMINATOR, 2400, 4,
                                 DSD_CH_LPF_PROFILE_6K25);
    rc |= expect_configured_mode("NXDN48 keeps 6.25 kHz LPF at 24 kHz", nxdn48, 24000,
                                 DSD_DEMOD_OUTPUT_FSK_DISCRIMINATOR, 2400, 4, DSD_CH_LPF_PROFILE_6K25);

    static dsd_opts nxdn96;
    DSD_MEMSET(&nxdn96, 0, sizeof(nxdn96));
    nxdn96.frame_nxdn96 = 1;
    rc |= expect_configured_mode("NXDN96 uses 12.5 kHz LPF", nxdn96, 48000, DSD_DEMOD_OUTPUT_FSK_DISCRIMINATOR, 4800, 4,
                                 DSD_CH_LPF_PROFILE_12K5);
    rc |= expect_configured_mode("NXDN96 keeps 12.5 kHz LPF at 24 kHz", nxdn96, 24000,
                                 DSD_DEMOD_OUTPUT_FSK_DISCRIMINATOR, 4800, 4, DSD_CH_LPF_PROFILE_12K5);

    static dsd_opts dmr;
    DSD_MEMSET(&dmr, 0, sizeof(dmr));
    dmr.frame_dmr = 1;
    rc |= expect_output_kind("DMR selects 4800 FSK discriminator", dmr, DSD_DEMOD_OUTPUT_FSK_DISCRIMINATOR, 4800, 4);
    rc |= expect_configured_channel_profile("DMR uses 12.5 kHz FSK channel LPF", dmr, 48000, DSD_CH_LPF_PROFILE_12K5);
    rc |= expect_configured_channel_profile("DMR keeps 12.5 kHz FSK channel LPF at 24 kHz", dmr, 24000,
                                            DSD_CH_LPF_PROFILE_12K5);

    static dsd_opts dstar;
    DSD_MEMSET(&dstar, 0, sizeof(dstar));
    dstar.frame_dstar = 1;
    rc |= expect_output_kind("D-STAR selects binary FSK discriminator", dstar, DSD_DEMOD_OUTPUT_FSK_DISCRIMINATOR, 4800,
                             2);
    rc |= expect_configured_mode("D-STAR uses 6.25 kHz LPF", dstar, 48000, DSD_DEMOD_OUTPUT_FSK_DISCRIMINATOR, 4800, 2,
                                 DSD_CH_LPF_PROFILE_6K25);
    rc |= expect_configured_mode("D-STAR keeps binary 6.25 kHz LPF at 24 kHz", dstar, 24000,
                                 DSD_DEMOD_OUTPUT_FSK_DISCRIMINATOR, 4800, 2, DSD_CH_LPF_PROFILE_6K25);

    static dsd_opts x2tdma;
    DSD_MEMSET(&x2tdma, 0, sizeof(x2tdma));
    x2tdma.frame_x2tdma = 1;
    rc |= expect_configured_mode("X2-TDMA uses 6 ksps 12.5 kHz LPF", x2tdma, 48000, DSD_DEMOD_OUTPUT_FSK_DISCRIMINATOR,
                                 6000, 4, DSD_CH_LPF_PROFILE_12K5);
    rc |= expect_configured_mode("X2-TDMA keeps 6 ksps 12.5 kHz LPF at 24 kHz", x2tdma, 24000,
                                 DSD_DEMOD_OUTPUT_FSK_DISCRIMINATOR, 6000, 4, DSD_CH_LPF_PROFILE_12K5);

    static dsd_opts ysf;
    DSD_MEMSET(&ysf, 0, sizeof(ysf));
    ysf.frame_ysf = 1;
    rc |= expect_configured_mode("YSF uses 12.5 kHz LPF", ysf, 48000, DSD_DEMOD_OUTPUT_FSK_DISCRIMINATOR, 4800, 4,
                                 DSD_CH_LPF_PROFILE_12K5);
    rc |= expect_configured_mode("YSF keeps 12.5 kHz LPF at 24 kHz", ysf, 24000, DSD_DEMOD_OUTPUT_FSK_DISCRIMINATOR,
                                 4800, 4, DSD_CH_LPF_PROFILE_12K5);

    static dsd_opts dpmr;
    DSD_MEMSET(&dpmr, 0, sizeof(dpmr));
    dpmr.frame_dpmr = 1;
    rc |= expect_configured_mode("dPMR uses 6.25 kHz LPF", dpmr, 48000, DSD_DEMOD_OUTPUT_FSK_DISCRIMINATOR, 2400, 4,
                                 DSD_CH_LPF_PROFILE_6K25);
    rc |= expect_configured_mode("dPMR keeps 6.25 kHz LPF at 24 kHz", dpmr, 24000, DSD_DEMOD_OUTPUT_FSK_DISCRIMINATOR,
                                 2400, 4, DSD_CH_LPF_PROFILE_6K25);

    static dsd_opts m17;
    DSD_MEMSET(&m17, 0, sizeof(m17));
    m17.frame_m17 = 1;
    rc |= expect_configured_mode("M17 uses 12.5 kHz LPF", m17, 48000, DSD_DEMOD_OUTPUT_FSK_DISCRIMINATOR, 4800, 4,
                                 DSD_CH_LPF_PROFILE_12K5);
    rc |= expect_configured_mode("M17 keeps 12.5 kHz LPF at 24 kHz", m17, 24000, DSD_DEMOD_OUTPUT_FSK_DISCRIMINATOR,
                                 4800, 4, DSD_CH_LPF_PROFILE_12K5);

    static dsd_opts provoice;
    DSD_MEMSET(&provoice, 0, sizeof(provoice));
    provoice.frame_provoice = 1;
    rc |= expect_configured_mode("ProVoice uses 9.6 ksps binary FSK", provoice, 48000,
                                 DSD_DEMOD_OUTPUT_FSK_DISCRIMINATOR, 9600, 2, DSD_CH_LPF_PROFILE_PROVOICE);
    rc |= expect_configured_mode("ProVoice keeps 9.6 ksps binary FSK at 24 kHz", provoice, 24000,
                                 DSD_DEMOD_OUTPUT_FSK_DISCRIMINATOR, 9600, 2, DSD_CH_LPF_PROFILE_PROVOICE);

    static dsd_opts auto_all;
    DSD_MEMSET(&auto_all, 0, sizeof(auto_all));
    auto_all.frame_p25p1 = 1;
    auto_all.frame_p25p2 = 1;
    auto_all.frame_dmr = 1;
    auto_all.frame_nxdn48 = 1;
    auto_all.frame_nxdn96 = 1;
    auto_all.frame_x2tdma = 1;
    auto_all.frame_ysf = 1;
    auto_all.frame_dstar = 1;
    auto_all.frame_dpmr = 1;
    auto_all.frame_provoice = 1;
    auto_all.frame_m17 = 1;
    rc |= expect_configured_mode("AUTO starts on 4.8 ksps wide 4FSK profile", auto_all, 48000,
                                 DSD_DEMOD_OUTPUT_FSK_DISCRIMINATOR, 4800, 4, DSD_CH_LPF_PROFILE_12K5);

    // Soapy inputs share tuning fields but must preserve the selected digital mode.
    static dsd_opts soapy_p25_c4fm;
    soapy_p25_c4fm = p25_c4fm;
    DSD_SNPRINTF(soapy_p25_c4fm.audio_in_dev, sizeof(soapy_p25_c4fm.audio_in_dev), "%s", "soapy");
    rc |= expect_output_kind("Soapy P25 C4FM selects FSK discriminator", soapy_p25_c4fm,
                             DSD_DEMOD_OUTPUT_FSK_DISCRIMINATOR, 4800, 4);
    rc |= expect_configured_mode("Soapy P25 C4FM uses P25 C4FM LPF", soapy_p25_c4fm, 48000,
                                 DSD_DEMOD_OUTPUT_FSK_DISCRIMINATOR, 4800, 4, DSD_CH_LPF_PROFILE_P25_C4FM);

    static dsd_opts soapy_p25p1_qpsk;
    soapy_p25p1_qpsk = p25p1_qpsk;
    DSD_SNPRINTF(soapy_p25p1_qpsk.audio_in_dev, sizeof(soapy_p25p1_qpsk.audio_in_dev), "%s", "soapy:driver=test");
    rc |= expect_configured_mode("Soapy P25 QPSK uses 4.8 ksps CQPSK symbols", soapy_p25p1_qpsk, 48000,
                                 DSD_DEMOD_OUTPUT_SYMBOL_CQPSK, 4800, 4, DSD_CH_LPF_PROFILE_P25_CQPSK);

    static dsd_opts soapy_p25p2_qpsk;
    soapy_p25p2_qpsk = p25p2_qpsk;
    DSD_SNPRINTF(soapy_p25p2_qpsk.audio_in_dev, sizeof(soapy_p25p2_qpsk.audio_in_dev), "%s", "soapy");
    rc |= expect_configured_mode("Soapy P25P2 QPSK uses 6 ksps CQPSK symbols", soapy_p25p2_qpsk, 48000,
                                 DSD_DEMOD_OUTPUT_SYMBOL_CQPSK, 6000, 4, DSD_CH_LPF_PROFILE_P25_CQPSK);

    static dsd_opts soapy_analog;
    DSD_MEMSET(&soapy_analog, 0, sizeof(soapy_analog));
    soapy_analog.analog_only = 1;
    DSD_SNPRINTF(soapy_analog.audio_in_dev, sizeof(soapy_analog.audio_in_dev), "%s", "soapy");
    rc |= expect_output_kind("Soapy analog-only stays monitor/audio path", soapy_analog, DSD_DEMOD_OUTPUT_AUDIO_MONITOR,
                             4800, 4);

    rc |= expect_live_symbol_status();
    rc |= expect_cqpsk_toggle_clears_output_contract_backlog();
    rc |= expect_cqpsk_toggle_restores_fsk_channel_profile();
    rc |= expect_rtl_metrics_do_not_nudge_cqpsk_bandedge();
    rc |= expect_rtl_metrics_exports_and_toggles();
    rc |= expect_public_control_wrapper_contracts();
    rc |= expect_fsk_snr_sps_uses_active_profile();
    rc |= expect_direct_output_open_rate_uses_demod_rate();
    rc |= expect_steady_state_watermark_disabled("rtl_tcp keeps demod watermark disabled", "rtltcp:127.0.0.1:1234");
    rc |= expect_steady_state_watermark_disabled("rtlsdr keeps demod watermark disabled", "rtl");
    rc |= expect_steady_state_watermark_disabled("soapy keeps demod watermark disabled", "soapy:driver=test");

    return rc;
}
