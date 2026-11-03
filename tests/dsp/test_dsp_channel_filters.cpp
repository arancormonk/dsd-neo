// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Unit tests for channel filters: RRC input filters (DC preservation), the
 * width-driven analog channel low-pass, the profile designs and the TETRA
 * channel profile. */

#include <atomic>
#include <cmath>
#include <cstring>
#include <dsd-neo/dsp/demod_pipeline.h>
#include <dsd-neo/dsp/demod_state.h>
#include <dsd-neo/dsp/firdes.h>
#include <dsd-neo/dsp/snr_bias.h>
#include <dsd-neo/io/rtl_stream_c.h>
#include <dsd-neo/runtime/analog_channel.h>
#include <dsd-neo/runtime/digital_channel.h>
#include <dsd-neo/runtime/mem.h>
#include <dsd-neo/runtime/rtl_stream_metrics_hooks.h>
#include <stdio.h>
#include "dsd-neo/core/safe_api.h"

extern "C" void init_rrc_filter_memory(void);
extern "C" float dmr_filter(float sample, int sps);
extern "C" float nxdn_filter(float sample, int sps);
extern "C" float dpmr_filter(float sample, int sps);

static int
approx_eq(float a, float b, float tol) {
    float d = a - b;
    if (d < 0) {
        d = -d;
    }
    return d <= tol;
}

static int
dc_pass_check(float (*f)(float, int), float dc, int sps, int warm, float tol) {
    float y = 0.0f;
    for (int i = 0; i < warm; i++) {
        y = f(dc, sps);
    }
    return approx_eq(y, dc, tol);
}

static int
test_rrc_dc(void) {
    init_rrc_filter_memory();
    const float dc = 0.1f;
    const int warm = 512; // exceed any filter length for steady-state
    // Allow small rounding tolerance
    if (!dc_pass_check(&dmr_filter, dc, 10, warm, 1e-4f)) {
        DSD_FPRINTF(stderr, "DMR DC fail\n");
        return 1;
    }
    if (!dc_pass_check(&nxdn_filter, dc, 20, warm, 1e-4f)) {
        DSD_FPRINTF(stderr, "NXDN DC fail\n");
        return 1;
    }
    if (!dc_pass_check(&dpmr_filter, dc, 20, warm, 1e-4f)) {
        DSD_FPRINTF(stderr, "DPMR DC fail\n");
        return 1;
    }
    return 0;
}

/* Run one block through full_demod so the channel LPF designs its plan for the
 * given rate, then report the plan. analog_width_hz 0 leaves the digital/profile
 * design path in charge. */
static int
design_plan(demod_state* s, int rate_hz, int profile, int analog_width_hz, float* taps_out, int* taps_len) {
    DSD_MEMSET(s, 0, sizeof(*s));
    s->rate_in = rate_hz;
    s->rate_out = rate_hz;
    s->complex_rate = rate_hz;
    s->rate_out2 = 0;
    s->mode_demod = &raw_demod;
    s->lowpassed = s->input_cb_buf;
    s->lp_len = 1024;
    s->channel_lpf_enable = 1;
    s->channel_lpf_profile = profile;
    if (analog_width_hz > 0) {
        s->analog_family = 1;
        s->analog_demod = DSD_ANALOG_DEMOD_FM;
        s->channel_lpf_width_hz = analog_width_hz;
    }
    full_demod(s);
    *taps_len = s->channel_lpf_plan_taps_len;
    if (*taps_len > 0) {
        DSD_MEMCPY(taps_out, s->channel_lpf_plan_taps, (size_t)(*taps_len) * sizeof(float));
    }
    return 0;
}

/* WIDE taps as the design produced them before the analog channel filter became width-driven: tap count and six taps
 * per rate, the centre tap last. Pinned rather than recomputed, so a change to the shared design path (window, guard,
 * WIDE cutoff) cannot move the WIDE and analog designs together unnoticed. Values within a tolerance, not a hash of
 * the float bits: the taps come from libm sin/cos, which may differ by an ulp between platforms. */
namespace {
struct pinned_tap {
    int index;
    float value;
};

struct pinned_wide_design {
    int rate_hz;
    int taps_len;
    pinned_tap taps[6];
};
} // namespace

static const pinned_wide_design kPinnedWide[] = {
    {24000,
     67,
     {{0, 1.280668765e-10f},
      {8, -2.041145053e-04f},
      {16, 3.228353802e-03f},
      {25, -2.323140018e-02f},
      {32, 2.464553267e-01f},
      {33, 7.166660428e-01f}}},
    {46875,
     131,
     {{0, 3.299232068e-11f},
      {16, -2.650803435e-05f},
      {32, 1.060506678e-03f},
      {49, -6.118888967e-03f},
      {64, 2.906197608e-01f},
      {65, 3.669324815e-01f}}},
    {48000,
     135,
     {{0, -1.853161938e-12f},
      {16, 2.840763773e-04f},
      {33, 1.674318220e-03f},
      {50, 4.081554245e-03f},
      {66, 2.870422006e-01f},
      {67, 3.583324254e-01f}}},
};

static int
expect_pinned_wide(const float* taps, int taps_len, const pinned_wide_design& pin) {
    if (taps_len != pin.taps_len) {
        DSD_FPRINTF(stderr, "WIDE at %d Hz designs %d taps, pinned %d\n", pin.rate_hz, taps_len, pin.taps_len);
        return 1;
    }
    for (const pinned_tap& t : pin.taps) {
        const double tol = 1e-6 * std::fabs((double)t.value) + 1e-9;
        if (std::fabs((double)taps[t.index] - (double)t.value) > tol) {
            DSD_FPRINTF(stderr, "WIDE at %d Hz tap %d = %.9e, pinned %.9e\n", pin.rate_hz, t.index,
                        (double)taps[t.index], (double)t.value);
            return 1;
        }
    }
    return 0;
}

/* 16 kHz reproduces today's WIDE design wherever that design succeeds: same
 * cutoff (8600 Hz), transition and window through the same firdes call. */
