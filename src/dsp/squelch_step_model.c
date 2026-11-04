// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* The noise the dynamic squelches see through the live I/Q rate step (issue #806); see squelch_step_model.h. */

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/dsp/iq_resampler.h>
#include <dsd-neo/dsp/live_iq_step.h>
#include <dsd-neo/dsp/nfm_noise_squelch.h>
#include <dsd-neo/dsp/squelch_floor.h>
#include <dsd-neo/dsp/squelch_step_model.h>
#include <dsd-neo/runtime/mem.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* The spectral factor: R_hb's spectrum on this many points, and the share of the factor's energy its cut and window
   may leave out or change. 1e-6, under the 1e-4 the plan allows: the factor's autocorrelation then stays within about
   0.1 % of R_hb[0] at every lag (1e-4 left 1.1 %, 19 taps), for about 60 taps. */
enum { SSM_DFT_POINTS = 4096, SSM_FACTOR_MAX_HALF = (DSD_SQUELCH_STEP_MAX_FACTOR_TAPS - 1) / 2 };

static const double k_factor_residual = 1e-6;
/* Kaiser's beta for a 40 dB window: 0.5842 (40 - 21)^0.4 + 0.07886 (40 - 21). */
static const double k_factor_kaiser_beta = 3.3952;
/* The most doubles the floor model's covariance table may take (64 MB): a ratio past any device's would need more. */
static const size_t k_max_cov_doubles = (size_t)8 << 20;
/* The floor plan takes the phase-averaged covariance while the bound on how far any output phase's covariance strays
   from it after the channel taps stays under this (relative): the per-phase results then average to its results within
   about the square of it (1e-4). The bound reaches 1.1e-3 on the Airspy grid (72 kHz, NFM 25k), where the two plans
   differ by under 2e-8; a step past it takes every output phase in full. */
static const double k_cyclo_negligible = 1e-2;

void
dsd_squelch_step_model_init(dsd_squelch_step_model* m) {
    if (!m) {
        return;
    }
    DSD_MEMSET(m, 0, sizeof(*m));
    dsd_iq_resampler_init(&m->step);
}

void
dsd_squelch_step_model_release(dsd_squelch_step_model* m) {
    if (!m) {
        return;
    }
    dsd_iq_resampler_free(&m->step);
    dsd_neo_aligned_free(m->resp);
    dsd_neo_aligned_free(m->cov);
    dsd_neo_aligned_free(m->chan);
    dsd_neo_aligned_free(m->ry_mean);
    dsd_squelch_step_model_init(m);
}

int
dsd_squelch_step_model_matches(const dsd_squelch_step_model* m, const dsd_live_iq_step_plan* plan) {
    return m && plan && m->ready && dsd_live_iq_step_plan_equal(&m->plan, plan);
}

/* ------------------------------------------------------------------------------------------- the cascade's noise */

/* R_hb: white complex noise of variance 2^(count - 1) (1 with no stage) at the capture rate through every stage. A
   stage maps R to R'[k] = sum_j g[j] R[2k - j], g its taps' autocorrelation; the support after it is
   (support + taps - 1) / 2 lags. */
