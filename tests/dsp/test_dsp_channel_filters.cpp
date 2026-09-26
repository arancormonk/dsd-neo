// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Unit tests for channel filters: RRC input filters (DC preservation) and the
 * width-driven analog channel low-pass. */

#include <atomic>
#include <cmath>
#include <cstring>
#include <dsd-neo/dsp/demod_pipeline.h>
#include <dsd-neo/dsp/demod_state.h>
#include <dsd-neo/dsp/firdes.h>
#include <dsd-neo/runtime/analog_channel.h>
#include <dsd-neo/runtime/mem.h>
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

/* Device-forced rates above ~51.4 kHz need more than the 144 digital taps. The
 * analog path designs them properly instead of dropping to the 63-tap prototype
 * built for 24 kHz; digital profiles keep their 144-tap cap and fallback. */
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
    /* Digital WIDE (the digital fallback profile) at the same forced rate is untouched. */
    design_plan(s, 78125, DSD_CH_LPF_PROFILE_WIDE, 0, taps, &len);
    if (len != 63) {
        DSD_FPRINTF(stderr, "digital WIDE at 78125 Hz: %d taps, want the 63-tap fallback\n", len);
        return 1;
    }
    design_plan(s, 78125, DSD_CH_LPF_PROFILE_12K5, 0, taps, &len);
    if (len != 63) {
        DSD_FPRINTF(stderr, "digital 12K5 at 78125 Hz: %d taps, want the 63-tap fallback\n", len);
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
 * passband that plan really has, at every rate: the 144-tap design, its cutoff held to 0.9 x Nyquist at low rates, and
 * the 63-tap fallback prototype above ~51.4 kHz, whose response scales with the rate. The plan passes that width within
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
        {12000, 33, 9600},   {16000, 45, 13200}, {24000, 67, 16000}, {46875, 131, 16000},
        {48000, 135, 16000}, {78125, 63, 0},     {104000, 63, 0},    {128000, 63, 0},
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
    dsd_neo_aligned_free(s);
    return rc;
}