static int
test_default_width_matches_wide(demod_state* s) {
    for (const pinned_wide_design& pin : kPinnedWide) {
        const int rate = pin.rate_hz;
        static float wide[DSD_CHANNEL_LPF_MAX_TAPS];
        static float analog[DSD_CHANNEL_LPF_MAX_TAPS];
        int wide_len = 0;
        int analog_len = 0;
        design_plan(s, rate, DSD_CH_LPF_PROFILE_WIDE, 0, wide, &wide_len);
        if (expect_pinned_wide(wide, wide_len, pin) != 0) {
            return 1;
        }
        design_plan(s, rate, DSD_CH_LPF_PROFILE_WIDE, DSD_ANALOG_NFM_WIDTH_DEFAULT_HZ, analog, &analog_len);
        if (wide_len <= 0 || wide_len != analog_len
            || std::memcmp(wide, analog, (size_t)wide_len * sizeof(float)) != 0) {
            DSD_FPRINTF(stderr, "16 kHz analog taps differ from WIDE at %d Hz (len %d vs %d)\n", rate, analog_len,
                        wide_len);
            return 1;
        }
        /* And through the public design entry point. */
        static float direct[DSD_CHANNEL_LPF_MAX_TAPS];
        const int direct_len =
            dsd_channel_lpf_design_analog(rate, DSD_ANALOG_NFM_WIDTH_DEFAULT_HZ, direct, DSD_CHANNEL_LPF_MAX_TAPS);
        if (direct_len != wide_len || std::memcmp(wide, direct, (size_t)wide_len * sizeof(float)) != 0) {
            DSD_FPRINTF(stderr, "public analog design differs from WIDE at %d Hz\n", rate);
            return 1;
        }
    }
    return 0;
}

static double
tap_response_db(const float* taps, int len, double rate_hz, double f_hz) {
    double re = 0.0;
    double im = 0.0;
    for (int n = 0; n < len; n++) {
        const double w = 2.0 * 3.14159265358979323846 * f_hz * (double)n / rate_hz;
        re += (double)taps[n] * cos(w);
        im -= (double)taps[n] * sin(w);
    }
    const double mag = sqrt(re * re + im * im);
    return 20.0 * log10(mag > 1e-12 ? mag : 1e-12);
}

/* The selected width is the protected passband; the transition sits outside it.
 * The Blackman window's real skirt is wider than the 1200 Hz design transition:
 * attenuation reaches about 30 dB at W/2 + 1200 Hz and 50 dB by W/2 + ~1450 Hz,
 * so the stopband bound is checked from W/2 + 1500 Hz. A 50 dB bound at
 * W/2 + 1200 Hz would take a longer design than WIDE's, and W = 16000 must
 * reproduce the WIDE taps bit for bit (test_default_width_matches_wide()), so
 * these bounds pin the skirt that design has. */
static int
test_width_response(void) {
    /* NFM widths, then the AM ones (issue #524): the same design at the AM range's edges, its default and a middle
       width. */
    const int widths[] = {8000, 12500, 16000, 25000, 5000, 6000, 10000, 20000};
    const double rate = 48000.0;
    for (int w : widths) {
        static float taps[DSD_CHANNEL_LPF_MAX_TAPS];
        const int len = dsd_channel_lpf_design_analog((int)rate, w, taps, DSD_CHANNEL_LPF_MAX_TAPS);
        if (len <= 0) {
            DSD_FPRINTF(stderr, "width %d did not design at 48 kHz\n", w);
            return 1;
        }
        /* Every width here is even, so W/2 is a whole number of Hz and the 25 Hz grid lands on it exactly. */
        const int half_hz = w / 2;
        const int nyquist_hz = (int)rate / 2;
        for (int f_hz = 0; f_hz <= half_hz; f_hz += 25) {
            const double db = tap_response_db(taps, len, rate, (double)f_hz);
            if (db < -1.0) {
                DSD_FPRINTF(stderr, "width %d passband %d Hz at %.2f dB (< -1 dB)\n", w, f_hz, db);
                return 1;
            }
        }
        for (int f_hz = half_hz + 1200; f_hz <= nyquist_hz; f_hz += 25) {
            const double db = tap_response_db(taps, len, rate, (double)f_hz);
            if (db > -29.0) {
                DSD_FPRINTF(stderr, "width %d skirt %d Hz at %.2f dB (> -29 dB)\n", w, f_hz, db);
                return 1;
            }
        }
        for (int f_hz = half_hz + 1500; f_hz <= nyquist_hz; f_hz += 25) {
            const double db = tap_response_db(taps, len, rate, (double)f_hz);
            if (db > -50.0) {
                DSD_FPRINTF(stderr, "width %d stopband %d Hz at %.2f dB (> -50 dB)\n", w, f_hz, db);
                return 1;
            }
        }
    }
    return 0;
}

/* Device-forced rates above ~51.4 kHz need more than 144 taps. The analog path
 * and, since issue #806, the digital profiles design them within the 288-tap
 * capacity instead of dropping to the 63-tap prototype built for 24 kHz. */
static int
test_forced_rate_capacity(demod_state* s) {
    static float taps[DSD_CHANNEL_LPF_MAX_TAPS];
    int len = 0;
    design_plan(s, 78125, DSD_CH_LPF_PROFILE_WIDE, DSD_ANALOG_NFM_WIDTH_DEFAULT_HZ, taps, &len);
    if (len != 219) {
        DSD_FPRINTF(stderr, "analog 16 kHz at 78125 Hz: %d taps, want 219\n", len);
        return 1;
    }
    if (tap_response_db(taps, len, 78125.0, 8000.0) < -1.0 || tap_response_db(taps, len, 78125.0, 9500.0) > -50.0) {
        DSD_FPRINTF(stderr, "analog 16 kHz at 78125 Hz does not have the 16 kHz shape\n");
        return 1;
    }
    design_plan(s, 96000, DSD_CH_LPF_PROFILE_WIDE, 25000, taps, &len);
    if (len != 269) {
        DSD_FPRINTF(stderr, "analog 25 kHz at 96 kHz: %d taps, want 269\n", len);
        return 1;
    }
    /* Digital WIDE (the digital fallback profile) at the same forced rate runs the 16 kHz analog design's taps, and
       12K5 its own 219-tap design: its 6.25 kHz edge passed, 50 dB down from 7750 Hz. */
    static float analog[DSD_CHANNEL_LPF_MAX_TAPS];
    int analog_len = 0;
    design_plan(s, 78125, DSD_CH_LPF_PROFILE_WIDE, DSD_ANALOG_NFM_WIDTH_DEFAULT_HZ, analog, &analog_len);
    design_plan(s, 78125, DSD_CH_LPF_PROFILE_WIDE, 0, taps, &len);
    if (len != 219 || len != analog_len || std::memcmp(taps, analog, (size_t)len * sizeof(float)) != 0) {
        DSD_FPRINTF(stderr, "digital WIDE at 78125 Hz: %d taps, want the 219-tap 16 kHz design\n", len);
        return 1;
    }
    design_plan(s, 78125, DSD_CH_LPF_PROFILE_12K5, 0, taps, &len);
    if (len != 219 || tap_response_db(taps, len, 78125.0, 6250.0) < -1.0
        || tap_response_db(taps, len, 78125.0, 7750.0) > -50.0) {
        DSD_FPRINTF(stderr, "digital 12K5 at 78125 Hz: %d taps without the 12.5 kHz shape, want 219\n", len);
        return 1;
    }
    return 0;
}