static int
ssm_halfband_recursion(dsd_squelch_step_model* m, const dsd_noise_squelch_stage* stages, int count) {
    double r[DSD_SQUELCH_STEP_MAX_HB_LAGS];
    double next[DSD_SQUELCH_STEP_MAX_HB_LAGS];
    double g[64];
    DSD_MEMSET(r, 0, sizeof(r));
    r[0] = count > 0 ? ldexp(1.0, count - 1) : 1.0;
    int support = 0;
    for (int i = 0; i < count; i++) {
        const int len = stages[i].len;
        for (int j = 0; j < len; j++) {
            double sum = 0.0;
            for (int t = 0; t + j < len; t++) {
                sum += (double)stages[i].taps[t] * (double)stages[i].taps[t + j];
            }
            g[j] = sum;
        }
        const int out_support = (support + len - 1) / 2;
        if (out_support >= DSD_SQUELCH_STEP_MAX_HB_LAGS) {
            return -1;
        }
        for (int k = 0; k <= out_support; k++) {
            double sum = 0.0;
            for (int j = -(len - 1); j <= len - 1; j++) {
                const int at = abs(2 * k - j);
                if (at <= support) {
                    sum += g[abs(j)] * r[at];
                }
            }
            next[k] = sum;
        }
        support = out_support;
        DSD_MEMCPY(r, next, (size_t)(support + 1) * sizeof(double));
    }
    DSD_MEMSET(m->r_hb, 0, sizeof(m->r_hb));
    DSD_MEMCPY(m->r_hb, r, (size_t)(support + 1) * sizeof(double));
    m->hb_lags = support + 1;
    return 0;
}

/* I0(x), by its series. */
static double
ssm_bessel_i0(double x) {
    const double q = 0.25 * x * x;
    double term = 1.0;
    double sum = 1.0;
    for (int k = 1; k < 100; k++) {
        term *= q / ((double)k * (double)k);
        sum += term;
        if (term < 1e-17 * sum) {
            break;
        }
    }
    return sum;
}

/* The Kaiser window's weight at offset n of a window of 2 half + 1 taps. */
static double
ssm_kaiser(int n, int half) {
    if (half == 0) {
        return 1.0;
    }
    const double u = (double)n / (double)half;
    const double inside = 1.0 - u * u;
    return ssm_bessel_i0(k_factor_kaiser_beta * sqrt(inside > 0.0 ? inside : 0.0))
           / ssm_bessel_i0(k_factor_kaiser_beta);
}

/* The zero-phase spectral factor of R_hb, taps 0..SSM_FACTOR_MAX_HALF of it (it is even) into @p a, from
   sqrt(DFT(R_hb)) on SSM_DFT_POINTS points; its whole energy into @p energy (Parseval). */
static void
ssm_factor_taps(const dsd_squelch_step_model* m, double* a, double* energy) {
    enum { HALF_POINTS = SSM_DFT_POINTS / 2 };

    double cosine[SSM_DFT_POINTS];
    for (int i = 0; i < SSM_DFT_POINTS; i++) {
        cosine[i] = cos(2.0 * M_PI * (double)i / (double)SSM_DFT_POINTS);
    }
    double amp[HALF_POINTS + 1];
    double e = 0.0;
    for (int k = 0; k <= HALF_POINTS; k++) {
        double s = m->r_hb[0];
        for (int lag = 1; lag < m->hb_lags; lag++) {
            s += 2.0 * m->r_hb[lag] * cosine[(k * lag) % SSM_DFT_POINTS];
        }
        /* The truncated recursion is exact: a spectrum a hair under zero (rounding in a stopband null) is zero. */
        amp[k] = sqrt(s > 0.0 ? s : 0.0);
        e += (k == 0 || k == HALF_POINTS ? 1.0 : 2.0) * amp[k] * amp[k];
    }
    *energy = e / (double)SSM_DFT_POINTS;
    for (int n = 0; n <= SSM_FACTOR_MAX_HALF; n++) {
        double sum = amp[0] + (((n & 1) != 0) ? -amp[HALF_POINTS] : amp[HALF_POINTS]);
        for (int k = 1; k < HALF_POINTS; k++) {
            sum += 2.0 * amp[k] * cosine[(k * n) % SSM_DFT_POINTS];
        }
        a[n] = sum / (double)SSM_DFT_POINTS;
    }
}

/* The factor's energy that a cut to 2 half + 1 Kaiser-windowed taps leaves out or changes. */
static double
ssm_factor_residual(const double* a, int half, double energy) {
    double kept = 0.0;
    double changed = 0.0;
    for (int n = -half; n <= half; n++) {
        const double tap = a[abs(n)];
        const double off = tap * (1.0 - ssm_kaiser(n, half));
        kept += tap * tap;
        changed += off * off;
    }
    return (energy - kept) + changed;
}

