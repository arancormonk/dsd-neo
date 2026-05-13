// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <dsd-neo/core/opts.h>
#include <dsd-neo/dsp/demod_pipeline.h>
#include <dsd-neo/dsp/demod_state.h>
#include <dsd-neo/io/rtl_demod_config.h>
#include <dsd-neo/runtime/ring.h>

#include "dsd-neo/core/opts_fwd.h"

static void
clear_demod_env(void) {
    unsetenv("DSD_NEO_AUDIO_LPF");
    unsetenv("DSD_NEO_CHANNEL_LPF");
    unsetenv("DSD_NEO_CQPSK");
    unsetenv("DSD_NEO_CQPSK_EQ");
    unsetenv("DSD_NEO_CQPSK_EQ_MODULUS");
    unsetenv("DSD_NEO_CQPSK_EQ_MU");
    unsetenv("DSD_NEO_CQPSK_EQ_TAPS");
    unsetenv("DSD_NEO_FLL");
    unsetenv("DSD_NEO_FM_AGC");
    unsetenv("DSD_NEO_FM_LIMITER");
    unsetenv("DSD_NEO_IQ_BALANCE");
    unsetenv("DSD_NEO_IQ_DC_BLOCK");
    unsetenv("DSD_NEO_RESAMP");
    unsetenv("DSD_NEO_TED");
}

static int
expect_sps(const char* label, const dsd_opts& opts, int rate_hz, int override_sps, int want_sps, int want_profile) {
    static demod_state demod;
    output_state output;
    std::memset(&demod, 0, sizeof(demod));
    std::memset(&output, 0, sizeof(output));
    demod.cqpsk_enable = opts.mod_qpsk ? 1 : 0;
    demod.rate_out = rate_hz;
    demod.ted_sps_override = override_sps;
    output.rate = static_cast<unsigned int>(rate_hz);

    rtl_demod_maybe_refresh_ted_sps_after_rate_change(&demod, &opts, &output);
    if (demod.ted_sps != want_sps) {
        std::fprintf(stderr, "%s: got ted_sps=%d want=%d\n", label, demod.ted_sps, want_sps);
        return 1;
    }
    if (want_profile >= 0 && demod.channel_lpf_profile != want_profile) {
        std::fprintf(stderr, "%s: got channel_lpf_profile=%d want=%d\n", label, demod.channel_lpf_profile,
                     want_profile);
        return 1;
    }
    return 0;
}

static int
expect_sdrpp_fm_family_default_chain(void) {
    dsd_opts opts;
    static demod_state demod;
    output_state output;
    std::memset(&opts, 0, sizeof(opts));
    std::memset(&demod, 0, sizeof(demod));
    std::memset(&output, 0, sizeof(output));

    opts.frame_p25p1 = 1;
    opts.mod_c4fm = 1;
    opts.rtl_squelch_level = 0.0;
    clear_demod_env();

    rtl_demod_init_for_mode(&demod, &output, &opts, 48000);
    rtl_demod_config_from_env_and_opts(&demod, &opts);
    rtl_demod_select_defaults_for_mode(&demod, &opts, &output);
    rtl_demod_maybe_update_resampler_after_rate_change(&demod, &output, 48000);

    int rc = 0;
    if (demod.cqpsk_enable != 0) {
        std::fprintf(stderr, "SDR++ FM-family default: cqpsk_enable=%d want 0\n", demod.cqpsk_enable);
        rc = 1;
    }
    if (demod.mode_demod != &dsd_fm_demod) {
        std::fprintf(stderr, "SDR++ FM-family default: mode_demod is not dsd_fm_demod\n");
        rc = 1;
    }
    if (demod.channel_lpf_enable != 1) {
        std::fprintf(stderr, "SDR++ FM-family default: channel_lpf_enable=%d want 1\n", demod.channel_lpf_enable);
        rc = 1;
    }
    if (demod.channel_lpf_profile != DSD_CH_LPF_PROFILE_P25_C4FM) {
        std::fprintf(stderr, "SDR++ FM-family default: channel_lpf_profile=%d want %d\n", demod.channel_lpf_profile,
                     DSD_CH_LPF_PROFILE_P25_C4FM);
        rc = 1;
    }
    if (demod.fm_demod_bw_hz != 12500) {
        std::fprintf(stderr, "SDR++ FM-family default: fm_demod_bw_hz=%d want 12500\n", demod.fm_demod_bw_hz);
        rc = 1;
    }
    if (demod.fm_audio_lpf_enable != 1) {
        std::fprintf(stderr, "SDR++ FM-family default: fm_audio_lpf_enable=%d want 1\n", demod.fm_audio_lpf_enable);
        rc = 1;
    }
    if (demod.fll_enabled != 0 || demod.ted_enabled != 0 || demod.fm_agc_enable != 0 || demod.fm_limiter_enable != 0
        || demod.iq_dc_block_enable != 0 || demod.iqbal_enable != 0 || demod.audio_lpf_enable != 0
        || demod.dc_block != 0 || demod.rate_out2 > 0) {
        std::fprintf(stderr,
                     "SDR++ FM-family default: unexpected block enabled "
                     "(fll=%d ted=%d agc=%d limiter=%d iqdc=%d iqbal=%d audiolpf=%d dcblock=%d rateout2=%d)\n",
                     demod.fll_enabled, demod.ted_enabled, demod.fm_agc_enable, demod.fm_limiter_enable,
                     demod.iq_dc_block_enable, demod.iqbal_enable, demod.audio_lpf_enable, demod.dc_block,
                     demod.rate_out2);
        rc = 1;
    }
    if (demod.channel_squelch_level != 0.0f) {
        std::fprintf(stderr, "SDR++ FM-family default: channel_squelch_level=%f want 0\n", demod.channel_squelch_level);
        rc = 1;
    }
    if (demod.resamp_enabled != 0 || output.rate != 48000U) {
        std::fprintf(stderr, "SDR++ FM-family default: resamp_enabled=%d output.rate=%u want bypass/48000\n",
                     demod.resamp_enabled, output.rate);
        rc = 1;
    }

    rtl_demod_cleanup(&demod);
    clear_demod_env();
    return rc;
}