/* An unrealizable width is an error: never a clamp, never the fallback. */
static int
test_unrealizable_width(demod_state* s) {
    static float taps[DSD_CHANNEL_LPF_MAX_TAPS];
    for (int i = 0; i < DSD_CHANNEL_LPF_MAX_TAPS; i++) {
        taps[i] = 7.0f;
    }
    if (dsd_channel_lpf_design_analog(16000, 16000, taps, DSD_CHANNEL_LPF_MAX_TAPS) != -1) {
        DSD_FPRINTF(stderr, "16 kHz width at 16 kHz rate designed instead of failing\n");
        return 1;
    }
    if (dsd_channel_lpf_design_analog(128000, 16000, taps, DSD_CHANNEL_LPF_MAX_TAPS) != -1) {
        DSD_FPRINTF(stderr, "128 kHz rate designed past the 288-tap capacity\n");
        return 1;
    }
    if (dsd_channel_lpf_design_analog(78125, 16000, taps, 144) != -1) {
        DSD_FPRINTF(stderr, "design ignored the caller's tap capacity\n");
        return 1;
    }
    if (dsd_channel_lpf_design_analog(48000, 0, taps, DSD_CHANNEL_LPF_MAX_TAPS) != -1
        || dsd_channel_lpf_design_analog(0, 16000, taps, DSD_CHANNEL_LPF_MAX_TAPS) != -1
        || dsd_channel_lpf_design_analog(48000, 16000, NULL, DSD_CHANNEL_LPF_MAX_TAPS) != -1) {
        DSD_FPRINTF(stderr, "degenerate design arguments accepted\n");
        return 1;
    }
    if (fabsf(taps[0] - 7.0f) > 1e-6f) {
        DSD_FPRINTF(stderr, "a failed design wrote taps\n");
        return 1;
    }
    /* Through the pipeline: no plan, so the block passes unfiltered rather than
     * through a clamped or prototype filter. */
    int len = -1;
    design_plan(s, 16000, DSD_CH_LPF_PROFILE_WIDE, 16000, taps, &len);
    if (len != 0) {
        DSD_FPRINTF(stderr, "unrealizable analog width left a %d-tap plan\n", len);
        return 1;
    }
    return 0;
}

/* CQPSK toggled on under -fA keeps the analog family flag but no longer produces monitor audio, so its channel filter
 * is the P25 CQPSK profile design, not the analog width. */
static int
test_cqpsk_under_analog_family_keeps_profile(demod_state* s) {
    static float profile[DSD_CHANNEL_LPF_MAX_TAPS];
    static float toggled[DSD_CHANNEL_LPF_MAX_TAPS];
    int profile_len = 0;
    design_plan(s, 48000, DSD_CH_LPF_PROFILE_P25_CQPSK, 0, profile, &profile_len);

    DSD_MEMSET(s, 0, sizeof(*s));
    s->rate_in = 48000;
    s->rate_out = 48000;
    s->complex_rate = 48000;
    s->mode_demod = &raw_demod;
    s->lowpassed = s->input_cb_buf;
    s->lp_len = 1024;
    s->channel_lpf_enable = 1;
    s->channel_lpf_profile = DSD_CH_LPF_PROFILE_P25_CQPSK;
    s->analog_family = 1;
    s->channel_lpf_width_hz = 12500;
    s->cqpsk_enable = 1;
    s->output_kind = DSD_DEMOD_OUTPUT_SYMBOL_CQPSK;
    if (dsd_demod_analog_monitor_active(s)) {
        DSD_FPRINTF(stderr, "CQPSK output reported as the analog monitor\n");
        return 1;
    }
    /* Squelched: the block designs its channel plan, then emits zero symbols without running the CQPSK loops. */
    s->channel_squelch_level.store(1.0f);
    full_demod(s);
    const int toggled_len = s->channel_lpf_plan_taps_len;
    DSD_MEMCPY(toggled, s->channel_lpf_plan_taps, (size_t)(toggled_len > 0 ? toggled_len : 0) * sizeof(float));
    if (profile_len <= 0 || toggled_len != profile_len
        || std::memcmp(profile, toggled, (size_t)profile_len * sizeof(float)) != 0
        || s->channel_lpf_plan_width_hz != 0) {
        DSD_FPRINTF(stderr, "CQPSK under the analog family designed %d taps (width %d), want the %d-tap profile\n",
                    toggled_len, s->channel_lpf_plan_width_hz, profile_len);
        return 1;
    }
    return 0;
}

/* A typed digital scan row on an analog session applies its symbol profile without a family switch: the monitor output
 * and the analog family flag stay, but the row's channel profile takes the place of the analog (WIDE) one, and the row
 * filters with that profile, as it did before the analog filter became width-driven, not with the analog width. */
static int
test_digital_row_profile_under_analog_family(demod_state* s) {
    static float profile[DSD_CHANNEL_LPF_MAX_TAPS];
    static float row[DSD_CHANNEL_LPF_MAX_TAPS];
    int profile_len = 0;
    int row_len = 0;
    design_plan(s, 48000, DSD_CH_LPF_PROFILE_12K5, 0, profile, &profile_len);
    design_plan(s, 48000, DSD_CH_LPF_PROFILE_12K5, DSD_ANALOG_NFM_WIDTH_DEFAULT_HZ, row, &row_len);
    if (dsd_demod_analog_monitor_active(s)) {
        DSD_FPRINTF(stderr, "a digital row's channel profile reported as the analog monitor\n");
        return 1;
    }
    if (profile_len <= 0 || row_len != profile_len
        || std::memcmp(profile, row, (size_t)profile_len * sizeof(float)) != 0 || s->channel_lpf_plan_width_hz != 0) {
        DSD_FPRINTF(stderr, "a 12K5 row under the analog family designed %d taps (width %d), want the %d-tap profile\n",
                    row_len, s->channel_lpf_plan_width_hz, profile_len);
        return 1;
    }
    return 0;
}

/* The width published for the legacy WIDE plan (the unset NFM default where the 16 kHz design does not fit) is the
 * passband that plan really has, at every rate: the design within 288 taps, its cutoff held to 0.9 x Nyquist at low
 * rates, and the 63-tap fallback prototype above DSD_DIGITAL_CHANNEL_MAX_RATE_HZ (~102.7 kHz), whose response scales
 * with the rate. The plan passes that width within
 * 1 dB and, where the rate leaves room, is down 20 dB a quarter past its own transition (Blackman,
 * 74 x R / (22 x taps)) beyond it, so the width is neither wider nor much narrower than what the plan passes. */
