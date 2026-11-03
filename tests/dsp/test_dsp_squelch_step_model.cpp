// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The dynamic squelches through the live I/Q rate step (issue #806). Noise is run through full_demod() itself (the real
 * half-band cascade, the real step and the real channel filter), and the plans the step model makes are held to what
 * that noise does:
 *
 * - R_hb from the half-band recursion against white noise through the real cascade at 2.5 and 3 MS/s (lags within 2 %
 *   of R[0]); the spectral factor's autocorrelation against R_hb.
 * - The model's covariance after the real step, averaged over the period, against the step's measured output (lags
 *   0..64 within 2 % of R[0]), and its per-phase power spread (under 5 %).
 * - Every selectable DSP bandwidth at the post-half-band rates of 2.5 and 3 MS/s (stand-ins: one 31-tap pass from twice
 *   that rate), with the widest NFM channel the bandwidth takes and with none, and windows starting at three phases of
 *   the step's period: the auto squelch's floor statistics (no opens over 60 s at +3/+6/+10 dB, the floor KNOWN within
 *   eight windows and within 0.5 dB, never learned again) and the noise squelch's (its references within 0.6 dB per
 *   band-pass and 0.3 dB over the band, noise reading about 0 dB and never opening at N = 3 over 20 s), the RTL-SDR
 *   chain at the same bandwidth held to the same bounds. The plans the half-band cascade alone gives (what a stepped
 *   chain ran before the model) miss the band by more. --matrix runs this part.
 * - Without a step full_demod()'s plans are today's designs, byte for byte; the plan key and the model follow the
 *   step's plan, and a build for the plan the model holds does nothing.
 */

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dsd-neo/core/power.h>
#include <dsd-neo/dsp/demod_pipeline.h>
#include <dsd-neo/dsp/demod_state.h>
#include <dsd-neo/dsp/halfband.h>
#include <dsd-neo/dsp/live_iq_step.h>
#include <dsd-neo/dsp/nfm_noise_squelch.h>
#include <dsd-neo/dsp/squelch_floor.h>
#include <dsd-neo/dsp/squelch_step_model.h>
#include <dsd-neo/io/rate_plan.h>
#include <dsd-neo/runtime/analog_channel.h>
#include <dsd-neo/runtime/mem.h>
#include <vector>
#include "dsd-neo/core/safe_api.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace {

int g_failures = 0;

#define CHECK(cond, ...)                                                                                               \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            DSD_FPRINTF(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                                                   \
            DSD_FPRINTF(stderr, __VA_ARGS__);                                                                          \
            DSD_FPRINTF(stderr, "\n");                                                                                 \
            g_failures++;                                                                                              \
        }                                                                                                              \
    } while (0)

constexpr int kBlockPairs = 16384;

double
db(double ratio) {
    return 10.0 * std::log10(ratio);
}

/* xorshift64* with Box-Muller, both values used. */
struct Rng {
    uint64_t s;
    int have = 0;
    double spare = 0.0;

    explicit Rng(uint64_t seed) : s(seed) {}

    double
    uniform() {
        s ^= s >> 12;
        s ^= s << 25;
        s ^= s >> 27;
        return ((double)((s * 0x2545F4914F6CDD1DULL) >> 11) + 0.5) * (1.0 / 9007199254740992.0);
    }

    double
    gauss() {
        if (have) {
            have = 0;
            return spare;
        }
        const double mag = std::sqrt(-2.0 * std::log(uniform()));
        const double ang = 2.0 * M_PI * uniform();
        spare = mag * std::sin(ang);
        have = 1;
        return mag * std::cos(ang);
    }
};

struct DemodBox {
    demod_state* s;

    DemodBox() : s(static_cast<demod_state*>(dsd_neo_aligned_malloc(sizeof(demod_state)))) {
        if (s) {
            DSD_MEMSET(s, 0, sizeof(*s));
        }
    }

    ~DemodBox() {
        if (s) {
            dsd_live_iq_step_disable(&s->live_iq_step);
            dsd_squelch_step_model_release(&s->squelch_step_model);
            dsd_neo_aligned_free(s->post_polydecim_taps);
            dsd_neo_aligned_free(s->post_polydecim_hist);
            dsd_neo_aligned_free(s);
        }
    }

    DemodBox(const DemodBox&) = delete;
    DemodBox& operator=(const DemodBox&) = delete;
};

/* The planner's final step plan for a device that runs at @p device_hz. */
dsd_live_iq_step_plan
plan_for(dsd_rate_plan_backend backend, uint32_t device_hz, uint32_t target_hz) {
    dsd_rate_plan_input in;
    DSD_MEMSET(&in, 0, sizeof(in));
    in.backend = backend;
    in.request_hz = target_hz;
    in.programmed_hz = device_hz;
    in.readback_hz = device_hz;
    in.target_hz = target_hz;
    in.max_passes = 10;
    dsd_rate_plan rp;
    const int rc = dsd_rate_plan_compute(&in, &rp);
    CHECK(rc == DSD_RATE_PLAN_OK && rp.step.enabled, "plan %u Hz -> %u Hz: %d %s", device_hz, target_hz, rc,
          rp.message);
    dsd_live_iq_step_plan p;
    DSD_MEMSET(&p, 0, sizeof(p));
    p.enabled = rp.step.enabled;
    p.passes = rp.passes;
    p.hb_out_num = rp.hb_out_num;
    p.hb_out_shift = rp.hb_out_shift;
    p.L = rp.step.L;
    p.M = rp.step.M;
    p.target_hz = (int)rp.target_hz;
    p.protect_hz = rp.step.protect_hz;
    p.stop_hz = rp.step.stop_hz;
    return p;
}

