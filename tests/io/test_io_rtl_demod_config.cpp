// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/dsp/demod_pipeline.h>
#include <dsd-neo/dsp/demod_state.h>
#include <dsd-neo/dsp/fsk_modem.h>
#include <dsd-neo/io/rtl_demod_config.h>
#include <dsd-neo/io/rtl_metrics.h>
#include <dsd-neo/io/rtl_stream_c.h>
#include <dsd-neo/platform/posix_compat.h>
#include <dsd-neo/runtime/analog_channel.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/mem.h>
#include <dsd-neo/runtime/ring.h>
#include <stdint.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "rtl_stream_test_support.h"

extern demod_state demod;
extern std::atomic<double> g_snr_c4fm_db;
extern std::atomic<double> g_snr_gfsk_db;
extern std::atomic<double> g_snr_qpsk_db;

/* A zeroed demod_state from the aligned allocator (its members need 64-byte alignment, which calloc() does not give);
 * release it with dsd_neo_aligned_free(). */
static demod_state*
alloc_zeroed_demod(void) {
    demod_state* demod = static_cast<demod_state*>(dsd_neo_aligned_malloc(sizeof(demod_state)));
    if (demod) {
        DSD_MEMSET(demod, 0, sizeof(*demod));
    }
    return demod;
}

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

    rtl_demod_maybe_refresh_ted_sps_after_rate_change(&demod, &opts, &output, /*preserve_active_profile=*/0);
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

/*
 * Retunes must keep the symbol profile the front end is already on (whatever the SPS hunt, the
 * trunking engine, or the operator last published) and recompute only the timing SPS for the
 * current output rate. Re-deriving the profile from the option flags here snapped multi-protocol
 * runs back to 4800/4 on every hop.
 */
static int
expect_preserved_profile(const char* label, const dsd_opts* opts, int rate_hz, int seed_rate, int seed_levels,
                         int override_sps, int want_rate, int want_levels, int want_sps) {
    static demod_state demod;
    output_state output;
    DSD_MEMSET(&demod, 0, sizeof(demod));
    DSD_MEMSET(&output, 0, sizeof(output));
    demod.output_kind = DSD_DEMOD_OUTPUT_FSK_DISCRIMINATOR;
    demod.rate_out = rate_hz;
    demod.symbol_rate_hz = seed_rate;
    demod.symbol_levels = seed_levels;
    demod.ted_sps_override = override_sps;
    output.rate = static_cast<unsigned int>(rate_hz);

    rtl_demod_maybe_refresh_ted_sps_after_rate_change(&demod, opts, &output, /*preserve_active_profile=*/1);

    int rc = 0;
    rc |= expect_int_eq(label, demod.symbol_rate_hz, want_rate);
    rc |= expect_int_eq(label, demod.symbol_levels, want_levels);
    rc |= expect_int_eq(label, demod.ted_sps, want_sps);
    return rc;
}

static int
expect_retune_preserves_active_profile(void) {
    int rc = 0;

    /* Multi-protocol opts: opts_symbol_rate_hz() answers 4800 because the enabled decoders span
     * more than one rate class, which is what -fa always looks like. */
    static dsd_opts fa_opts;
    DSD_MEMSET(&fa_opts, 0, sizeof(fa_opts));
    fa_opts.frame_dmr = 1;
    fa_opts.frame_nxdn48 = 1;

    rc |= expect_preserved_profile("retune keeps hunt profile 2400/4", &fa_opts, 48000, 2400, 4, 0, 2400, 4, 20);
    rc |= expect_preserved_profile("retune keeps hunt profile at 24 kHz", &fa_opts, 24000, 2400, 4, 0, 2400, 4, 10);
    rc |= expect_preserved_profile("retune keeps two-level profile", &fa_opts, 48000, 4800, 2, 0, 4800, 2, 10);

    static dsd_opts fa_provoice;
    DSD_MEMSET(&fa_provoice, 0, sizeof(fa_provoice));
    fa_provoice.frame_dmr = 1;
    fa_provoice.frame_provoice = 1;
    rc |= expect_preserved_profile("retune keeps ProVoice 9600/2", &fa_provoice, 48000, 9600, 2, 0, 9600, 2, 5);

    /* The mod_qpsk snap to 4800 belongs to stream open only. */
    static dsd_opts p25p1_qpsk_opts;
    DSD_MEMSET(&p25p1_qpsk_opts, 0, sizeof(p25p1_qpsk_opts));
    p25p1_qpsk_opts.frame_p25p1 = 1;
    p25p1_qpsk_opts.mod_qpsk = 1;
    rc |=
        expect_preserved_profile("retune does not snap QPSK to 4800", &p25p1_qpsk_opts, 48000, 2400, 4, 0, 2400, 4, 20);

    /* Replay RESET events reach the refresh with no opts at all. */
    rc |= expect_preserved_profile("replay reset keeps 9600/2 without opts", NULL, 48000, 9600, 2, 0, 9600, 2, 5);
    rc |= expect_preserved_profile("replay reset keeps 2400/4 without opts", NULL, 48000, 2400, 4, 0, 2400, 4, 20);

    /* An unset profile still falls back to the opts-derived default. */
    rc |= expect_preserved_profile("unset profile falls back to opts", &fa_opts, 48000, 0, 0, 0, 4800, 4, 10);
    rc |=
        expect_preserved_profile("unset profile without opts falls back to 4800/4", NULL, 48000, 0, 0, 0, 4800, 4, 10);

    /* A manual TED SPS override still wins over the recomputed value. */
    rc |= expect_preserved_profile("ted_sps override still wins", &fa_opts, 48000, 2400, 4, 8, 2400, 4, 8);

    return rc;
}

/*
 * The FSK modem only resets its DC-centering and peak-AGC estimators when its configuration
 * actually changes, so preserving the profile across a retune also stops the needless reset.
 */
static int
expect_preserved_profile_keeps_fsk_modem_config(void) {
    static demod_state demod;
    output_state output;
    static dsd_opts fa_opts;
    int rc = 0;

    DSD_MEMSET(&fa_opts, 0, sizeof(fa_opts));
    fa_opts.frame_dmr = 1;
    fa_opts.frame_nxdn48 = 1;

    for (int preserve = 0; preserve <= 1; preserve++) {
        DSD_MEMSET(&demod, 0, sizeof(demod));
        DSD_MEMSET(&output, 0, sizeof(output));
        demod.output_kind = DSD_DEMOD_OUTPUT_FSK_DISCRIMINATOR;
        demod.rate_out = 48000;
        demod.symbol_rate_hz = 2400;
        demod.symbol_levels = 4;
        demod.channel_lpf_profile = DSD_CH_LPF_PROFILE_6K25;
        output.rate = 48000U;

        dsd_fsk_modem_config cfg;
        cfg.sample_rate_hz = 48000;
        cfg.symbol_rate_hz = 2400;
        cfg.levels = 4;
        cfg.channel_profile = DSD_CH_LPF_PROFILE_6K25;
        dsd_fsk_modem_configure(&demod.fsk_modem_state, &cfg);
        demod.fsk_modem_state.dc_est = 0.5f;

        rtl_demod_maybe_refresh_ted_sps_after_rate_change(&demod, &fa_opts, &output, preserve);

        if (preserve) {
            rc |= expect_double_near("preserved profile keeps FSK modem estimators",
                                     static_cast<double>(demod.fsk_modem_state.dc_est), 0.5, 1e-6);
        } else {
            rc |= expect_double_near("opts-derived profile resets FSK modem estimators",
                                     static_cast<double>(demod.fsk_modem_state.dc_est), 0.0, 1e-6);
        }
    }
    return rc;
}