static int
test_legacy_wide_width(demod_state* s) {
    struct legacy_case {
        int rate_hz;
        int taps_len;
        int width_hz; /* 0: only the response bounds */
    };

    const legacy_case cases[] = {
        {12000, 33, 9600},
        {16000, 45, 13200},
        {24000, 67, 16000},
        {46875, 131, 16000},
        {48000, 135, 16000},
        {78125, 219, 16000},
        {DSD_DIGITAL_CHANNEL_MAX_RATE_HZ, 287, 16000},
        {104000, 63, 0},
        {128000, 63, 0},
    };
    static float taps[DSD_CHANNEL_LPF_MAX_TAPS];
    for (const legacy_case& c : cases) {
        int len = 0;
        design_plan(s, c.rate_hz, DSD_CH_LPF_PROFILE_WIDE, 0, taps, &len);
        const int width_hz = dsd_channel_lpf_legacy_wide_width_hz(c.rate_hz);
        if (len != c.taps_len || width_hz <= 0 || (c.width_hz > 0 && width_hz != c.width_hz)) {
            DSD_FPRINTF(stderr, "legacy WIDE at %d Hz: %d taps (want %d), width %d (want %d)\n", c.rate_hz, len,
                        c.taps_len, width_hz, c.width_hz);
            return 1;
        }
        const double rate = (double)c.rate_hz;
        const double half_hz = (double)width_hz * 0.5;
        for (int f_hz = 0; (double)f_hz <= half_hz; f_hz += 25) {
            if (tap_response_db(taps, len, rate, (double)f_hz) < -1.0) {
                DSD_FPRINTF(stderr, "legacy WIDE at %d Hz: %d Hz inside the %d Hz width is below -1 dB\n", c.rate_hz,
                            f_hz, width_hz);
                return 1;
            }
        }
        if (tap_response_db(taps, len, rate, half_hz) < -1.0) {
            DSD_FPRINTF(stderr, "legacy WIDE at %d Hz: the %d Hz width's edge is below -1 dB\n", c.rate_hz, width_hz);
            return 1;
        }
        const double transition_hz = dsd_window_max_attenuation(DSD_WIN_BLACKMAN) * rate / (22.0 * (double)len);
        const double skirt_hz = half_hz + 1.25 * transition_hz;
        if (skirt_hz < rate * 0.5 && tap_response_db(taps, len, rate, skirt_hz) > -20.0) {
            DSD_FPRINTF(stderr, "legacy WIDE at %d Hz: %.0f Hz past the %d Hz width is above -20 dB\n", c.rate_hz,
                        skirt_hz, width_hz);
            return 1;
        }
    }
    if (dsd_channel_lpf_legacy_wide_width_hz(0) != 0 || dsd_channel_lpf_legacy_wide_width_hz(-48000) != 0) {
        DSD_FPRINTF(stderr, "legacy WIDE width reported for a rate of 0 or less\n");
        return 1;
    }
    return 0;
}

/* A plan's fingerprint: its tap count and three sums over the taps (DC gain, energy, and the second moment about the
 * centre tap), each sensitive to any change of the design. */
namespace {
struct plan_fingerprint {
    int taps_len;
    double sum;
    double energy;
    double moment2;
};

struct pinned_profile_plan {
    int profile;
    int rate_hz;
    plan_fingerprint fp;
};
} // namespace

static plan_fingerprint
fingerprint_of(const float* taps, int len) {
    plan_fingerprint fp = {len, 0.0, 0.0, 0.0};
    const double c = (double)(len - 1) * 0.5;
    for (int n = 0; n < len; n++) {
        const double h = (double)taps[n];
        const double k = (double)n - c;
        fp.sum += h;
        fp.energy += h * h;
        fp.moment2 += h * k * k;
    }
    return fp;
}

/* The size of the terms the second moment cancels down from: the sum of |h| k^2. */
static double
moment2_scale_of(const float* taps, int len) {
    const double c = (double)(len - 1) * 0.5;
    double scale = 0.0;
    for (int n = 0; n < len; n++) {
        const double k = (double)n - c;
        scale += std::fabs((double)taps[n]) * k * k;
    }
    return scale;
}

/* Every pre-TETRA profile's plan at the rates TETRA runs at and the 24/48 kHz rates the other modes run at: the
 * profile designs as they were before the TETRA profile existed, and at 144 kHz, above DSD_DIGITAL_CHANNEL_MAX_RATE_HZ,
 * the 63-tap fallback prototypes. At 72 kHz each profile now runs its own design within the 288-tap capacity (issue
 * #806) where it ran the fallback prototype, cut at a third of the rate, before. Fingerprints within a tolerance rather
 * than a hash of the float bits, as for kPinnedWide. */