/* The spectral factor: the shortest cut whose residual is under k_factor_residual of the energy, windowed and scaled
   so its power is R_hb[0]. */
static int
ssm_spectral_factor(dsd_squelch_step_model* m) {
    double a[SSM_FACTOR_MAX_HALF + 1];
    double energy = 0.0;
    ssm_factor_taps(m, a, &energy);
    if (!(energy > 0.0)) {
        return -1;
    }
    int half = -1;
    for (int h = 0; h <= SSM_FACTOR_MAX_HALF; h++) {
        if (ssm_factor_residual(a, h, energy) < k_factor_residual * energy) {
            half = h;
            break;
        }
    }
    if (half < 0) {
        return -1;
    }
    double power = 0.0;
    for (int n = -half; n <= half; n++) {
        const double tap = a[abs(n)] * ssm_kaiser(n, half);
        m->factor[n + half] = tap;
        power += tap * tap;
    }
    const double scale = sqrt(m->r_hb[0] / power);
    for (int i = 0; i < 2 * half + 1; i++) {
        m->factor[i] *= scale;
    }
    m->factor_len = 2 * half + 1;
    return 0;
}

/* -------------------------------------------------------------------------------------- the step's responses */

static int
ssm_gcd(int a, int b) {
    while (b != 0) {
        const int t = a % b;
        a = b;
        b = t;
    }
    return a;
}

/* The first output of the response to the factor started at input k x spacing: an input j reaches outputs n with
   j <= n M / L < j + taps per phase, so the factor's response lies in [floor(k spacing L / M), that + resp_len). */
static int64_t
ssm_resp_origin(const dsd_squelch_step_model* m, int k) {
    return ((int64_t)k * (int64_t)m->spacing * (int64_t)m->plan.L) / (int64_t)m->plan.M;
}

/* The response windows: resp_len outputs, past the factor and the step's taps; spacing inputs apart, coprime to M so
   the M starts take every input phase once, and far enough apart that no two windows meet. */
static void
ssm_response_layout(dsd_squelch_step_model* m) {
    const int64_t L = m->plan.L;
    const int64_t M = m->plan.M;
    const int64_t span = (int64_t)m->factor_len + (int64_t)m->step.taps_per_phase;
    m->resp_len = (int)(((span * L) + M - 1) / M) + 1;
    int spacing = (int)((((int64_t)m->resp_len * M) + L - 1) / L);
    while (ssm_gcd(spacing, (int)M) != 1) {
        spacing++;
    }
    m->spacing = spacing;
}

/* Run the factor started at every input phase through the step, each response into its window of resp. Every output
   outside the windows must be exactly zero: one that is not means the windows missed a response. */
static int
ssm_record_responses(dsd_squelch_step_model* m) {
    const int M = m->plan.M;
    const int64_t total_in = ((int64_t)M * (int64_t)m->spacing) + m->factor_len + m->step.taps_per_phase;
    float in[2 * DSD_LIVE_IQ_STEP_PASS_PAIRS];
    float out[2 * DSD_LIVE_IQ_STEP_PASS_PAIRS];
    dsd_iq_resampler_clear(&m->step);
    int64_t j = 0;
    int64_t n = 0;
    int k = 0;
    while (j < total_in) {
        int count = 0;
        for (; count < DSD_LIVE_IQ_STEP_PASS_PAIRS && j < total_in; count++, j++) {
            const int64_t start = j / m->spacing;
            const int64_t at = j - (start * m->spacing);
            in[(size_t)count * 2U] = (start < M && at < m->factor_len) ? (float)m->factor[at] : 0.0f;
            in[((size_t)count * 2U) + 1U] = 0.0f;
        }
        const int made = dsd_iq_resampler_process(&m->step, in, (size_t)count, out, DSD_LIVE_IQ_STEP_PASS_PAIRS);
        if (made < 0) {
            return -1;
        }
        for (int i = 0; i < made; i++, n++) {
            while (k < M && n >= ssm_resp_origin(m, k) + m->resp_len) {
                k++;
            }
            const int64_t origin = k < M ? ssm_resp_origin(m, k) : INT64_MAX;
            const float y = out[(size_t)i * 2U];
            /* Outside every window the step saw only zeros, so its output is exactly zero: anything else, however
               small, is a response the windows missed, and the compare is exact on purpose. */
            if (n >= origin) {
                m->resp[((size_t)k * (size_t)m->resp_len) + (size_t)(n - origin)] = y;
            } else if (y != 0.0f) {
                return -1;
            }
        }
    }
    dsd_iq_resampler_clear(&m->step);
    return 0;
}

