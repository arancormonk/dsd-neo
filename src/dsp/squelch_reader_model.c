// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* The noise the dynamic squelches see through a replay or network I/Q reader's resampler (issue #899); see
   squelch_reader_model.h. */

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/dsp/iq_resampler.h>
#include <dsd-neo/dsp/nfm_noise_squelch.h>
#include <dsd-neo/dsp/squelch_floor.h>
#include <dsd-neo/dsp/squelch_reader_model.h>
#include <dsd-neo/runtime/mem.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include "squelch_cascade_noise.h"

/* The most doubles any one table may take (64 MB): the reader's output covariance, the cascade's workspace, the
   responses, and the exact plan's covariance. 8,046 Hz read into a 72 kHz chain (L = 4000) takes at most about 2.9 Mi
   (the cascade's workspace). */
static const size_t k_max_table_doubles = (size_t)8 << 20;

void
dsd_squelch_reader_model_init(dsd_squelch_reader_model* m) {
    if (!m) {
        return;
    }
    DSD_MEMSET(m, 0, sizeof(*m));
    dsd_iq_resampler_init(&m->reader);
}

/* The every-phase plan's tables: freed, and that plan not available. */
static void
srm_release_exact(dsd_squelch_reader_model* m) {
    dsd_neo_aligned_free(m->resp_phase);
    dsd_neo_aligned_free(m->resp);
    dsd_neo_aligned_free(m->cov);
    dsd_neo_aligned_free(m->chan);
    m->resp_phase = NULL;
    m->resp = NULL;
    m->cov = NULL;
    m->chan = NULL;
    m->exact_ready = 0;
}

void
dsd_squelch_reader_model_release(dsd_squelch_reader_model* m) {
    if (!m) {
        return;
    }
    dsd_iq_resampler_free(&m->reader);
    dsd_neo_aligned_free(m->mean);
    dsd_neo_aligned_free(m->mean_chan);
    srm_release_exact(m);
    dsd_squelch_reader_model_init(m);
}

int
dsd_squelch_reader_model_matches(const dsd_squelch_reader_model* m, const dsd_iq_reader_plan* plan, int passes) {
    return m && plan && m->ready && plan->enabled && m->passes == passes && dsd_iq_reader_plan_equal(&m->plan, plan);
}

/* ------------------------------------------------------------------------------------------------- integer helpers */

static int
srm_gcd(int a, int b) {
    while (b != 0) {
        const int t = a % b;
        a = b;
        b = t;
    }
    return a;
}

/* floor(a / b) and the matching non-negative a mod b, for b > 0 and any a. */
static int64_t
srm_floor_div(int64_t a, int64_t b) {
    const int64_t q = a / b;
    return (a % b != 0 && a < 0) ? q - 1 : q;
}

static int64_t
srm_mod(int64_t a, int64_t b) {
    return a - (srm_floor_div(a, b) * b);
}

/* count x each doubles, or 0 past the table limit. */
static size_t
srm_table(int64_t count, int64_t each) {
    if (count < 1 || each < 1 || (uint64_t)each > (uint64_t)k_max_table_doubles / (uint64_t)count) {
        return 0U;
    }
    return (size_t)count * (size_t)each;
}

static double*
srm_doubles(size_t n) {
    double* p = (double*)dsd_neo_aligned_malloc(n * sizeof(double));
    if (p) {
        DSD_MEMSET(p, 0, n * sizeof(double));
    }
    return p;
}

/* ---------------------------------------------------------------------------------------------- the reader's bank */

/* Tap n of the reader's prototype (L x taps per phase long, at L times the input rate): the bank holds phase p's taps
   p, p + L, ... in reverse. */
static double
srm_tap(const dsd_iq_resampler* r, int64_t n) {
    const int64_t L = r->L;
    const int64_t T = r->taps_per_phase;
    return (double)r->bank[((n % L) * T) + (T - 1 - (n / L))];
}

/* The reader's output k (counted from its clear) is the prototype at L-times index k M against input i: it holds
   h[k M - i L] x_i, its phase p = k M mod L's taps h[p + j L] against inputs floor(k M / L) - j. Outputs k and k + m
   share the inputs where their tap indices differ by d = floor((k + m) M / L) - floor(k M / L) phases' worth. */