/* The same post-half-band rate from one 31-tap pass: what the long runs feed (twice hb_out, not the device rate). */
dsd_live_iq_step_plan
stand_in(dsd_live_iq_step_plan p) {
    p.passes = 1;
    return p;
}

int
stages_for(int passes, dsd_noise_squelch_stage* st) {
    for (int i = 0; i < passes; i++) {
        st[i].taps = (i == 0) ? hb31_q15_taps : hb_q15_taps;
        st[i].len = (i == 0) ? 31 : HB_TAPS;
    }
    return passes;
}

/* full_demod() on the FM monitor with the cascade of @p passes, the step of @p plan when it steps (committed, and
   its squelch model built as the rate chain builds it), the channel filter at @p width_hz (0: off) and the level
   squelch at 0: each block leaves the channel filter's output in lowpassed. */
int
configure(demod_state* s, int passes, int rate_hz, const dsd_live_iq_step_plan* plan, int width_hz) {
    s->mode_demod = &dsd_fm_demod;
    s->analog_family = 1;
    s->analog_demod = DSD_ANALOG_DEMOD_FM;
    s->output_kind = DSD_DEMOD_OUTPUT_AUDIO_MONITOR;
    s->channel_lpf_profile = DSD_CH_LPF_PROFILE_WIDE;
    s->channel_lpf_enable = width_hz > 0 ? 1 : 0;
    s->channel_lpf_width_hz = width_hz;
    s->rate_in = rate_hz;
    s->rate_out = rate_hz;
    s->complex_rate = rate_hz;
    s->downsample_passes = passes;
    s->squelch_mode = DSD_SQUELCH_MODE_LEVEL;
    s->squelch_env = 1.0f;
    if (!plan || !plan->enabled) {
        return 0;
    }
    int rc = dsd_demod_live_step_prepare(s, plan);
    if (rc == DSD_LIVE_IQ_STEP_OK) {
        rc = dsd_live_iq_step_commit(&s->live_iq_step);
    }
    dsd_live_iq_step_finish(&s->live_iq_step);
    if (rc == DSD_LIVE_IQ_STEP_OK) {
        rc = dsd_demod_squelch_step_model_sync(s);
    }
    return rc;
}

/* @p in_pairs of complex Gaussian noise of variance @p var through full_demod(); @p sink sees each block's channel
   output. */
template <class Sink>
void
feed_noise(demod_state* s, Rng& rng, double var, long in_pairs, Sink&& sink) {
    static float buf[2 * kBlockPairs];
    const double sd = std::sqrt(var / 2.0);
    while (in_pairs > 0) {
        const int n = in_pairs < kBlockPairs ? (int)in_pairs : kBlockPairs;
        for (int i = 0; i < 2 * n; i++) {
            buf[i] = (float)(sd * rng.gauss());
        }
        DSD_MEMCPY(s->input_cb_buf, buf, (size_t)n * 2U * sizeof(float));
        s->lowpassed = s->input_cb_buf;
        s->lp_len = n * 2;
        full_demod(s);
        if (!s->front_end_empty && s->lowpassed && s->lp_len > 0) {
            sink(s->lowpassed, s->lp_len / 2);
        }
        in_pairs -= n;
    }
}

/* E{z_n conj(z_{n+m})} measured over a stream (real part: the imaginary part is noise around 0). */
struct AutoCorr {
    int lags;
    std::vector<double> hist_re, hist_im, sum;
    long count = 0;
    long pos = 0;

    explicit AutoCorr(int n) : lags(n), hist_re((size_t)n, 0.0), hist_im((size_t)n, 0.0), sum((size_t)n, 0.0) {}

    void
    add(const float* iq, int pairs) {
        for (int i = 0; i < pairs; i++) {
            const double re = (double)iq[(size_t)i * 2U];
            const double im = (double)iq[(size_t)i * 2U + 1U];
            hist_re[(size_t)(pos % lags)] = re;
            hist_im[(size_t)(pos % lags)] = im;
            const int have = pos + 1 < lags ? (int)(pos + 1) : lags;
            for (int m = 0; m < have; m++) {
                const size_t at = (size_t)((pos - m) % lags);
                sum[(size_t)m] += (re * hist_re[at]) + (im * hist_im[at]);
            }
            pos++;
        }
        count += pairs;
    }

    double
    at(int m) const {
        return sum[(size_t)m] / (double)(count - m);
    }
};

/* ------------------------------------------------------------------------------------- the cascade and the step */