static const pinned_profile_plan kPinnedProfilePlans[] = {
    {DSD_CH_LPF_PROFILE_WIDE, 24000, {67, 1.000000044123e+00, 7.010672899040e-01, 1.246256990636e-03}},
    {DSD_CH_LPF_PROFILE_WIDE, 36000, {101, 1.000000047609e+00, 4.674807939356e-01, 6.170353924117e-03}},
    {DSD_CH_LPF_PROFILE_WIDE, 48000, {135, 1.000000043625e+00, 3.506488793216e-01, 1.163743565682e-02}},
    {DSD_CH_LPF_PROFILE_WIDE, 72000, {201, 9.999999725643e-01, 2.337403369479e-01, 2.528430381746e-02}},
    {DSD_CH_LPF_PROFILE_WIDE, 144000, {63, 9.999694824219e-01, 6.500525316224e-01, 3.027343750000e-02}},
    {DSD_CH_LPF_PROFILE_6K25, 24000, {67, 1.000000029227e+00, 2.948037693301e-01, 2.315160999963e-02}},
    {DSD_CH_LPF_PROFILE_6K25, 36000, {101, 9.999999974000e-01, 1.966430407009e-01, 3.010174991654e-02}},
    {DSD_CH_LPF_PROFILE_6K25, 48000, {135, 9.999999585420e-01, 1.475220596537e-01, 3.172625972043e-02}},
    {DSD_CH_LPF_PROFILE_6K25, 72000, {201, 1.000000003576e+00, 9.832151896291e-02, 1.205265591082e-01}},
    {DSD_CH_LPF_PROFILE_6K25, 144000, {63, 1.000061035156e+00, 4.001026339829e-01, -2.777099609375e-02}},
    {DSD_CH_LPF_PROFILE_12K5, 24000, {67, 1.000000005881e+00, 5.552399412907e-01, -4.978083753815e-03}},
    {DSD_CH_LPF_PROFILE_12K5, 36000, {101, 9.999999773692e-01, 3.702645817960e-01, -1.284346668253e-02}},
    {DSD_CH_LPF_PROFILE_12K5, 48000, {135, 1.000000012621e+00, 2.777366960401e-01, -2.093521426676e-02}},
    {DSD_CH_LPF_PROFILE_12K5, 72000, {201, 1.000000037795e+00, 1.851323300283e-01, -5.185234030770e-02}},
    {DSD_CH_LPF_PROFILE_12K5, 144000, {63, 1.000061035156e+00, 4.001026339829e-01, -2.777099609375e-02}},
    {DSD_CH_LPF_PROFILE_PROVOICE, 24000, {67, 1.000000005881e+00, 5.552399412907e-01, -4.978083753815e-03}},
    {DSD_CH_LPF_PROFILE_PROVOICE, 36000, {101, 9.999999773692e-01, 3.702645817960e-01, -1.284346668253e-02}},
    {DSD_CH_LPF_PROFILE_PROVOICE, 48000, {135, 1.000000012621e+00, 2.777366960401e-01, -2.093521426676e-02}},
    {DSD_CH_LPF_PROFILE_PROVOICE, 72000, {201, 1.000000037795e+00, 1.851323300283e-01, -5.185234030770e-02}},
    {DSD_CH_LPF_PROFILE_PROVOICE, 144000, {63, 9.999694824219e-01, 6.500525316224e-01, 3.027343750000e-02}},
    {DSD_CH_LPF_PROFILE_P25_C4FM, 24000, {67, 1.000000005881e+00, 5.552399412907e-01, -4.978083753815e-03}},
    {DSD_CH_LPF_PROFILE_P25_C4FM, 36000, {101, 9.999999773692e-01, 3.702645817960e-01, -1.284346668253e-02}},
    {DSD_CH_LPF_PROFILE_P25_C4FM, 48000, {135, 1.000000012621e+00, 2.777366960401e-01, -2.093521426676e-02}},
    {DSD_CH_LPF_PROFILE_P25_C4FM, 72000, {201, 1.000000037795e+00, 1.851323300283e-01, -5.185234030770e-02}},
    {DSD_CH_LPF_PROFILE_P25_C4FM, 144000, {63, 9.999694824219e-01, 6.500525316224e-01, 3.027343750000e-02}},
    {DSD_CH_LPF_PROFILE_P25_CQPSK, 24000, {67, 1.000000035420e+00, 5.885637850060e-01, 4.549548854177e-03}},
    {DSD_CH_LPF_PROFILE_P25_CQPSK, 36000, {101, 9.999999938664e-01, 3.924798176319e-01, 9.664304763394e-03}},
    {DSD_CH_LPF_PROFILE_P25_CQPSK, 48000, {135, 9.999999959922e-01, 2.943987285497e-01, 1.357281783322e-02}},
    {DSD_CH_LPF_PROFILE_P25_CQPSK, 72000, {201, 1.000000032672e+00, 1.962399100049e-01, 3.912490265602e-02}},
    {DSD_CH_LPF_PROFILE_P25_CQPSK, 144000, {63, 9.999694824219e-01, 6.500525316224e-01, 3.027343750000e-02}},
};

static int
fingerprint_matches(double got, double want) {
    return std::fabs(got - want) <= 1e-6 * std::fabs(want) + 1e-9;
}

/* The second moment cancels terms of either sign, so a last-bit change of the taps (FMA contraction on arm64, a
   fast-math build) moves it by a part in 1e7 of those terms, not of the result: bound it by their size. The tap count,
   DC gain and energy keep the tight bound and are what tell a design change. */
static int
moment_matches(double got, double want, double scale) {
    return std::fabs(got - want) <= 1e-5 * scale + 1e-9;
}

static int
test_profile_plans_pinned(demod_state* s) {
    static float taps[DSD_CHANNEL_LPF_MAX_TAPS];
    for (const pinned_profile_plan& pin : kPinnedProfilePlans) {
        int len = 0;
        design_plan(s, pin.rate_hz, pin.profile, 0, taps, &len);
        const plan_fingerprint fp = fingerprint_of(taps, len);
        if (fp.taps_len != pin.fp.taps_len || !fingerprint_matches(fp.sum, pin.fp.sum)
            || !fingerprint_matches(fp.energy, pin.fp.energy)
            || !moment_matches(fp.moment2, pin.fp.moment2, moment2_scale_of(taps, len))) {
            DSD_FPRINTF(stderr,
                        "profile %d at %d Hz: %d taps sum %.12e energy %.12e moment %.12e, pinned %d taps sum %.12e "
                        "energy %.12e moment %.12e\n",
                        pin.profile, pin.rate_hz, fp.taps_len, fp.sum, fp.energy, fp.moment2, pin.fp.taps_len,
                        pin.fp.sum, pin.fp.energy, pin.fp.moment2);
            return 1;
        }
    }
    return 0;
}

/* The TETRA profile id is the same in all three mirrored enums. */
static int
test_tetra_profile_ids(void) {
    if (DSD_CH_LPF_PROFILE_TETRA != 6 || RTL_STREAM_CHANNEL_PROFILE_TETRA != 6
        || DSD_RTL_STREAM_CHANNEL_PROFILE_TETRA != 6) {
        DSD_FPRINTF(stderr, "TETRA profile ids %d/%d/%d, want 6 in each enum\n", (int)DSD_CH_LPF_PROFILE_TETRA,
                    (int)RTL_STREAM_CHANNEL_PROFILE_TETRA, (int)DSD_RTL_STREAM_CHANNEL_PROFILE_TETRA);
        return 1;
    }
    return 0;
}

/* The protected edge is the channel each profile keeps: TETRA's 25 kHz channel at 11250 Hz, the others unchanged. */
static int
test_protected_edges(void) {
    struct edge_case {
        int profile;
        double edge_hz;
    };

    const edge_case cases[] = {
        {DSD_CH_LPF_PROFILE_WIDE, 8000.0},
        {DSD_CH_LPF_PROFILE_6K25, 3125.0},
        {DSD_CH_LPF_PROFILE_12K5, 6250.0},
        {DSD_CH_LPF_PROFILE_PROVOICE, 6250.0},
        {DSD_CH_LPF_PROFILE_P25_C4FM, 6250.0},
        {DSD_CH_LPF_PROFILE_P25_CQPSK, 6250.0},
        {DSD_CH_LPF_PROFILE_TETRA, 11250.0},
        {7, 8000.0},
        {-1, 8000.0},
    };
    for (const edge_case& c : cases) {
        const double got = dsd_channel_lpf_protected_edge_hz(c.profile);
        if (std::fabs(got - c.edge_hz) > 1e-9) {
            DSD_FPRINTF(stderr, "profile %d protects %.1f Hz, want %.1f\n", c.profile, got, c.edge_hz);
            return 1;
        }
    }
    return 0;
}