/* Response @p k through the @p len channel taps into chan: its @p lags outputs (resp_len + len - 1). */
static void
ssm_response_through_taps(const dsd_squelch_step_model* m, int k, const float* taps, int len, int lags) {
    const float* y = m->resp + ((size_t)k * (size_t)m->resp_len);
    for (int t = 0; t < lags; t++) {
        double sum = 0.0;
        const int lo = t - (m->resp_len - 1) > 0 ? t - (m->resp_len - 1) : 0;
        const int hi = t < len - 1 ? t : len - 1;
        for (int i = lo; i <= hi; i++) {
            sum += (double)taps[i] * (double)y[t - i];
        }
        m->chan[t] = sum;
    }
}

/* The exact covariance of every output phase behind the @p len channel taps into cov (L rows of @p lags): every input
   phase's response adds its products to the output phases it covers, so summed over the M starts (every noise input up
   to a whole number of periods) each row holds E{z_n z_{n+m}} for its output phase n. */
static void
ssm_accumulate_cov(dsd_squelch_step_model* m, const float* taps, int len, int lags) {
    const int L = m->plan.L;
    DSD_MEMSET(m->cov, 0, (size_t)L * (size_t)lags * sizeof(double));
    for (int k = 0; k < m->plan.M; k++) {
        ssm_response_through_taps(m, k, taps, len, lags);
        const int base = (int)(ssm_resp_origin(m, k) % L);
        for (int t = 0; t < lags; t++) {
            const double zt = m->chan[t];
            double* row = m->cov + ((size_t)((base + t) % L) * (size_t)lags);
            for (int lag = 0; lag + t < lags; lag++) {
                row[lag] += zt * m->chan[t + lag];
            }
        }
    }
}

/* The step's own output covariance (no channel taps), averaged over the period into ry_mean, and the largest distance
   of any output phase's from it into cyclo_dev: what the floor plan's channel finish works from. */
static void
ssm_pre_channel_stats(dsd_squelch_step_model* m) {
    static const float k_unit_tap[1] = {1.0f};
    const int L = m->plan.L;
    const int lags = m->resp_len;
    ssm_accumulate_cov(m, k_unit_tap, 1, lags);
    for (int lag = 0; lag < lags; lag++) {
        double sum = 0.0;
        for (int n = 0; n < L; n++) {
            sum += m->cov[((size_t)n * (size_t)lags) + (size_t)lag];
        }
        m->ry_mean[lag] = sum / (double)L;
    }
    /* R(n, n - m) is R(n - m, n): the rows hold both directions. */
    double dev = 0.0;
    for (int n = 0; n < L; n++) {
        for (int lag = 0; lag < lags; lag++) {
            dev = fmax(dev, fabs(m->cov[((size_t)n * (size_t)lags) + (size_t)lag] - m->ry_mean[lag]));
        }
    }
    m->cyclo_dev = dev;
}

/* The response store and the floor model's scratch. Every count is checked, by division so no product wraps, and
   every size formed before the first allocation: none is read back from the model between the calls. */
