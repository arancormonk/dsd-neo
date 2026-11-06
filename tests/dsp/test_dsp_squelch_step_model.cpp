// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The dynamic squelches through the live I/Q rate step (issue #806), held to full_demod() itself (the real half-band
 * cascade, the real step and the real channel filter). What can be computed is computed exactly; noise runs only as
 * long as each bound's sampling error needs.
 *
 * - R_hb from the half-band recursion against the real cascade, exactly (an impulse at every input phase), at 2.5, 3
 *   and 10 MS/s; the spectral factor's autocorrelation against R_hb.
 * - The step model's averaged covariance against the real step, exactly, at every 12 to 72 kHz leg of 2.5 and 3 MS/s;
 *   its per-phase power spread.
 * - The periodic floor design at a period of one against today's stationary model; the averaged floor plan against
 *   every output phase in full on every Airspy grid point and a SoapySDR ratio near the 4096-term limit.
 * - The window statistics the classifier sees (N_eff, beta) against the covariance measured on a real stepped stream.
 * - Without a step full_demod()'s plans are today's designs, bit for bit and pinned to b08406ebc's; the plan key and the
 *   model follow the step's plan, and a build for the plan the model holds does nothing.
 * - --matrix: every selectable DSP bandwidth at the post-half-band rates of 2.5 and 3 MS/s (stand-ins: one 31-tap pass
 *   from twice hb_out), the widest NFM channel the bandwidth takes and none: the auto squelch's floor (trackers at
 *   +3/+6/+10 dB at three phases of the step's period), the noise squelch and the measured window statistics, held to
 *   the bounds DSP_SQUELCH_FLOOR and DSP_NFM_NOISE_SQUELCH hold the RTL-SDR chain to (Durations says how long each runs
 *   and why). The plans the half-band cascade alone gives (what a stepped chain ran before the model) miss the band.
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
    if (plan == NULL || plan->enabled == 0) {
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

/* ------------------------------------------------------------------------------------- the cascade and the step */

/* Unit impulses through full_demod(), one at every input phase of a chain whose statistics repeat every @p period
   inputs: impulse k sits at lead + k x @p gap (gap = 1 mod period, so the period's phases are each hit once, and far
   enough apart that no two responses meet within @p lags outputs). For white unit noise in, the summed output
   autocorrelation is the output covariance summed over one output period, exactly: R(n, n') = sum_j g_n[j] g_n'[j]
   over every input j, and shifting an input by a period shifts its response by an output period. */
std::vector<double>
impulse_autocorr(demod_state* s, int period, int gap, int lags) {
    const long lead = 4L * gap;
    const long total = lead + ((long)period * gap) + (4L * gap);
    std::vector<double> sum((size_t)lags, 0.0);
    std::vector<float> out;
    std::vector<float> block((size_t)kBlockPairs * 2U);
    for (long at = 0; at < total; at += kBlockPairs) {
        const int n = total - at < kBlockPairs ? (int)(total - at) : kBlockPairs;
        for (int i = 0; i < n; i++) {
            const long j = at + i - lead;
            block[(size_t)i * 2U] = (j >= 0 && j % gap == 0 && j / gap < period) ? 1.0f : 0.0f;
            block[(size_t)i * 2U + 1U] = 0.0f;
        }
        DSD_MEMCPY(s->input_cb_buf, block.data(), (size_t)n * 2U * sizeof(float));
        s->lowpassed = s->input_cb_buf;
        s->lp_len = n * 2;
        full_demod(s);
        if (!s->front_end_empty && s->lowpassed != NULL && s->lp_len > 0) {
            out.insert(out.end(), s->lowpassed, s->lowpassed + s->lp_len);
        }
    }
    const size_t pairs = out.size() / 2U;
    for (size_t n = 0; n < pairs; n++) {
        const double y = (double)out[n * 2U];
        if (y == 0.0) {
            continue;
        }
        for (int m = 0; m < lags && n + (size_t)m < pairs; m++) {
            sum[(size_t)m] += y * (double)out[(n + (size_t)m) * 2U];
        }
    }
    return sum;
}

/* The smallest gap, 1 more than a multiple of @p period, that leaves @p spacing outputs between impulses at an output
   rate of @p ratio outputs per input. */
int
impulse_gap(int period, double ratio, int spacing) {
    int gap = period + 1;
    while ((double)gap * ratio < (double)spacing) {
        gap += period;
    }
    return gap;
}

/* R_hb from the recursion against the real cascade, exactly: impulses at each of the 2^passes input phases, white
   noise of variance 2^(passes - 1). Within 1e-5 of R[0] (the cascade runs in float). */
void
test_halfband_recursion(void) {
    const struct {
        uint32_t device;
        int target;
    } chains[] = {{2500000u, 48000}, {3000000u, 48000}, {2500000u, 16000}, {10000000u, 48000}};

    for (const auto& c : chains) {
        const dsd_live_iq_step_plan plan = plan_for(DSD_RATE_PLAN_BACKEND_AIRSPY, c.device, (uint32_t)c.target);
        dsd_noise_squelch_stage st[DSD_NOISE_SQUELCH_MAX_STAGES];
        dsd_squelch_step_model m;
        dsd_squelch_step_model_init(&m);
        const int rc = dsd_squelch_step_model_build(&m, &plan, st, stages_for(plan.passes, st));
        CHECK(rc == 0 && m.ready && m.hb_lags > 1, "build %u -> %d", c.device, c.target);
        const DemodBox box;
        const double hb_out = std::ldexp((double)plan.hb_out_num, -(int)plan.hb_out_shift);
        configure(box.s, plan.passes, (int)hb_out, NULL, 0);
        const int period = 1 << plan.passes;
        const int lags = m.hb_lags + 3;
        /* A cascade's response spans under 64 outputs; 64 more keep the lags clear. */
        const std::vector<double> r = impulse_autocorr(box.s, period, impulse_gap(period, 1.0 / period, 128), lags);
        double worst = 0.0;
        for (int lag = 0; lag < lags; lag++) {
            const double model = lag < m.hb_lags ? m.r_hb[lag] : 0.0;
            worst = std::fmax(worst, std::fabs(std::ldexp(r[(size_t)lag], plan.passes - 1) - model) / m.r_hb[0]);
        }
        printf("R_hb %u Hz, %d passes: %d lags, R[0] %.6f, the real cascade within %.2e of R[0]\n", c.device,
               plan.passes, m.hb_lags, m.r_hb[0], worst);
        CHECK(worst < 1e-5, "R_hb %u Hz: worst lag %.3e of R[0]", c.device, worst);

        /* The spectral factor: odd, even, and its autocorrelation is R_hb. */
        CHECK(m.factor_len >= 1 && (m.factor_len & 1) == 1 && m.factor_len <= DSD_SQUELCH_STEP_MAX_FACTOR_TAPS,
              "factor taps %d", m.factor_len);
        double factor_worst = 0.0;
        for (int lag = 0; lag < m.factor_len; lag++) {
            double sum = 0.0;
            for (int t = 0; t + lag < m.factor_len; t++) {
                sum += m.factor[t] * m.factor[t + lag];
            }
            const double want = lag < m.hb_lags ? m.r_hb[lag] : 0.0;
            factor_worst = std::fmax(factor_worst, std::fabs(sum - want) / m.r_hb[0]);
        }
        /* Mirror taps agree to rounding: within 1e-12 of the factor's largest tap. */
        double factor_peak = 0.0;
        for (int t = 0; t < m.factor_len; t++) {
            factor_peak = std::fmax(factor_peak, std::fabs(m.factor[t]));
        }
        bool symmetric = factor_peak > 0.0;
        for (int t = 0; t < m.factor_len; t++) {
            symmetric = symmetric && std::fabs(m.factor[t] - m.factor[m.factor_len - 1 - t]) <= 1e-12 * factor_peak;
        }
        printf("  spectral factor: %d taps, autocorrelation within %.2e of R[0]\n", m.factor_len, factor_worst);
        /* A cut leaving e of the energy out moves the autocorrelation at most 2 sqrt(e) + e of R[0] (1e-6: 2e-3). */
        CHECK(symmetric && factor_worst < 2.001e-3, "factor: symmetric %d, worst %.3e", symmetric ? 1 : 0,
              factor_worst);
        dsd_squelch_step_model_release(&m);
    }
}

/* The step model's averaged covariance against the real step in full_demod(), exactly: the chain with no pass ahead of
   the step (white noise straight in at hb_out, the model's factor then a single tap), impulses at each of the M input
   phases, every lag of the responses' support within 1e-5 of R[0]. Every 12 to 72 kHz leg of 2.5 and 3 MS/s; and the
   model's per-phase power spread under 5 %. */
void
test_step_covariance(void) {
    const struct {
        uint32_t device;
        int target;
    } chains[] = {{2500000u, 48000}, {3000000u, 48000}, {2500000u, 24000}, {3000000u, 24000},
                  {2500000u, 12000}, {3000000u, 12000}, {2500000u, 72000}, {3000000u, 16000}};

    for (const auto& c : chains) {
        dsd_live_iq_step_plan plan = plan_for(DSD_RATE_PLAN_BACKEND_AIRSPY, c.device, (uint32_t)c.target);
        plan.passes = 0;
        const DemodBox box;
        CHECK(configure(box.s, 0, c.target, &plan, 0) == 0, "configure %u -> %d", c.device, c.target);
        dsd_squelch_step_model* m = &box.s->squelch_step_model;
        CHECK(m->ready && m->factor_len == 1, "model %u -> %d", c.device, c.target);
        const int lags = m->resp_len;
        const double ratio = (double)plan.L / (double)plan.M;
        const std::vector<double> r = impulse_autocorr(box.s, plan.M, impulse_gap(plan.M, ratio, (2 * lags) + 8), lags);
        double worst = 0.0;
        for (int lag = 0; lag < lags; lag++) {
            worst = std::fmax(worst, std::fabs((r[(size_t)lag] / (double)plan.L) - m->ry_mean[lag]) / m->ry_mean[0]);
        }
        /* The per-phase power: every output phase's covariance in full. */
        dsd_squelch_floor_plan fp;
        CHECK(dsd_squelch_step_model_floor_plan_exact(m, &fp, NULL, 0, c.target) == 0, "floor plan");
        double lo = 1e300;
        double hi = 0.0;
        for (int n = 0; n < plan.L; n++) {
            lo = std::fmin(lo, m->cov[(size_t)n * (size_t)lags]);
            hi = std::fmax(hi, m->cov[(size_t)n * (size_t)lags]);
        }
        const double spread = (hi - lo) / m->ry_mean[0];
        printf("step %u Hz -> %d Hz (L/M %d/%d): the real step within %.2e of R[0] at %d lags, per-phase power spread "
               "%.4f %%\n",
               c.device, c.target, plan.L, plan.M, worst, lags, 100.0 * spread);
        CHECK(worst < 1e-5, "step %u -> %d: worst lag %.3e of R[0]", c.device, c.target, worst);
        CHECK(spread < 0.05, "step %u -> %d: per-phase spread %.4f", c.device, c.target, spread);
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
    /* Two blocks' worth: the first that leaves channel samples designs both plans. */
    feed_noise(s, rng, 1e-3, (long)(0.01 * in_rate) + (2L * kBlockPairs), [](const float*, int) {});
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
        const DemodBox box;
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
        CHECK(fp.valid && same_floor_plan(fp, s->squelch_floor.plan), "%d passes at %d Hz: floor plan differs",
              c.passes, c.rate);
        static dsd_noise_squelch_plan np;
        (void)dsd_noise_squelch_plan_design(&np, taps, taps_len, st, count, c.rate);
        CHECK(same_noise_plan(np, s->noise_squelch.plan), "%d passes at %d Hz: noise plan differs", c.passes, c.rate);
        CHECK(!s->squelch_step_model.ready && s->squelch_plan.step.enabled == 0, "%d passes: a step model", c.passes);
    }
    printf("no step: full_demod()'s floor and noise plans are today's designs, bit for bit\n");
}

/* The plan key follows the step: another plan designs other plans, and no step goes back to today's; the model is
   kept for its plan and rebuilt for another; a stepping chain with no model has no valid plan (gate open). */
void
test_plan_key_follows_the_step(void) {
    /* 24 kHz: the noise plans calibrate on half the samples 48 kHz takes. */
    const int rate = 24000;
    const dsd_live_iq_step_plan a = plan_for(DSD_RATE_PLAN_BACKEND_AIRSPY, 2500000u, (uint32_t)rate);
    const dsd_live_iq_step_plan b = plan_for(DSD_RATE_PLAN_BACKEND_AIRSPY, 3000000u, (uint32_t)rate);
    const DemodBox box;
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
/* The longest lag the measured covariance keeps: past it every chain's correlation is under 1e-3 of its power, so the
   squares left out move N_eff by under 1e-4. */
constexpr int kCovLags = 160;

/*
 * How long each part of a chain runs, in channel samples, sized to what its bound needs:
 *
 * - floor: the trackers learn in 8 windows and then hold; the floor tracks NOISE windows with an 80 ms time constant,
 *   whose steady-state scatter is sqrt(alpha / (2 - alpha)) / sqrt(N_eff) = 0.49 / sqrt(N_eff), 0.17 dB at the
 *   narrowest plan (N_eff 158) against the 0.5 dB bound; a +3 dB sub-window on noise is more than 9 standard deviations
 *   out (sqrt(2 / N_eff) per 20 ms at N_eff 79), so a longer run adds no opens. 4 s: 100 windows past learning.
 * - noise: Q's maximum over n windows grows as sqrt(2 ln n), so 3 s (150 windows) reaches 84 % of what 20 s (1000)
 *   reaches; the median's scatter is 1.25 sigma_Q / sqrt(150), a few hundredths of a dB against the 0.5 dB bound.
 * - meter: a 300 Hz band-pass measured for T seconds scatters by 1 / sqrt(300 T) in power, 0.10 dB at 6 s; with the
 *   plan's own 2 s calibration (0.18 dB) the worst of 29 band-passes stays under 0.5 dB (0.39 dB measured), under the
 *   0.6 dB bound.
 * - cov: the measured covariance's N_eff scatters by about 2 / sqrt(samples) (0.6 % at 100,000) against the 3 % bound.
 */
struct Durations {
    long floor;
    long noise;
    long meter;
    long cov;
};

/* The auto squelch's trackers (every margin at every window start phase), the noise squelch, the band-pass meters and
   the measured covariance on one stream of channel samples. */
struct Runners {
    dsd_squelch_floor floor[kTrackers];
    uint64_t opens[kTrackers] = {};
    uint64_t known_at[kTrackers] = {};
    int relearned[kTrackers] = {};
    dsd_noise_squelch noise;
    uint64_t noise_opens = 0;
    std::vector<double> q;
    uint64_t q_seen = 0;
    int noise_valid = 0;
    long offset[kPhases] = {};
    long samples = 0;
    Durations len = {0, 0, 0, 0};
    float prev_re = 0.0f;
    float prev_im = 0.0f;
    /* The measured covariance: the last kCovLags samples, sum z_n conj(z_{n-m}) (real part) at each lag, and the mean
       lag-L coherence Re(u_n conj(u_{n-L})), u = z / |z|. */
    int lag = 1;
    double hist_re[kCovLags] = {};
    double hist_im[kCovLags] = {};
    double cov[kCovLags] = {};
    double coh = 0.0;
    long cov_n = 0;
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
             const Durations& len) {
    for (int i = 0; i < kTrackers; i++) {
        DSD_MEMSET(&r->floor[i], 0, sizeof(r->floor[i]));
        dsd_squelch_floor_set_margin(&r->floor[i], kMargins[i % 3]);
        dsd_squelch_floor_set_plan(&r->floor[i], fp);
    }
    r->noise_valid = np->valid;
    DSD_MEMSET(&r->noise, 0, sizeof(r->noise));
    dsd_noise_squelch_set_threshold(&r->noise, DSD_SQUELCH_MARGIN_MIN_DB);
    dsd_noise_squelch_set_plan(&r->noise, np);
    for (int p = 0; p < kPhases; p++) {
        r->offset[p] = (long)((period * p) / kPhases) + (p == kPhases - 1 ? 1 : 0);
    }
    r->len = len;
    r->lag = fp->lag;
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
runners_noise(Runners* r, float d, const float* iq) {
    uint8_t flag = 0;
    dsd_noise_squelch_process(&r->noise, &d, iq, 1, &flag);
    r->noise_opens += (flag & DSD_SQUELCH_FLAG_CLOSED) == 0U ? 1U : 0U;
    if (r->noise.windows != r->q_seen) {
        r->q_seen = r->noise.windows;
        r->q.push_back(r->noise.quieting_db);
    }
}

/* One sample into the measured covariance and coherence. */
void
runners_cov(Runners* r, const float* z) {
    const double re = (double)z[0];
    const double im = (double)z[1];
    const long n = r->cov_n;
    const int have = n + 1 < kCovLags ? (int)(n + 1) : kCovLags;
    const size_t at = (size_t)(n % kCovLags);
    r->hist_re[at] = re;
    r->hist_im[at] = im;
    for (int m = 0; m < have; m++) {
        const size_t back = (size_t)((n - m) % kCovLags);
        r->cov[m] += (re * r->hist_re[back]) + (im * r->hist_im[back]);
    }
    if (n >= r->lag) {
        const size_t back = (size_t)((n - r->lag) % kCovLags);
        const double br = r->hist_re[back];
        const double bi = r->hist_im[back];
        const double mags = std::sqrt(((re * re) + (im * im)) * ((br * br) + (bi * bi)));
        r->coh += mags > 0.0 ? ((re * br) + (im * bi)) / mags : 0.0;
    }
    r->cov_n++;
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
            if (r->samples >= r->offset[p] && r->samples < r->offset[p] + r->len.floor) {
                for (int m = 0; m < 3; m++) {
                    runners_track(r, (p * 3) + m, z);
                }
            }
        }
        /* The noise squelch at window phase 0 only: the step's per-phase power spread is at most 2e-6 (its float
           taps; test_step_covariance), so its 20 ms windows read the same noise at every start phase, and the trackers
           already run all three. */
        if (r->noise_valid != 0 && r->samples < r->len.noise) {
            runners_noise(r, d, z);
        }
        if (r->samples < r->len.meter) {
            for (int i = 0; i < meter_count; i++) {
                meter_add(&meters[i], d);
            }
        }
        if (r->samples < r->len.cov) {
            runners_cov(r, z);
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
    double neff_off = 0.0; /* the measured N_eff over the plan's, less 1 */
    double beta_off = 0.0; /* the measured coherence less beta */
};

/* The 40 ms window statistics the classifier sees, from the stream's measured covariance: a window of N samples'
   power varies as (sum over |m| < N of (N - |m|) R(m)^2) / (N R(0))^2, which is 1 / N_eff (within 3 %: its sampling
   scatter is about 2 / sqrt(samples), 0.6 % at 100,000); the lag-L coherence averages beta (within five of its standard
   deviations, sqrt(N / (2 N_eff samples))). */
int
window_stats_ok(const Runners& r, const dsd_squelch_floor_plan& fp, const char* name, Outcome* o) {
    const double count = (double)r.cov_n;
    const int window = fp.rate_hz / 25;
    const double r0 = r.cov[0] / count;
    double var = (double)window * r0 * r0;
    for (int m = 1; m < kCovLags && m < window; m++) {
        const double rm = r.cov[m] / (count - (double)m);
        var += 2.0 * (double)(window - m) * rm * rm;
    }
    const double neff = ((double)window * (double)window * r0 * r0) / var;
    o->neff_off = neff / fp.neff - 1.0;
    o->beta_off = (r.coh / (count - (double)r.lag)) - fp.beta;
    const double coh_bound = 5.0 * std::sqrt((double)window / (2.0 * fp.neff * count));
    if (std::fabs(o->neff_off) > 0.03 || std::fabs(o->beta_off) > coh_bound) {
        DSD_FPRINTF(stderr, "%s: measured N_eff %+.2f %%, coherence %+.5f off the plan (bound %.5f)\n", name,
                    100.0 * o->neff_off, o->beta_off, coh_bound);
        return 0;
    }
    return 1;
}

/* The trackers' bounds (DSP_SQUELCH_FLOOR's): no opens, KNOWN within 5..8 windows, never learned again, the floor
   within 0.5 dB of the noise power (unit noise in: the plan's noise gain). */
void
floor_bounds(const Runners& r, const dsd_squelch_floor_plan& fp, const char* name, Outcome* o) {
    for (int i = 0; i < kTrackers; i++) {
        dsd_squelch_floor_status st;
        dsd_squelch_floor_get_status(&r.floor[i], &st);
        const double off = std::fabs(db(st.floor_power / fp.noise_gain));
        o->floor_worst_db = std::fmax(o->floor_worst_db, off);
        o->known_max = r.known_at[i] > o->known_max ? r.known_at[i] : o->known_max;
        const bool ok = r.opens[i] == 0U && r.known_at[i] >= (uint64_t)DSD_SQUELCH_FLOOR_LEARN_NEED
                        && r.known_at[i] <= 8U && r.relearned[i] == 0 && st.state == DSD_SQUELCH_FLOOR_KNOWN
                        && off <= 0.5;
        if (!ok) {
            DSD_FPRINTF(stderr, "%s, margin %d, phase %d: opens %llu, known at %llu, relearned %d, floor %+.2f dB\n",
                        name, kMargins[i % 3], i / 3, (unsigned long long)r.opens[i], (unsigned long long)r.known_at[i],
                        r.relearned[i], db(st.floor_power / fp.noise_gain));
            o->floor_ok = 0;
        }
    }
}

/* The noise squelch's bounds (DSP_NFM_NOISE_SQUELCH's): noise reads a median within 0.5 dB of 0 and never reaches
   3 dB or opens; the references are within 0.6 dB per band-pass and 0.3 dB over the band. */
void
noise_bounds(const Runners& r, const Meter* meters, int meter_count, const char* name, Outcome* o) {
    meter_error(&meters[0], &o->cal_worst, &o->cal_whole);
    if (meter_count > 1) {
        meter_error(&meters[1], &o->interim_worst, &o->interim_whole);
    }
    o->median_db = percentile(r.q, 2, 50.0);
    o->top_db = percentile(r.q, 2, 100.0);
    if (std::fabs(o->median_db) >= 0.5 || o->top_db >= 3.0 || r.noise_opens != 0U) {
        DSD_FPRINTF(stderr, "%s: noise median %+.2f dB, max %+.2f dB, %llu open\n", name, o->median_db, o->top_db,
                    (unsigned long long)r.noise_opens);
        o->noise_ok = 0;
    }
    if (o->cal_worst >= 0.6 || std::fabs(o->cal_whole) >= 0.3) {
        DSD_FPRINTF(stderr, "%s: calibration worst band-pass %.3f dB, whole band %+.3f dB\n", name, o->cal_worst,
                    o->cal_whole);
        o->noise_ok = 0;
    }
}

/* The plans a chain runs: behind the step @p plan its model's, or with no step (NULL, the RTL-SDR chain: one 31-tap
   pass from twice the rate) today's designs. */
void
design_chain_plans(demod_state* s, const dsd_live_iq_step_plan* plan, int rate, const float* taps, int taps_len,
                   dsd_squelch_floor_plan* fp, dsd_noise_squelch_plan* np) {
    if (plan != NULL) {
        CHECK(dsd_squelch_step_model_floor_plan(&s->squelch_step_model, fp, taps, taps_len, rate) == 0,
              "stepped floor plan at %d Hz", rate);
        (void)dsd_squelch_step_model_noise_plan(&s->squelch_step_model, np, taps, taps_len, rate);
        return;
    }
    dsd_noise_squelch_stage st[DSD_NOISE_SQUELCH_MAX_STAGES];
    const int count = stages_for(1, st);
    CHECK(dsd_squelch_floor_plan_design(fp, taps, taps_len, st[0].taps, st[0].len, rate) == 0, "floor plan at %d Hz",
          rate);
    (void)dsd_noise_squelch_plan_design(np, taps, taps_len, st, count, rate);
}

/* Unit noise into a chain at @p rate (full_demod() through one 31-tap pass, from twice hb_out then the step of
   @p plan, or from twice the rate with no step when @p plan is NULL) at @p width_hz, its plans held to the bounds for
   the lengths in @p len; @p interim also measures the plans the half-band stage alone gives at the channel rate (what
   a stepped chain ran before the model). */
Outcome
run_chain(const char* name, const dsd_live_iq_step_plan* plan, int rate, int width_hz, int interim, uint64_t seed,
          const Durations& len) {
    Outcome o;
    const DemodBox box;
    demod_state* s = box.s;
    CHECK(configure(s, 1, rate, plan, width_hz) == 0, "%s: configure", name);
    Rng rng(seed);
    /* One block designs the channel plan the squelch plans take, and fills the filters. */
    feed_noise(s, rng, 1.0, 4096, [](const float*, int) {});
    const float* taps = width_hz > 0 ? s->channel_lpf_plan_taps : NULL;
    const int taps_len = width_hz > 0 ? s->channel_lpf_plan_taps_len : 0;
    static dsd_squelch_floor_plan fp;
    static dsd_noise_squelch_plan np;
    static dsd_noise_squelch_plan interim_np;
    design_chain_plans(s, plan, rate, taps, taps_len, &fp, &np);
    DSD_MEMSET(&interim_np, 0, sizeof interim_np);
    if (interim != 0) {
        dsd_squelch_floor_plan interim_fp;
        design_chain_plans(s, NULL, rate, taps, taps_len, &interim_fp, &interim_np);
    }
    static Runners r;
    r = Runners();
    Durations run = len;
    if (np.valid == 0) {
        run.noise = 0;
        run.meter = 0;
    }
    runners_init(&r, &fp, &np, plan != NULL ? plan->L : 1, run);
    static Meter meters[2];
    meter_init(&meters[0], &np, rate / 10);
    meter_init(&meters[1], &interim_np, rate / 10);
    const int meter_count = np.valid == 0 ? 0 : (interim_np.valid != 0 ? 2 : 1);
    const long outputs = std::max(std::max(run.floor + r.offset[kPhases - 1], run.noise), std::max(run.meter, run.cov));
    const double in_per_out =
        plan != NULL ? 2.0 * std::ldexp((double)plan->hb_out_num, -(int)plan->hb_out_shift) / (double)rate : 2.0;
    feed_noise(s, rng, 1.0, (long)((double)outputs * in_per_out) + 4096,
               [&](const float* iq, int pairs) { runners_add(&r, meters, meter_count, iq, pairs); });
    if (run.floor > 0) {
        floor_bounds(r, fp, name, &o);
    }
    const int stats_ok = window_stats_ok(r, fp, name, &o);
    o.noise_valid = np.valid;
    if (run.noise > 0 && run.meter > 0) {
        noise_bounds(r, meters, meter_count, name, &o);
    }
    printf(
        "  %-30s lag %d beta %+.4f N_eff %7.2f | measured N_eff %+5.2f %% coh %+.4f | floor %.2f dB known <= %llu %s",
        name, fp.lag, fp.beta, fp.neff, 100.0 * o.neff_off, o.beta_off, o.floor_worst_db,
        (unsigned long long)o.known_max, (o.floor_ok != 0 && stats_ok != 0) ? "ok" : "FAIL");
    if (run.noise > 0 && run.meter > 0) {
        printf(" | noise median %+.2f max %+.2f cal %.2f/%+.2f dB %s", o.median_db, o.top_db, o.cal_worst, o.cal_whole,
               o.noise_ok != 0 ? "ok" : "FAIL");
        if (meter_count > 1) {
            printf(" (half-band alone %.2f/%+.2f dB)", o.interim_worst, o.interim_whole);
        }
    } else if (np.valid == 0) {
        printf(" | no noise plan");
    }
    printf("\n");
    CHECK(o.floor_ok != 0 && o.noise_ok != 0 && stats_ok != 0, "%s", name);
    return o;
}

/* The widest NFM channel a DSP rate takes (0: none). */
int
widest_nfm_hz(int rate) {
    const int widest = dsd_analog_width_max_for_rate(rate);
    const int capped = widest < 25000 ? widest : 25000;
    return capped >= dsd_analog_width_min_hz(DSD_ANALOG_DEMOD_FM) ? capped : 0;
}

/* Relative closeness, against the larger magnitude (or 1e-300). */
bool
close_rel(double a, double b, double rel) {
    return std::fabs(a - b) <= rel * std::fmax(std::fmax(std::fabs(a), std::fabs(b)), 1e-300);
}

/* R_h(m) for real taps h of length n (0 beyond them). */
double
taps_autocorr(const float* h, int n, int m) {
    m = m < 0 ? -m : m;
    double sum = 0.0;
    for (int k = 0; k + m < n; k++) {
        sum += (double)h[k] * (double)h[k + m];
    }
    return sum;
}

/* With a period of one sample the periodic design is today's stationary model: the channel taps' noise autocorrelation
   behind a half-band stage (its own autocorrelation at even lags), every lag of its support, within 1e-12. */
void
test_period_one_is_todays_model(void) {
    const struct {
        int rate;
        int width;
        int hb; /* 0 none, 15 or 31 taps */
    } chains[] = {{24000, 12500, 15}, {48000, 25000, 31}, {12000, 0, 15}, {48000, 12500, 0}, {16000, 13200, 15}};

    for (const auto& c : chains) {
        std::vector<float> taps(DSD_NOISE_SQUELCH_MAX_TAPS, 0.0f);
        int len = 1;
        taps[0] = 1.0f;
        if (c.width > 0) {
            len = dsd_channel_lpf_design_analog(c.rate, c.width, taps.data(), DSD_NOISE_SQUELCH_MAX_TAPS);
        }
        const float* hb = c.hb == 31 ? hb31_q15_taps : (c.hb == 15 ? hb_q15_taps : NULL);
        const int hb_len = c.hb;
        const int hb_span = hb != NULL ? (hb_len - 1) / 2 : 0;
        const int lags = len + hb_span;
        std::vector<double> cov((size_t)lags, 0.0);
        for (int m = 0; m < lags; m++) {
            if (hb == NULL) {
                cov[(size_t)m] = taps_autocorr(taps.data(), len, m);
                continue;
            }
            for (int j = -hb_span; j <= hb_span; j++) {
                cov[(size_t)m] += taps_autocorr(taps.data(), len, m - j) * taps_autocorr(hb, hb_len, 2 * j);
            }
        }
        dsd_squelch_floor_periodic_noise noise;
        noise.period = 1;
        noise.lags = lags;
        noise.cov = cov.data();
        dsd_squelch_floor_plan periodic;
        dsd_squelch_floor_plan today;
        const float* plan_taps = c.width > 0 ? taps.data() : NULL;
        const int plan_len = c.width > 0 ? len : 0;
        const int rc_p = dsd_squelch_floor_plan_design_periodic(&periodic, plan_taps, plan_len, &noise, c.rate);
        const int rc_t = dsd_squelch_floor_plan_design(&today, plan_taps, plan_len, hb, hb_len, c.rate);
        const bool same = rc_p == 0 && rc_t == 0 && periodic.lag == today.lag && periodic.rate_hz == today.rate_hz
                          && close_rel(periodic.rho_lag, today.rho_lag, 1e-12)
                          && close_rel(periodic.beta, today.beta, 1e-12) && close_rel(periodic.neff, today.neff, 1e-12)
                          && close_rel(periodic.sqrt_neff, today.sqrt_neff, 1e-12)
                          && close_rel(periodic.noise_gain, today.noise_gain, 1e-12);
        CHECK(same, "%d Hz width %d hb %d: periodic lag %d rho %.15g beta %.15g neff %.15g vs %d %.15g %.15g %.15g",
              c.rate, c.width, c.hb, periodic.lag, periodic.rho_lag, periodic.beta, periodic.neff, today.lag,
              today.rho_lag, today.beta, today.neff);
    }
    printf("period one: the periodic design is today's stationary model within 1e-12\n");
}

/* The step plans the averaged floor path is held to: every planned Airspy grid point, and a SoapySDR ratio near the
   4096-term limit (hb_out 65,488 Hz, 2,095,616 Hz over 32, to 48 kHz: L/M 3000/4093, 4093 prime). */
std::vector<dsd_live_iq_step_plan>
floor_plan_grid(void) {
    const uint32_t devices[] = {2500000u, 3000000u, 10000000u, 6000000u};
    const int targets[] = {4000, 6000, 8000, 12000, 16000, 24000, 48000, 72000};
    std::vector<dsd_live_iq_step_plan> plans;
    for (const uint32_t device : devices) {
        for (const int target : targets) {
            plans.push_back(plan_for(DSD_RATE_PLAN_BACKEND_AIRSPY, device, (uint32_t)target));
        }
    }
    dsd_live_iq_step_plan soapy;
    DSD_MEMSET(&soapy, 0, sizeof soapy);
    soapy.enabled = 1;
    soapy.passes = 5;
    soapy.hb_out_num = 65488;
    soapy.L = 3000;
    soapy.M = 4093;
    soapy.target_hz = 48000;
    soapy.protect_hz = 21600;
    soapy.stop_hz = 26400;
    plans.push_back(soapy);
    return plans;
}

/* The averaged plan's distance from the per-phase one: N_eff and the noise gain relative, beta and rho absolute. */
double
floor_plan_distance(const dsd_squelch_floor_plan& a, const dsd_squelch_floor_plan& b) {
    const double rel = std::fmax(std::fabs(a.neff / b.neff - 1.0), std::fabs(a.noise_gain / b.noise_gain - 1.0));
    return std::fmax(rel, std::fmax(std::fabs(a.beta - b.beta), std::fabs(a.rho_lag - b.rho_lag)));
}

/* The floor plan's averaged path against every output phase in full, with the widest NFM channel and none. */
void
test_floor_plan_matches_every_phase(void) {
    double worst = 0.0;
    double worst_dev = 0.0;
    double worst_bound = 0.0;
    int compared = 0;
    for (const dsd_live_iq_step_plan& plan : floor_plan_grid()) {
        dsd_squelch_step_model m;
        dsd_squelch_step_model_init(&m);
        dsd_noise_squelch_stage st[DSD_NOISE_SQUELCH_MAX_STAGES];
        CHECK(dsd_squelch_step_model_build(&m, &plan, st, stages_for(plan.passes, st)) == 0, "build %d/%d", plan.L,
              plan.M);
        worst_dev = std::fmax(worst_dev, m.cyclo_dev / m.ry_mean[0]);
        const int widths[2] = {widest_nfm_hz(plan.target_hz), 0};
        for (int width : widths) {
            std::vector<float> taps(DSD_NOISE_SQUELCH_MAX_TAPS);
            const int len = width > 0 ? dsd_channel_lpf_design_analog(plan.target_hz, width, taps.data(),
                                                                      DSD_NOISE_SQUELCH_MAX_TAPS)
                                      : 0;
            const float* run = len > 0 ? taps.data() : NULL;
            dsd_squelch_floor_plan fast;
            dsd_squelch_floor_plan full;
            const int rc_f = dsd_squelch_step_model_floor_plan(&m, &fast, run, len, plan.target_hz);
            const int rc_x = dsd_squelch_step_model_floor_plan_exact(&m, &full, run, len, plan.target_hz);
            const double off = floor_plan_distance(fast, full);
            double abs_sum = 0.0;
            for (int i = 0; i < len; i++) {
                abs_sum += std::fabs((double)taps[(size_t)i]);
            }
            abs_sum = len > 0 ? abs_sum : 1.0;
            const double bound = m.cyclo_dev * abs_sum * abs_sum / full.noise_gain;
            worst_bound = std::fmax(worst_bound, bound);
            worst = std::fmax(worst, off);
            compared++;
            CHECK(rc_f == 0 && rc_x == 0 && fast.lag == full.lag && off <= 1e-7,
                  "L/M %d/%d to %d Hz, width %d: averaged and per-phase plans differ by %.3e", plan.L, plan.M,
                  plan.target_hz, width, off);
        }
        /* A step whose phases stray past the bound takes every output phase in full. */
        if (plan.L == 576 && plan.M == 625) {
            const double dev = m.cyclo_dev;
            m.cyclo_dev = m.ry_mean[0];
            dsd_squelch_floor_plan fallback;
            dsd_squelch_floor_plan full;
            (void)dsd_squelch_step_model_floor_plan(&m, &fallback, NULL, 0, plan.target_hz);
            (void)dsd_squelch_step_model_floor_plan_exact(&m, &full, NULL, 0, plan.target_hz);
            m.cyclo_dev = dev;
            CHECK(fallback.valid && floor_plan_distance(fallback, full) == 0.0, "the per-phase fallback did not run");
        }
        dsd_squelch_step_model_release(&m);
    }
    printf("floor plan: the averaged path matches every output phase in full within %.2e at %d plans (the step's "
           "per-phase distance at most %.2e of its power, the bound behind the channel taps %.2e)\n",
           worst, compared, worst_dev, worst_bound);
    CHECK(worst_bound < 1e-2, "a grid plan takes the per-phase path: bound %.3e", worst_bound);
}

/* No-step plans pinned to what b08406ebc designs (captured from its squelch_floor.c and nfm_noise_squelch.c), on taps
   the library holds as constants, so a change to today's designs fails here. Floor plans within 1e-12; the noise
   plans' references within 1e-9 (their calibration goes through libm, whose last bits differ by platform). */
void
test_no_step_floor_plans_pinned(void) {
    struct FloorPin {
        const float* taps;
        int len;
        const float* hb;
        int hb_len;
        int rate;
        int lag;
        double rho, beta, neff, gain;
    };

    const FloorPin floors[] = {
        {hb31_q15_taps, 31, hb_q15_taps, HB_TAPS, 24000, 2, 0.18824940506799365, 0.14851454364148514,
         449.97918440223742, 0.18265616731685597},
        {NULL, 0, hb31_q15_taps, 31, 48000, 1, 0.069640312669278351, 0.054728591643388086, 1876.9859891794351,
         0.46573801897466183},
        {hb_q15_taps, HB_TAPS, NULL, 0, 12000, 3, -0.043873321952547915, -0.034466323369962339, 260.22667775558,
         0.34779196977615356},
    };
    for (const FloorPin& f : floors) {
        dsd_squelch_floor_plan p;
        const int rc = dsd_squelch_floor_plan_design(&p, f.taps, f.len, f.hb, f.hb_len, f.rate);
        const bool same = rc == 0 && p.lag == f.lag && close_rel(p.rho_lag, f.rho, 1e-12)
                          && close_rel(p.beta, f.beta, 1e-12) && close_rel(p.neff, f.neff, 1e-12)
                          && close_rel(p.noise_gain, f.gain, 1e-12);
        CHECK(same, "floor plan at %d Hz moved from b08406ebc: lag %d rho %.17g beta %.17g neff %.17g gain %.17g",
              f.rate, p.lag, p.rho_lag, p.beta, p.neff, p.noise_gain);
    }
}

void
test_no_step_noise_plans_pinned(void) {
    struct NoisePin {
        const float* taps;
        int len;
        int passes;
        int rate;
        int sub_bands;
        double hi, edge, sum, first, last, cascade31;
    };

    const NoisePin noises[] = {
        {NULL, 0, 5, 48000, 15, 21600.0, 24000.0, 1.7235432287442722, 0.11121109459149793, 0.11059553746973597,
         0.65564140244914859},
        {hb31_q15_taps, 31, 1, 48000, 15, 9380.0, 10180.0, 0.40466541234672926, 0.032010452607711612,
         0.021798198853429498, 0.0},
    };
    for (const NoisePin& n : noises) {
        static dsd_noise_squelch_plan p;
        dsd_noise_squelch_stage st[DSD_NOISE_SQUELCH_MAX_STAGES];
        const int count = stages_for(n.passes, st);
        const int rc = dsd_noise_squelch_plan_design(&p, n.taps, n.len, st, count, n.rate);
        const bool same = rc == 0 && p.sub_bands == n.sub_bands && p.bands == (2 * n.sub_bands) - 1
                          && close_rel(p.hi_hz, n.hi, 1e-12) && close_rel(p.edge_hz, n.edge, 1e-12)
                          && close_rel(p.p_ref_sum, n.sum, 1e-9) && close_rel(p.p_ref[0], n.first, 1e-9)
                          && close_rel(p.p_ref[p.bands - 1], n.last, 1e-9)
                          && std::fabs(p.cascade[31] - n.cascade31) <= 1e-12;
        CHECK(same, "noise plan at %d Hz, %d passes moved from b08406ebc: hi %.17g sum %.17g first %.17g last %.17g",
              n.rate, n.passes, p.hi_hz, p.p_ref_sum, p.p_ref[0], p.p_ref[p.bands - 1]);
    }
    printf("no step: today's floor and noise plans match b08406ebc's\n");
}

/* The floor plan's window statistics measured on the real stepped stream: the covariance of 100,000 channel samples
   behind the widest NFM channel at 48 and 72 kHz from 2.5 MS/s and 24 kHz from 3 MS/s, where the correlation is long
   enough that a slip in N_eff's sum moves it by a quarter or more. */
void
test_window_statistics(void) {
    const struct {
        uint32_t device;
        int target;
        int width;
    } chains[] = {{2500000u, 48000, 25000}, {2500000u, 72000, 25000}, {3000000u, 24000, 20400}};

    const Durations len = {0, 0, 0, 100000};
    for (const auto& c : chains) {
        const dsd_live_iq_step_plan plan =
            stand_in(plan_for(DSD_RATE_PLAN_BACKEND_AIRSPY, c.device, (uint32_t)c.target));
        char name[96];
        DSD_SNPRINTF(name, sizeof name, "%u/%d Hz, width %d", c.device, c.target, c.width);
        (void)run_chain(name, &plan, c.target, c.width, 0, 0x57A7ULL + c.device + (uint64_t)c.target, len);
    }
}

/* Every selectable DSP bandwidth at the post-half-band rates of 2.5 and 3 MS/s (stand-ins: one 31-tap pass from twice
   hb_out), the widest NFM channel it takes and none: each chain's floor, noise squelch and window statistics held to
   the bounds DSP_SQUELCH_FLOOR and DSP_NFM_NOISE_SQUELCH hold the RTL-SDR chain to, the trackers at three phases of the
   step's period, and the RTL-SDR chain at the same bandwidth (one 31-tap pass from twice the rate) held to the same
   bounds for the same lengths. A stepped chain's noise plan is valid exactly where the RTL-SDR chain's is: the same band
   rule, so a stepped design refused for any other reason (the factor's length, the step's output, a step refusing a
   pass, no calibrated power) fails here. */
void
test_matrix(void) {
    const uint32_t devices[] = {2500000u, 3000000u};
    const int targets[] = {4000, 6000, 8000, 12000, 16000, 24000, 48000, 72000};
    int interim_misses = 0;
    for (const int target : targets) {
        const int widths[2] = {widest_nfm_hz(target), 0};
        /* Lengths in channel samples (Durations): 4 s of trackers, 3 s of noise squelch, 6 s of meters, 100,000
           samples of covariance. */
        const Durations len = {4L * target, 3L * target, 6L * target, 100000};
        for (int w = 0; w < 2; w++) {
            if (w == 0 && widths[0] == 0) {
                continue;
            }
            char name[96];
            DSD_SNPRINTF(name, sizeof name, "RTL %d Hz, width %d", target, widths[w]);
            const Outcome rtl =
                run_chain(name, NULL, target, widths[w], 0, 0x5EED0000ULL + (uint64_t)target + (uint64_t)w, len);
            for (const uint32_t device : devices) {
                const dsd_live_iq_step_plan plan =
                    stand_in(plan_for(DSD_RATE_PLAN_BACKEND_AIRSPY, device, (uint32_t)target));
                DSD_SNPRINTF(name, sizeof name, "%u/%d Hz, width %d", device, target, widths[w]);
                /* The half-band stage alone is measured where it misses: 24 kHz with no channel filter. There the
                   meters run 20 s: both plans' references are compared to the 0.6 dB bound, the half-band stage
                   alone's within a tenth of a dB of it. */
                const int interim = (target == 24000 && widths[w] == 0) ? 1 : 0;
                Durations chain_len = len;
                chain_len.meter = interim != 0 ? 20L * target : len.meter;
                const Outcome step = run_chain(name, &plan, target, widths[w], interim,
                                               0xA1E5ULL + device + (uint64_t)target + (uint64_t)w, chain_len);
                CHECK(step.noise_valid == rtl.noise_valid, "%s: noise plan valid %d, the RTL-SDR chain's %d", name,
                      step.noise_valid, rtl.noise_valid);
                interim_misses +=
                    (interim != 0 && (step.interim_worst >= 0.6 || std::fabs(step.interim_whole) >= 0.3)) ? 1 : 0;
            }
        }
    }
    printf("the half-band stage alone misses the noise references at %d of 2 stepped plans at 24 kHz unfiltered\n",
           interim_misses);
    /* What a stepped chain ran before the model: at 24 kHz with no channel filter its references are off by more than
       DSP_NFM_NOISE_SQUELCH allows (the matrix tells the two apart). */
    CHECK(interim_misses >= 1, "the half-band stage alone never missed: the matrix does not test the step");
}
} // namespace

int
main(int argc, char** argv) {
    const bool matrix = argc > 1 && std::strcmp(argv[1], "--matrix") == 0;
    if (!matrix) {
        test_halfband_recursion();
        test_step_covariance();
        test_no_step_plans_are_todays();
        test_plan_key_follows_the_step();
        test_window_statistics();
        test_period_one_is_todays_model();
        test_floor_plan_matches_every_phase();
        test_no_step_floor_plans_pinned();
        test_no_step_noise_plans_pinned();
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