/* The TETRA channel design: GNU Radio's window design (cutoff 12.5 kHz, 2.5 kHz Blackman transition) at every rate the
 * TETRA chain runs at, odd and symmetric, within its measured mask. */
static int
test_tetra_channel_design(void) {
    static float taps[DSD_CHANNEL_LPF_MAX_TAPS];
    static float gr[DSD_CHANNEL_LPF_MAX_TAPS];
    for (int rate = 36000; rate <= 144000; rate += 18000) {
        const int n = dsd_tetra_channel_design(rate, taps, DSD_CHANNEL_LPF_MAX_TAPS);
        const int want = rate == 36000 ? 49 : rate == 72000 ? 97 : rate == 144000 ? 193 : -1;
        if (n < 3 || (n % 2) == 0 || n > 193 || (want > 0 && n != want)) {
            DSD_FPRINTF(stderr, "TETRA channel at %d Hz: %d taps (want odd, <= 193, %d)\n", rate, n, want);
            return 1;
        }
        const int gr_n =
            dsd_firdes_low_pass(1.0, (double)rate, 12500.0, 2500.0, DSD_WIN_BLACKMAN, gr, DSD_CHANNEL_LPF_MAX_TAPS);
        if (gr_n != n || std::memcmp(taps, gr, (size_t)n * sizeof(float)) != 0) {
            DSD_FPRINTF(stderr, "TETRA channel at %d Hz is not the 12.5 kHz / 2.5 kHz Blackman design\n", rate);
            return 1;
        }
        /* Symmetric to float rounding: the Blackman window's two halves come from separate cos() evaluations. */
        double peak = 0.0;
        for (int i = 0; i < n; i++) {
            peak = std::fmax(peak, std::fabs((double)taps[i]));
        }
        for (int i = 0; i < n / 2; i++) {
            if (std::fabs((double)taps[i] - (double)taps[n - 1 - i]) > 1e-6 * peak) {
                DSD_FPRINTF(stderr, "TETRA channel at %d Hz: tap %d is not symmetric\n", rate, i);
                return 1;
            }
        }
        const double fs = (double)rate;
        for (int f_hz = 0; f_hz <= 11250; f_hz += 25) {
            const double db = tap_response_db(taps, n, fs, (double)f_hz);
            if (db < -0.35) {
                DSD_FPRINTF(stderr, "TETRA channel at %d Hz: %d Hz at %.3f dB (< -0.35 dB)\n", rate, f_hz, db);
                return 1;
            }
        }
        const double edge_db = tap_response_db(taps, n, fs, 12150.0);
        if (std::fabs(edge_db - -3.2) > 0.5) {
            DSD_FPRINTF(stderr, "TETRA channel at %d Hz: 12150 Hz at %.3f dB (want -3.2 +- 0.5 dB)\n", rate, edge_db);
            return 1;
        }
        const double skirt_db = tap_response_db(taps, n, fs, 13750.0);
        if (skirt_db > -28.0) {
            DSD_FPRINTF(stderr, "TETRA channel at %d Hz: 13750 Hz at %.3f dB (> -28 dB)\n", rate, skirt_db);
            return 1;
        }
        for (int f_hz = 15000; f_hz <= rate / 2; f_hz += 25) {
            const double db = tap_response_db(taps, n, fs, (double)f_hz);
            if (db > -75.0) {
                DSD_FPRINTF(stderr, "TETRA channel at %d Hz: %d Hz at %.3f dB (> -75 dB)\n", rate, f_hz, db);
                return 1;
            }
        }
    }
    /* No clamp and no fallback: a rate that cannot hold the transition, a short buffer or a bad argument is refused
       without writing. */
    for (int i = 0; i < DSD_CHANNEL_LPF_MAX_TAPS; i++) {
        taps[i] = 7.0f;
    }
    if (dsd_tetra_channel_design(18000, taps, DSD_CHANNEL_LPF_MAX_TAPS) != -1
        || dsd_tetra_channel_design(27000, taps, DSD_CHANNEL_LPF_MAX_TAPS) != -1
        || dsd_tetra_channel_design(144000, taps, 192) != -1 || dsd_tetra_channel_design(0, taps, 288) != -1
        || dsd_tetra_channel_design(72000, NULL, DSD_CHANNEL_LPF_MAX_TAPS) != -1) {
        DSD_FPRINTF(stderr, "TETRA channel design accepted an unrealizable rate, a short buffer or a bad argument\n");
        return 1;
    }
    if (std::fabs((double)taps[0] - 7.0) > 1e-9) {
        DSD_FPRINTF(stderr, "a refused TETRA channel design wrote taps\n");
        return 1;
    }
    return 0;
}

/* One squelched block of a TETRA-profile CQPSK stream at rate_hz, with the chain marked active or not: the block
 * designs its channel plan and emits zero symbols without running the CQPSK loops. An inactive block is not CQPSK, the
 * plain cause that keeps the chain off. */
static void
run_tetra_profile_block(demod_state* s, int rate_hz, int active) {
    s->rate_in = rate_hz;
    s->rate_out = rate_hz;
    s->complex_rate = rate_hz;
    s->rate_out2 = 0;
    s->mode_demod = &raw_demod;
    s->lowpassed = s->input_cb_buf;
    s->lp_len = 1024;
    s->channel_lpf_enable = 1;
    s->channel_lpf_profile = DSD_CH_LPF_PROFILE_TETRA;
    s->symbol_rate_hz = 18000;
    s->ted_sps = rate_hz / 18000;
    s->cqpsk_enable = active ? 1 : 0;
    s->output_kind = active ? DSD_DEMOD_OUTPUT_SYMBOL_CQPSK : DSD_DEMOD_OUTPUT_FSK_DISCRIMINATOR;
    s->tetra_chain = active ? 1 : 0;
    s->channel_squelch_level.store(1.0f);
    full_demod(s);
}

/* The TETRA profile installs its own design only while the TETRA chain runs. Inactive, it runs the legacy WIDE plan
 * exactly (the design within 288 taps, or the 63-tap fallback above DSD_DIGITAL_CHANNEL_MAX_RATE_HZ), and the
 * plan follows the chain both ways at one rate and profile. */