static int
expect_qpsk_op25_default_chain(void) {
    dsd_opts opts;
    static demod_state demod;
    output_state output;
    std::memset(&opts, 0, sizeof(opts));
    std::memset(&demod, 0, sizeof(demod));
    std::memset(&output, 0, sizeof(output));

    opts.frame_p25p1 = 1;
    opts.mod_qpsk = 1;
    opts.rtl_squelch_level = 0.0;
    clear_demod_env();

    rtl_demod_init_for_mode(&demod, &output, &opts, 48000);
    rtl_demod_config_from_env_and_opts(&demod, &opts);
    rtl_demod_select_defaults_for_mode(&demod, &opts, &output);

    int rc = 0;
    if (demod.cqpsk_enable != 1) {
        std::fprintf(stderr, "QPSK OP25 default: cqpsk_enable=%d want 1\n", demod.cqpsk_enable);
        rc = 1;
    }
    if (demod.mode_demod != &qpsk_differential_demod) {
        std::fprintf(stderr, "QPSK OP25 default: mode_demod is not qpsk_differential_demod\n");
        rc = 1;
    }
    if (demod.ted_enabled != 1 || demod.fll_enabled != 0) {
        std::fprintf(stderr, "QPSK OP25 default: ted_enabled=%d want 1 fll_enabled=%d want 0\n", demod.ted_enabled,
                     demod.fll_enabled);
        rc = 1;
    }
    if (demod.fm_demod_bw_hz != 0 || demod.fm_audio_lpf_enable != 0) {
        std::fprintf(stderr, "QPSK OP25 default: fm_demod_bw_hz=%d fm_audio_lpf_enable=%d want 0/0\n",
                     demod.fm_demod_bw_hz, demod.fm_audio_lpf_enable);
        rc = 1;
    }
    if (demod.channel_lpf_profile != DSD_CH_LPF_PROFILE_P25_CQPSK) {
        std::fprintf(stderr, "QPSK OP25 default: channel_lpf_profile=%d want %d\n", demod.channel_lpf_profile,
                     DSD_CH_LPF_PROFILE_P25_CQPSK);
        rc = 1;
    }
    if (demod.cqpsk_eq_enable != 1 || demod.output_scale != 1.0f) {
        std::fprintf(stderr, "QPSK OP25 default: cqpsk_eq_enable=%d output_scale=%f want 1/1\n", demod.cqpsk_eq_enable,
                     demod.output_scale);
        rc = 1;
    }
    if (demod.channel_squelch_level != 0.0f) {
        std::fprintf(stderr, "QPSK OP25 default: channel_squelch_level=%f want 0\n", demod.channel_squelch_level);
        rc = 1;
    }

    rtl_demod_cleanup(&demod);
    clear_demod_env();
    return rc;
}