/* R_hb from the recursion against white noise through the real cascade, at the real passes. */
void
test_halfband_recursion(void) {
    const struct {
        uint32_t device;
        int target;
    } chains[] = {{2500000u, 48000}, {3000000u, 48000}, {2500000u, 16000}};

    for (const auto& c : chains) {
        const dsd_live_iq_step_plan plan = plan_for(DSD_RATE_PLAN_BACKEND_AIRSPY, c.device, (uint32_t)c.target);
        dsd_noise_squelch_stage st[DSD_NOISE_SQUELCH_MAX_STAGES];
        dsd_squelch_step_model m;
        dsd_squelch_step_model_init(&m);
        const int rc = dsd_squelch_step_model_build(&m, &plan, st, stages_for(plan.passes, st));
        CHECK(rc == 0 && m.ready && m.hb_lags > 1, "build %u -> %d", c.device, c.target);
        DemodBox box;
        const double hb_out = std::ldexp((double)plan.hb_out_num, -(int)plan.hb_out_shift);
        configure(box.s, plan.passes, (int)hb_out, NULL, 0);
        Rng rng(0x4B5EEDULL + c.device);
        AutoCorr ac(m.hb_lags + 3);
        /* White noise of variance 2^(passes - 1): the model's input; 50,000 outputs. */
        const long in_pairs = 50000L << plan.passes;
        feed_noise(box.s, rng, std::ldexp(1.0, plan.passes - 1), in_pairs,
                   [&](const float* iq, int pairs) { ac.add(iq, pairs); });
        double worst = 0.0;
        for (int lag = 0; lag < m.hb_lags + 3; lag++) {
            const double model = lag < m.hb_lags ? m.r_hb[lag] : 0.0;
            worst = std::fmax(worst, std::fabs(ac.at(lag) - model) / m.r_hb[0]);
        }
        printf("R_hb %u Hz, %d passes: %d lags, R[0] %.5f (measured %.5f), worst lag %.4f of R[0]\n", c.device,
               plan.passes, m.hb_lags, m.r_hb[0], ac.at(0), worst);
        CHECK(worst < 0.02, "R_hb %u Hz: worst lag %.4f of R[0]", c.device, worst);

        /* The spectral factor: odd, even, and its autocorrelation is R_hb. */
        CHECK(m.factor_len >= 1 && (m.factor_len & 1) == 1 && m.factor_len <= DSD_SQUELCH_STEP_MAX_FACTOR_TAPS,
              "factor taps %d", m.factor_len);
        double factor_worst = 0.0;
        for (int lag = 0; lag < m.factor_len; lag++) {
            double r = 0.0;
            for (int t = 0; t + lag < m.factor_len; t++) {
                r += m.factor[t] * m.factor[t + lag];
            }
            const double want = lag < m.hb_lags ? m.r_hb[lag] : 0.0;
            factor_worst = std::fmax(factor_worst, std::fabs(r - want) / m.r_hb[0]);
        }
        int symmetric = 1;
        for (int t = 0; t < m.factor_len; t++) {
            symmetric &= m.factor[t] == m.factor[m.factor_len - 1 - t];
        }
        printf("  spectral factor: %d taps, autocorrelation within %.2e of R[0]\n", m.factor_len, factor_worst);
        /* A cut leaving e of the energy out moves the autocorrelation at most 2 sqrt(e) + e of R[0] (1e-6: 2e-3). */
        CHECK(symmetric && factor_worst < 2.001e-3, "factor: symmetric %d, worst %.3e", symmetric, factor_worst);
        dsd_squelch_step_model_release(&m);
    }
}

/* The model's covariance after the real step (no channel filter), averaged over the period, against the step's
   measured output; its per-phase power spread. */
void
test_step_covariance(void) {
    const struct {
        uint32_t device;
        int target;
    } chains[] = {{2500000u, 48000}, {3000000u, 48000}, {2500000u, 72000}, {3000000u, 16000}};

    for (const auto& c : chains) {
        const dsd_live_iq_step_plan plan = plan_for(DSD_RATE_PLAN_BACKEND_AIRSPY, c.device, (uint32_t)c.target);
        DemodBox box;
        CHECK(configure(box.s, plan.passes, c.target, &plan, 0) == 0, "configure %u -> %d", c.device, c.target);
        dsd_squelch_step_model* m = &box.s->squelch_step_model;
        dsd_squelch_floor_plan fp;
        CHECK(dsd_squelch_step_model_floor_plan(m, &fp, NULL, 0, c.target) == 0, "floor plan");
        const int L = plan.L;
        const int lags = m->resp_len;
        std::vector<double> avg((size_t)lags, 0.0);
        double lo = 1e300;
        double hi = 0.0;
        for (int n = 0; n < L; n++) {
            for (int lag = 0; lag < lags; lag++) {
                /* Both sides of each lag: E z_n z_{n+m} and E z_{n-m} z_n average to the same. */
                avg[(size_t)lag] += m->cov[(size_t)n * (size_t)lags + (size_t)lag] / (double)L;
            }
            lo = std::fmin(lo, m->cov[(size_t)n * (size_t)lags]);
            hi = std::fmax(hi, m->cov[(size_t)n * (size_t)lags]);
        }
        Rng rng(0xC0FA11ULL + c.device + (uint64_t)c.target);
        const int measured_lags = lags < 65 ? lags : 65;
        AutoCorr ac(measured_lags);
        feed_noise(box.s, rng, std::ldexp(1.0, plan.passes - 1), (long)c.device * 2L,
                   [&](const float* iq, int pairs) { ac.add(iq, pairs); });
        double worst = 0.0;
        for (int lag = 0; lag < measured_lags; lag++) {
            worst = std::fmax(worst, std::fabs(ac.at(lag) - avg[(size_t)lag]) / avg[0]);
        }
        const double spread = (hi - lo) / avg[0];
        printf("step %u Hz -> %d Hz (L/M %d/%d): power %.5f (measured %.5f), lags 0..%d within %.4f of R[0], "
               "per-phase power spread %.4f %%, noise gain %.5f\n",
               c.device, c.target, plan.L, plan.M, avg[0], ac.at(0), measured_lags - 1, worst, 100.0 * spread,
               fp.noise_gain);
        CHECK(worst < 0.02, "step %u -> %d: worst lag %.4f of R[0]", c.device, c.target, worst);
        CHECK(spread < 0.05, "step %u -> %d: per-phase spread %.4f", c.device, c.target, spread);
        CHECK(std::fabs(fp.noise_gain - avg[0]) <= 1e-12 * avg[0], "noise gain %.9f vs %.9f", fp.noise_gain, avg[0]);
    }
}

