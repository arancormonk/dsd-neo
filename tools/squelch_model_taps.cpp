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
 *
 * tools/squelch_paths_model.py (issue #625) adds three fields, "<profile> <hb> <channel_rate_hz>": profile -1 keeps the
 * analog-family design above, a dsd_ch_lpf_profile designs that profile's plan off the analog family (the EDACS FSK
 * path's PROVOICE, the M17 encoder's WIDE), and -2 runs no channel filter. Its line then also carries the auto
 * squelch's classifier plan for those taps behind the half-band stage hb (0 none, 15 or 31 taps) at the channel rate
 * (dsd_squelch_floor_plan_design()).
 *
 * "squelch_model_taps track" runs the auto squelch's tracker itself instead (tools/squelch_paths_model.py's timing
 * checks): a first stdin line "<channel_rate_hz> <margin_db> <hb> <taps_len> <tap>..." sets the plan, then interleaved
 * float32 I/Q follows to EOF; stdout gets one flag byte per sample (DSD_SQUELCH_FLAG_CLOSED when the gate was closed),
 * from a tracker that starts LEARNING.
 */

#include <dsd-neo/dsp/demod_pipeline.h>
#include <dsd-neo/dsp/demod_state.h>
#include <dsd-neo/dsp/halfband.h>
#include <dsd-neo/dsp/squelch_floor.h>
#include <dsd-neo/runtime/analog_channel.h>
#include <dsd-neo/runtime/mem.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dsd-neo/core/safe_api.h"

static void
print_taps(const float* taps, int len) {
    DSD_FPRINTF(stdout, "[");
    for (int i = 0; i < len; i++) {
        DSD_FPRINTF(stdout, "%s%.9g", i ? "," : "", (double)taps[i]);
    }
    DSD_FPRINTF(stdout, "]");
}

/* One block through full_demod() so the channel LPF designs its plan, as DSP_CHANNEL_FILTERS does: the analog family's
   (profile -1), or a profile's off the family. */
static int
design_plan(demod_state* s, int rate_hz, int kind, int width_hz, int profile) {
    DSD_MEMSET(s, 0, sizeof(*s));
    s->rate_in = rate_hz;
    s->rate_out = rate_hz;
    s->rate_out2 = 0;
    s->mode_demod = &raw_demod;
    s->lowpassed = s->input_cb_buf;
    s->lp_len = 1024;
    s->channel_lpf_enable = 1;
    s->channel_lpf_profile = profile < 0 ? DSD_CH_LPF_PROFILE_WIDE : profile;
    s->analog_family = profile < 0 ? 1 : 0;
    s->analog_demod = kind;
    s->channel_lpf_width_hz = profile < 0 ? width_hz : 0;
    full_demod(s);
    return s->channel_lpf_plan_taps_len;
}

/* The auto squelch's classifier plan for the channel taps (none when taps_len is 0) behind half-band stage hb. */
static void
print_floor_plan(const float* taps, int taps_len, int hb, int channel_rate) {
    const float* hb_taps = hb == 31 ? hb31_q15_taps : (hb == 15 ? hb_q15_taps : NULL);
    const int hb_len = hb == 31 ? 31 : (hb == 15 ? HB_TAPS : 0);
    dsd_squelch_floor_plan fp;
    DSD_MEMSET(&fp, 0, sizeof fp);
    (void)dsd_squelch_floor_plan_design(&fp, taps_len > 0 ? taps : NULL, taps_len, hb_taps, hb_len, channel_rate);
    DSD_FPRINTF(stdout,
                ",\"profile_plan\":1,\"floor_plan\":{\"valid\":%d,\"rate_hz\":%d,\"lag\":%d,\"rho_lag\":%.17g,"
                "\"beta\":%.17g,\"neff\":%.17g,\"sqrt_neff\":%.17g,\"noise_gain\":%.17g}",
                fp.valid, fp.rate_hz, fp.lag, fp.rho_lag, fp.beta, fp.neff, fp.sqrt_neff, fp.noise_gain);
}

