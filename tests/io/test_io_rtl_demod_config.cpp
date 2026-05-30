// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <cstdio>
#include <cstdlib>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/dsp/demod_state.h>
#include <dsd-neo/io/rtl_demod_config.h>
#include <dsd-neo/io/rtl_stream_c.h>
#include <dsd-neo/runtime/ring.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"

extern demod_state demod;

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
    if (want_kind == DSD_DEMOD_OUTPUT_SYMBOL_FSK) {
        if (demod->cqpsk_enable != 0 || demod->ted_enabled != 0 || demod->fll_enabled != 0 || demod->fm_agc_enable != 0
            || demod->fm_limiter_enable != 0) {
            DSD_FPRINTF(stderr, "%s: FSK symbol path left non-symbol controls enabled\n", label);
            rc = 1;
        }
    }
    if (want_kind == DSD_DEMOD_OUTPUT_SYMBOL_CQPSK && demod->ted_enabled != 1) {
        DSD_FPRINTF(stderr, "%s: CQPSK symbol path did not force TED on\n", label);
        rc = 1;
    }

    rtl_demod_maybe_update_resampler_after_rate_change(demod, &output, 48000);
    if ((want_kind == DSD_DEMOD_OUTPUT_SYMBOL_FSK || want_kind == DSD_DEMOD_OUTPUT_SYMBOL_CQPSK)
        && output.rate != 48000U) {
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
    if (want_kind == DSD_DEMOD_OUTPUT_SYMBOL_FSK) {
        if (demod->cqpsk_enable != 0 || demod->ted_enabled != 0 || demod->fll_enabled != 0 || demod->fm_agc_enable != 0
            || demod->fm_limiter_enable != 0) {
            DSD_FPRINTF(stderr, "%s: FSK symbol path left non-symbol controls enabled\n", label);
            rc = 1;
        }
    }
    if (want_kind == DSD_DEMOD_OUTPUT_SYMBOL_CQPSK && demod->ted_enabled != 1) {
        DSD_FPRINTF(stderr, "%s: CQPSK symbol path did not force TED on\n", label);
        rc = 1;
    }
    if ((want_kind == DSD_DEMOD_OUTPUT_SYMBOL_FSK || want_kind == DSD_DEMOD_OUTPUT_SYMBOL_CQPSK)
        && output.rate != rtl_dsp_bw_hz) {
        DSD_FPRINTF(stderr, "%s: symbol output changed public output rate to %u\n", label, output.rate);
        rc = 1;
    }

    rtl_demod_cleanup(demod);
    std::free(demod);
    return rc;
}

