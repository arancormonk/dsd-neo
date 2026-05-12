// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <cstdio>
#include <cstring>

#include <dsd-neo/core/opts.h>
#include <dsd-neo/dsp/demod_state.h>
#include <dsd-neo/io/rtl_demod_config.h>
#include <dsd-neo/runtime/ring.h>

#include "dsd-neo/core/opts_fwd.h"

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

    return rc;
}