static int
test_tetra_profile_plan(demod_state* s) {
    static float wide[DSD_CHANNEL_LPF_MAX_TAPS];
    static float want[DSD_CHANNEL_LPF_MAX_TAPS];
    const int inactive_rates[] = {
        24000, 36000, 48000, 72000, 78125, DSD_DIGITAL_CHANNEL_MAX_RATE_HZ, DSD_DIGITAL_CHANNEL_MAX_RATE_HZ + 1,
        144000};
    for (int rate : inactive_rates) {
        int wide_len = 0;
        design_plan(s, rate, DSD_CH_LPF_PROFILE_WIDE, 0, wide, &wide_len);
        DSD_MEMSET(s, 0, sizeof(*s));
        run_tetra_profile_block(s, rate, 0);
        const int len = s->channel_lpf_plan_taps_len;
        if (wide_len <= 0 || len != wide_len
            || std::memcmp(s->channel_lpf_plan_taps, wide, (size_t)wide_len * sizeof(float)) != 0) {
            DSD_FPRINTF(stderr, "inactive TETRA profile at %d Hz planned %d taps, want the %d-tap WIDE plan\n", rate,
                        len, wide_len);
            return 1;
        }
    }
    for (int rate = 36000; rate <= 144000; rate += 18000) {
        const int want_len = dsd_tetra_channel_design(rate, want, DSD_CHANNEL_LPF_MAX_TAPS);
        DSD_MEMSET(s, 0, sizeof(*s));
        run_tetra_profile_block(s, rate, 1);
        if (want_len <= 0 || s->channel_lpf_plan_taps_len != want_len
            || std::memcmp(s->channel_lpf_plan_taps, want, (size_t)want_len * sizeof(float)) != 0) {
            DSD_FPRINTF(stderr, "active TETRA chain at %d Hz planned %d taps, want the %d-tap TETRA design\n", rate,
                        s->channel_lpf_plan_taps_len, want_len);
            return 1;
        }
    }
    /* Same rate and profile, the chain leaving and entering: the plan is redesigned each way. */
    int wide_len = 0;
    design_plan(s, 72000, DSD_CH_LPF_PROFILE_WIDE, 0, wide, &wide_len);
    const int tetra_len = dsd_tetra_channel_design(72000, want, DSD_CHANNEL_LPF_MAX_TAPS);
    DSD_MEMSET(s, 0, sizeof(*s));
    run_tetra_profile_block(s, 72000, 1);
    run_tetra_profile_block(s, 72000, 0);
    if (s->channel_lpf_plan_taps_len != wide_len
        || std::memcmp(s->channel_lpf_plan_taps, wide, (size_t)wide_len * sizeof(float)) != 0) {
        DSD_FPRINTF(stderr, "TETRA chain leaving at 72 kHz kept a %d-tap plan, want the %d-tap WIDE plan\n",
                    s->channel_lpf_plan_taps_len, wide_len);
        return 1;
    }
    run_tetra_profile_block(s, 72000, 1);
    if (s->channel_lpf_plan_taps_len != tetra_len
        || std::memcmp(s->channel_lpf_plan_taps, want, (size_t)tetra_len * sizeof(float)) != 0) {
        DSD_FPRINTF(stderr, "TETRA chain entering at 72 kHz kept a %d-tap plan, want the %d-tap TETRA design\n",
                    s->channel_lpf_plan_taps_len, tetra_len);
        return 1;
    }
    return 0;
}

/* The digital profiles' cutoffs (demod_pipeline.cpp): the protected edge plus the 600 Hz guard, CQPSK's 7250 Hz. */
namespace {
struct digital_profile {
    int profile;
    const char* name;
    double cutoff_hz;
};
} // namespace

static const digital_profile kDigitalProfiles[] = {
    {DSD_CH_LPF_PROFILE_WIDE, "WIDE", 8600.0},         {DSD_CH_LPF_PROFILE_6K25, "6K25", 3725.0},
    {DSD_CH_LPF_PROFILE_12K5, "12K5", 6850.0},         {DSD_CH_LPF_PROFILE_PROVOICE, "PROVOICE", 6850.0},
    {DSD_CH_LPF_PROFILE_P25_C4FM, "P25_C4FM", 6850.0}, {DSD_CH_LPF_PROFILE_P25_CQPSK, "P25_CQPSK", 7250.0},
};

/* The rates the digital designs are checked at: the 24/36/48 kHz ones, past the old 144-tap cap (51,374 Hz), the RTL
 * 72 kHz DSP bandwidth, the Airspy rates a capture leaves the half-band cascade at, and DSD_DIGITAL_CHANNEL_MAX_RATE_HZ
 * itself. */
static const int kDigitalDesignRates[] = {
    24000, 36000, 48000, 51374, 62500, 72000, 78125, 93750, 96000, DSD_DIGITAL_CHANNEL_MAX_RATE_HZ,
};

static int
expect_digital_shape(const digital_profile& p, int rate, const float* taps, int len) {
    const double fs = (double)rate;
    const double edge_hz = dsd_channel_lpf_protected_edge_hz(p.profile);
    for (int f_hz = 0; (double)f_hz <= edge_hz; f_hz += 50) {
        if (tap_response_db(taps, len, fs, (double)f_hz) < -1.0) {
            DSD_FPRINTF(stderr, "%s at %d Hz: %d Hz inside the %.0f Hz edge is below -1 dB\n", p.name, rate, f_hz,
                        edge_hz);
            return 1;
        }
    }
    for (int f_hz = (int)p.cutoff_hz + 900; f_hz <= rate / 2; f_hz += 50) {
        const double db = tap_response_db(taps, len, fs, (double)f_hz);
        if (db > -50.0) {
            DSD_FPRINTF(stderr, "%s at %d Hz: %d Hz at %.2f dB (> -50 dB)\n", p.name, rate, f_hz, db);
            return 1;
        }
    }
    return 0;
}

/* Every digital profile runs its own design, the 1200 Hz Blackman low-pass at its cutoff within the 288-tap capacity,
 * at every rate up to DSD_DIGITAL_CHANNEL_MAX_RATE_HZ, with its passband to the protected edge and 50 dB down from
 * 900 Hz past the cutoff (issue #806: above ~51.4 kHz they ran the 63-tap prototype for 24 kHz, cut at a third of the
 * rate for WIDE and the P25 and ProVoice profiles). 1 Hz above that rate no design fits and every profile runs its
 * fallback prototype, the same taps as at 144 kHz. */