/* ------------------------------------------------------------------------------------ plans, keys and the cache */

uint64_t
bits_of(double x) {
    uint64_t u = 0U;
    DSD_MEMCPY(&u, &x, sizeof u);
    return u;
}

/* Every field of two floor plans, bit for bit. */
int
same_floor_plan(const dsd_squelch_floor_plan& a, const dsd_squelch_floor_plan& b) {
    return a.valid == b.valid && a.rate_hz == b.rate_hz && a.lag == b.lag && bits_of(a.rho_lag) == bits_of(b.rho_lag)
           && bits_of(a.beta) == bits_of(b.beta) && bits_of(a.neff) == bits_of(b.neff)
           && bits_of(a.sqrt_neff) == bits_of(b.sqrt_neff) && bits_of(a.noise_gain) == bits_of(b.noise_gain);
}

/* Every field of two noise plans, bit for bit. */
int
same_noise_plan(const dsd_noise_squelch_plan& a, const dsd_noise_squelch_plan& b) {
    int same = a.valid == b.valid && a.rate_hz == b.rate_hz && bits_of(a.edge_hz) == bits_of(b.edge_hz)
               && bits_of(a.lo_hz) == bits_of(b.lo_hz) && bits_of(a.hi_hz) == bits_of(b.hi_hz)
               && a.sub_bands == b.sub_bands && a.bands == b.bands && bits_of(a.p_ref_sum) == bits_of(b.p_ref_sum)
               && a.cascade_len == b.cascade_len;
    for (int k = 0; k < DSD_NOISE_SQUELCH_MAX_BANDS; k++) {
        same &= bits_of(a.p_ref[k]) == bits_of(b.p_ref[k]);
        for (int q = 0; q < DSD_NOISE_SQUELCH_SECTIONS; q++) {
            same &= bits_of(a.section[k][q].gain) == bits_of(b.section[k][q].gain)
                    && bits_of(a.section[k][q].a1) == bits_of(b.section[k][q].a1)
                    && bits_of(a.section[k][q].a2) == bits_of(b.section[k][q].a2);
        }
    }
    for (int k = 0; k < DSD_NOISE_SQUELCH_CASCADE_TAPS; k++) {
        same &= bits_of(a.cascade[k]) == bits_of(b.cascade[k]);
    }
    return same;
}

/* A few blocks of noise through full_demod() under @p mode, so its squelch plans are designed. */
void
run_plans(demod_state* s, int mode, double in_rate) {
    s->squelch_mode = mode;
    s->squelch_margin_db = 10;
    Rng rng(0x91A5ULL);
    feed_noise(s, rng, 1e-3, (long)(0.1 * in_rate), [](const float*, int) {});
}

/* Without a step full_demod()'s plans are today's designs (the cascade's last stage, or the whole cascade for the
   noise squelch), bit for bit. */
void
test_no_step_plans_are_todays(void) {
    const struct {
        int passes;
        int rate;
        int width;
    } chains[] = {{5, 48000, 12500}, {1, 24000, 0}, {6, 16000, 13200}, {0, 48000, 25000}};

    for (const auto& c : chains) {
        DemodBox box;
        demod_state* s = box.s;
        configure(s, c.passes, c.rate, NULL, c.width);
        const double in_rate = std::ldexp((double)c.rate, c.passes);
        run_plans(s, DSD_SQUELCH_MODE_AUTO, in_rate);
        run_plans(s, DSD_SQUELCH_MODE_NOISE, in_rate);
        const float* taps = c.width > 0 ? s->channel_lpf_plan_taps : NULL;
        const int taps_len = c.width > 0 ? s->channel_lpf_plan_taps_len : 0;
        dsd_noise_squelch_stage st[DSD_NOISE_SQUELCH_MAX_STAGES];
        const int count = stages_for(c.passes, st);
        dsd_squelch_floor_plan fp;
        (void)dsd_squelch_floor_plan_design(&fp, taps, taps_len, count > 0 ? st[count - 1].taps : NULL,
                                            count > 0 ? st[count - 1].len : 0, c.rate);
        static dsd_noise_squelch_plan np;
        (void)dsd_noise_squelch_plan_design(&np, taps, taps_len, st, count, c.rate);
        CHECK(fp.valid && same_floor_plan(fp, s->squelch_floor.plan), "%d passes at %d Hz: floor plan differs",
              c.passes, c.rate);
        CHECK(same_noise_plan(np, s->noise_squelch.plan), "%d passes at %d Hz: noise plan differs", c.passes, c.rate);
        CHECK(!s->squelch_step_model.ready && s->squelch_plan.step.enabled == 0, "%d passes: a step model", c.passes);
    }
    printf("no step: full_demod()'s floor and noise plans are today's designs, bit for bit\n");
}

/* The plan key follows the step: another plan designs other plans, and no step goes back to today's; the model is
   kept for its plan and rebuilt for another; a stepping chain with no model has no valid plan (gate open). */