static double
srm_reader_cov(const dsd_iq_resampler* r, int64_t k, int64_t m) {
    const int64_t L = r->L;
    const int64_t M = r->M;
    const int T = r->taps_per_phase;
    const int64_t a = k * M;
    const int64_t b = (k + m) * M;
    const int64_t d = (b / L) - (a / L);
    /* Phase p's tap j is bank[p T + T - 1 - j]. */
    const float* ca = r->bank + ((a % L) * T) + (T - 1);
    const float* cb = r->bank + ((b % L) * T) + (T - 1);
    double sum = 0.0;
    for (int64_t j = 0; j + d < T; j++) {
        sum += (double)ca[-j] * (double)cb[-(j + d)];
    }
    return sum;
}

/* ------------------------------------------------------------------------------------ the covariance and its mean */

/* The decimators' alignment (src/dsp/simd_fir.cpp): stage s's output n is centred on its input 2n, the cascade
   module's convention (src/dsp/squelch_cascade_noise.h) the sum at input 2n, so stage s reads its input c_s = (len_s -
   1) / 2 later. Carried to the reader's output that is a shift of sum_s c_s 2^s. */
static int64_t
srm_stage_shift(const dsd_noise_squelch_stage* stages, int count) {
    int64_t shift = 0;
    for (int s = count - 1; s >= 0; s--) {
        shift = (2 * shift) + ((stages[s].len - 1) / 2);
    }
    return shift;
}

/* The reader's output covariance at every one of its L phases into @p in (L rows of @p lags: until no input is shared),
   row j that of the reader's output j + @p shift, scaled to the input variance @p var. */
static void
srm_reader_cov_rows(const dsd_iq_resampler* r, int64_t shift, double var, int64_t lags, double* in) {
    for (int64_t j = 0; j < r->L; j++) {
        for (int64_t lag = 0; lag < lags; lag++) {
            in[(j * lags) + lag] = var * srm_reader_cov(r, j + shift, lag);
        }
    }
}

/* The covariance @p out averaged over its period, and the largest distance of any phase's from it, into @p m. */
static int
srm_mean_and_distance(dsd_squelch_reader_model* m, const dsd_squelch_floor_periodic_noise* out) {
    m->period = out->period;
    m->lags = out->lags;
    m->mean = srm_doubles((size_t)out->lags);
    m->mean_chan = srm_doubles((size_t)out->lags + DSD_NOISE_SQUELCH_MAX_TAPS - 1U);
    if (!m->mean || !m->mean_chan) {
        return -1;
    }
    const size_t lags = (size_t)out->lags;
    for (size_t lag = 0; lag < lags; lag++) {
        double sum = 0.0;
        for (int n = 0; n < out->period; n++) {
            sum += out->cov[((size_t)n * lags) + lag];
        }
        m->mean[lag] = sum / (double)out->period;
    }
    double dev = 0.0;
    for (int n = 0; n < out->period; n++) {
        for (size_t lag = 0; lag < lags; lag++) {
            dev = fmax(dev, fabs(out->cov[((size_t)n * lags) + lag] - m->mean[lag]));
        }
    }
    m->cyclo_dev = dev;
    return 0;
}

/* The reader's output covariance, lined up on the decimators and scaled to the input variance 2^(passes - 1), through
   the cascade phase by phase; the mean over the period it leaves and the largest distance from it into @p m. */
static int
srm_cascade_stats(dsd_squelch_reader_model* m, const dsd_noise_squelch_stage* stages, int count) {
    const dsd_iq_resampler* r = &m->reader;
    const int64_t lags = ((((int64_t)r->taps_per_phase * r->L) + r->M - 1) / r->M) + 1;
    const size_t in_len = srm_table(r->L, lags);
    const size_t work_len = in_len ? dsd_squelch_cascade_noise_work_len(r->L, (int)lags, stages, count) : 0U;
    if (in_len == 0U || work_len == 0U || work_len > 2U * k_max_table_doubles) {
        return -1;
    }
    double* in = srm_doubles(in_len);
    double* work = srm_doubles(work_len);
    int rc = -1;
    if (in && work) {
        srm_reader_cov_rows(r, srm_stage_shift(stages, count), ldexp(1.0, count > 0 ? count - 1 : 0), lags, in);
        const dsd_squelch_floor_periodic_noise y = {r->L, (int)lags, in};
        dsd_squelch_floor_periodic_noise out;
        if (dsd_squelch_cascade_noise(&y, stages, count, work, work_len, &out) == 0) {
            rc = srm_mean_and_distance(m, &out);
        }
    }
    dsd_neo_aligned_free(in);
    dsd_neo_aligned_free(work);
    return rc;
}

/* ----------------------------------------------------------------------------------------------- the responses */

/* An input's response over outputs [origin, origin + len) of one stage, in @p y. */
typedef struct {
    int64_t origin;
    int len;
} srm_span;