static int
ssm_allocate(dsd_squelch_step_model* m) {
    const int L = m->plan.L;
    const int M = m->plan.M;
    const int resp_len = m->resp_len;
    if (L < 1 || M < 1 || resp_len < 1 || (size_t)resp_len > k_max_cov_doubles / (size_t)M) {
        return -1;
    }
    const int cov_lags = resp_len + DSD_NOISE_SQUELCH_MAX_TAPS - 1;
    if ((size_t)cov_lags > k_max_cov_doubles / (size_t)L) {
        return -1;
    }
    const size_t resp_bytes = (size_t)M * (size_t)resp_len * sizeof(float);
    const size_t cov_bytes = (size_t)L * (size_t)cov_lags * sizeof(double);
    const size_t chan_bytes = (size_t)cov_lags * sizeof(double);
    const size_t ry_mean_bytes = (size_t)resp_len * sizeof(double);
    m->cov_lags = cov_lags;
    m->resp = (float*)dsd_neo_aligned_malloc(resp_bytes);
    m->cov = (double*)dsd_neo_aligned_malloc(cov_bytes);
    m->chan = (double*)dsd_neo_aligned_malloc(chan_bytes);
    m->ry_mean = (double*)dsd_neo_aligned_malloc(ry_mean_bytes);
    if (!m->resp || !m->cov || !m->chan || !m->ry_mean) {
        return -1;
    }
    DSD_MEMSET(m->resp, 0, resp_bytes);
    return 0;
}

static int
ssm_build_args_ok(const dsd_live_iq_step_plan* plan, const dsd_noise_squelch_stage* stages, int stage_count) {
    if (!plan || !plan->enabled || plan->L < 1 || plan->M < plan->L || stage_count != plan->passes || stage_count < 0
        || stage_count > DSD_NOISE_SQUELCH_MAX_STAGES || (stage_count > 0 && !stages)) {
        return 0;
    }
    for (int i = 0; i < stage_count; i++) {
        if (!stages[i].taps || stages[i].len < 1 || stages[i].len > 64) {
            return 0;
        }
    }
    return 1;
}

int
dsd_squelch_step_model_build(dsd_squelch_step_model* m, const dsd_live_iq_step_plan* plan,
                             const dsd_noise_squelch_stage* stages, int stage_count) {
    if (!m) {
        return -1;
    }
    if (dsd_squelch_step_model_matches(m, plan)) {
        return 0;
    }
    dsd_squelch_step_model_release(m);
    if (!ssm_build_args_ok(plan, stages, stage_count) || ssm_halfband_recursion(m, stages, stage_count) != 0
        || ssm_spectral_factor(m) != 0) {
        dsd_squelch_step_model_release(m);
        return -1;
    }
    m->plan = *plan;
    const int rc = dsd_iq_resampler_configure_ratio(&m->step, plan->L, plan->M, plan->hb_out_num, plan->hb_out_shift,
                                                    plan->protect_hz, plan->stop_hz, DSD_LIVE_IQ_STEP_PASS_PAIRS);
    if (rc != DSD_IQ_RESAMPLER_OK) {
        dsd_squelch_step_model_release(m);
        return -1;
    }
    ssm_response_layout(m);
    if (ssm_allocate(m) != 0 || ssm_record_responses(m) != 0) {
        dsd_squelch_step_model_release(m);
        return -1;
    }
    ssm_pre_channel_stats(m);
    m->ready = 1;
    return 0;
}

/* --------------------------------------------------------------------------------------------- the plans */

/* The arguments a floor plan takes: @p m ready for the rate, and channel taps (NULL or 0: one unit tap) it can hold. */
static int
ssm_floor_args(const dsd_squelch_step_model* m, const float** taps, int* taps_len, int rate_hz) {
    static const float k_unit_tap[1] = {1.0f};
    if (!m || !m->ready || *taps_len > DSD_NOISE_SQUELCH_MAX_TAPS || rate_hz != m->plan.target_hz) {
        return 0;
    }
    if (!*taps || *taps_len <= 0) {
        *taps = k_unit_tap;
        *taps_len = 1;
    }
    return 1;
}

