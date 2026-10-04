// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Tap source for tools/squelch_model.py: prints the channel-filter plans the demodulator really runs, as JSON lines.
 *
 * Not part of the build. tools/squelch_model.py compiles it against the static DSP, runtime, IQ and platform libraries
 * of a configured build tree (the link line of dsd-neo_test_dsp_channel_filters) and reads its output.
 *
 * The first line holds the half-band decimator taps and the analog channel constants. Then each stdin line
 * "<rate_out_hz> <width_hz> <kind>" (kind 0 = FM, 1 = AM) gets one JSON line with the plan full_demod() designs for an
 * analog-family monitor at that demod rate: the width-driven analog design for width_hz > 0, the legacy WIDE profile
 * plan (144-tap cap, 63-tap fallback) for width_hz 0. It also reports the validator's view of the width
 * (dsd_analog_width_realizable(), the tap count for the rate, the widest width the rate fits and the legacy WIDE
 * width) and whether dsd_channel_lpf_design_analog() called directly gives the same taps as the plan.
 */

#include <dsd-neo/dsp/demod_pipeline.h>
#include <dsd-neo/dsp/demod_state.h>
#include <dsd-neo/dsp/halfband.h>
#include <dsd-neo/runtime/analog_channel.h>
#include <dsd-neo/runtime/mem.h>
#include <stdio.h>
#include <stdlib.h>
#include "dsd-neo/core/safe_api.h"

static void
print_taps(const float* taps, int len) {
    DSD_FPRINTF(stdout, "[");
    for (int i = 0; i < len; i++) {
        DSD_FPRINTF(stdout, "%s%.9g", i ? "," : "", (double)taps[i]);
    }
    DSD_FPRINTF(stdout, "]");
}

/* One block through full_demod() so the channel LPF designs its plan, as DSP_CHANNEL_FILTERS does. */
static int
design_plan(demod_state* s, int rate_hz, int kind, int width_hz) {
    DSD_MEMSET(s, 0, sizeof(*s));
    s->rate_in = rate_hz;
    s->rate_out = rate_hz;
    s->rate_out2 = 0;
    s->mode_demod = &raw_demod;
    s->lowpassed = s->input_cb_buf;
    s->lp_len = 1024;
    s->channel_lpf_enable = 1;
    s->channel_lpf_profile = DSD_CH_LPF_PROFILE_WIDE;
    s->analog_family = 1;
    s->analog_demod = kind;
    s->channel_lpf_width_hz = width_hz;
    full_demod(s);
    return s->channel_lpf_plan_taps_len;
}

int
main(void) {
    demod_state* s = static_cast<demod_state*>(dsd_neo_aligned_malloc(sizeof(demod_state)));
    if (!s) {
        return 1;
    }
    DSD_FPRINTF(stdout, "{\"header\":1,\"transition_hz\":%d,\"guard_hz\":%d,\"max_taps\":%d,\"hb15\":",
                DSD_ANALOG_CHANNEL_TRANSITION_HZ, DSD_ANALOG_CHANNEL_GUARD_HZ, DSD_ANALOG_CHANNEL_MAX_TAPS);
    print_taps(hb_q15_taps, HB_TAPS);
    DSD_FPRINTF(stdout, ",\"hb31\":");
    print_taps(hb31_q15_taps, 31);
    DSD_FPRINTF(stdout, "}\n");

    static float direct[DSD_ANALOG_CHANNEL_MAX_TAPS];
    char line[128];
    int rc = 0;
    while (fgets(line, sizeof line, stdin)) {
        char* end = NULL;
        const long rate = strtol(line, &end, 10);
        char* end2 = NULL;
        const long width = strtol(end, &end2, 10);
        char* end3 = NULL;
        const long kind = strtol(end2, &end3, 10);
        if (end == line || end2 == end || end3 == end2 || rate <= 0 || rate > 10000000 || width < 0 || width > 1000000
            || (kind != DSD_ANALOG_DEMOD_FM && kind != DSD_ANALOG_DEMOD_AM)) {
            DSD_FPRINTF(stderr, "bad request: %s", line);
            rc = 1;
            continue;
        }
        const int taps_len = design_plan(s, (int)rate, (int)kind, (int)width);
        int direct_len = -1;
        int direct_same = 0;
        if (width > 0) {
            direct_len = dsd_channel_lpf_design_analog((int)rate, (int)width, direct, DSD_ANALOG_CHANNEL_MAX_TAPS);
            direct_same = direct_len == taps_len ? 1 : 0;
            for (int i = 0; direct_same && i < taps_len; i++) {
                direct_same = direct[i] == s->channel_lpf_plan_taps[i] ? 1 : 0;
            }
        }
        DSD_FPRINTF(stdout,
                    "{\"rate_out\":%ld,\"width_hz\":%ld,\"kind\":%ld,\"realizable\":%d,\"taps_for_rate\":%d,"
                    "\"max_width_for_rate\":%d,\"legacy_wide_width_hz\":%d,\"direct_len\":%d,\"direct_same\":%d,"
                    "\"taps\":",
                    rate, width, kind, width > 0 ? dsd_analog_width_realizable((int)width, (int)rate) : 0,
                    dsd_analog_channel_taps_for_rate((int)rate), dsd_analog_width_max_for_rate((int)rate),
                    dsd_channel_lpf_legacy_wide_width_hz((int)rate), direct_len, direct_same);
        print_taps(s->channel_lpf_plan_taps, taps_len > 0 ? taps_len : 0);
        DSD_FPRINTF(stdout, "}\n");
    }
    dsd_neo_aligned_free(s);
    return rc;
}