/* The tracker over stdin's I/Q, one flag byte per sample to stdout. */
static int
run_tracker(void) {
    static char header[65536];
    if (!fgets(header, sizeof header, stdin)) {
        return 1;
    }
    static float taps[DSD_ANALOG_CHANNEL_MAX_TAPS];
    char* cur = header;
    char* next = NULL;
    const long rate = strtol(cur, &next, 10);
    cur = next;
    const long margin = strtol(cur, &next, 10);
    cur = next;
    const long hb = strtol(cur, &next, 10);
    cur = next;
    const long taps_len = strtol(cur, &next, 10);
    if (next == cur || rate <= 0 || taps_len < 0 || taps_len > DSD_ANALOG_CHANNEL_MAX_TAPS) {
        DSD_FPRINTF(stderr, "bad track header\n");
        return 1;
    }
    for (long i = 0; i < taps_len; i++) {
        cur = next;
        taps[i] = strtof(cur, &next);
        if (next == cur) {
            DSD_FPRINTF(stderr, "bad tap %ld\n", i);
            return 1;
        }
    }
    const float* hb_taps = hb == 31 ? hb31_q15_taps : (hb == 15 ? hb_q15_taps : NULL);
    const int hb_len = hb == 31 ? 31 : (hb == 15 ? HB_TAPS : 0);
    dsd_squelch_floor_plan plan;
    if (dsd_squelch_floor_plan_design(&plan, taps_len > 0 ? taps : NULL, (int)taps_len, hb_taps, hb_len, (int)rate)
        != 0) {
        DSD_FPRINTF(stderr, "plan not valid\n");
        return 1;
    }
    dsd_squelch_floor* t = static_cast<dsd_squelch_floor*>(calloc(1, sizeof(dsd_squelch_floor)));
    if (!t) {
        return 1;
    }
    dsd_squelch_floor_set_plan(t, &plan);
    dsd_squelch_floor_set_margin(t, (int)margin);
    dsd_squelch_floor_reset(t);
    static float iq[2 * 8192];
    static uint8_t flags[8192];
    size_t got = 0;
    while ((got = fread(iq, 2 * sizeof(float), 8192, stdin)) > 0) {
        dsd_squelch_floor_process(t, iq, (int)got, flags);
        if (fwrite(flags, 1, got, stdout) != got) {
            free(t);
            return 1;
        }
    }
    free(t);
    return 0;
}

int
main(int argc, char** argv) {
    if (argc > 1 && strcmp(argv[1], "track") == 0) {
        return run_tracker();
    }
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
        /* The optional profile fields (tools/squelch_paths_model.py). */
        char* end4 = NULL;
        long profile = strtol(end3, &end4, 10);
        const int with_profile = end4 != end3 ? 1 : 0;
        long hb = 0;
        long channel_rate = rate;
        if (with_profile) {
            char* end5 = NULL;
            hb = strtol(end4, &end5, 10);
            char* end6 = NULL;
            channel_rate = strtol(end5, &end6, 10);
            if (end5 == end4 || end6 == end5 || profile < -2 || profile > DSD_CH_LPF_PROFILE_P25_CQPSK
                || (hb != 0 && hb != 15 && hb != 31) || channel_rate < rate || channel_rate > 10000000) {
                DSD_FPRINTF(stderr, "bad request: %s", line);
                rc = 1;
                continue;
            }
        } else {
            profile = -1;
        }
        const int taps_len = profile == -2 ? 0 : design_plan(s, (int)rate, (int)kind, (int)width, (int)profile);
        int direct_len = -1;
        int direct_same = 0;
        if (width > 0 && profile == -1) {
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
        if (with_profile) {
            DSD_FPRINTF(stdout, ",\"profile\":%ld,\"hb\":%ld,\"channel_rate\":%ld,\"protected_edge_hz\":%.1f", profile,
                        hb, channel_rate, profile >= 0 ? dsd_channel_lpf_protected_edge_hz((int)profile) : 0.0);
            print_floor_plan(s->channel_lpf_plan_taps, taps_len, (int)hb, (int)channel_rate);
        }
        DSD_FPRINTF(stdout, "}\n");
    }
    dsd_neo_aligned_free(s);
    return rc;
}