static int
expect_live_symbol_controls_guarded(void) {
    int rc = 0;

    DSD_MEMSET(&demod, 0, sizeof(demod));
    demod.output_kind = DSD_DEMOD_OUTPUT_SYMBOL_FSK;
    demod.symbol_rate_hz = 4800;
    demod.symbol_levels = 4;

    rtl_stream_toggle_fll(1);
    rtl_stream_toggle_ted(1);
    rtl_stream_set_ted_force(1);
    rtl_stream_set_fm_agc(1);
    rtl_stream_set_fm_limiter(1);

    int cq = -1;
    int fll = -1;
    int ted = -1;
    rtl_stream_dsp_get(&cq, &fll, &ted);
    if (cq != 0 || fll != 0 || ted != 0 || rtl_stream_get_ted_force() != 0 || rtl_stream_get_fm_agc() != 0
        || rtl_stream_get_fm_limiter() != 0) {
        DSD_FPRINTF(stderr, "FSK symbol output did not report guarded non-symbol controls as off\n");
        rc = 1;
    }
    if (demod.fll_enabled != 0 || demod.ted_enabled != 0 || demod.ted_force != 0 || demod.fm_agc_enable != 0
        || demod.fm_limiter_enable != 0) {
        DSD_FPRINTF(stderr, "FSK symbol output retained raw non-symbol control state\n");
        rc = 1;
    }

    DSD_MEMSET(&demod, 0, sizeof(demod));
    demod.output_kind = DSD_DEMOD_OUTPUT_SYMBOL_CQPSK;
    demod.cqpsk_enable = 1;
    demod.fll_enabled = 1;
    demod.ted_force = 1;
    demod.fm_agc_enable = 1;
    demod.fm_limiter_enable = 1;

    rtl_stream_toggle_fll(1);
    rtl_stream_toggle_ted(0);
    rtl_stream_set_ted_force(1);
    rtl_stream_set_fm_agc(1);
    rtl_stream_set_fm_limiter(1);

    cq = -1;
    fll = -1;
    ted = -1;
    rtl_stream_dsp_get(&cq, &fll, &ted);
    if (cq != 1 || fll != 0 || ted != 1 || rtl_stream_get_ted_force() != 0 || rtl_stream_get_fm_agc() != 0
        || rtl_stream_get_fm_limiter() != 0) {
        DSD_FPRINTF(stderr, "CQPSK symbol output did not guard non-symbol controls or force TED status\n");
        rc = 1;
    }
    if (demod.fll_enabled != 0 || demod.ted_force != 0 || demod.fm_agc_enable != 0 || demod.fm_limiter_enable != 0
        || demod.ted_enabled != 1) {
        DSD_FPRINTF(stderr, "CQPSK symbol output retained raw non-symbol control state\n");
        rc = 1;
    }

    DSD_MEMSET(&demod, 0, sizeof(demod));
    demod.output_kind = DSD_DEMOD_OUTPUT_AUDIO_MONITOR;
    rtl_stream_toggle_fll(1);
    rtl_stream_toggle_ted(1);
    rtl_stream_set_ted_force(1);
    rtl_stream_set_fm_agc(1);
    rtl_stream_set_fm_limiter(1);

    cq = -1;
    fll = -1;
    ted = -1;
    rtl_stream_dsp_get(&cq, &fll, &ted);
    if (cq != 0 || fll != 1 || ted != 1 || rtl_stream_get_ted_force() != 1 || rtl_stream_get_fm_agc() != 1
        || rtl_stream_get_fm_limiter() != 1) {
        DSD_FPRINTF(stderr, "Audio monitor/non-symbol output did not retain live DSP controls\n");
        rc = 1;
    }

    return rc;
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
    rc |= expect_output_kind("P25 C4FM selects FSK symbols", p25_c4fm, DSD_DEMOD_OUTPUT_SYMBOL_FSK, 4800, 4);
    rc |= expect_configured_mode("P25 C4FM uses P25 C4FM LPF", p25_c4fm, 48000, DSD_DEMOD_OUTPUT_SYMBOL_FSK, 4800, 4,
                                 DSD_CH_LPF_PROFILE_P25_C4FM);
    rc |= expect_configured_mode("P25 C4FM keeps profile at 24 kHz", p25_c4fm, 24000, DSD_DEMOD_OUTPUT_SYMBOL_FSK, 4800,
                                 4, DSD_CH_LPF_PROFILE_P25_C4FM);

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
    rc |= expect_output_kind("NXDN48 selects 2400-symbol FSK", nxdn48, DSD_DEMOD_OUTPUT_SYMBOL_FSK, 2400, 4);
    rc |= expect_configured_mode("NXDN48 uses 6.25 kHz LPF", nxdn48, 48000, DSD_DEMOD_OUTPUT_SYMBOL_FSK, 2400, 4,
                                 DSD_CH_LPF_PROFILE_6K25);
    rc |= expect_configured_mode("NXDN48 keeps 6.25 kHz LPF at 24 kHz", nxdn48, 24000, DSD_DEMOD_OUTPUT_SYMBOL_FSK,
                                 2400, 4, DSD_CH_LPF_PROFILE_6K25);

    static dsd_opts nxdn96;
    DSD_MEMSET(&nxdn96, 0, sizeof(nxdn96));
    nxdn96.frame_nxdn96 = 1;
    rc |= expect_configured_mode("NXDN96 uses 12.5 kHz LPF", nxdn96, 48000, DSD_DEMOD_OUTPUT_SYMBOL_FSK, 4800, 4,
                                 DSD_CH_LPF_PROFILE_12K5);
    rc |= expect_configured_mode("NXDN96 keeps 12.5 kHz LPF at 24 kHz", nxdn96, 24000, DSD_DEMOD_OUTPUT_SYMBOL_FSK,
                                 4800, 4, DSD_CH_LPF_PROFILE_12K5);

    static dsd_opts dmr;
    DSD_MEMSET(&dmr, 0, sizeof(dmr));
    dmr.frame_dmr = 1;
    rc |= expect_output_kind("DMR selects 4800-symbol FSK", dmr, DSD_DEMOD_OUTPUT_SYMBOL_FSK, 4800, 4);
    rc |= expect_configured_channel_profile("DMR uses 12.5 kHz FSK channel LPF", dmr, 48000, DSD_CH_LPF_PROFILE_12K5);
    rc |= expect_configured_channel_profile("DMR keeps 12.5 kHz FSK channel LPF at 24 kHz", dmr, 24000,
                                            DSD_CH_LPF_PROFILE_12K5);

    static dsd_opts dstar;
    DSD_MEMSET(&dstar, 0, sizeof(dstar));
    dstar.frame_dstar = 1;
    rc |= expect_output_kind("D-STAR selects binary FSK", dstar, DSD_DEMOD_OUTPUT_SYMBOL_FSK, 4800, 2);
    rc |= expect_configured_mode("D-STAR uses 6.25 kHz LPF", dstar, 48000, DSD_DEMOD_OUTPUT_SYMBOL_FSK, 4800, 2,
                                 DSD_CH_LPF_PROFILE_6K25);
    rc |= expect_configured_mode("D-STAR keeps binary 6.25 kHz LPF at 24 kHz", dstar, 24000,
                                 DSD_DEMOD_OUTPUT_SYMBOL_FSK, 4800, 2, DSD_CH_LPF_PROFILE_6K25);

    static dsd_opts x2tdma;
    DSD_MEMSET(&x2tdma, 0, sizeof(x2tdma));
    x2tdma.frame_x2tdma = 1;
    rc |= expect_configured_mode("X2-TDMA uses 6 ksps 12.5 kHz LPF", x2tdma, 48000, DSD_DEMOD_OUTPUT_SYMBOL_FSK, 6000,
                                 4, DSD_CH_LPF_PROFILE_12K5);
    rc |= expect_configured_mode("X2-TDMA keeps 6 ksps 12.5 kHz LPF at 24 kHz", x2tdma, 24000,
                                 DSD_DEMOD_OUTPUT_SYMBOL_FSK, 6000, 4, DSD_CH_LPF_PROFILE_12K5);

    static dsd_opts ysf;
    DSD_MEMSET(&ysf, 0, sizeof(ysf));
    ysf.frame_ysf = 1;
    rc |= expect_configured_mode("YSF uses 12.5 kHz LPF", ysf, 48000, DSD_DEMOD_OUTPUT_SYMBOL_FSK, 4800, 4,
                                 DSD_CH_LPF_PROFILE_12K5);
    rc |= expect_configured_mode("YSF keeps 12.5 kHz LPF at 24 kHz", ysf, 24000, DSD_DEMOD_OUTPUT_SYMBOL_FSK, 4800, 4,
                                 DSD_CH_LPF_PROFILE_12K5);

    static dsd_opts dpmr;
    DSD_MEMSET(&dpmr, 0, sizeof(dpmr));
    dpmr.frame_dpmr = 1;
    rc |= expect_configured_mode("dPMR uses 6.25 kHz LPF", dpmr, 48000, DSD_DEMOD_OUTPUT_SYMBOL_FSK, 2400, 4,
                                 DSD_CH_LPF_PROFILE_6K25);
    rc |= expect_configured_mode("dPMR keeps 6.25 kHz LPF at 24 kHz", dpmr, 24000, DSD_DEMOD_OUTPUT_SYMBOL_FSK, 2400, 4,
                                 DSD_CH_LPF_PROFILE_6K25);

    static dsd_opts m17;
    DSD_MEMSET(&m17, 0, sizeof(m17));
    m17.frame_m17 = 1;
    rc |= expect_configured_mode("M17 uses 12.5 kHz LPF", m17, 48000, DSD_DEMOD_OUTPUT_SYMBOL_FSK, 4800, 4,
                                 DSD_CH_LPF_PROFILE_12K5);
    rc |= expect_configured_mode("M17 keeps 12.5 kHz LPF at 24 kHz", m17, 24000, DSD_DEMOD_OUTPUT_SYMBOL_FSK, 4800, 4,
                                 DSD_CH_LPF_PROFILE_12K5);

    static dsd_opts provoice;
    DSD_MEMSET(&provoice, 0, sizeof(provoice));
    provoice.frame_provoice = 1;
    rc |= expect_configured_mode("ProVoice uses 9.6 ksps binary FSK", provoice, 48000, DSD_DEMOD_OUTPUT_SYMBOL_FSK,
                                 9600, 2, DSD_CH_LPF_PROFILE_PROVOICE);
    rc |= expect_configured_mode("ProVoice keeps 9.6 ksps binary FSK at 24 kHz", provoice, 24000,
                                 DSD_DEMOD_OUTPUT_SYMBOL_FSK, 9600, 2, DSD_CH_LPF_PROFILE_PROVOICE);

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
                                 DSD_DEMOD_OUTPUT_SYMBOL_FSK, 4800, 4, DSD_CH_LPF_PROFILE_12K5);

    // Soapy inputs share tuning fields but must preserve the selected digital mode.
    static dsd_opts soapy_p25_c4fm;
    soapy_p25_c4fm = p25_c4fm;
    DSD_SNPRINTF(soapy_p25_c4fm.audio_in_dev, sizeof(soapy_p25_c4fm.audio_in_dev), "%s", "soapy");
    rc |=
        expect_output_kind("Soapy P25 C4FM selects FSK symbols", soapy_p25_c4fm, DSD_DEMOD_OUTPUT_SYMBOL_FSK, 4800, 4);
    rc |= expect_configured_mode("Soapy P25 C4FM uses P25 C4FM LPF", soapy_p25_c4fm, 48000, DSD_DEMOD_OUTPUT_SYMBOL_FSK,
                                 4800, 4, DSD_CH_LPF_PROFILE_P25_C4FM);

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

    rc |= expect_live_symbol_controls_guarded();
    rc |= expect_cqpsk_toggle_restores_fsk_channel_profile();
    rc |= expect_steady_state_watermark_disabled("rtl_tcp keeps demod watermark disabled", "rtltcp:127.0.0.1:1234");
    rc |= expect_steady_state_watermark_disabled("rtlsdr keeps demod watermark disabled", "rtl");
    rc |= expect_steady_state_watermark_disabled("soapy keeps demod watermark disabled", "soapy:driver=test");

    return rc;
}
