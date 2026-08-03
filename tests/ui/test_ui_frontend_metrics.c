// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <assert.h>
#include <stdio.h>

#include <dsd-neo/app_control/frontend.h>
#include <dsd-neo/app_control/snapshot.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/runtime/rtl_stream_metrics_hooks.h>

static const dsd_opts* g_latest_opts;
static const dsd_state* g_latest_state;
static int g_latest_opts_calls;
static int g_latest_state_calls;
static int g_snr_c4fm_eye_calls;
static int g_snr_gfsk_eye_calls;
static int g_snr_qpsk_const_calls;
static double g_snr_c4fm = 23.5;
static double g_snr_cqpsk = 19.25;
static double g_snr_gfsk = 17.75;

const dsd_opts*
dsd_app_get_latest_opts_snapshot(void) {
    g_latest_opts_calls++;
    return g_latest_opts;
}

const dsd_state*
dsd_app_get_latest_snapshot(void) {
    g_latest_state_calls++;
    return g_latest_state;
}

static void
fill_metric_inputs(dsd_opts* opts, dsd_state* state) {
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    opts->rtlsdr_ppm_error = -3;
    /* Radio input: everything the hooks publish describes an RTL stream, and only
     * an RTL input has one. See test_metrics_ignore_stream_hooks_off_radio(). */
    opts->audio_in_type = AUDIO_IN_RTL;
}

static int
hook_output_kind(void) {
    return DSD_FRONTEND_RTL_OUTPUT_SYMBOL_CQPSK;
}

static unsigned int
hook_output_rate(void) {
    return 48000U;
}

static int
hook_symbol_profile(int* out_symbol_rate_hz, int* out_levels, int* out_channel_profile) {
    if (out_symbol_rate_hz) {
        *out_symbol_rate_hz = 4800;
    }
    if (out_levels) {
        *out_levels = 4;
    }
    if (out_channel_profile) {
        *out_channel_profile = 5;
    }
    return 0;
}

static int
hook_cqpsk_status(int* out_cqpsk_enable, int* out_cqpsk_timing_active) {
    if (out_cqpsk_enable) {
        *out_cqpsk_enable = 1;
    }
    if (out_cqpsk_timing_active) {
        *out_cqpsk_timing_active = 1;
    }
    return 0;
}

static double
hook_snr_c4fm(void) {
    return g_snr_c4fm;
}

static double
hook_snr_cqpsk(void) {
    return g_snr_cqpsk;
}

static double
hook_snr_gfsk(void) {
    return g_snr_gfsk;
}

static double
hook_snr_c4fm_eye(void) {
    ++g_snr_c4fm_eye_calls;
    return 7.25;
}

static double
hook_snr_gfsk_eye(void) {
    ++g_snr_gfsk_eye_calls;
    return 6.5;
}

static double
hook_snr_qpsk_const(void) {
    ++g_snr_qpsk_const_calls;
    return 9.75;
}

static void
reset_snr_hook_fakes(double c4fm, double cqpsk, double gfsk) {
    g_snr_c4fm = c4fm;
    g_snr_cqpsk = cqpsk;
    g_snr_gfsk = gfsk;
    g_snr_c4fm_eye_calls = 0;
    g_snr_gfsk_eye_calls = 0;
    g_snr_qpsk_const_calls = 0;
}