/*
 * Cover the wiring, not just the helper: run the real retune finalize path and confirm the
 * published profile survives it.
 */
static int
expect_finalize_rate_chain_preserves_profile(void) {
    int rc = 0;
    rtl_stream_test_finalize_profile_result result;

    static dsd_opts fa_opts;
    DSD_MEMSET(&fa_opts, 0, sizeof(fa_opts));
    fa_opts.frame_dmr = 1;
    fa_opts.frame_nxdn48 = 1;

    DSD_MEMSET(&result, 0, sizeof(result));
    rc |= expect_int_eq(
        "finalize seam accepts NXDN48 profile",
        rtl_stream_test_finalize_rate_chain_profile(&fa_opts, 48000, 2400, 4, DSD_CH_LPF_PROFILE_6K25, &result), 0);
    rc |= expect_int_eq("retune finalize keeps 2400 sym/s", result.symbol_rate_hz, 2400);
    rc |= expect_int_eq("retune finalize keeps four levels", result.symbol_levels, 4);
    rc |= expect_int_eq("retune finalize keeps ted_sps 20", result.ted_sps, 20);
    rc |= expect_int_eq("retune finalize leaves override clear", result.ted_sps_override, 0);
    rc |= expect_int_eq("retune finalize keeps integer sps flag", result.sps_is_integer, 1);
    rc |= expect_int_eq("retune finalize keeps channel profile", result.channel_lpf_profile, DSD_CH_LPF_PROFILE_6K25);

    DSD_MEMSET(&result, 0, sizeof(result));
    rc |= expect_int_eq(
        "finalize seam accepts ProVoice profile without opts",
        rtl_stream_test_finalize_rate_chain_profile(NULL, 48000, 9600, 2, DSD_CH_LPF_PROFILE_PROVOICE, &result), 0);
    rc |= expect_int_eq("replay reset finalize keeps 9600 sym/s", result.symbol_rate_hz, 9600);
    rc |= expect_int_eq("replay reset finalize keeps two levels", result.symbol_levels, 2);
    rc |= expect_int_eq("replay reset finalize keeps ted_sps 5", result.ted_sps, 5);

    return rc;
}

static int
expect_output_kind(const char* label, const dsd_opts& opts, int want_kind, int want_sym_rate, int want_levels) {
    demod_state* demod = alloc_zeroed_demod();
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
        DSD_FPRINTF(stderr, "%s: symbol output changed public output rate to %u\n", label, output.rate.load());
        rc = 1;
    }

    rtl_demod_cleanup(demod);
    dsd_neo_aligned_free(demod);
    return rc;
}

static int
expect_configured_channel_profile(const char* label, const dsd_opts& opts, int rtl_dsp_bw_hz, int want_profile) {
    demod_state* demod = alloc_zeroed_demod();
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
    dsd_neo_aligned_free(demod);
    return rc;
}

static int
expect_configured_mode(const char* label, const dsd_opts& opts, int rtl_dsp_bw_hz, int want_kind, int want_sym_rate,
                       int want_levels, int want_profile) {
    demod_state* demod = alloc_zeroed_demod();
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
        DSD_FPRINTF(stderr, "%s: symbol output changed public output rate to %u\n", label, output.rate.load());
        rc = 1;
    }

    rtl_demod_cleanup(demod);
    dsd_neo_aligned_free(demod);
    return rc;
}