int
dsd_squelch_step_model_floor_plan_exact(dsd_squelch_step_model* m, dsd_squelch_floor_plan* out, const float* taps,
                                        int taps_len, int rate_hz) {
    if (!out) {
        return -1;
    }
    DSD_MEMSET(out, 0, sizeof(*out));
    const float* run_taps = taps;
    int run_len = taps_len;
    if (!ssm_floor_args(m, &run_taps, &run_len, rate_hz)) {
        return -1;
    }
    const int lags = m->resp_len + run_len - 1;
    ssm_accumulate_cov(m, run_taps, run_len, lags);
    dsd_squelch_floor_periodic_noise noise;
    noise.period = m->plan.L;
    noise.lags = lags;
    noise.cov = m->cov;
    return dsd_squelch_floor_plan_design_periodic(out, taps, taps_len, &noise, rate_hz);
}

/* The phase-averaged covariance behind the channel taps into chan (@p lags of it), exact because the channel taps are
   time-invariant: R(m) = sum_d R_c(d) R_y(m - d), R_c the taps' autocorrelation and R_y the step's averaged covariance.
   Returns the bound on how far any output phase's covariance can be from it, relative to its power: the step's largest
   per-phase distance times (sum |c|)^2, over R(0). */
static double
ssm_mean_cov_through_taps(dsd_squelch_step_model* m, const float* taps, int len, int lags) {
    double abs_sum = 0.0;
    for (int d = 0; d < len; d++) {
        double r = 0.0;
        for (int i = 0; i + d < len; i++) {
            r += (double)taps[i] * (double)taps[i + d];
        }
        m->rc[d] = r;
        abs_sum += fabs((double)taps[d]);
    }
    const int ry_lags = m->resp_len;
    for (int lag = 0; lag < lags; lag++) {
        double sum = 0.0;
        const int lo = lag - (len - 1);
        const int hi = lag + (len - 1);
        for (int k = lo; k <= hi; k++) {
            const int at = abs(k);
            if (at < ry_lags) {
                sum += m->ry_mean[at] * m->rc[abs(lag - k)];
            }
        }
        m->chan[lag] = sum;
    }
    return m->chan[0] > 0.0 ? m->cyclo_dev * abs_sum * abs_sum / m->chan[0] : INFINITY;
}

int
dsd_squelch_step_model_floor_plan(dsd_squelch_step_model* m, dsd_squelch_floor_plan* out, const float* taps,
                                  int taps_len, int rate_hz) {
    if (!out) {
        return -1;
    }
    DSD_MEMSET(out, 0, sizeof(*out));
    const float* run_taps = taps;
    int run_len = taps_len;
    if (!ssm_floor_args(m, &run_taps, &run_len, rate_hz)) {
        return -1;
    }
    const int lags = m->resp_len + run_len - 1;
    if (ssm_mean_cov_through_taps(m, run_taps, run_len, lags) > k_cyclo_negligible) {
        return dsd_squelch_step_model_floor_plan_exact(m, out, taps, taps_len, rate_hz);
    }
    dsd_squelch_floor_periodic_noise noise;
    noise.period = 1;
    noise.lags = lags;
    noise.cov = m->chan;
    return dsd_squelch_floor_plan_design_periodic(out, taps, taps_len, &noise, rate_hz);
}

int
dsd_squelch_step_model_noise_plan(dsd_squelch_step_model* m, dsd_noise_squelch_plan* out, const float* taps,
                                  int taps_len, int rate_hz) {
    if (!out) {
        return -1;
    }
    if (!m || !m->ready || rate_hz != m->plan.target_hz) {
        DSD_MEMSET(out, 0, sizeof(*out));
        return -1;
    }
    return dsd_noise_squelch_plan_design_stepped(out, taps, taps_len, m->factor, m->factor_len, &m->step, rate_hz);
}