/* The reader's response to input @p i: outputs k with 0 <= k M - i L < L x taps per phase. */
static srm_span
srm_reader_response(const dsd_iq_resampler* r, int64_t i, double* y) {
    const int64_t L = r->L;
    const int64_t M = r->M;
    const int64_t n = L * (int64_t)r->taps_per_phase;
    srm_span s;
    s.origin = -srm_floor_div(-(i * L), M);
    const int64_t last = srm_floor_div((i * L) + n - 1, M);
    s.len = (int)(last - s.origin + 1);
    for (int k = 0; k < s.len; k++) {
        y[k] = srm_tap(r, ((s.origin + k) * M) - (i * L));
    }
    return s;
}

/* @p in (over @p s) through a half-band stage as the decimator runs it, output q = sum_t b_t x_{2q - c + t}, into
   @p out. */
static srm_span
srm_stage_response(const dsd_noise_squelch_stage* st, srm_span s, const double* in, double* out) {
    const int64_t c = (st->len - 1) / 2;
    srm_span o;
    o.origin = -srm_floor_div(-(s.origin - c), 2);
    const int64_t last = srm_floor_div(s.origin + s.len - 1 + c, 2);
    o.len = (int)(last - o.origin + 1);
    for (int q = 0; q < o.len; q++) {
        double sum = 0.0;
        for (int t = 0; t < st->len; t++) {
            const int64_t at = (2 * (o.origin + q)) - c + t - s.origin;
            if (at >= 0 && at < s.len) {
                sum += (double)st->taps[t] * in[at];
            }
        }
        out[q] = sum;
    }
    return o;
}

/* The longest span any response takes, at the reader and after each stage: a bound on every one. */
static int
srm_response_bound(const dsd_iq_resampler* r, const dsd_noise_squelch_stage* stages, int count) {
    int64_t len = ((((int64_t)r->taps_per_phase * r->L) + r->M - 1) / r->M) + 1;
    int64_t most = len;
    for (int s = 0; s < count; s++) {
        len = ((len + stages[s].len) / 2) + 1;
        most = len > most ? len : most;
    }
    return most < INT32_MAX ? (int)most : -1;
}

/* The responses' store and the exact plan's scratch for @p inputs input phases of responses @p bound long. Every size
   is checked against the table limit before the first allocation. */
static int
srm_allocate(dsd_squelch_reader_model* m, int64_t inputs, int bound) {
    m->inputs = (int)inputs;
    m->resp_len = bound;
    m->cov_lags = bound + DSD_NOISE_SQUELCH_MAX_TAPS - 1;
    const size_t resp_n = srm_table(inputs, bound);
    const size_t chan_n = srm_table(inputs, m->cov_lags);
    const size_t cov_n = srm_table(m->period, m->cov_lags);
    if (inputs > INT32_MAX || resp_n == 0U || chan_n == 0U || cov_n == 0U) {
        return -1;
    }
    m->resp_phase = (int*)dsd_neo_aligned_malloc((size_t)inputs * sizeof(int));
    m->resp = srm_doubles(resp_n);
    m->chan = srm_doubles(chan_n);
    m->cov = srm_doubles(cov_n);
    return (m->resp_phase && m->resp && m->chan && m->cov) ? 0 : -1;
}

/* Input @p i's response through the reader and every stage, alternating between @p a and @p b (resp_len each), into its
   row of resp and the phase of its first output into resp_phase. */
static void
srm_record_response(dsd_squelch_reader_model* m, const dsd_noise_squelch_stage* stages, int count, int64_t i, double* a,
                    double* b) {
    srm_span s = srm_reader_response(&m->reader, i, a);
    double* cur = a;
    double* next = b;
    for (int st = 0; st < count; st++) {
        s = srm_stage_response(&stages[st], s, cur, next);
        double* t = cur;
        cur = next;
        next = t;
    }
    m->resp_phase[i] = (int)srm_mod(s.origin, m->period);
    DSD_MEMCPY(m->resp + ((size_t)i * (size_t)m->resp_len), cur, (size_t)s.len * sizeof(double));
}

/* Every input phase's response through the reader and the cascade into resp (unscaled: a unit input), the scratch the
   exact plan runs in with it. Q = M 2^passes / gcd(L, 2^passes) input phases: an input Q later is the same response P
   outputs later. */