static void
test_metrics_fallback_and_runtime_hooks(void) {
    static dsd_opts opts;
    static dsd_state state;
    dsd_frontend_metrics metrics;
    fill_metric_inputs(&opts, &state);

    dsd_rtl_stream_metrics_hooks_set(NULL);
    g_latest_opts = &opts;
    g_latest_state = NULL;
    assert(dsd_app_frontend_get_metrics(&metrics) == 0);
    assert(metrics.output_rate_hz == 0U);
    assert(metrics.snr_c4fm_db == -100.0);
    assert(metrics.snr_gfsk_eye_db == -100.0);
    assert(metrics.requested_ppm == -3);
    assert(metrics.tuner_gain_is_auto == 1);
    assert(metrics.spectrum_size == 0);
    assert(dsd_app_frontend_constellation_get(NULL, 0) == 0);

    dsd_rtl_stream_metrics_hooks hooks = {0};
    hooks.output_kind = hook_output_kind;
    hooks.output_rate_hz = hook_output_rate;
    hooks.symbol_profile = hook_symbol_profile;
    hooks.cqpsk_status = hook_cqpsk_status;
    hooks.snr_c4fm_db = hook_snr_c4fm;
    hooks.snr_c4fm_eye_db = hook_snr_c4fm_eye;
    hooks.snr_cqpsk_db = hook_snr_cqpsk;
    hooks.snr_gfsk_db = hook_snr_gfsk;
    hooks.snr_gfsk_eye_db = hook_snr_gfsk_eye;
    hooks.snr_qpsk_const_db = hook_snr_qpsk_const;
    dsd_rtl_stream_metrics_hooks_set(&hooks);
    reset_snr_hook_fakes(23.5, 19.25, 17.75);
    g_latest_opts = &opts;
    g_latest_state = &state;
    assert(dsd_app_frontend_get_metrics(&metrics) == 0);
    assert(metrics.output_kind == DSD_FRONTEND_RTL_OUTPUT_SYMBOL_CQPSK);
    assert(metrics.output_rate_hz == 48000U);
    assert(metrics.symbol_rate_hz == 4800);
    assert(metrics.symbol_levels == 4);
    assert(metrics.channel_profile == 5);
    assert(metrics.cqpsk_enable == 1);
    assert(metrics.cqpsk_timing_active == 1);
    assert(metrics.snr_c4fm_db == 23.5);
    assert(metrics.snr_cqpsk_db == 19.25);
    assert(metrics.snr_gfsk_db == 17.75);
    assert(metrics.snr_c4fm_eye_db == -100.0);
    assert(metrics.snr_gfsk_eye_db == -100.0);
    assert(metrics.snr_qpsk_const_db == -100.0);
    assert(g_snr_c4fm_eye_calls == 0);
    assert(g_snr_gfsk_eye_calls == 0);
    assert(g_snr_qpsk_const_calls == 0);

    assert(dsd_app_frontend_get_metrics_with_snr_fallbacks(&metrics, DSD_FRONTEND_SNR_FALLBACK_ALL) == 0);
    assert(metrics.snr_c4fm_db == 23.5);
    assert(metrics.snr_cqpsk_db == 19.25);
    assert(metrics.snr_gfsk_db == 17.75);
    assert(metrics.snr_c4fm_eye_db == -100.0);
    assert(metrics.snr_gfsk_eye_db == -100.0);
    assert(metrics.snr_qpsk_const_db == -100.0);
    assert(g_snr_c4fm_eye_calls == 0);
    assert(g_snr_gfsk_eye_calls == 0);
    assert(g_snr_qpsk_const_calls == 0);

    reset_snr_hook_fakes(-100.0, -100.0, -100.0);
    assert(dsd_app_frontend_get_metrics_with_snr_fallbacks(&metrics, DSD_FRONTEND_SNR_FALLBACK_C4FM_EYE
                                                                         | DSD_FRONTEND_SNR_FALLBACK_QPSK_CONST)
           == 0);
    assert(metrics.snr_c4fm_eye_db == 7.25);
    assert(metrics.snr_qpsk_const_db == 9.75);
    assert(metrics.snr_gfsk_eye_db == -100.0);
    assert(g_snr_c4fm_eye_calls == 1);
    assert(g_snr_gfsk_eye_calls == 0);
    assert(g_snr_qpsk_const_calls == 1);

    reset_snr_hook_fakes(-100.0, -100.0, -100.0);
    assert(dsd_app_frontend_get_metrics_with_snr_fallbacks(&metrics, DSD_FRONTEND_SNR_FALLBACK_GFSK_EYE) == 0);
    assert(metrics.snr_c4fm_eye_db == -100.0);
    assert(metrics.snr_gfsk_eye_db == 6.5);
    assert(metrics.snr_qpsk_const_db == -100.0);
    assert(g_snr_c4fm_eye_calls == 0);
    assert(g_snr_gfsk_eye_calls == 1);
    assert(g_snr_qpsk_const_calls == 0);
    dsd_rtl_stream_metrics_hooks_set(NULL);
}

/*
 * Metrics read from a snapshot the caller already holds.
 *
 * A frontend building one frame reads metrics and snapshot fields both. Through the
 * implicit accessors that is two consumes, and a publish landing between them leaves
 * the two halves of the frame describing different generations. This variant exists
 * so the caller can consume once; the whole point is that it does not consume again.
 */
static void
test_metrics_for_snapshot_does_not_consume(void) {
    /* Static, like the case below: these are far too large for the stack. */
    static dsd_opts opts;
    static dsd_state state;
    dsd_frontend_metrics from_accessors;
    dsd_frontend_metrics from_snapshot;

    fill_metric_inputs(&opts, &state);
    g_latest_opts = &opts;
    g_latest_state = &state;

    g_latest_opts_calls = 0;
    g_latest_state_calls = 0;
    assert(dsd_app_frontend_get_metrics_with_snr_fallbacks(&from_accessors, DSD_FRONTEND_SNR_FALLBACK_ALL) == 0);
    assert(g_latest_opts_calls == 1);
    assert(g_latest_state_calls == 1);

    g_latest_opts_calls = 0;
    g_latest_state_calls = 0;
    assert(dsd_app_frontend_get_metrics_for_snapshot(&opts, &state, &from_snapshot, DSD_FRONTEND_SNR_FALLBACK_ALL)
           == 0);
    assert(g_latest_opts_calls == 0);
    assert(g_latest_state_calls == 0);

    /* Same inputs, same answer: the two differ only in where the snapshot comes from. */
    assert(from_snapshot.requested_ppm == from_accessors.requested_ppm);
    assert(from_snapshot.output_rate_hz == from_accessors.output_rate_hz);
    assert(from_snapshot.symbol_rate_hz == from_accessors.symbol_rate_hz);
    assert(from_snapshot.cqpsk_enable == from_accessors.cqpsk_enable);

    /* Missing snapshots are the pre-first-publish state, not an error; a missing
     * output buffer is the only thing there is to reject. */
    assert(dsd_app_frontend_get_metrics_for_snapshot(NULL, NULL, &from_snapshot, 0U) == 0);
    assert(dsd_app_frontend_get_metrics_for_snapshot(&opts, &state, NULL, 0U) < 0);

    g_latest_opts = NULL;
    g_latest_state = NULL;
}