static int
expect_live_symbol_status(void) {
    int rc = 0;

    DSD_MEMSET(&demod, 0, sizeof(demod));
    demod.output_kind = DSD_DEMOD_OUTPUT_FSK_DISCRIMINATOR;
    demod.symbol_rate_hz = 4800;
    demod.symbol_levels = 4;
    rtl_stream_test_publish_demod_snapshot();

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
    rtl_stream_test_publish_demod_snapshot();

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
    rc |= expect_int_eq("RTL active state defaults inactive", rtl_stream_is_active(), 0);

    DSD_MEMSET(&demod, 0, sizeof(demod));
    demod.ted_state.e_ema = 0.25f;
    rtl_stream_test_publish_demod_snapshot();
    rc |= expect_int_eq("RTL timing bias exports Q14", rtl_stream_cqpsk_timing_bias(nullptr), 4096);
    demod.ted_state.e_ema = -0.5f;
    rtl_stream_test_publish_demod_snapshot();
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
    rc |= expect_int_eq("RTL symbol profile snapshot after update",
                        rtl_stream_get_symbol_profile_full(&symbol_rate, &levels, nullptr), 0);
    rc |= expect_int_eq("RTL updated profile rate", symbol_rate, 9600);
    rc |= expect_int_eq("RTL updated profile levels", levels, 2);
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

static int
expect_passes_for_device_rate(void) {
    int rc = 0;
    /* SoapySDDC on an RX-888 can only offer 2 MSPS at the stock ADC clock: 2e6 >> 5 = 62500 Hz
       is the closest landing above the requested 48 kHz DSP bandwidth. */
    rc |= expect_int_eq("2 MSPS to 48 kHz picks 5 passes", rtl_stream_test_passes_for_actual_rate(2000000U, 48000), 5);
    rc |= expect_int_eq("2 MSPS to 24 kHz picks 6 passes", rtl_stream_test_passes_for_actual_rate(2000000U, 24000), 6);
    /* With adc_frequency=98304000 the device offers the requested rate exactly. */
    rc |= expect_int_eq("1.536 MSPS to 48 kHz picks 5 passes", rtl_stream_test_passes_for_actual_rate(1536000U, 48000),
                        5);
    rc |=
        expect_int_eq("2.5 MSPS to 48 kHz picks 5 passes", rtl_stream_test_passes_for_actual_rate(2500000U, 48000), 5);
    /* Never decimate below the requested bandwidth. */
    rc |= expect_int_eq("rate below bandwidth keeps every sample",
                        rtl_stream_test_passes_for_actual_rate(32000U, 48000), 0);
    rc |= expect_int_eq("rate equal to bandwidth keeps every sample",
                        rtl_stream_test_passes_for_actual_rate(48000U, 48000), 0);
    rc |= expect_int_eq("zero rate is rejected", rtl_stream_test_passes_for_actual_rate(0U, 48000), 0);
    rc |= expect_int_eq("zero bandwidth is rejected", rtl_stream_test_passes_for_actual_rate(2000000U, 0), 0);
    return rc;
}

static int
expect_digital_resample_policy(void) {
    static constexpr int kModeAuto = DSD_DIGITAL_RESAMPLE_AUTO;
    static constexpr int kModeOn = DSD_DIGITAL_RESAMPLE_ON;
    static constexpr int kModeOff = DSD_DIGITAL_RESAMPLE_OFF;
    static constexpr int kForced = 1;
    static constexpr int kNotForced = 0;

    int rc = 0;
    unsigned int output_rate_hz = 0U;
    int resamp_enabled = -1;

    /* A device-imposed 62500 Hz gives 13.02 SPS at 4800 sym/s, so auto mode normalizes to 48 kHz. */
    rc |= expect_int_eq("device-forced non-integer SPS helper rc",
                        rtl_stream_test_digital_resample_chain(DSD_DEMOD_OUTPUT_FSK_DISCRIMINATOR, 62500, 48000, 4800,
                                                               kModeAuto, kForced, &output_rate_hz, &resamp_enabled),
                        0);
    rc |= expect_int_eq("device-forced non-integer SPS resamples", resamp_enabled, 1);
    rc |= expect_int_eq("device-forced non-integer SPS publishes target", (int)output_rate_hz, 48000);

    /* The same rate chosen by the user's own DSP bandwidth is left alone. */
    rc |= expect_int_eq("user bandwidth non-integer SPS helper rc",
                        rtl_stream_test_digital_resample_chain(DSD_DEMOD_OUTPUT_FSK_DISCRIMINATOR, 62500, 48000, 4800,
                                                               kModeAuto, kNotForced, &output_rate_hz, &resamp_enabled),
                        0);
    rc |= expect_int_eq("user bandwidth non-integer SPS bypasses", resamp_enabled, 0);
    rc |= expect_int_eq("user bandwidth non-integer SPS keeps demod rate", (int)output_rate_hz, 62500);

    /* An integer SPS never needs the resampler, however the rate was chosen. */
    rc |= expect_int_eq("device-forced integer SPS helper rc",
                        rtl_stream_test_digital_resample_chain(DSD_DEMOD_OUTPUT_FSK_DISCRIMINATOR, 96000, 48000, 4800,
                                                               kModeAuto, kForced, &output_rate_hz, &resamp_enabled),
                        0);
    rc |= expect_int_eq("device-forced integer SPS bypasses", resamp_enabled, 0);
    rc |= expect_int_eq("device-forced integer SPS keeps demod rate", (int)output_rate_hz, 96000);

    /* CQPSK output is symbol-rate already; the Gardner loop absorbs fractional SPS. */
    rc |= expect_int_eq("cqpsk helper rc",
                        rtl_stream_test_digital_resample_chain(DSD_DEMOD_OUTPUT_SYMBOL_CQPSK, 62500, 48000, 4800,
                                                               kModeAuto, kForced, &output_rate_hz, &resamp_enabled),
                        0);
    rc |= expect_int_eq("cqpsk symbols are never resampled", resamp_enabled, 0);
    rc |= expect_int_eq("cqpsk keeps demod rate", (int)output_rate_hz, 62500);

    /* Explicit modes override the auto heuristic in both directions. */
    rc |= expect_int_eq("mode off helper rc",
                        rtl_stream_test_digital_resample_chain(DSD_DEMOD_OUTPUT_FSK_DISCRIMINATOR, 62500, 48000, 4800,
                                                               kModeOff, kForced, &output_rate_hz, &resamp_enabled),
                        0);
    rc |= expect_int_eq("mode off bypasses", resamp_enabled, 0);
    rc |= expect_int_eq("mode off keeps demod rate", (int)output_rate_hz, 62500);

    rc |= expect_int_eq("mode on helper rc",
                        rtl_stream_test_digital_resample_chain(DSD_DEMOD_OUTPUT_FSK_DISCRIMINATOR, 24000, 48000, 4800,
                                                               kModeOn, kNotForced, &output_rate_hz, &resamp_enabled),
                        0);
    rc |= expect_int_eq("mode on resamples an integer SPS rate", resamp_enabled, 1);
    rc |= expect_int_eq("mode on publishes target", (int)output_rate_hz, 48000);

    /* A target that cannot produce an integer SPS is not worth the resampler. */
    rc |= expect_int_eq("unhelpful target helper rc",
                        rtl_stream_test_digital_resample_chain(DSD_DEMOD_OUTPUT_FSK_DISCRIMINATOR, 62500, 50000, 4800,
                                                               kModeAuto, kForced, &output_rate_hz, &resamp_enabled),
                        0);
    rc |= expect_int_eq("unhelpful target bypasses", resamp_enabled, 0);
    rc |= expect_int_eq("unhelpful target keeps demod rate", (int)output_rate_hz, 62500);

    /* Even mode on declines a target that cannot yield an integer SPS (documented behavior). */
    rc |= expect_int_eq("mode on unhelpful target helper rc",
                        rtl_stream_test_digital_resample_chain(DSD_DEMOD_OUTPUT_FSK_DISCRIMINATOR, 62500, 50000, 4800,
                                                               kModeOn, kForced, &output_rate_hz, &resamp_enabled),
                        0);
    rc |= expect_int_eq("mode on unhelpful target bypasses", resamp_enabled, 0);
    rc |= expect_int_eq("mode on unhelpful target keeps demod rate", (int)output_rate_hz, 62500);
    return rc;
}

static int
expect_source_policy_matrix(void) {
    static constexpr int kRadioSourceRtlUsb = 0;
    static constexpr int kRadioSourceRtlTcp = 1;
    static constexpr int kRadioSourceSoapy = 2;
    static constexpr int kRadioSourceIqReplay = 3;

    int rc = 0;

    int kinds[8] = {};
    int rtltcp[8] = {};
    int soapy[8] = {};
    int replay[8] = {};
    int family[8] = {};
    char names[96] = {};
    char soapy_args[64] = {};
    rc |= expect_int_eq("source policy rejects null kind",
                        rtl_stream_test_source_policy_matrix(NULL, rtltcp, soapy, replay, family,
                                                             sizeof(kinds) / sizeof(kinds[0]), names, sizeof(names),
                                                             soapy_args, sizeof(soapy_args)),
                        -1);
    rc |= expect_int_eq("source policy rejects null rtltcp",
                        rtl_stream_test_source_policy_matrix(kinds, NULL, soapy, replay, family,
                                                             sizeof(kinds) / sizeof(kinds[0]), names, sizeof(names),
                                                             soapy_args, sizeof(soapy_args)),
                        -1);
    rc |= expect_int_eq("source policy rejects null soapy",
                        rtl_stream_test_source_policy_matrix(kinds, rtltcp, NULL, replay, family,
                                                             sizeof(kinds) / sizeof(kinds[0]), names, sizeof(names),
                                                             soapy_args, sizeof(soapy_args)),
                        -1);
    rc |= expect_int_eq("source policy rejects null replay",
                        rtl_stream_test_source_policy_matrix(kinds, rtltcp, soapy, NULL, family,
                                                             sizeof(kinds) / sizeof(kinds[0]), names, sizeof(names),
                                                             soapy_args, sizeof(soapy_args)),
                        -1);
    rc |= expect_int_eq("source policy rejects null family",
                        rtl_stream_test_source_policy_matrix(kinds, rtltcp, soapy, replay, NULL,
                                                             sizeof(kinds) / sizeof(kinds[0]), names, sizeof(names),
                                                             soapy_args, sizeof(soapy_args)),
                        -1);
    rc |= expect_int_eq("source policy rejects short arrays",
                        rtl_stream_test_source_policy_matrix(kinds, rtltcp, soapy, replay, family, 7U, names,
                                                             sizeof(names), soapy_args, sizeof(soapy_args)),
                        -1);
    rc |= expect_int_eq("source policy rejects null names",
                        rtl_stream_test_source_policy_matrix(kinds, rtltcp, soapy, replay, family,
                                                             sizeof(kinds) / sizeof(kinds[0]), NULL, sizeof(names),
                                                             soapy_args, sizeof(soapy_args)),
                        -1);
    rc |= expect_int_eq("source policy rejects empty names",
                        rtl_stream_test_source_policy_matrix(kinds, rtltcp, soapy, replay, family,
                                                             sizeof(kinds) / sizeof(kinds[0]), names, 0U, soapy_args,
                                                             sizeof(soapy_args)),
                        -1);
    rc |= expect_int_eq("source policy rejects null Soapy args",
                        rtl_stream_test_source_policy_matrix(kinds, rtltcp, soapy, replay, family,
                                                             sizeof(kinds) / sizeof(kinds[0]), names, sizeof(names),
                                                             NULL, sizeof(soapy_args)),
                        -1);
    rc |= expect_int_eq("source policy rejects empty Soapy args",
                        rtl_stream_test_source_policy_matrix(kinds, rtltcp, soapy, replay, family,
                                                             sizeof(kinds) / sizeof(kinds[0]), names, sizeof(names),
                                                             soapy_args, 0U),
                        -1);
    rc |= expect_int_eq("source policy matrix helper",
                        rtl_stream_test_source_policy_matrix(kinds, rtltcp, soapy, replay, family,
                                                             sizeof(kinds) / sizeof(kinds[0]), names, sizeof(names),
                                                             soapy_args, sizeof(soapy_args)),
                        0);
    rc |= expect_int_eq("null source defaults RTL USB", kinds[0], kRadioSourceRtlUsb);
    rc |= expect_int_eq("empty source defaults RTL USB", kinds[1], kRadioSourceRtlUsb);
    rc |= expect_int_eq("rtltcp bare source", kinds[2], kRadioSourceRtlTcp);
    rc |= expect_int_eq("rtltcp arg source", kinds[3], kRadioSourceRtlTcp);
    rc |= expect_int_eq("soapy bare source", kinds[4], kRadioSourceSoapy);
    rc |= expect_int_eq("soapy arg source", kinds[5], kRadioSourceSoapy);
    rc |= expect_int_eq("iq replay source", kinds[6], kRadioSourceIqReplay);
    rc |= expect_int_eq("rtl spec source", kinds[7], kRadioSourceRtlUsb);
    rc |= expect_int_eq("rtltcp predicate only matches rtltcp", rtltcp[2] + rtltcp[3], 2);
    rc |= expect_int_eq("soapy predicate only matches soapy", soapy[4] + soapy[5], 2);
    rc |= expect_int_eq("replay predicate only matches replay", replay[6], 1);
    for (size_t i = 0U; i < sizeof(family) / sizeof(family[0]); i++) {
        rc |= expect_int_eq("radio-family predicate covers known source", family[i], 1);
    }
    rc |= expect_int_eq("perf source names stable",
                        std::strcmp(names, "rtl|rtl|rtltcp|rtltcp|soapy|soapy|iq_replay|rtl"), 0);
    rc |= expect_int_eq("soapy args extraction stable", std::strcmp(soapy_args, "|||driver=rtlsdr"), 0);
    return rc;
}

static int
expect_mode_policy_matrix(void) {
    int rc = 0;

    int mode_policy[32] = {};
    rc |= expect_int_eq("mode policy rejects null output", rtl_stream_test_mode_policy_matrix(NULL, 32U), -1);
    rc |= expect_int_eq("mode policy rejects short output", rtl_stream_test_mode_policy_matrix(mode_policy, 31U), -1);
    rc |=
        expect_int_eq("mode policy matrix helper",
                      rtl_stream_test_mode_policy_matrix(mode_policy, sizeof(mode_policy) / sizeof(mode_policy[0])), 0);
    rc |= expect_int_eq("null opts are not digital", mode_policy[0], 0);
    for (int i = 1; i <= 11; i++) {
        rc |= expect_int_eq("digital protocol mode detected", mode_policy[i], 1);
    }
    rc |= expect_int_eq("empty opts are not digital", mode_policy[12], 0);
    rc |= expect_int_eq("null opts have no wide four-level mode", mode_policy[13], 0);
    for (int i = 14; i <= 17; i++) {
        rc |= expect_int_eq("wide four-level protocol mode detected", mode_policy[i], 1);
    }
    rc |= expect_int_eq("P25P1 is not wide four-level FSK", mode_policy[18], 0);
    rc |= expect_int_eq("null opts have no 12.5k/CQPSK mode", mode_policy[19], 0);
    for (int i = 20; i <= 28; i++) {
        rc |= expect_int_eq("12.5k/CQPSK bandwidth mode detected", mode_policy[i], 1);
    }
    rc |= expect_int_eq("NXDN48 is not a 12.5k/CQPSK bandwidth mode", mode_policy[29], 0);
    return rc;
}

static int
expect_fsk_profile_policy_matrix(void) {
    int rc = 0;

    int profiles[21] = {};
    rc |= expect_int_eq("FSK profile rejects null output", rtl_stream_test_fsk_profile_policy_matrix(NULL, 21U), -1);
    rc |=
        expect_int_eq("FSK profile rejects short output", rtl_stream_test_fsk_profile_policy_matrix(profiles, 20U), -1);
    rc |= expect_int_eq("FSK profile policy matrix helper",
                        rtl_stream_test_fsk_profile_policy_matrix(profiles, sizeof(profiles) / sizeof(profiles[0])), 0);
    rc |= expect_int_eq("null sym-rate opts reject", profiles[0], -1);
    rc |= expect_int_eq("null frame opts reject", profiles[1], -1);
    rc |= expect_int_eq("ProVoice sym-rate profile", profiles[2], DSD_CH_LPF_PROFILE_PROVOICE);
    rc |= expect_int_eq("NXDN48 sym-rate profile", profiles[3], DSD_CH_LPF_PROFILE_6K25);
    rc |= expect_int_eq("dPMR sym-rate profile", profiles[4], DSD_CH_LPF_PROFILE_6K25);
    rc |= expect_int_eq("X2-TDMA sym-rate profile", profiles[5], DSD_CH_LPF_PROFILE_12K5);
    rc |= expect_int_eq("DMR frame profile", profiles[6], DSD_CH_LPF_PROFILE_12K5);
    rc |= expect_int_eq("P25P1 frame profile", profiles[7], DSD_CH_LPF_PROFILE_P25_C4FM);
    rc |= expect_int_eq("P25P2 frame profile", profiles[8], DSD_CH_LPF_PROFILE_P25_C4FM);
    rc |= expect_int_eq("D-STAR frame profile", profiles[9], DSD_CH_LPF_PROFILE_6K25);
    rc |= expect_int_eq("X2-TDMA frame profile", profiles[10], DSD_CH_LPF_PROFILE_12K5);
    rc |= expect_int_eq("ProVoice frame profile", profiles[11], DSD_CH_LPF_PROFILE_PROVOICE);
    rc |= expect_int_eq("2400 symbol-rate fallback profile", profiles[12], DSD_CH_LPF_PROFILE_6K25);
    rc |= expect_int_eq("9600 symbol-rate fallback profile", profiles[13], DSD_CH_LPF_PROFILE_PROVOICE);
    rc |= expect_int_eq("6000 symbol-rate fallback profile", profiles[14], DSD_CH_LPF_PROFILE_12K5);
    rc |= expect_int_eq("4800 two-level fallback profile", profiles[15], DSD_CH_LPF_PROFILE_6K25);
    rc |= expect_int_eq("4800 four-level fallback profile", profiles[16], DSD_CH_LPF_PROFILE_12K5);
    rc |= expect_int_eq("wide fallback profile", profiles[17], DSD_CH_LPF_PROFILE_WIDE);
    rc |= expect_int_eq("current mode opts profile wins", profiles[18], DSD_CH_LPF_PROFILE_PROVOICE);
    rc |= expect_int_eq("current mode two-level fallback", profiles[19], DSD_CH_LPF_PROFILE_6K25);
    rc |= expect_int_eq("current mode default fallback", profiles[20], DSD_CH_LPF_PROFILE_12K5);
    return rc;
}

static int
expect_private_policy_matrices(void) {
    int rc = 0;

    rc |= expect_source_policy_matrix();
    rc |= expect_mode_policy_matrix();
    rc |= expect_fsk_profile_policy_matrix();
    return rc;
}

/* -fA: the analog preset leaves every digital frame decoder off. */
static void
make_analog_opts(dsd_opts* opts) {
    DSD_MEMSET(opts, 0, sizeof(*opts));
    opts->analog_only = 1;
    opts->monitor_input_audio = 1;
    opts->analog_demod = DSD_ANALOG_DEMOD_FM;
}

/* Stream-open demod configuration at one DSP bandwidth, then the start-time
 * finalize against the (here: unforced) output rate. */
static int
configure_and_finalize(demod_state* demod, dsd_opts* opts, int rtl_dsp_bw_hz, char* err, size_t err_size) {
    output_state output;
    DSD_MEMSET(&output, 0, sizeof(output));
    output.rate = rtl_dsp_bw_hz;
    rtl_demod_init_for_mode(demod, &output, opts, rtl_dsp_bw_hz);
    rtl_demod_config_from_env_and_opts(demod, opts);
    rtl_demod_select_defaults_for_mode(demod, opts, &output);
    return rtl_demod_finalize_analog_channel(demod, opts, err, err_size);
}

static void
set_channel_lpf_env(const char* value) {
    if (value) {
        (void)dsd_setenv("DSD_NEO_CHANNEL_LPF", value, 1);
    } else {
        (void)dsd_unsetenv("DSD_NEO_CHANNEL_LPF");
    }
    dsd_neo_config_init();
}

/*
 * The unset NFM default keeps today's enable rule bit for bit: the LPF turns on
 * only from a 20 kHz rate_in. Where the 16 kHz default does not fit the rate the
 * legacy WIDE design stays in charge (width 0) instead of failing the start.
 */
static int
expect_analog_legacy_default_enable(void) {
    struct {
        int bw_hz;
        int want_enable;
        int want_width;
    } rows[] = {{12000, 0, 0}, {16000, 0, 0}, {24000, 1, 16000}, {48000, 1, 16000}};

    int rc = 0;
    set_channel_lpf_env(NULL);
    for (size_t i = 0; i < sizeof rows / sizeof rows[0]; i++) {
        demod_state* demod = alloc_zeroed_demod();
        if (!demod) {
            DSD_FPRINTF(stderr, "analog default: allocation failed\n");
            return 1;
        }
        static dsd_opts opts;
        make_analog_opts(&opts);
        char err[DSD_ANALOG_ERROR_TEXT_MAX] = {0};
        char label[96];
        DSD_SNPRINTF(label, sizeof label, "analog default @%d", rows[i].bw_hz);
        rc |= expect_int_eq(label, configure_and_finalize(demod, &opts, rows[i].bw_hz, err, sizeof err), 0);
        rc |= expect_int_eq("analog default output kind", demod->output_kind, DSD_DEMOD_OUTPUT_AUDIO_MONITOR);
        rc |= expect_int_eq("analog default family", demod->analog_family, 1);
        rc |= expect_int_eq("analog default kind", demod->analog_demod, DSD_ANALOG_DEMOD_FM);
        rc |= expect_int_eq("analog default LPF enable", demod->channel_lpf_enable, rows[i].want_enable);
        rc |= expect_int_eq("analog default width", demod->channel_lpf_width_hz, rows[i].want_width);
        rc |= expect_int_eq("analog default profile", demod->channel_lpf_profile, DSD_CH_LPF_PROFILE_WIDE);
        rc |= expect_int_eq("analog default deemph", demod->deemph, 1);
        rtl_demod_cleanup(demod);
        dsd_neo_aligned_free(demod);
    }
    return rc;
}

/*
 * IQ replay rewrites rate_in from the capture after stream configuration. The unset
 * default keeps the enable decision configuration made (the historical behaviour),
 * rather than re-deciding it from the replay's rate_in at finalize.
 */
static int
expect_analog_default_enable_survives_replay_rate(void) {
    int rc = 0;
    set_channel_lpf_env(NULL);
    const int configured_bw[] = {48000, 12000};
    const int replay_rate_in[] = {12000, 48000};
    for (int i = 0; i < 2; i++) {
        demod_state* demod = alloc_zeroed_demod();
        if (!demod) {
            DSD_FPRINTF(stderr, "analog default replay rate: allocation failed\n");
            return 1;
        }
        output_state output;
        DSD_MEMSET(&output, 0, sizeof(output));
        output.rate = configured_bw[i];
        static dsd_opts opts;
        make_analog_opts(&opts);
        rtl_demod_init_for_mode(demod, &output, &opts, configured_bw[i]);
        rtl_demod_config_from_env_and_opts(demod, &opts);
        const int configured_enable = demod->channel_lpf_enable;
        demod->rate_in = replay_rate_in[i];
        demod->rate_out = 48000;
        char err[DSD_ANALOG_ERROR_TEXT_MAX] = {0};
        rc |= expect_int_eq("replay finalize", rtl_demod_finalize_analog_channel(demod, &opts, err, sizeof err), 0);
        rc |=
            expect_int_eq("replay keeps the configured enable decision", demod->channel_lpf_enable, configured_enable);
        rc |= expect_int_eq("replay default width", demod->channel_lpf_width_hz, 16000);
        rtl_demod_cleanup(demod);
        dsd_neo_aligned_free(demod);
    }
    return rc;
}

/* An explicit width, including 16000, turns the channel filter on at any rate that fits it. */
static int
expect_analog_explicit_width_forces_lpf(void) {
    struct {
        int bw_hz;
        int width_hz;
    } rows[] = {{12000, 8000}, {16000, 12500}, {24000, 16000}, {48000, 25000}};

    int rc = 0;
    set_channel_lpf_env(NULL);
    for (size_t i = 0; i < sizeof rows / sizeof rows[0]; i++) {
        demod_state* demod = alloc_zeroed_demod();
        if (!demod) {
            DSD_FPRINTF(stderr, "explicit width: allocation failed\n");
            return 1;
        }
        static dsd_opts opts;
        make_analog_opts(&opts);
        opts.analog_nfm_bandwidth_hz = rows[i].width_hz;
        char err[DSD_ANALOG_ERROR_TEXT_MAX] = {0};
        rc |= expect_int_eq("explicit width accepted",
                            configure_and_finalize(demod, &opts, rows[i].bw_hz, err, sizeof err), 0);
        rc |= expect_int_eq("explicit width forces LPF", demod->channel_lpf_enable, 1);
        rc |= expect_int_eq("explicit width drives the filter", demod->channel_lpf_width_hz, rows[i].width_hz);
        rtl_demod_cleanup(demod);
        dsd_neo_aligned_free(demod);
    }

    /* A width the rate cannot fit fails the start with the validator's text. */
    demod_state* demod = alloc_zeroed_demod();
    if (!demod) {
        DSD_FPRINTF(stderr, "unrealizable explicit width: allocation failed\n");
        return 1;
    }
    static dsd_opts opts;
    make_analog_opts(&opts);
    opts.analog_nfm_bandwidth_hz = 16000;
    char err[DSD_ANALOG_ERROR_TEXT_MAX] = {0};
    rc |= expect_int_eq("16 kHz at 16 kHz refused", configure_and_finalize(demod, &opts, 16000, err, sizeof err), -1);
    if (!std::strstr(err, "NFM bandwidth 16 kHz") || !std::strstr(err, "set the RTL DSP bandwidth to 24 or 48 kHz")) {
        DSD_FPRINTF(stderr, "unrealizable explicit width message: %s\n", err);
        rc = 1;
    }
    rtl_demod_cleanup(demod);
    dsd_neo_aligned_free(demod);
    return rc;
}

/* DSD_NEO_CHANNEL_LPF=0 turns the filter off; an explicit width cannot run without it. */
static int
expect_analog_env_off_conflict(void) {
    int rc = 0;
    set_channel_lpf_env("0");
    demod_state* demod = alloc_zeroed_demod();
    if (!demod) {
        DSD_FPRINTF(stderr, "env-off conflict: allocation failed\n");
        set_channel_lpf_env(NULL);
        return 1;
    }
    static dsd_opts opts;
    make_analog_opts(&opts);
    opts.analog_nfm_bandwidth_hz = 12500;
    char err[DSD_ANALOG_ERROR_TEXT_MAX] = {0};
    rc |= expect_int_eq("explicit width with LPF env off refused",
                        configure_and_finalize(demod, &opts, 48000, err, sizeof err), -1);
    if (!std::strstr(err, "DSD_NEO_CHANNEL_LPF")) {
        DSD_FPRINTF(stderr, "env-off conflict message does not name the variable: %s\n", err);
        rc = 1;
    }
    rtl_demod_cleanup(demod);
    dsd_neo_aligned_free(demod);

    /* The unset default simply follows the environment. */
    demod = alloc_zeroed_demod();
    if (!demod) {
        DSD_FPRINTF(stderr, "env-off default: allocation failed\n");
        set_channel_lpf_env(NULL);
        return 1;
    }
    make_analog_opts(&opts);
    rc |= expect_int_eq("default with LPF env off starts", configure_and_finalize(demod, &opts, 48000, err, sizeof err),
                        0);
    rc |= expect_int_eq("default with LPF env off leaves LPF off", demod->channel_lpf_enable, 0);
    rtl_demod_cleanup(demod);
    dsd_neo_aligned_free(demod);
    set_channel_lpf_env(NULL);
    return rc;
}

/* The M17 encoder shares the analog front end but is not the analog family. */
static int
expect_m17_encoder_unchanged(void) {
    int rc = 0;
    set_channel_lpf_env(NULL);
    const int both[] = {0, 1};
    for (int analog_only : both) {
        demod_state* demod = alloc_zeroed_demod();
        if (!demod) {
            DSD_FPRINTF(stderr, "M17 encoder: allocation failed\n");
            return 1;
        }
        static dsd_opts opts;
        DSD_MEMSET(&opts, 0, sizeof(opts));
        opts.m17encoder = 1;
        opts.analog_only = analog_only;
        opts.analog_nfm_bandwidth_hz = 8000; /* ignored: not the analog family */
        char err[DSD_ANALOG_ERROR_TEXT_MAX] = {0};
        rc |= expect_int_eq("M17 encoder starts", configure_and_finalize(demod, &opts, 48000, err, sizeof err), 0);
        rc |= expect_int_eq("M17 encoder monitor output", demod->output_kind, DSD_DEMOD_OUTPUT_AUDIO_MONITOR);
        rc |= expect_int_eq("M17 encoder not analog family", demod->analog_family, 0);
        rc |= expect_int_eq("M17 encoder keeps profile design", demod->channel_lpf_width_hz, 0);
        rc |= expect_int_eq("M17 encoder WIDE profile", demod->channel_lpf_profile, DSD_CH_LPF_PROFILE_WIDE);
        rc |= expect_int_eq("M17 encoder LPF rule", demod->channel_lpf_enable, 1);
        rc |= expect_int_eq("M17 encoder deemph", demod->deemph, 1);
        rtl_demod_cleanup(demod);
        dsd_neo_aligned_free(demod);
    }
    return rc;
}

/*
 * The analog family's monitor audio comes from the FM discriminator. DSD_NEO_CQPSK=1, or a QPSK modulation left on the
 * options, must not put a -fA open on the CQPSK path, which would hand the monitor differential phase symbols instead
 * of audio; a live switch to analog lands on FM the same way. A digital open under the override still runs CQPSK.
 */
static int
expect_analog_open_ignores_cqpsk(void) {
    int rc = 0;
    set_channel_lpf_env(NULL);
    (void)dsd_setenv("DSD_NEO_CQPSK", "1", 1);
    dsd_neo_config_init();
    for (int mod_qpsk = 0; mod_qpsk <= 1; mod_qpsk++) {
        demod_state* demod = alloc_zeroed_demod();
        if (!demod) {
            DSD_FPRINTF(stderr, "analog open under DSD_NEO_CQPSK=1: allocation failed\n");
            rc = 1;
            break;
        }
        static dsd_opts opts;
        make_analog_opts(&opts);
        opts.mod_qpsk = mod_qpsk;
        char err[DSD_ANALOG_ERROR_TEXT_MAX] = {0};
        char label[128];
        DSD_SNPRINTF(label, sizeof label, "-fA under DSD_NEO_CQPSK=1 (mod_qpsk %d)", mod_qpsk);
        rc |= expect_int_eq(label, configure_and_finalize(demod, &opts, 48000, err, sizeof err), 0);
        rc |= expect_int_eq("  monitor output", demod->output_kind, DSD_DEMOD_OUTPUT_AUDIO_MONITOR);
        rc |= expect_int_eq("  CQPSK off", demod->cqpsk_enable, 0);
        rc |= expect_int_eq("  FM discriminator", demod->mode_demod == &dsd_fm_demod ? 1 : 0, 1);
        rc |= expect_int_eq("  no symbol timing", demod->ted_enabled, 0);
        rc |= expect_int_eq("  analog channel", demod->channel_lpf_width_hz, DSD_ANALOG_NFM_WIDTH_DEFAULT_HZ);
        rc |= expect_int_eq("  analog profile", demod->channel_lpf_profile, DSD_CH_LPF_PROFILE_WIDE);
        rc |= expect_int_eq("  de-emphasis", demod->deemph, 1);
        rtl_demod_cleanup(demod);
        dsd_neo_aligned_free(demod);
    }

    demod_state* demod = alloc_zeroed_demod();
    if (demod) {
        static dsd_opts opts;
        DSD_MEMSET(&opts, 0, sizeof(opts));
        opts.frame_dmr = 1;
        opts.mod_c4fm = 1;
        char err[DSD_ANALOG_ERROR_TEXT_MAX] = {0};
        rc |= expect_int_eq("DMR under DSD_NEO_CQPSK=1 starts",
                            configure_and_finalize(demod, &opts, 48000, err, sizeof err), 0);
        rc |= expect_int_eq("  CQPSK symbols", demod->output_kind, DSD_DEMOD_OUTPUT_SYMBOL_CQPSK);
        rc |= expect_int_eq("  CQPSK on", demod->cqpsk_enable, 1);
        rtl_demod_cleanup(demod);
        dsd_neo_aligned_free(demod);
    } else {
        DSD_FPRINTF(stderr, "digital open under DSD_NEO_CQPSK=1: allocation failed\n");
        rc = 1;
    }
    (void)dsd_unsetenv("DSD_NEO_CQPSK");
    dsd_neo_config_init();
    return rc;
}

/* Native AM is not available yet; asking for it must not silently run FM. */
static int
expect_analog_am_refused(void) {
    int rc = 0;
    demod_state* demod = alloc_zeroed_demod();
    if (!demod) {
        DSD_FPRINTF(stderr, "AM refusal: allocation failed\n");
        return 1;
    }
    static dsd_opts opts;
    make_analog_opts(&opts);
    opts.analog_demod = DSD_ANALOG_DEMOD_AM;
    char err[DSD_ANALOG_ERROR_TEXT_MAX] = {0};
    rc |= expect_int_eq("AM start refused", configure_and_finalize(demod, &opts, 48000, err, sizeof err), -1);
    if (!std::strstr(err, "AM")) {
        DSD_FPRINTF(stderr, "AM refusal message: %s\n", err);
        rc = 1;
    }
    rc |= expect_int_eq("AM check refused",
                        rtl_demod_check_analog_channel(DSD_ANALOG_DEMOD_AM, 6000, 48000, err, sizeof err), -1);
    rc |= expect_int_eq("unset FM default never refused",
                        rtl_demod_check_analog_channel(DSD_ANALOG_DEMOD_FM, 0, 8000, err, sizeof err), 0);
    rtl_demod_cleanup(demod);
    dsd_neo_aligned_free(demod);
    return rc;
}

/*
 * Only the unset NFM default keeps the historical enable rule and the legacy WIDE fallback. The unset AM default is a
 * requested width like any explicit one: it forces the channel filter on at the AM default and is validated. Pinned
 * below the stream's AM refusal so the rule holds when that refusal goes.
 */
static int
expect_unset_default_rule_keyed_on_nfm(void) {
    int rc = 0;
    set_channel_lpf_env(NULL);
    rc |=
        expect_int_eq("unset NFM default stays unset", rtl_demod_analog_requested_width_hz(DSD_ANALOG_DEMOD_FM, 0), 0);
    rc |= expect_int_eq("unset AM default is requested", rtl_demod_analog_requested_width_hz(DSD_ANALOG_DEMOD_AM, 0),
                        DSD_ANALOG_AM_WIDTH_DEFAULT_HZ);
    rc |= expect_int_eq("explicit width kept", rtl_demod_analog_requested_width_hz(DSD_ANALOG_DEMOD_AM, 9000), 9000);

    /* 12 kHz DSP bandwidth: rate_in below 20 kHz, so the legacy rule leaves the filter off for the NFM default. */
    demod_state* demod = alloc_zeroed_demod();
    if (!demod) {
        DSD_FPRINTF(stderr, "unset default rule: allocation failed\n");
        return 1;
    }
    static dsd_opts opts;
    make_analog_opts(&opts);
    char err[DSD_ANALOG_ERROR_TEXT_MAX] = {0};
    rc |= expect_int_eq("12 kHz NFM default start", configure_and_finalize(demod, &opts, 12000, err, sizeof err), 0);
    rc |= expect_int_eq("12 kHz NFM default leaves LPF off", demod->channel_lpf_enable, 0);
    (void)rtl_demod_apply_analog_channel(demod, DSD_ANALOG_DEMOD_AM, 0);
    rc |= expect_int_eq("12 kHz AM default forces LPF on", demod->channel_lpf_enable, 1);
    rc |= expect_int_eq("12 kHz AM default width", demod->channel_lpf_width_hz, DSD_ANALOG_AM_WIDTH_DEFAULT_HZ);
    rc |= expect_int_eq("AM default kept for rate changes", demod->analog_width_request_hz,
                        DSD_ANALOG_AM_WIDTH_DEFAULT_HZ);
    (void)rtl_demod_apply_analog_channel(demod, DSD_ANALOG_DEMOD_FM, 0);
    rc |= expect_int_eq("back to the NFM default restores the legacy rule", demod->channel_lpf_enable, 0);
    rc |= expect_int_eq("NFM default request stays unset", demod->analog_width_request_hz, 0);
    rtl_demod_cleanup(demod);
    dsd_neo_aligned_free(demod);

    /* With no DSP rate yet (no stream), only the kind, range and environment rules apply. */
    rc |= expect_int_eq("no-rate in-range width accepted",
                        rtl_demod_check_analog_channel(DSD_ANALOG_DEMOD_FM, 25000, 0, err, sizeof err), 0);
    rc |= expect_int_eq("no-rate out-of-range width refused",
                        rtl_demod_check_analog_channel(DSD_ANALOG_DEMOD_FM, 30000, 0, err, sizeof err), -1);
    if (!std::strstr(err, "outside the supported range")) {
        DSD_FPRINTF(stderr, "no-rate range message: %s\n", err);
        rc = 1;
    }
    set_channel_lpf_env("0");
    rc |= expect_int_eq("no-rate explicit width with LPF env off refused",
                        rtl_demod_check_analog_channel(DSD_ANALOG_DEMOD_FM, 12500, 0, err, sizeof err), -1);
    set_channel_lpf_env(NULL);
    return rc;
}

/*
 * Only 0 in the options means "the kind's default": a negative width is refused with an actionable message, both when
 * a stream starts (the finalize against its rate) and by the shared check the live requests use, never treated as the
 * unset default.
 */
static int
expect_negative_width_refused(void) {
    int rc = 0;
    set_channel_lpf_env(NULL);
    char err[DSD_ANALOG_ERROR_TEXT_MAX] = {0};
    rc |= expect_int_eq("negative NFM width refused at a rate",
                        rtl_demod_check_analog_channel(DSD_ANALOG_DEMOD_FM, -1, 48000, err, sizeof err), -1);
    if (!std::strstr(err, "NFM bandwidth -1 Hz is negative; set 0 for the 16 kHz default or a width from 8000 to "
                          "25000 Hz")) {
        DSD_FPRINTF(stderr, "negative width message: %s\n", err);
        rc = 1;
    }
    rc |= expect_int_eq("negative NFM width refused with no rate",
                        rtl_demod_check_analog_channel(DSD_ANALOG_DEMOD_FM, -12500, 0, err, sizeof err), -1);

    demod_state* demod = alloc_zeroed_demod();
    if (!demod) {
        DSD_FPRINTF(stderr, "negative width finalize: allocation failed\n");
        return 1;
    }
    static dsd_opts opts;
    make_analog_opts(&opts);
    opts.analog_nfm_bandwidth_hz = -1;
    DSD_MEMSET(err, 0, sizeof err);
    rc |= expect_int_eq("negative NFM width fails the start",
                        configure_and_finalize(demod, &opts, 48000, err, sizeof err), -1);
    if (!std::strstr(err, "NFM bandwidth -1 Hz is negative")) {
        DSD_FPRINTF(stderr, "negative width start message: %s\n", err);
        rc = 1;
    }
    rtl_demod_cleanup(demod);
    dsd_neo_aligned_free(demod);
    return rc;
}

/*
 * An I/Q replay whose sidecar decimates after the demodulator runs the channel filter at rate_out x post_downsample,
 * not at the rate_out a width is designed and checked at: a requested width is refused there with the cause and the
 * fix, while the unset NFM default and every post_downsample-1 chain pass.
 */
static int
expect_post_decimation_rule(void) {
    int rc = 0;
    char err[DSD_ANALOG_ERROR_TEXT_MAX] = {0};
    rc |=
        expect_int_eq("no post decimation passes",
                      rtl_demod_check_analog_post_decimation(DSD_ANALOG_DEMOD_FM, 12500, 16000, 1, err, sizeof err), 0);
    rc |= expect_int_eq("unset NFM default passes post decimation",
                        rtl_demod_check_analog_post_decimation(DSD_ANALOG_DEMOD_FM, 0, 16000, 3, err, sizeof err), 0);
    rc |= expect_int_eq("explicit width refused under post decimation",
                        rtl_demod_check_analog_post_decimation(DSD_ANALOG_DEMOD_FM, 12500, 16000, 3, err, sizeof err),
                        -1);
    if (!std::strstr(err, "NFM bandwidth 12.5 kHz cannot be applied to this I/Q replay: post_downsample 3 runs the "
                          "channel filter at 48000 Hz, not the 16000 Hz demod rate")
        || !std::strstr(err, "use a capture with post_downsample 1")) {
        DSD_FPRINTF(stderr, "post-decimation message: %s\n", err);
        rc = 1;
    }
    rc |= expect_int_eq("unset AM default is a requested width",
                        rtl_demod_check_analog_post_decimation(DSD_ANALOG_DEMOD_AM, 0, 16000, 2, err, sizeof err), -1);

    /* The stream-start finalize applies the same rule against the stream's own chain. */
    set_channel_lpf_env(NULL);
    demod_state* demod = alloc_zeroed_demod();
    if (!demod) {
        DSD_FPRINTF(stderr, "post decimation finalize: allocation failed\n");
        return 1;
    }
    output_state output;
    DSD_MEMSET(&output, 0, sizeof(output));
    output.rate = 48000;
    static dsd_opts opts;
    make_analog_opts(&opts);
    opts.analog_nfm_bandwidth_hz = 12500;
    rtl_demod_init_for_mode(demod, &output, &opts, 48000);
    rtl_demod_config_from_env_and_opts(demod, &opts);
    rtl_demod_select_defaults_for_mode(demod, &opts, &output);
    demod->post_downsample = 3;
    demod->rate_out = 16000;
    rc |= expect_int_eq("finalize refuses a width under post decimation",
                        rtl_demod_finalize_analog_channel(demod, &opts, err, sizeof err), -1);
    opts.analog_nfm_bandwidth_hz = 0;
    rc |= expect_int_eq("finalize keeps the default under post decimation",
                        rtl_demod_finalize_analog_channel(demod, &opts, err, sizeof err), 0);
    rtl_demod_cleanup(demod);
    dsd_neo_aligned_free(demod);
    return rc;
}

/*
 * CQPSK toggled on under -fA keeps the analog family flag but runs the P25 CQPSK profile filter, and a typed digital
 * scan row's symbol profile keeps the monitor output with the row's channel profile. A retune that moves rate_out must
 * not re-apply the analog channel (WIDE) over either; back on the analog channel, it resolves the width again.
 */
static int
expect_rate_refresh_leaves_cqpsk_profile(void) {
    int rc = 0;
    set_channel_lpf_env(NULL);
    demod_state* demod = alloc_zeroed_demod();
    if (!demod) {
        DSD_FPRINTF(stderr, "rate refresh CQPSK profile: allocation failed\n");
        return 1;
    }
    static dsd_opts opts;
    make_analog_opts(&opts);
    opts.analog_nfm_bandwidth_hz = 12500;
    char err[DSD_ANALOG_ERROR_TEXT_MAX] = {0};
    rc |=
        expect_int_eq("analog start for CQPSK toggle", configure_and_finalize(demod, &opts, 48000, err, sizeof err), 0);
    demod->cqpsk_enable = 1;
    demod->output_kind = DSD_DEMOD_OUTPUT_SYMBOL_CQPSK;
    demod->channel_lpf_profile = DSD_CH_LPF_PROFILE_P25_CQPSK;
    const int enable_before = demod->channel_lpf_enable;
    demod->rate_out = 78125;
    rc |=
        expect_int_eq("CQPSK under -fA refresh", rtl_demod_refresh_analog_channel_for_rate(demod, err, sizeof err), 0);
    rc |= expect_int_eq("CQPSK profile kept across the rate change", demod->channel_lpf_profile,
                        DSD_CH_LPF_PROFILE_P25_CQPSK);
    rc |= expect_int_eq("CQPSK filter enable kept", demod->channel_lpf_enable, enable_before);
    rc |= expect_int_eq("analog family still set", demod->analog_family, 1);

    /* CQPSK off again for a typed DMR row: the monitor output, but the row's channel profile, not the analog one. */
    demod->cqpsk_enable = 0;
    demod->output_kind = DSD_DEMOD_OUTPUT_AUDIO_MONITOR;
    demod->channel_lpf_profile = DSD_CH_LPF_PROFILE_12K5;
    rc |= expect_int_eq("typed row refresh", rtl_demod_refresh_analog_channel_for_rate(demod, err, sizeof err), 0);
    rc |= expect_int_eq("typed row profile kept across the rate change", demod->channel_lpf_profile,
                        DSD_CH_LPF_PROFILE_12K5);

    /* Back on the analog channel (an analog request restores WIDE), the refresh resolves the analog width again. */
    demod->channel_lpf_profile = DSD_CH_LPF_PROFILE_WIDE;
    demod->channel_lpf_width_hz = 0;
    rc |= expect_int_eq("monitor refresh", rtl_demod_refresh_analog_channel_for_rate(demod, err, sizeof err), 0);
    rc |= expect_int_eq("monitor refresh keeps WIDE", demod->channel_lpf_profile, DSD_CH_LPF_PROFILE_WIDE);
    rc |= expect_int_eq("monitor refresh resolves the explicit width", demod->channel_lpf_width_hz, 12500);
    rtl_demod_cleanup(demod);
    dsd_neo_aligned_free(demod);
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

    rc |= expect_analog_legacy_default_enable();
    rc |= expect_analog_default_enable_survives_replay_rate();
    rc |= expect_analog_explicit_width_forces_lpf();
    rc |= expect_analog_env_off_conflict();
    rc |= expect_m17_encoder_unchanged();
    rc |= expect_analog_open_ignores_cqpsk();
    rc |= expect_analog_am_refused();
    rc |= expect_unset_default_rule_keyed_on_nfm();
    rc |= expect_negative_width_refused();
    rc |= expect_post_decimation_rule();
    rc |= expect_rate_refresh_leaves_cqpsk_profile();

    rc |= expect_live_symbol_status();
    rc |= expect_cqpsk_toggle_clears_output_contract_backlog();
    rc |= expect_cqpsk_toggle_restores_fsk_channel_profile();
    rc |= expect_rtl_metrics_do_not_nudge_cqpsk_bandedge();
    rc |= expect_rtl_metrics_exports_and_toggles();
    rc |= expect_public_control_wrapper_contracts();
    rc |= expect_fsk_snr_sps_uses_active_profile();
    rc |= expect_retune_preserves_active_profile();
    rc |= expect_preserved_profile_keeps_fsk_modem_config();
    rc |= expect_finalize_rate_chain_preserves_profile();
    rc |= expect_direct_output_open_rate_uses_demod_rate();
    rc |= expect_passes_for_device_rate();
    rc |= expect_digital_resample_policy();
    rc |= expect_private_policy_matrices();
    rc |= expect_steady_state_watermark_disabled("rtl_tcp keeps demod watermark disabled", "rtltcp:127.0.0.1:1234");
    rc |= expect_steady_state_watermark_disabled("rtlsdr keeps demod watermark disabled", "rtl");
    rc |= expect_steady_state_watermark_disabled("soapy keeps demod watermark disabled", "soapy:driver=test");

    return rc;
}