static int
srm_responses(dsd_squelch_reader_model* m, const dsd_noise_squelch_stage* stages, int count) {
    const dsd_iq_resampler* r = &m->reader;
    const int64_t inputs = ((int64_t)r->M << count) / srm_gcd(r->L, 1 << count);
    const int bound = srm_response_bound(r, stages, count);
    if (bound < 1 || srm_allocate(m, inputs, bound) != 0) {
        return -1;
    }
    double* a = srm_doubles((size_t)bound);
    double* b = srm_doubles((size_t)bound);
    if (a && b) {
        for (int64_t i = 0; i < inputs; i++) {
            srm_record_response(m, stages, count, i, a, b);
        }
    }
    const int rc = (a && b) ? 0 : -1;
    dsd_neo_aligned_free(a);
    dsd_neo_aligned_free(b);
    return rc;
}

/* -------------------------------------------------------------------------------------------------------- build */

static int
srm_build_args_ok(const dsd_iq_reader_plan* plan, const dsd_noise_squelch_stage* stages, int stage_count) {
    if (!plan || !plan->enabled || stage_count < 0 || stage_count > DSD_NOISE_SQUELCH_MAX_STAGES
        || (stage_count > 0 && !stages)) {
        return 0;
    }
    for (int i = 0; i < stage_count; i++) {
        if (!stages[i].taps || stages[i].len < 1 || stages[i].len > DSD_SQUELCH_CASCADE_MAX_STAGE_TAPS) {
            return 0;
        }
    }
    return 1;
}

int
dsd_squelch_reader_model_build(dsd_squelch_reader_model* m, const dsd_iq_reader_plan* plan,
                               const dsd_noise_squelch_stage* stages, int stage_count) {
    if (!m) {
        return -1;
    }
    if (dsd_squelch_reader_model_matches(m, plan, stage_count)) {
        return 0;
    }
    dsd_squelch_reader_model_release(m);
    if (!srm_build_args_ok(plan, stages, stage_count)
        || dsd_iq_reader_plan_configure(plan, &m->reader, 1U) != DSD_IQ_RESAMPLER_OK) {
        dsd_squelch_reader_model_release(m);
        return -1;
    }
    m->plan = *plan;
    m->passes = stage_count;
    if (srm_cascade_stats(m, stages, stage_count) != 0) {
        dsd_squelch_reader_model_release(m);
        return -1;
    }
    /* The every-phase tables past the size limit (a reader that decimates by a large M ahead of many passes: Q of
       hundreds of thousands) leave the averaged plan, which such a reader's small spread takes anyway. */
    if (srm_responses(m, stages, stage_count) == 0) {
        m->exact_ready = 1;
    } else {
        srm_release_exact(m);
    }
    m->ready = 1;
    return 0;
}

/* --------------------------------------------------------------------------------------------------- the plans */

/* The arguments a floor plan takes: @p m ready for the rate (the reader's output over 2^passes), and channel taps
   (NULL or 0: one unit tap) it can hold. */
static int
srm_floor_args(const dsd_squelch_reader_model* m, const float** taps, int* taps_len, int rate_hz) {
    static const float k_unit_tap[1] = {1.0f};
    if (!m || !m->ready || *taps_len > DSD_NOISE_SQUELCH_MAX_TAPS || rate_hz < 1
        || ((int64_t)rate_hz << m->passes) != (int64_t)m->plan.out_hz) {
        return 0;
    }
    if (!*taps || *taps_len <= 0) {
        *taps = k_unit_tap;
        *taps_len = 1;
    }
    return 1;
}

/* Each input phase's response through the @p len channel taps into its row of chan (@p lags of it). */
static void
srm_responses_through_taps(dsd_squelch_reader_model* m, const float* taps, int len, int lags) {
    for (int k = 0; k < m->inputs; k++) {
        const double* y = m->resp + ((size_t)k * (size_t)m->resp_len);
        double* z = m->chan + ((size_t)k * (size_t)m->cov_lags);
        for (int t = 0; t < lags; t++) {
            double sum = 0.0;
            const int lo = t - (m->resp_len - 1) > 0 ? t - (m->resp_len - 1) : 0;
            const int hi = t < len - 1 ? t : len - 1;
            for (int i = lo; i <= hi; i++) {
                sum += (double)taps[i] * y[t - i];
            }
            z[t] = sum;
        }
    }
}

/* Every output phase's covariance behind the @p len channel taps into cov (period rows of @p lags): each input phase's
   response through the taps adds its products to the output phases it covers, so summed over the inputs of one input
   period each row holds E{z_n z_{n+m}} for its output phase n, then scaled to the input variance 2^(passes - 1). Row
   by row, every response's samples that fall on it in turn (input phase, then sample, ascending), so one row is
   written at a time while the responses stay in cache: a period of thousands of phases (8,046 Hz read into 72 kHz:
   4000) holds tens of megabytes of rows. */