void
test_plan_key_follows_the_step(void) {
    const int rate = 48000;
    const dsd_live_iq_step_plan a = plan_for(DSD_RATE_PLAN_BACKEND_AIRSPY, 2500000u, (uint32_t)rate);
    const dsd_live_iq_step_plan b = plan_for(DSD_RATE_PLAN_BACKEND_AIRSPY, 3000000u, (uint32_t)rate);
    DemodBox box;
    demod_state* s = box.s;
    CHECK(configure(s, a.passes, rate, &a, 12500) == 0, "configure a");
    run_plans(s, DSD_SQUELCH_MODE_AUTO, 2500000.0);
    run_plans(s, DSD_SQUELCH_MODE_NOISE, 2500000.0);
    dsd_squelch_floor_plan fa;
    static dsd_noise_squelch_plan na;
    CHECK(dsd_squelch_step_model_floor_plan(&s->squelch_step_model, &fa, s->channel_lpf_plan_taps,
                                            s->channel_lpf_plan_taps_len, rate)
              == 0,
          "floor plan a");
    CHECK(dsd_squelch_step_model_noise_plan(&s->squelch_step_model, &na, s->channel_lpf_plan_taps,
                                            s->channel_lpf_plan_taps_len, rate)
              == 0,
          "noise plan a");
    CHECK(same_floor_plan(fa, s->squelch_floor.plan) && same_noise_plan(na, s->noise_squelch.plan),
          "the stepped chain's plans are not the model's");
    CHECK(dsd_live_iq_step_plan_equal(&s->squelch_plan.step, &a)
              && dsd_live_iq_step_plan_equal(&s->noise_plan.step, &a),
          "the plan keys do not hold the step");
    /* The noise plan reproduces exactly (fixed seed). */
    static dsd_noise_squelch_plan again;
    (void)dsd_squelch_step_model_noise_plan(&s->squelch_step_model, &again, s->channel_lpf_plan_taps,
                                            s->channel_lpf_plan_taps_len, rate);
    CHECK(same_noise_plan(na, again), "the stepped noise plan does not reproduce");

    /* The same plan again keeps the model as it is. */
    const float* resp = s->squelch_step_model.resp;
    CHECK(dsd_demod_squelch_step_model_sync(s) == 0 && s->squelch_step_model.resp == resp, "the model was rebuilt");

    /* Another plan: the model is rebuilt for it and both plans follow. */
    int rc = dsd_demod_live_step_prepare(s, &b);
    rc = rc == DSD_LIVE_IQ_STEP_OK ? dsd_live_iq_step_commit(&s->live_iq_step) : rc;
    dsd_live_iq_step_finish(&s->live_iq_step);
    CHECK(rc == DSD_LIVE_IQ_STEP_OK && dsd_demod_squelch_step_model_sync(s) == 0
              && dsd_squelch_step_model_matches(&s->squelch_step_model, &b),
          "the model did not follow plan b");
    run_plans(s, DSD_SQUELCH_MODE_AUTO, 3000000.0);
    run_plans(s, DSD_SQUELCH_MODE_NOISE, 3000000.0);
    dsd_squelch_floor_plan fb;
    (void)dsd_squelch_step_model_floor_plan(&s->squelch_step_model, &fb, s->channel_lpf_plan_taps,
                                            s->channel_lpf_plan_taps_len, rate);
    CHECK(same_floor_plan(fb, s->squelch_floor.plan) && !same_floor_plan(fa, fb), "the floor plan did not follow b");
    CHECK(!same_noise_plan(na, s->noise_squelch.plan), "the noise plan did not follow b");

    /* No model for the running step: no valid plan, so the gate stays open. */
    dsd_squelch_step_model_release(&s->squelch_step_model);
    s->channel_lpf_width_hz = 16000;
    run_plans(s, DSD_SQUELCH_MODE_AUTO, 3000000.0);
    dsd_squelch_floor_status st;
    dsd_squelch_floor_get_status(&s->squelch_floor, &st);
    CHECK(!s->squelch_floor.plan.valid && st.gate_open, "a stepping chain with no model has a plan");

    /* The step off: today's plan. */
    dsd_live_iq_step_disable(&s->live_iq_step);
    run_plans(s, DSD_SQUELCH_MODE_AUTO, 3000000.0);
    dsd_squelch_floor_plan today;
    (void)dsd_squelch_floor_plan_design(&today, s->channel_lpf_plan_taps, s->channel_lpf_plan_taps_len, hb_q15_taps,
                                        HB_TAPS, rate);
    CHECK(same_floor_plan(today, s->squelch_floor.plan) && s->squelch_plan.step.enabled == 0,
          "with the step off the plan is not today's");

    /* Refusals leave the model empty. */
    dsd_squelch_step_model m;
    dsd_squelch_step_model_init(&m);
    dsd_noise_squelch_stage stages[DSD_NOISE_SQUELCH_MAX_STAGES];
    dsd_live_iq_step_plan off = a;
    off.enabled = 0;
    CHECK(dsd_squelch_step_model_build(&m, &off, stages, stages_for(a.passes, stages)) == -1 && !m.ready,
          "a plan that does not step built");
    CHECK(dsd_squelch_step_model_build(&m, &a, stages, stages_for(a.passes - 1, stages)) == -1 && !m.ready,
          "a cascade that is not the plan's built");
    dsd_squelch_floor_plan none;
    CHECK(dsd_squelch_step_model_floor_plan(&m, &none, NULL, 0, rate) == -1 && !none.valid, "an empty model planned");
    CHECK(dsd_squelch_step_model_build(&m, &a, stages, stages_for(a.passes, stages)) == 0
              && dsd_squelch_step_model_floor_plan(&m, &none, NULL, 0, rate + 1) == -1,
          "a plan for another rate");
    dsd_squelch_step_model_release(&m);
    printf("plan key: the step's plan keys both plans, the model is kept for its plan and rebuilt for another\n");
}

/* --------------------------------------------------------------------------------------------- the matrix */