static int
expect_explicit_channel_squelch_passthrough(void) {
    dsd_opts opts;
    static demod_state demod;
    output_state output;
    std::memset(&opts, 0, sizeof(opts));
    std::memset(&demod, 0, sizeof(demod));
    std::memset(&output, 0, sizeof(output));

    opts.frame_p25p1 = 1;
    opts.mod_c4fm = 1;
    opts.rtl_squelch_level = 0.25;
    clear_demod_env();

    rtl_demod_init_for_mode(&demod, &output, &opts, 48000);
    rtl_demod_config_from_env_and_opts(&demod, &opts);

    int rc = 0;
    if (demod.channel_squelch_level != 0.25f) {
        std::fprintf(stderr, "explicit squelch: channel_squelch_level=%f want 0.25\n", demod.channel_squelch_level);
        rc = 1;
    }

    rtl_demod_cleanup(&demod);
    clear_demod_env();
    return rc;
}

static int
expect_narrow_fm_profile_when_channel_lpf_disabled(void) {
    dsd_opts opts;
    static demod_state demod;
    output_state output;
    std::memset(&opts, 0, sizeof(opts));
    std::memset(&demod, 0, sizeof(demod));
    std::memset(&output, 0, sizeof(output));

    opts.frame_nxdn48 = 1;
    clear_demod_env();
    setenv("DSD_NEO_CHANNEL_LPF", "0", 1);

    rtl_demod_init_for_mode(&demod, &output, &opts, 48000);
    rtl_demod_config_from_env_and_opts(&demod, &opts);

    int rc = 0;
    if (demod.channel_lpf_enable != 0) {
        std::fprintf(stderr, "NXDN48 LPF-off: channel_lpf_enable=%d want 0\n", demod.channel_lpf_enable);
        rc = 1;
    }
    if (demod.channel_lpf_profile != DSD_CH_LPF_PROFILE_6K25) {
        std::fprintf(stderr, "NXDN48 LPF-off: channel_lpf_profile=%d want %d\n", demod.channel_lpf_profile,
                     DSD_CH_LPF_PROFILE_6K25);
        rc = 1;
    }
    if (demod.fm_demod_bw_hz != 6250) {
        std::fprintf(stderr, "NXDN48 LPF-off: fm_demod_bw_hz=%d want 6250\n", demod.fm_demod_bw_hz);
        rc = 1;
    }

    rtl_demod_cleanup(&demod);
    clear_demod_env();
    return rc;
}

int
main(void) {
    int rc = 0;

    rc |= expect_sdrpp_fm_family_default_chain();
    rc |= expect_qpsk_op25_default_chain();
    rc |= expect_explicit_channel_squelch_passthrough();

    dsd_opts p25p2_qpsk;
    std::memset(&p25p2_qpsk, 0, sizeof(p25p2_qpsk));
    p25p2_qpsk.frame_p25p2 = 1;
    p25p2_qpsk.mod_qpsk = 1;
    rc |= expect_sps("P25P2-only QPSK uses 6 ksps", p25p2_qpsk, 48000, 0, 8, DSD_CH_LPF_PROFILE_P25_CQPSK);

    dsd_opts p25p1_qpsk;
    std::memset(&p25p1_qpsk, 0, sizeof(p25p1_qpsk));
    p25p1_qpsk.frame_p25p1 = 1;
    p25p1_qpsk.mod_qpsk = 1;
    rc |= expect_sps("P25P1 QPSK uses 4.8 ksps", p25p1_qpsk, 48000, 0, 10, DSD_CH_LPF_PROFILE_P25_CQPSK);

    dsd_opts p25_trunk_qpsk;
    std::memset(&p25_trunk_qpsk, 0, sizeof(p25_trunk_qpsk));
    p25_trunk_qpsk.frame_p25p1 = 1;
    p25_trunk_qpsk.frame_p25p2 = 1;
    p25_trunk_qpsk.mod_qpsk = 1;
    rc |= expect_sps("P25 trunk QPSK defaults to CC rate", p25_trunk_qpsk, 48000, 0, 10, DSD_CH_LPF_PROFILE_P25_CQPSK);
    rc |= expect_sps("P25 trunk TDMA override wins", p25_trunk_qpsk, 48000, 8, 8, DSD_CH_LPF_PROFILE_P25_CQPSK);

    rc |= expect_narrow_fm_profile_when_channel_lpf_disabled();

    return rc;
}