static void
srm_accumulate_cov(dsd_squelch_reader_model* m, const float* taps, int len, int lags) {
    const int P = m->period;
    srm_responses_through_taps(m, taps, len, lags);
    const double scale = ldexp(1.0, m->passes > 0 ? m->passes - 1 : 0);
    for (int r = 0; r < P; r++) {
        double* row = m->cov + ((size_t)r * (size_t)lags);
        DSD_MEMSET(row, 0, (size_t)lags * sizeof(double));
        for (int k = 0; k < m->inputs; k++) {
            const double* z = m->chan + ((size_t)k * (size_t)m->cov_lags);
            const int first = r - m->resp_phase[k];
            for (int t = first < 0 ? first + P : first; t < lags; t += P) {
                const double zt = z[t];
                for (int lag = 0; lag + t < lags; lag++) {
                    row[lag] += zt * z[t + lag];
                }
            }
        }
        for (int lag = 0; lag < lags; lag++) {
            row[lag] *= scale;
        }
    }
}

int
dsd_squelch_reader_model_floor_plan_exact(dsd_squelch_reader_model* m, dsd_squelch_floor_plan* out, const float* taps,
                                          int taps_len, int rate_hz) {
    if (!out) {
        return -1;
    }
    DSD_MEMSET(out, 0, sizeof(*out));
    const float* run_taps = taps;
    int run_len = taps_len;
    if (!srm_floor_args(m, &run_taps, &run_len, rate_hz) || !m->exact_ready) {
        return -1;
    }
    const int lags = m->resp_len + run_len - 1;
    srm_accumulate_cov(m, run_taps, run_len, lags);
    dsd_squelch_floor_periodic_noise noise;
    noise.period = m->period;
    noise.lags = lags;
    noise.cov = m->cov;
    return dsd_squelch_floor_plan_design_periodic_mean(out, taps, taps_len, &noise, rate_hz);
}

/* The phase-averaged covariance behind the channel taps into mean_chan (exact: the taps are time-invariant),
   described by @p noise. Returns whether it may stand for every output phase (dsd_squelch_cascade_mean_suffices(), the
   step model's rule); 0 when it cannot be formed. */
static int
srm_mean_through_taps(dsd_squelch_reader_model* m, const float* taps, int len,
                      dsd_squelch_floor_periodic_noise* noise) {
    const dsd_squelch_floor_periodic_noise mean = {1, m->lags, m->mean};
    /* Not reachable (the scratch holds DSD_NOISE_SQUELCH_MAX_TAPS taps, which srm_floor_args() holds them to): should
       it happen, the plan takes every output phase in full. */
    if (dsd_squelch_cascade_noise_through_taps(&mean, taps, len, m->rc, sizeof(m->rc) / sizeof(m->rc[0]), m->mean_chan,
                                               (size_t)(m->lags + DSD_NOISE_SQUELCH_MAX_TAPS - 1), noise)
        != 0) {
        return 0;
    }
    return dsd_squelch_cascade_mean_suffices(m->cyclo_dev, taps, len, m->mean_chan[0]);
}

int
dsd_squelch_reader_model_floor_plan(dsd_squelch_reader_model* m, dsd_squelch_floor_plan* out, const float* taps,
                                    int taps_len, int rate_hz) {
    if (!out) {
        return -1;
    }
    DSD_MEMSET(out, 0, sizeof(*out));
    const float* run_taps = taps;
    int run_len = taps_len;
    if (!srm_floor_args(m, &run_taps, &run_len, rate_hz)) {
        return -1;
    }
    dsd_squelch_floor_periodic_noise noise;
    if (!srm_mean_through_taps(m, run_taps, run_len, &noise)) {
        return dsd_squelch_reader_model_floor_plan_exact(m, out, taps, taps_len, rate_hz);
    }
    return dsd_squelch_floor_plan_design_periodic_mean(out, taps, taps_len, &noise, rate_hz);
}

int
dsd_squelch_reader_model_noise_plan(const dsd_squelch_reader_model* m, dsd_noise_squelch_plan* out, const float* taps,
                                    int taps_len, const dsd_noise_squelch_stage* stages, int stage_count, int rate_hz) {
    if (!out) {
        return -1;
    }
    if (!m || !m->ready || stage_count != m->passes || rate_hz < 1
        || ((int64_t)rate_hz << m->passes) != (int64_t)m->plan.out_hz) {
        DSD_MEMSET(out, 0, sizeof(*out));
        return -1;
    }
    return dsd_noise_squelch_plan_design_reader(out, taps, taps_len, stages, stage_count, rate_hz, &m->reader);
}