/* dsd_fm_demod()'s phase step: the small-angle series where it holds, atan2f elsewhere. */
float
disc_step(float im, float re) {
    if (re > 1.0e-7f && std::fabs(im) <= (0.35f * re)) {
        const float x = im / re;
        const float x2 = x * x;
        return x * (1.0f + x2 * (-0.3333333333333333f + x2 * 0.2f));
    }
    return std::atan2(im, re);
}

/* A plan's band-passes (transposed direct form II, as the squelch runs them) on a discriminator output: each one's
   power after the first @p settle samples. */
struct Meter {
    const dsd_noise_squelch_plan* plan;
    double s1[DSD_NOISE_SQUELCH_MAX_BANDS][DSD_NOISE_SQUELCH_SECTIONS];
    double s2[DSD_NOISE_SQUELCH_MAX_BANDS][DSD_NOISE_SQUELCH_SECTIONS];
    double e[DSD_NOISE_SQUELCH_MAX_BANDS];
    long n;
    long settle;
};

constexpr int kMargins[3] = {3, 6, 10};
constexpr int kPhases = 3;
constexpr int kTrackers = 3 * kPhases;

/* The auto squelch's trackers (every margin at every window start phase) and the noise squelch's (one per phase) on a
   stream of channel samples, each starting @p offset[phase] samples in, and each band-pass's power over the stream. */
struct Runners {
    dsd_squelch_floor floor[kTrackers];
    uint64_t opens[kTrackers] = {};
    uint64_t known_at[kTrackers] = {};
    int relearned[kTrackers] = {};
    dsd_noise_squelch noise[kPhases];
    uint64_t noise_opens[kPhases] = {};
    std::vector<double> q[kPhases];
    uint64_t q_seen[kPhases] = {};
    int noise_valid = 0;
    long offset[kPhases] = {};
    long samples = 0;
    long noise_end = 0; /* the noise squelch runs this many samples */
    float prev_re = 0.0f;
    float prev_im = 0.0f;
};

void
meter_init(Meter* m, const dsd_noise_squelch_plan* plan, long settle) {
    DSD_MEMSET(m, 0, sizeof(*m));
    m->plan = plan;
    m->settle = settle;
}

void
meter_add(Meter* m, float d) {
    for (int k = 0; k < m->plan->bands; k++) {
        double y = (double)d;
        for (int q = 0; q < DSD_NOISE_SQUELCH_SECTIONS; q++) {
            const dsd_noise_squelch_biquad* b = &m->plan->section[k][q];
            const double out = (b->gain * y) + m->s1[k][q];
            m->s1[k][q] = m->s2[k][q] - (b->a1 * out);
            m->s2[k][q] = (-b->gain * y) - (b->a2 * out);
            y = out;
        }
        if (m->n >= m->settle) {
            m->e[k] += y * y;
        }
    }
    m->n++;
}

/* A plan's references against the measured band powers: the worst band-pass and the whole band, in dB. */
void
meter_error(const Meter* m, double* worst, double* whole) {
    const double count = (double)(m->n - m->settle);
    double sum = 0.0;
    *worst = 0.0;
    for (int k = 0; k < m->plan->bands; k++) {
        const double p = m->e[k] / count;
        *worst = std::fmax(*worst, std::fabs(db(m->plan->p_ref[k] / p)));
        sum += k < m->plan->sub_bands ? p : 0.0;
    }
    *whole = db(m->plan->p_ref_sum / sum);
}

void
runners_init(Runners* r, const dsd_squelch_floor_plan* fp, const dsd_noise_squelch_plan* np, int period,
             long noise_end) {
    for (int i = 0; i < kTrackers; i++) {
        DSD_MEMSET(&r->floor[i], 0, sizeof(r->floor[i]));
        dsd_squelch_floor_set_margin(&r->floor[i], kMargins[i % 3]);
        dsd_squelch_floor_set_plan(&r->floor[i], fp);
    }
    r->noise_valid = np->valid;
    for (int p = 0; p < kPhases; p++) {
        DSD_MEMSET(&r->noise[p], 0, sizeof(r->noise[p]));
        dsd_noise_squelch_set_threshold(&r->noise[p], DSD_SQUELCH_MARGIN_MIN_DB);
        dsd_noise_squelch_set_plan(&r->noise[p], np);
        r->offset[p] = (long)((period * p) / kPhases) + (p == kPhases - 1 ? 1 : 0);
    }
    r->noise_end = noise_end;
}

void
runners_track(Runners* r, int i, const float* iq) {
    uint8_t flag = 0;
    dsd_squelch_floor_process(&r->floor[i], iq, 1, &flag);
    r->opens[i] += (flag & DSD_SQUELCH_FLAG_CLOSED) == 0U ? 1U : 0U;
    dsd_squelch_floor_status st;
    dsd_squelch_floor_get_status(&r->floor[i], &st);
    if (st.state == DSD_SQUELCH_FLOOR_KNOWN && r->known_at[i] == 0U) {
        r->known_at[i] = st.windows;
    } else if (st.state == DSD_SQUELCH_FLOOR_LEARNING && r->known_at[i] != 0U) {
        r->relearned[i] = 1;
    }
}

void
runners_noise(Runners* r, int p, float d, const float* iq) {
    uint8_t flag = 0;
    dsd_noise_squelch_process(&r->noise[p], &d, iq, 1, &flag);
    r->noise_opens[p] += (flag & DSD_SQUELCH_FLAG_CLOSED) == 0U ? 1U : 0U;
    if (r->noise[p].windows != r->q_seen[p]) {
        r->q_seen[p] = r->noise[p].windows;
        r->q[p].push_back(r->noise[p].quieting_db);
    }
}

