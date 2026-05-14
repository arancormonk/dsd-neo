// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <dsd-neo/core/opts.h>
#include <dsd-neo/dsp/demod_state.h>
#include <dsd-neo/io/rtl_demod_config.h>
#include <dsd-neo/io/rtl_stream_c.h>
#include <dsd-neo/runtime/ring.h>

#include "dsd-neo/core/opts_fwd.h"

extern demod_state demod;

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
expect_output_kind(const char* label, const dsd_opts& opts, int want_kind, int want_sym_rate, int want_levels) {
    demod_state* demod = static_cast<demod_state*>(std::calloc(1, sizeof(*demod)));
    output_state output;
    std::memset(&output, 0, sizeof(output));
    output.rate = 48000U;
    if (!demod) {
        std::fprintf(stderr, "%s: allocation failed\n", label);
        return 1;
    }

    rtl_demod_init_for_mode(demod, &output, &opts, 48000);
    int rc = 0;
    if (demod->output_kind != want_kind) {
        std::fprintf(stderr, "%s: got output_kind=%d want=%d\n", label, demod->output_kind, want_kind);
        rc = 1;
    }
    if (demod->symbol_rate_hz != want_sym_rate) {
        std::fprintf(stderr, "%s: got symbol_rate_hz=%d want=%d\n", label, demod->symbol_rate_hz, want_sym_rate);
        rc = 1;
    }
    if (demod->symbol_levels != want_levels) {
        std::fprintf(stderr, "%s: got symbol_levels=%d want=%d\n", label, demod->symbol_levels, want_levels);
        rc = 1;
    }
    if (want_kind == DSD_DEMOD_OUTPUT_SYMBOL_FSK) {
        if (demod->cqpsk_enable != 0 || demod->ted_enabled != 0 || demod->fll_enabled != 0 || demod->fm_agc_enable != 0
            || demod->fm_limiter_enable != 0) {
            std::fprintf(stderr, "%s: FSK symbol path left non-symbol controls enabled\n", label);
            rc = 1;
        }
    }
    if (want_kind == DSD_DEMOD_OUTPUT_SYMBOL_CQPSK && demod->ted_enabled != 1) {
        std::fprintf(stderr, "%s: CQPSK symbol path did not force TED on\n", label);
        rc = 1;
    }

    rtl_demod_maybe_update_resampler_after_rate_change(demod, &output, 48000);
    if ((want_kind == DSD_DEMOD_OUTPUT_SYMBOL_FSK || want_kind == DSD_DEMOD_OUTPUT_SYMBOL_CQPSK)
        && output.rate != 48000U) {
        std::fprintf(stderr, "%s: symbol output changed public output rate to %u\n", label, output.rate);
        rc = 1;
    }

    rtl_demod_cleanup(demod);
    std::free(demod);
    return rc;
}

static int
expect_live_symbol_controls_guarded(void) {
    int rc = 0;

    std::memset(&demod, 0, sizeof(demod));
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
        std::fprintf(stderr, "FSK symbol output did not report guarded non-symbol controls as off\n");
        rc = 1;
    }
    if (demod.fll_enabled != 0 || demod.ted_enabled != 0 || demod.ted_force != 0 || demod.fm_agc_enable != 0
        || demod.fm_limiter_enable != 0) {
        std::fprintf(stderr, "FSK symbol output retained raw non-symbol control state\n");
        rc = 1;
    }

    std::memset(&demod, 0, sizeof(demod));
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
        std::fprintf(stderr, "CQPSK symbol output did not guard non-symbol controls or force TED status\n");
        rc = 1;
    }
    if (demod.fll_enabled != 0 || demod.ted_force != 0 || demod.fm_agc_enable != 0 || demod.fm_limiter_enable != 0
        || demod.ted_enabled != 1) {
        std::fprintf(stderr, "CQPSK symbol output retained raw non-symbol control state\n");
        rc = 1;
    }

    std::memset(&demod, 0, sizeof(demod));
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
        std::fprintf(stderr, "Audio monitor/non-symbol output did not retain live DSP controls\n");
        rc = 1;
    }

    return rc;
}

int
main(void) {
    int rc = 0;

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

    dsd_opts p25_c4fm;
    std::memset(&p25_c4fm, 0, sizeof(p25_c4fm));
    p25_c4fm.frame_p25p1 = 1;
    rc |= expect_output_kind("P25 C4FM selects FSK symbols", p25_c4fm, DSD_DEMOD_OUTPUT_SYMBOL_FSK, 4800, 4);

    rc |= expect_output_kind("P25 QPSK selects CQPSK symbols", p25p1_qpsk, DSD_DEMOD_OUTPUT_SYMBOL_CQPSK, 4800, 4);

    dsd_opts nxdn48;
    std::memset(&nxdn48, 0, sizeof(nxdn48));
    nxdn48.frame_nxdn48 = 1;
    rc |= expect_output_kind("NXDN48 selects 2400-symbol FSK", nxdn48, DSD_DEMOD_OUTPUT_SYMBOL_FSK, 2400, 4);

    dsd_opts dstar;
    std::memset(&dstar, 0, sizeof(dstar));
    dstar.frame_dstar = 1;
    rc |= expect_output_kind("D-STAR selects binary FSK", dstar, DSD_DEMOD_OUTPUT_SYMBOL_FSK, 4800, 2);

    dsd_opts soapy_p25;
    std::memset(&soapy_p25, 0, sizeof(soapy_p25));
    soapy_p25.frame_p25p1 = 1;
    std::snprintf(soapy_p25.audio_in_dev, sizeof(soapy_p25.audio_in_dev), "%s", "soapy");
    rc |= expect_output_kind("Soapy stays monitor/audio path", soapy_p25, DSD_DEMOD_OUTPUT_AUDIO_MONITOR, 4800, 4);

    rc |= expect_live_symbol_controls_guarded();

    return rc;
}