/*
 * A session on a non-radio input publishes none of the RTL stream's readings.
 *
 * The stream globals behind these hooks outlive the session that filled them, so a
 * host that starts a second session on a different input used to show the previous
 * run's output rate, symbol clock and SNR next to a decoder that never had a
 * demodulator. Only the Android app can reach this -- the CLI is one input per
 * process -- and there it put a live-looking carrier lock on screen for a UDP feed.
 */
static void
test_metrics_ignore_stream_hooks_off_radio(void) {
    static dsd_opts opts;
    static dsd_state state;
    dsd_frontend_metrics metrics;

    dsd_rtl_stream_metrics_hooks hooks = {0};
    hooks.output_kind = hook_output_kind;
    hooks.output_rate_hz = hook_output_rate;
    hooks.symbol_profile = hook_symbol_profile;
    hooks.cqpsk_status = hook_cqpsk_status;
    hooks.snr_c4fm_db = hook_snr_c4fm;
    hooks.snr_c4fm_eye_db = hook_snr_c4fm_eye;
    hooks.snr_cqpsk_db = hook_snr_cqpsk;
    hooks.snr_gfsk_db = hook_snr_gfsk;
    hooks.snr_gfsk_eye_db = hook_snr_gfsk_eye;
    hooks.snr_qpsk_const_db = hook_snr_qpsk_const;
    dsd_rtl_stream_metrics_hooks_set(&hooks);

    /* Hooks installed and reporting healthy values throughout: the input type is
     * the only thing that changes between the two halves of this case. */
    const dsd_audio_in_type non_radio[] = {AUDIO_IN_PULSE, AUDIO_IN_UDP, AUDIO_IN_TCP, AUDIO_IN_WAV,
                                           AUDIO_IN_SYMBOL_BIN};
    for (size_t i = 0; i < sizeof(non_radio) / sizeof(non_radio[0]); i++) {
        fill_metric_inputs(&opts, &state);
        opts.audio_in_type = non_radio[i];
        reset_snr_hook_fakes(23.5, 19.25, 17.75);

        assert(dsd_app_frontend_get_metrics_for_snapshot(&opts, &state, &metrics, DSD_FRONTEND_SNR_FALLBACK_ALL) == 0);
        assert(metrics.output_rate_hz == 0U);
        assert(metrics.symbol_rate_hz == 0);
        assert(metrics.symbol_levels == 0);
        assert(metrics.cqpsk_enable == 0);
        assert(metrics.stream_active == 0);
        assert(metrics.carrier_lock == 0);
        assert(metrics.cfo_hz == 0.0);
        /* The invalid sentinel, not a number: a frontend renders this as "no
         * reading" rather than as a measurement. */
        assert(metrics.snr_c4fm_db == -100.0);
        assert(metrics.snr_cqpsk_db == -100.0);
        assert(metrics.snr_gfsk_db == -100.0);
        /* The eye/constellation estimators are never consulted either. */
        assert(metrics.snr_c4fm_eye_db == -100.0);
        assert(metrics.snr_gfsk_eye_db == -100.0);
        assert(metrics.snr_qpsk_const_db == -100.0);
        assert(g_snr_c4fm_eye_calls == 0);
        assert(g_snr_gfsk_eye_calls == 0);
        assert(g_snr_qpsk_const_calls == 0);
        /* Requested PPM is opts-derived, so it survives; it describes the request,
         * not a measurement. */
        assert(metrics.requested_ppm == -3);
    }

    /* Same hooks, radio input: the readings come back. This is what proves the
     * assertions above are about the input type and not about a broken hook table. */
    fill_metric_inputs(&opts, &state);
    reset_snr_hook_fakes(23.5, 19.25, 17.75);
    assert(dsd_app_frontend_get_metrics_for_snapshot(&opts, &state, &metrics, DSD_FRONTEND_SNR_FALLBACK_ALL) == 0);
    assert(metrics.output_rate_hz == 48000U);
    assert(metrics.symbol_rate_hz == 4800);
    assert(metrics.snr_c4fm_db == 23.5);

    dsd_rtl_stream_metrics_hooks_set(NULL);
}

int
main(void) {
    test_metrics_for_snapshot_does_not_consume();
    test_metrics_fallback_and_runtime_hooks();
    test_metrics_ignore_stream_hooks_off_radio();
    printf("UI_FRONTEND_METRICS: OK\n");
    return 0;
}