/* One block of channel samples through every runner and the meters. */
void
runners_add(Runners* r, Meter* meters, int meter_count, const float* iq, int pairs) {
    for (int k = 0; k < pairs; k++, r->samples++) {
        const float* z = iq + (size_t)k * 2U;
        const float re = (z[0] * r->prev_re) + (z[1] * r->prev_im);
        const float im = (z[1] * r->prev_re) - (z[0] * r->prev_im);
        const float d = disc_step(im, re);
        r->prev_re = z[0];
        r->prev_im = z[1];
        for (int p = 0; p < kPhases; p++) {
            if (r->samples < r->offset[p]) {
                continue;
            }
            for (int m = 0; m < 3; m++) {
                runners_track(r, (p * 3) + m, z);
            }
            if (r->noise_valid && r->samples < r->noise_end + r->offset[p]) {
                runners_noise(r, p, d, z);
            }
        }
        if (r->samples < r->noise_end) {
            for (int i = 0; i < meter_count; i++) {
                meter_add(&meters[i], d);
            }
        }
    }
}

double
percentile(std::vector<double> v, size_t skip, double pct) {
    if (v.size() <= skip) {
        return 0.0;
    }
    v.erase(v.begin(), v.begin() + (long)skip);
    std::sort(v.begin(), v.end());
    const size_t at = (size_t)std::lround(pct / 100.0 * (double)(v.size() - 1));
    return v[at];
}

/* What one chain did. */
struct Outcome {
    int floor_ok = 1;
    double floor_worst_db = 0.0;
    uint64_t known_max = 0;
    int noise_valid = 0;
    int noise_ok = 1;
    double median_db = 0.0;
    double top_db = 0.0;
    double cal_worst = 0.0;
    double cal_whole = 0.0;
    double interim_worst = 0.0;
    double interim_whole = 0.0;
};

/* @p seconds of unit noise into a chain (full_demod() with @p passes from @p in_rate, the step of @p plan or none) at
   @p width_hz, its plans held to the bounds; @p interim, when given, also measures the plans the half-band stage alone
   gives at the channel rate (what a stepped chain ran before the model). */
Outcome
run_chain(const char* name, int passes, double in_rate, int rate, const dsd_live_iq_step_plan* plan, int width_hz,
          int interim, uint64_t seed) {
    Outcome o;
    DemodBox box;
    demod_state* s = box.s;
    CHECK(configure(s, passes, rate, plan, width_hz) == 0, "%s: configure", name);
    Rng rng(seed);
    /* One block designs the channel plan the squelch plans take. */
    feed_noise(s, rng, 1.0, 4096, [](const float*, int) {});
    const float* taps = width_hz > 0 ? s->channel_lpf_plan_taps : NULL;
    const int taps_len = width_hz > 0 ? s->channel_lpf_plan_taps_len : 0;
    static dsd_squelch_floor_plan fp;
    static dsd_noise_squelch_plan np;
    static dsd_noise_squelch_plan interim_np;
    dsd_noise_squelch_stage st[DSD_NOISE_SQUELCH_MAX_STAGES];
    const int count = stages_for(passes, st);
    const int stepped = plan && plan->enabled;
    if (stepped) {
        CHECK(dsd_squelch_step_model_floor_plan(&s->squelch_step_model, &fp, taps, taps_len, rate) == 0,
              "%s: floor plan", name);
        (void)dsd_squelch_step_model_noise_plan(&s->squelch_step_model, &np, taps, taps_len, rate);
    } else {
        CHECK(dsd_squelch_floor_plan_design(&fp, taps, taps_len, count > 0 ? st[count - 1].taps : NULL,
                                            count > 0 ? st[count - 1].len : 0, rate)
                  == 0,
              "%s: floor plan", name);
        (void)dsd_noise_squelch_plan_design(&np, taps, taps_len, st, count, rate);
    }
    (void)dsd_noise_squelch_plan_design(&interim_np, taps, taps_len, st, count, rate);
    static Runners r;
    r = Runners();
    const long noise_samples = 20L * rate;
    runners_init(&r, &fp, &np, stepped ? plan->L : 1, noise_samples);
    static Meter meters[2];
    meter_init(&meters[0], &np, rate / 10);
    meter_init(&meters[1], &interim_np, rate / 10);
    const int meter_count = !np.valid ? 0 : (interim && interim_np.valid ? 2 : 1);
    feed_noise(s, rng, 1.0, (long)(60.0 * in_rate),
               [&](const float* iq, int pairs) { runners_add(&r, meters, meter_count, iq, pairs); });
    /* Unit noise at the input: the channel's noise power is the plan's noise gain. */
    const double expect = fp.noise_gain;
    for (int i = 0; i < kTrackers; i++) {
        dsd_squelch_floor_status st_now;
        dsd_squelch_floor_get_status(&r.floor[i], &st_now);
        const double off = std::fabs(db(st_now.floor_power / expect));
        o.floor_worst_db = std::fmax(o.floor_worst_db, off);
        o.known_max = r.known_at[i] > o.known_max ? r.known_at[i] : o.known_max;
        const int ok = r.opens[i] == 0U && r.known_at[i] >= (uint64_t)DSD_SQUELCH_FLOOR_LEARN_NEED
                       && r.known_at[i] <= 8U && !r.relearned[i] && st_now.state == DSD_SQUELCH_FLOOR_KNOWN
                       && off <= 0.5;
        if (!ok) {
            DSD_FPRINTF(stderr, "%s, margin %d, phase %d: opens %llu, known at %llu, relearned %d, floor %+.2f dB\n",
                        name, kMargins[i % 3], i / 3, (unsigned long long)r.opens[i], (unsigned long long)r.known_at[i],
                        r.relearned[i], db(st_now.floor_power / expect));
            o.floor_ok = 0;
        }
    }
    o.noise_valid = np.valid;
    if (np.valid) {
        meter_error(&meters[0], &o.cal_worst, &o.cal_whole);
        if (meter_count > 1) {
            meter_error(&meters[1], &o.interim_worst, &o.interim_whole);
        }
        for (int p = 0; p < kPhases; p++) {
            const double median = percentile(r.q[p], 2, 50.0);
            const double top = percentile(r.q[p], 2, 100.0);
            o.median_db = std::fabs(median) > std::fabs(o.median_db) ? median : o.median_db;
            o.top_db = std::fmax(o.top_db, top);
            if (std::fabs(median) >= 0.5 || top >= 3.0 || r.noise_opens[p] != 0U) {
                DSD_FPRINTF(stderr, "%s, phase %d: noise median %+.2f dB, max %+.2f dB, %llu open\n", name, p, median,
                            top, (unsigned long long)r.noise_opens[p]);
                o.noise_ok = 0;
            }
        }
        if (o.cal_worst >= 0.6 || std::fabs(o.cal_whole) >= 0.3) {
            DSD_FPRINTF(stderr, "%s: calibration worst band-pass %.3f dB, whole band %+.3f dB\n", name, o.cal_worst,
                        o.cal_whole);
            o.noise_ok = 0;
        }
    }
    printf("  %-34s lag %d rho %+.4f beta %+.4f N_eff %7.2f gain %.5f | floor %.2f dB known <= %llu %s", name, fp.lag,
           fp.rho_lag, fp.beta, fp.neff, fp.noise_gain, o.floor_worst_db, (unsigned long long)o.known_max,
           o.floor_ok ? "ok" : "FAIL");
    if (np.valid) {
        printf(" | noise median %+.2f max %+.2f cal %.2f/%+.2f dB %s", o.median_db, o.top_db, o.cal_worst, o.cal_whole,
               o.noise_ok ? "ok" : "FAIL");
        if (meter_count > 1) {
            printf(" (half-band alone %.2f/%+.2f dB)", o.interim_worst, o.interim_whole);
        }
    } else {
        printf(" | no noise plan");
    }
    printf("\n");
    CHECK(o.floor_ok && o.noise_ok, "%s", name);
    return o;
}