static int
test_digital_designs_to_max_rate(demod_state* s) {
    static float taps[DSD_CHANNEL_LPF_MAX_TAPS];
    static float want[DSD_CHANNEL_LPF_MAX_TAPS];
    for (const digital_profile& p : kDigitalProfiles) {
        for (int rate : kDigitalDesignRates) {
            int len = 0;
            design_plan(s, rate, p.profile, 0, taps, &len);
            const int want_len = dsd_firdes_low_pass(1.0, (double)rate, p.cutoff_hz, 1200.0, DSD_WIN_BLACKMAN, want,
                                                     DSD_CHANNEL_LPF_MAX_TAPS);
            if (want_len <= 0 || len != want_len || std::memcmp(taps, want, (size_t)len * sizeof(float)) != 0) {
                DSD_FPRINTF(stderr, "%s at %d Hz: a %d-tap plan, want its %d-tap design\n", p.name, rate, len,
                            want_len);
                return 1;
            }
            if (expect_digital_shape(p, rate, taps, len) != 0) {
                return 1;
            }
        }
        static float past[DSD_CHANNEL_LPF_MAX_TAPS];
        int past_len = 0;
        int high_len = 0;
        design_plan(s, DSD_DIGITAL_CHANNEL_MAX_RATE_HZ + 1, p.profile, 0, past, &past_len);
        design_plan(s, 144000, p.profile, 0, taps, &high_len);
        if (past_len != 63 || high_len != 63 || std::memcmp(past, taps, (size_t)past_len * sizeof(float)) != 0) {
            DSD_FPRINTF(stderr,
                        "%s 1 Hz above DSD_DIGITAL_CHANNEL_MAX_RATE_HZ: a %d-tap plan, want the 63-tap fallback\n",
                        p.name, past_len);
            return 1;
        }
    }
    return 0;
}

/* The noise bandwidth the SNR bias uses is the plan's own, Bn = (Fs/2) x sum(h^2) / sum(h)^2 over the taps that run:
 * each profile's design up to DSD_DIGITAL_CHANNEL_MAX_RATE_HZ, the fallback prototype above it. */
static int
test_noise_bandwidth_follows_plan(demod_state* s) {
    const int rates[] = {
        24000,  48000, 72000, 78125, 93750, DSD_DIGITAL_CHANNEL_MAX_RATE_HZ, DSD_DIGITAL_CHANNEL_MAX_RATE_HZ + 1,
        128000, 144000};
    static float taps[DSD_CHANNEL_LPF_MAX_TAPS];
    double worst = 0.0;
    for (const digital_profile& p : kDigitalProfiles) {
        for (int rate : rates) {
            int len = 0;
            design_plan(s, rate, p.profile, 0, taps, &len);
            double sum = 0.0;
            double energy = 0.0;
            for (int i = 0; i < len; i++) {
                sum += (double)taps[i];
                energy += (double)taps[i] * (double)taps[i];
            }
            const double bn = (double)rate * 0.5 * energy / (sum * sum);
            const double got = dsd_snr_noise_bandwidth_hz(rate, p.profile, DSD_SNR_TETRA_STAGE_WIDE);
            const double err = std::fabs(got / bn - 1.0);
            worst = std::fmax(worst, err);
            if (err > 0.005) {
                DSD_FPRINTF(stderr, "%s at %d Hz: SNR noise bandwidth %.1f Hz, the %d-tap plan's is %.1f Hz\n", p.name,
                            rate, got, len, bn);
                return 1;
            }
        }
    }
    DSD_FPRINTF(stderr, "SNR noise bandwidth within %.3f%% of every plan's\n", worst * 100.0);
    return 0;
}

/* The runtime validator mirrors the DSP design constants. */
static int
test_runtime_mirror(void) {
    const double atten = dsd_window_max_attenuation(DSD_WIN_BLACKMAN);
    if (fabs(atten - (double)DSD_ANALOG_CHANNEL_WINDOW_ATTENUATION_DB) > 1e-9) {
        DSD_FPRINTF(stderr, "Blackman attenuation %.3f differs from the runtime mirror\n", atten);
        return 1;
    }
    for (int rate = 4000; rate <= 130000; rate += 125) {
        const int want =
            dsd_firdes_compute_ntaps((double)rate, (double)DSD_ANALOG_CHANNEL_TRANSITION_HZ, DSD_WIN_BLACKMAN);
        if (dsd_analog_channel_taps_for_rate(rate) != want) {
            DSD_FPRINTF(stderr, "tap count at %d Hz: runtime %d firdes %d\n", rate,
                        dsd_analog_channel_taps_for_rate(rate), want);
            return 1;
        }
    }
    /* Every width the validator accepts designs, and every width it rejects fails. */
    static float taps[DSD_CHANNEL_LPF_MAX_TAPS];
    const int rates[] = {6000, 8000, 12000, 16000, 24000, 46875, 48000, 62500, 78125, 96000, 104000};
    for (int rate : rates) {
        for (int w = 2000; w <= 30000; w += 100) {
            const int realizable = dsd_analog_width_realizable(w, rate);
            const int len = dsd_channel_lpf_design_analog(rate, w, taps, DSD_CHANNEL_LPF_MAX_TAPS);
            if ((len > 0) != (realizable != 0)) {
                DSD_FPRINTF(stderr, "width %d at %d Hz: validator %d design %d\n", w, rate, realizable, len);
                return 1;
            }
        }
    }
    return 0;
}

int
main(void) {
    /* demod_state carries 64-byte-aligned members, so it needs the aligned allocator, not calloc(). */
    demod_state* s = static_cast<demod_state*>(dsd_neo_aligned_malloc(sizeof(demod_state)));
    if (!s) {
        return 1;
    }
    DSD_MEMSET(s, 0, sizeof(*s));
    int rc = 0;
    rc |= test_rrc_dc();
    rc |= test_default_width_matches_wide(s);
    rc |= test_width_response();
    rc |= test_forced_rate_capacity(s);
    rc |= test_unrealizable_width(s);
    rc |= test_cqpsk_under_analog_family_keeps_profile(s);
    rc |= test_digital_row_profile_under_analog_family(s);
    rc |= test_legacy_wide_width(s);
    rc |= test_runtime_mirror();
    rc |= test_profile_plans_pinned(s);
    rc |= test_tetra_profile_ids();
    rc |= test_protected_edges();
    rc |= test_tetra_channel_design();
    rc |= test_tetra_profile_plan(s);
    rc |= test_digital_designs_to_max_rate(s);
    rc |= test_noise_bandwidth_follows_plan(s);
    dsd_neo_aligned_free(s);
    return rc;
}