/* The widest NFM channel a DSP rate takes (0: none). */
int
widest_nfm_hz(int rate) {
    const int widest = dsd_analog_width_max_for_rate(rate);
    const int capped = widest < 25000 ? widest : 25000;
    return capped >= dsd_analog_width_min_hz(DSD_ANALOG_DEMOD_FM) ? capped : 0;
}

/* Every selectable DSP bandwidth at the post-half-band rates of 2.5 and 3 MS/s, against the RTL-SDR chain at the same
   bandwidth. */
void
test_matrix(void) {
    const uint32_t devices[] = {2500000u, 3000000u};
    const int targets[] = {4000, 6000, 8000, 12000, 16000, 24000, 48000, 72000};
    int interim_misses = 0;
    int interim_plans = 0;
    for (int target : targets) {
        const int widths[2] = {widest_nfm_hz(target), 0};
        for (int w = 0; w < 2; w++) {
            if (w == 0 && widths[0] == 0) {
                continue;
            }
            char name[96];
            DSD_SNPRINTF(name, sizeof name, "RTL %d Hz, width %d", target, widths[w]);
            const Outcome rtl = run_chain(name, 1, 2.0 * target, target, NULL, widths[w], 0,
                                          0x5EED0000ULL + (uint64_t)target + (uint64_t)w);
            for (uint32_t device : devices) {
                const dsd_live_iq_step_plan plan =
                    stand_in(plan_for(DSD_RATE_PLAN_BACKEND_AIRSPY, device, (uint32_t)target));
                const double hb_out = std::ldexp((double)plan.hb_out_num, -(int)plan.hb_out_shift);
                DSD_SNPRINTF(name, sizeof name, "%u/%d Hz, width %d", device, target, widths[w]);
                const Outcome step = run_chain(name, 1, 2.0 * hb_out, target, &plan, widths[w], 1,
                                               0xA1E5ULL + device + (uint64_t)target + (uint64_t)w);
                CHECK(step.noise_valid == rtl.noise_valid, "%s: noise plan valid %d, RTL %d", name, step.noise_valid,
                      rtl.noise_valid);
                if (step.noise_valid) {
                    interim_plans++;
                    interim_misses += (step.interim_worst >= 0.6 || std::fabs(step.interim_whole) >= 0.3) ? 1 : 0;
                }
            }
        }
    }
    printf("the half-band stage alone misses the noise references at %d of %d stepped plans\n", interim_misses,
           interim_plans);
    /* What a stepped chain ran before the model: at 24 kHz with no channel filter its references are off by more than
       DSP_NFM_NOISE_SQUELCH allows (the matrix tells the two apart). */
    CHECK(interim_misses >= 1, "the half-band stage alone never missed: the matrix does not test the step");
}
} // namespace

int
main(int argc, char** argv) {
    const int matrix = argc > 1 && std::strcmp(argv[1], "--matrix") == 0;
    if (!matrix) {
        test_halfband_recursion();
        test_step_covariance();
        test_no_step_plans_are_todays();
        test_plan_key_follows_the_step();
    } else {
        test_matrix();
    }
    if (g_failures != 0) {
        DSD_FPRINTF(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    printf("squelch step model: all passed\n");
    return 0;
}
