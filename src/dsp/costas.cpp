// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Gardner symbol recovery block for GR - Copyright 2010, 2011, 2012, 2013, 2014, 2015 KA1RBI
 * Costas loop for carrier recovery - Copyright 2006,2010-2012 Free Software Foundation, Inc.
 * Lock detector based on Yair Linn's research - Copyright 2022 gnorbury@bondcar.com
 * Port to dsd-neo - Copyright (C) 2026 by arancormonk
 *
 * This is a direct port of OP25's CQPSK signal chain:
 *   AGC -> Gardner (timing only) -> diff_phasor -> Costas (at symbol rate)
 *
 * The key insight from OP25 (p25_demodulator_dev.py line 486):
 *   self.connect(self.if_out, self.agc, self.fll, self.clock, self.diffdec, self.costas, ...)
 *
 * Where:
 *   - clock = op25_repeater.gardner_cc (timing recovery only, NO carrier tracking)
 *   - diffdec = digital.diff_phasor_cc (differential decoding at symbol rate)
 *   - costas = op25_repeater.costas_loop_cc (carrier tracking at symbol rate)
 */

#include <cmath>
#include <cstdio>
#include <dsd-neo/dsp/costas.h>
#include <dsd-neo/dsp/demod_state.h>
#include <dsd-neo/runtime/config.h>
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/dsp/ted.h"

static void fll_band_edge_design_filter(dsd_fll_band_edge_state_t* f, int sps, float rolloff, int n_taps);

namespace {

constexpr float kTwoPi = 6.28318530717958647692f;
constexpr float kPi = 3.14159265358979323846f;
static bool g_warned_ted_dl_oversize = false;

static inline bool
debug_cqpsk_enabled(void) {
    const dsdneoRuntimeConfig* cfg = dsd_neo_get_config();
    if (!cfg) {
        dsd_neo_config_init(NULL);
        cfg = dsd_neo_get_config();
    }
    return cfg && cfg->debug_cqpsk_enable;
}

static inline void
fll_band_edge_clear_delay(dsd_fll_band_edge_state_t* f) {
    if (!f) {
        return;
    }
    for (int i = 0; i < FLL_BAND_EDGE_MAX_TAPS * 2; i++) {
        f->delay_r[i] = 0.0f;
        f->delay_i[i] = 0.0f;
    }
}

/* MMSE interpolator parameters - match OP25/GNU Radio */
#define MMSE_NTAPS  8
#define MMSE_NSTEPS 16

/* GNU Radio MMSE 8-tap polyphase interpolator coefficients (subset at 1/16 steps) */
static const float mmse_taps[MMSE_NSTEPS + 1][MMSE_NTAPS] = {
    /* Row 0/128 (mu=0): output = sample[4] */
    {0.00000e+00f, 0.00000e+00f, 0.00000e+00f, 0.00000e+00f, 1.00000e+00f, 0.00000e+00f, 0.00000e+00f, 0.00000e+00f},
    /* Row 8/128 (mu=0.0625) */
    {-1.23337e-03f, 6.84261e-03f, -2.24178e-02f, 6.57852e-02f, 9.83392e-01f, -4.04519e-02f, 9.56876e-03f,
     -1.54221e-03f},
    /* Row 16/128 (mu=0.125) */
    {-2.43121e-03f, 1.35716e-02f, -4.49929e-02f, 1.36968e-01f, 9.55956e-01f, -7.43154e-02f, 1.80759e-02f,
     -2.94361e-03f},
    /* Row 24/128 (mu=0.1875) */
    {-3.55283e-03f, 1.99599e-02f, -6.70018e-02f, 2.12443e-01f, 9.18329e-01f, -1.01501e-01f, 2.53295e-02f,
     -4.16581e-03f},
    /* Row 32/128 (mu=0.25) */
    {-4.55932e-03f, 2.57844e-02f, -8.77011e-02f, 2.91006e-01f, 8.71305e-01f, -1.22047e-01f, 3.11866e-02f,
     -5.17776e-03f},
    /* Row 40/128 (mu=0.3125) */
    {-5.41467e-03f, 3.08323e-02f, -1.06342e-01f, 3.71376e-01f, 8.15826e-01f, -1.36111e-01f, 3.55525e-02f,
     -5.95620e-03f},
    /* Row 48/128 (mu=0.375) */
    {-6.08674e-03f, 3.49066e-02f, -1.22185e-01f, 4.52218e-01f, 7.52958e-01f, -1.43968e-01f, 3.83800e-02f,
     -6.48585e-03f},
    /* Row 56/128 (mu=0.4375) */
    {-6.54823e-03f, 3.78315e-02f, -1.34515e-01f, 5.32164e-01f, 6.83875e-01f, -1.45993e-01f, 3.96678e-02f,
     -6.75943e-03f},
    /* Row 64/128 (mu=0.5): midpoint */
    {-6.77751e-03f, 3.94578e-02f, -1.42658e-01f, 6.09836e-01f, 6.09836e-01f, -1.42658e-01f, 3.94578e-02f,
     -6.77751e-03f},
    /* Row 72/128 (mu=0.5625) */
    {-6.73929e-03f, 3.95900e-02f, -1.46043e-01f, 6.92808e-01f, 5.22267e-01f, -1.33190e-01f, 3.75341e-02f,
     -6.50285e-03f},
    /* Row 80/128 (mu=0.625) */
    {-6.48585e-03f, 3.83800e-02f, -1.43968e-01f, 7.52958e-01f, 4.52218e-01f, -1.22185e-01f, 3.49066e-02f,
     -6.08674e-03f},
    /* Row 88/128 (mu=0.6875) */
    {-5.95620e-03f, 3.55525e-02f, -1.36111e-01f, 8.15826e-01f, 3.71376e-01f, -1.06342e-01f, 3.08323e-02f,
     -5.41467e-03f},
    /* Row 96/128 (mu=0.75) */
    {-5.17776e-03f, 3.11866e-02f, -1.22047e-01f, 8.71305e-01f, 2.91006e-01f, -8.77011e-02f, 2.57844e-02f,
     -4.55932e-03f},
    /* Row 104/128 (mu=0.8125) */
    {-4.16581e-03f, 2.53295e-02f, -1.01501e-01f, 9.18329e-01f, 2.12443e-01f, -6.70018e-02f, 1.99599e-02f,
     -3.55283e-03f},
    /* Row 112/128 (mu=0.875) */
    {-2.94361e-03f, 1.80759e-02f, -7.43154e-02f, 9.55956e-01f, 1.36968e-01f, -4.49929e-02f, 1.35716e-02f,
     -2.43121e-03f},
    /* Row 120/128 (mu=0.9375) */
    {-1.54221e-03f, 9.56876e-03f, -4.04519e-02f, 9.83392e-01f, 6.57852e-02f, -2.24178e-02f, 6.84261e-03f,
     -1.23337e-03f},
    /* Row 128/128 (mu=1.0): output = sample[3] */
    {0.00000e+00f, 0.00000e+00f, 0.00000e+00f, 1.00000e+00f, 0.00000e+00f, 0.00000e+00f, 0.00000e+00f, 0.00000e+00f},
};

/* 8-tap MMSE FIR interpolation */
static inline float
mmse_interp_8tap(const float* s, float mu) {
    float idx_f = mu * (float)MMSE_NSTEPS;
    int idx_lo = (int)idx_f;
    float frac = idx_f - (float)idx_lo;

    if (idx_lo < 0) {
        idx_lo = 0;
        frac = 0.0f;
    }
    if (idx_lo >= MMSE_NSTEPS) {
        idx_lo = MMSE_NSTEPS - 1;
        frac = 1.0f;
    }
    int idx_hi = idx_lo + 1;

    const float* taps_lo = mmse_taps[idx_lo];
    const float* taps_hi = mmse_taps[idx_hi];
    float one_minus_frac = 1.0f - frac;

    float acc = 0.0f;
    for (int k = 0; k < MMSE_NTAPS; k++) {
        float tap = one_minus_frac * taps_lo[k] + frac * taps_hi[k];
        acc += tap * s[k];
    }
    return acc;
}

/* Complex MMSE interpolation */
static inline void
mmse_interp_cc(const float* dl, float mu, float* out_r, float* out_j) {
    float sr[MMSE_NTAPS];
    float sj[MMSE_NTAPS];

    for (int k = 0; k < MMSE_NTAPS; k++) {
        const size_t kk = (size_t)k;
        sr[k] = dl[kk * 2];
        sj[k] = dl[kk * 2 + 1];
    }

    *out_r = mmse_interp_8tap(sr, mu);
    *out_j = mmse_interp_8tap(sj, mu);
}

/* Saturating clip equivalent to GNU Radio's branchless_clip. */
static inline float
clipf_limit(float x, float limit) {
    if (x > limit) {
        return limit;
    }
    if (x < -limit) {
        return -limit;
    }
    return x;
}

static inline void
dsd_sincosf(float phase, float* out_sin, float* out_cos) {
#if defined(__GNUC__) && !defined(__clang__) && !defined(_MSC_VER)
    __builtin_sincosf(phase, out_sin, out_cos);
#else
    *out_sin = sinf(phase);
    *out_cos = cosf(phase);
#endif
}

static inline void
dsd_sincosf_clamped_half_pi(float phase, float* out_sin, float* out_cos) {
    float x2 = phase * phase;
    *out_sin = phase
               * (1.0f
                  + x2
                        * (-0.16666666666666666667f
                           + x2
                                 * (0.00833333333333333333f
                                    + x2
                                          * (-0.00019841269841269841f
                                             + x2 * (0.00000275573192239859f + x2 * -0.00000002505210838544f)))));
    *out_cos = 1.0f
               + x2
                     * (-0.5f
                        + x2
                              * (0.04166666666666666667f
                                 + x2
                                       * (-0.00138888888888888889f
                                          + x2 * (0.00002480158730158730f + x2 * -0.00000027557319223986f))));
}

static inline void
dsd_sincosf_wrapped_two_pi(float phase, float* out_sin, float* out_cos) {
    if (!std::isfinite(phase) || phase < -kTwoPi || phase > kTwoPi) {
        dsd_sincosf(phase, out_sin, out_cos);
        return;
    }

    if (phase > kPi) {
        phase -= kTwoPi;
    } else if (phase < -kPi) {
        phase += kTwoPi;
    }

    if (phase > (kPi / 2.0f)) {
        float sin_v = 0.0f;
        float cos_v = 0.0f;
        dsd_sincosf_clamped_half_pi(kPi - phase, &sin_v, &cos_v);
        *out_sin = sin_v;
        *out_cos = -cos_v;
        return;
    }
    if (phase < (-kPi / 2.0f)) {
        float sin_v = 0.0f;
        float cos_v = 0.0f;
        dsd_sincosf_clamped_half_pi(-kPi - phase, &sin_v, &cos_v);
        *out_sin = sin_v;
        *out_cos = -cos_v;
        return;
    }

    dsd_sincosf_clamped_half_pi(phase, out_sin, out_cos);
}

static int
cqpsk_symbol_rate_hz(const demod_state* d) {
    if (!d || d->rate_out <= 0 || d->ted_sps <= 0) {
        return 4800;
    }
    return (d->rate_out + (d->ted_sps / 2)) / d->ted_sps;
}

static float
op25_gardner_gain_mu_for_state(const demod_state* d, const ted_state_t* ted) {
    const float requested = (d && d->ted_gain > 0.0f) ? d->ted_gain : 0.025f;
    const dsdneoRuntimeConfig* cfg = dsd_neo_get_config();
    if (!cfg) {
        dsd_neo_config_init(NULL);
        cfg = dsd_neo_get_config();
    }
    if ((d && d->ted_gain_is_set) || (cfg && cfg->ted_gain_is_set)) {
        return requested;
    }

    if (cqpsk_symbol_rate_hz(d) < 5500) {
        return requested;
    }
    if (!ted || ted->lock_count < 240) {
        return requested;
    }

    float lock_metric = ted->lock_accum / (float)ted->lock_count;
    if (lock_metric < 0.05f) {
        return requested;
    }

    return 0.018f;
}

/*
 * OP25 QPSK phase error detector.
 *
 * From costas_loop_cc_impl.cc phase_detector_4():
 *   return ((sample.real() > 0 ? 1.0 : -1.0) * sample.imag() -
 *           (sample.imag() > 0 ? 1.0 : -1.0) * sample.real());
 *
 * This expects the OP25 differential QPSK constellation at diagonal positions.
 */
static inline float
phase_detector_4(float real, float imag) {
    return ((real > 0.0f ? 1.0f : -1.0f) * imag - (imag > 0.0f ? 1.0f : -1.0f) * real);
}

constexpr float kCqpskCostasDetectorTargetMag = 0.85f * 0.85f;
constexpr float kCqpskCostasConfidenceFloorMag = 0.10f;
constexpr float kCqpskCostasConfidenceFullMag = 0.35f;
constexpr float kCqpskCostasConfidenceFloorMag2 = kCqpskCostasConfidenceFloorMag * kCqpskCostasConfidenceFloorMag;
constexpr float kCqpskCostasConfidenceFullMag2 = kCqpskCostasConfidenceFullMag * kCqpskCostasConfidenceFullMag;
constexpr float kCqpskCostasErrorSmoothAlpha = 0.25f;
constexpr float kCqpskCostasErrorSmoothAlphaMin = 0.10f;
constexpr float kCqpskCostasErrorKickDeltaLow = 0.02f;
constexpr float kCqpskCostasErrorKickDeltaHigh = 0.18f;
constexpr float kCqpskCostasErrorSmoothBootstrap = 1.0e-6f;

static inline float
smoothstep(float edge0, float edge1, float x) {
    if (x <= edge0) {
        return 0.0f;
    }
    if (x >= edge1) {
        return 1.0f;
    }
    float t = (x - edge0) / (edge1 - edge0);
    return t * t * (3.0f - 2.0f * t);
}

static inline float
cqpsk_costas_confidence_from_mag(float mag) {
    if (!std::isfinite(mag)) {
        return 0.0f;
    }
    return smoothstep(kCqpskCostasConfidenceFloorMag, kCqpskCostasConfidenceFullMag, mag);
}

static inline float
cqpsk_costas_error_smooth_alpha(float error_raw, float error_smooth) {
    if (!std::isfinite(error_raw) || !std::isfinite(error_smooth)
        || std::fabs(error_smooth) <= kCqpskCostasErrorSmoothBootstrap) {
        return kCqpskCostasErrorSmoothAlpha;
    }

    /* Treat abrupt discriminator disagreement as a phase kick and lower the
     * EMA gain; sustained error converges back through the smoothed state. */
    float kick =
        smoothstep(kCqpskCostasErrorKickDeltaLow, kCqpskCostasErrorKickDeltaHigh, std::fabs(error_raw - error_smooth));
    return kCqpskCostasErrorSmoothAlpha + (kCqpskCostasErrorSmoothAlphaMin - kCqpskCostasErrorSmoothAlpha) * kick;
}

static inline float
normalize_costas_detector_sample(float real, float imag, float* out_real, float* out_imag) {
    float mag2 = real * real + imag * imag;
    if (!std::isfinite(mag2)) {
        *out_real = 0.0f;
        *out_imag = 0.0f;
        return 0.0f;
    }

    if (mag2 <= kCqpskCostasConfidenceFloorMag2) {
        *out_real = real;
        *out_imag = imag;
        return 0.0f;
    }

    float mag = sqrtf(mag2);
    float confidence = (mag2 >= kCqpskCostasConfidenceFullMag2) ? 1.0f : cqpsk_costas_confidence_from_mag(mag);

    float scale = kCqpskCostasDetectorTargetMag / mag;
    if (!std::isfinite(scale)) {
        *out_real = 0.0f;
        *out_imag = 0.0f;
        return 0.0f;
    }
    *out_real = real * scale;
    *out_imag = imag * scale;
    return confidence;
}

/* NaN check helper */
#define IS_NAN(x) ((x) != (x))

static inline int
clampi(int value, int min_value, int max_value) {
    if (value < min_value) {
        return min_value;
    }
    if (value > max_value) {
        return max_value;
    }
    return value;
}

static inline float
clampf_range(float value, float min_value, float max_value) {
    if (value < min_value) {
        return min_value;
    }
    if (value > max_value) {
        return max_value;
    }
    return value;
}

static inline int
q14_from_ratio(float ratio, int max_q14) {
    const int scaled = (int)std::lrint(ratio * 16384.0f);
    return clampi(scaled, 0, max_q14);
}

static inline int
percent_from_count(int count, int total) {
    if (total <= 0) {
        return 0;
    }
    const int pct = (int)std::lrint((100.0 * (double)count) / (double)total);
    return clampi(pct, 0, 100);
}

struct gardner_loop_context_t {
    float* iq_in;
    float* iq_out;
    float* dl;
    int buf_len;
    int nc;
    int i;
    int o;
    int dl_index;
    int twice_sps;
    float mu;
    float omega;
    float gain_mu;
    float gain_omega;
    float last_r;
    float last_j;
    float lock_accum;
    int lock_count;
};

struct costas_metrics_acc_t {
    float err_abs_acc;
    float err_raw_abs_acc;
    float confidence_acc;
    int zero_conf_count;
};

struct costas_loop_context_t {
    float phase;
    float freq;
    float alpha;
    float beta;
    float max_freq;
    float min_freq;
    float max_phase;
    float min_phase;
    float error_smooth;
    float last_error;
    costas_metrics_acc_t metrics;
};

struct fll_loop_context_t {
    float phase;
    float freq;
    float alpha;
    float beta;
    float max_freq;
    float min_freq;
    int n_taps;
    float* delay_r;
    float* delay_i;
    int delay_idx;
};

static inline int
gardner_need_reinit(const ted_state_t* ted, int sps, int* is_first_init) {
    const int first_init = (ted->omega_mid == 0.0f || ted->twice_sps < 2);
    if (is_first_init) {
        *is_first_init = first_init;
    }
    if (first_init) {
        return 1;
    }
    return (ted->sps > 0 && ted->sps != sps) ? 1 : 0;
}

static inline int
gardner_reinit_state(demod_state* d, ted_state_t* ted, int sps, int is_first_init, float* omega) {
    if (debug_cqpsk_enabled()) {
        DSD_FPRINTF(stderr, "[GARDNER] TED %s: sps=%d->%d old_omega=%.3f old_mu=%.3f (mu=%d for warmup)\n",
                    is_first_init ? "init" : "sps_change", ted->sps, sps, ted->omega, ted->mu, sps);
    }

    ted->mu = (float)sps;
    *omega = (float)sps;
    ted->omega = *omega;
    ted->omega_rel = 0.002f;
    ted->omega_mid = *omega;
    ted->omega_min = *omega * (1.0f - ted->omega_rel);
    ted->omega_max = *omega * (1.0f + ted->omega_rel);

    const int twice_sps_op25 = 2 * (int)ceilf(ted->omega_max);
    const int twice_sps_mmse = (int)ceilf(ted->omega_max / 2.0f) + MMSE_NTAPS + 1;
    const int twice_sps_required = (twice_sps_op25 > twice_sps_mmse) ? twice_sps_op25 : twice_sps_mmse;
    if (twice_sps_required > TED_DL_SIZE) {
        if (!g_warned_ted_dl_oversize) {
            DSD_FPRINTF(stderr, "[GARDNER] disabled: required delay line %d exceeds TED_DL_SIZE=%d (sps=%d)\n",
                        twice_sps_required, TED_DL_SIZE, sps);
            g_warned_ted_dl_oversize = true;
        }
        d->lp_len = 0;
        return 0;
    }

    ted->twice_sps = twice_sps_required;
    ted->dl_index = 0;
    ted->sps = sps;
    ted->dl[0] = 0.0f;
    ted->dl[1] = 0.0f;
    return 1;
}

static inline void
gardner_init_loop_context(demod_state* d, ted_state_t* ted, float gain_mu, float gain_omega,
                          gardner_loop_context_t* ctx) {
    ctx->iq_in = d->lowpassed;
    ctx->iq_out = d->timing_buf;
    ctx->dl = ted->dl;
    ctx->buf_len = d->lp_len;
    ctx->nc = d->lp_len >> 1;
    ctx->i = 0;
    ctx->o = 0;
    ctx->dl_index = ted->dl_index;
    ctx->twice_sps = ted->twice_sps;
    ctx->mu = ted->mu;
    ctx->omega = ted->omega;
    ctx->gain_mu = gain_mu;
    ctx->gain_omega = gain_omega;
    ctx->last_r = ted->last_r;
    ctx->last_j = ted->last_j;
    ctx->lock_accum = ted->lock_accum;
    ctx->lock_count = ted->lock_count;
}

static inline void
gardner_commit_loop_context(demod_state* d, ted_state_t* ted, const gardner_loop_context_t* ctx) {
    ted->mu = ctx->mu;
    ted->omega = ctx->omega;
    ted->dl_index = ctx->dl_index;
    ted->last_r = ctx->last_r;
    ted->last_j = ctx->last_j;
    ted->lock_accum = ctx->lock_accum;
    ted->lock_count = ctx->lock_count;

    if (ctx->o >= 2) {
        d->lowpassed = ctx->iq_out;
        d->lp_len = ctx->o;
    } else {
        d->lp_len = 0;
    }
}

static inline void
gardner_push_delay_sample(gardner_loop_context_t* ctx, float in_r, float in_j) {
    const size_t dl_i = (size_t)ctx->dl_index;
    const size_t dl_i2 = (size_t)ctx->dl_index + (size_t)ctx->twice_sps;
    ctx->dl[dl_i * 2] = in_r;
    ctx->dl[dl_i * 2 + 1] = in_j;
    ctx->dl[dl_i2 * 2] = in_r;
    ctx->dl[dl_i2 * 2 + 1] = in_j;
    ctx->dl_index++;
    if (ctx->dl_index >= ctx->twice_sps) {
        ctx->dl_index = 0;
    }
}

static inline int
gardner_consume_until_ready(gardner_loop_context_t* ctx) {
    while (ctx->mu > 1.0f && ctx->i < ctx->nc) {
        ctx->mu -= 1.0f;

        const size_t ii = (size_t)ctx->i;
        float in_r = ctx->iq_in[ii * 2];
        float in_j = ctx->iq_in[ii * 2 + 1];
        if (IS_NAN(in_r)) {
            in_r = 0.0f;
        }
        if (IS_NAN(in_j)) {
            in_j = 0.0f;
        }

        gardner_push_delay_sample(ctx, in_r, in_j);
        ctx->i++;
    }
    return (ctx->i < ctx->nc) ? 1 : 0;
}

static inline void
gardner_compute_half_timing(float mu, float omega, int* half_sps, float* half_mu) {
    const float half_omega = omega / 2.0f;
    int hsps = (int)floorf(half_omega);
    float hmu = mu + half_omega - (float)hsps;
    if (hmu > 1.0f) {
        hmu -= 1.0f;
        hsps += 1;
    }
    if (hsps < 0) {
        hsps = 0;
    }
    *half_sps = hsps;
    *half_mu = hmu;
}

static inline int
gardner_interpolate_symbol(const gardner_loop_context_t* ctx, int half_sps, float half_mu, float* mid_r, float* mid_j,
                           float* sym_r, float* sym_j) {
    const int max_mid_idx = ctx->dl_index + MMSE_NTAPS - 1;
    const int max_sym_idx = ctx->dl_index + half_sps + MMSE_NTAPS - 1;
    if (max_mid_idx >= 2 * ctx->twice_sps || max_sym_idx >= 2 * ctx->twice_sps) {
        return 0;
    }

    mmse_interp_cc(ctx->dl + (size_t)ctx->dl_index * 2, ctx->mu, mid_r, mid_j);
    mmse_interp_cc(ctx->dl + (size_t)(ctx->dl_index + half_sps) * 2, half_mu, sym_r, sym_j);
    return 1;
}

static inline float
gardner_compute_symbol_error(float last_r, float last_j, float sym_r, float sym_j, float mid_r, float mid_j) {
    const float error_real = (last_r - sym_r) * mid_r;
    const float error_imag = (last_j - sym_j) * mid_j;
    float symbol_error = error_real + error_imag;
    if (IS_NAN(symbol_error)) {
        symbol_error = 0.0f;
    }
    return clipf_limit(symbol_error, 1.0f);
}

static inline void
gardner_update_lock_accum(gardner_loop_context_t* ctx, float sym_r, float sym_j, float mid_r, float mid_j) {
    const float ie2 = sym_r * sym_r;
    const float io2 = mid_r * mid_r;
    const float qe2 = sym_j * sym_j;
    const float qo2 = mid_j * mid_j;
    const float yi = ((ie2 + io2) != 0.0f) ? (ie2 - io2) / (ie2 + io2) : 0.0f;
    const float yq = ((qe2 + qo2) != 0.0f) ? (qe2 - qo2) / (qe2 + qo2) : 0.0f;
    ctx->lock_accum += yi + yq;
    ctx->lock_count++;
}

static inline void
gardner_update_loop(gardner_loop_context_t* ctx, const ted_state_t* ted, float symbol_error, float sym_r, float sym_j) {
    const float sym_mag = sqrtf(sym_r * sym_r + sym_j * sym_j);
    ctx->omega += ctx->gain_omega * symbol_error * sym_mag;
    ctx->omega = ted->omega_mid + clipf_limit(ctx->omega - ted->omega_mid, ted->omega_rel);
    ctx->mu += ctx->omega + ctx->gain_mu * symbol_error;
}

static inline void
costas_init_if_needed(dsd_costas_loop_state_t* c) {
    if (c->initialized) {
        return;
    }
    const float loop_bw = 0.008f;
    const float damping = 0.70710678118654752440f;
    const float denom = 1.0f + 2.0f * damping * loop_bw + loop_bw * loop_bw;
    c->alpha = (4.0f * damping * loop_bw) / denom;
    c->beta = (4.0f * loop_bw * loop_bw) / denom;
    c->max_freq = 1.0f;
    c->min_freq = -1.0f;
    c->damping = damping;
    c->loop_bw = loop_bw;
    c->initialized = 1;
}

static inline void
costas_prepare_loop_context(const dsd_costas_loop_state_t* c, costas_loop_context_t* ctx) {
    ctx->max_phase = kPi / 2.0f;
    ctx->min_phase = -ctx->max_phase;
    ctx->phase = std::isfinite(c->phase) ? clampf_range(c->phase, ctx->min_phase, ctx->max_phase) : 0.0f;
    ctx->freq = c->freq;
    ctx->alpha = c->alpha;
    ctx->beta = c->beta;
    ctx->max_freq = c->max_freq;
    ctx->min_freq = c->min_freq;
    ctx->error_smooth = std::isfinite(c->error_smooth) ? c->error_smooth : 0.0f;
    ctx->last_error = 0.0f;
    ctx->metrics.err_abs_acc = 0.0f;
    ctx->metrics.err_raw_abs_acc = 0.0f;
    ctx->metrics.confidence_acc = 0.0f;
    ctx->metrics.zero_conf_count = 0;
}

static inline void
costas_process_symbol(costas_loop_context_t* ctx, float in_r, float in_j, float* out_r, float* out_j) {
    float nco_r = 0.0f;
    float nco_j = 0.0f;
    dsd_sincosf_clamped_half_pi(-ctx->phase, &nco_j, &nco_r);

    const float rot_r = in_r * nco_r - in_j * nco_j;
    const float rot_j = in_r * nco_j + in_j * nco_r;

    float det_r = 0.0f;
    float det_j = 0.0f;
    const float confidence = normalize_costas_detector_sample(rot_r, rot_j, &det_r, &det_j);

    float error = 0.0f;
    float error_raw = 0.0f;
    if (confidence <= 0.0f || !std::isfinite(confidence)) {
        ctx->error_smooth = 0.0f;
        ctx->metrics.zero_conf_count++;
    } else {
        error_raw = clipf_limit(phase_detector_4(det_r, det_j) * confidence, 1.0f);
        const float smooth_alpha = cqpsk_costas_error_smooth_alpha(error_raw, ctx->error_smooth);
        ctx->error_smooth += smooth_alpha * (error_raw - ctx->error_smooth);
        error = clipf_limit(ctx->error_smooth, 1.0f);
        ctx->metrics.confidence_acc += confidence;
    }

    ctx->last_error = error;
    ctx->metrics.err_abs_acc += fabsf(error);
    ctx->metrics.err_raw_abs_acc += fabsf(error_raw);

    ctx->freq += ctx->beta * error;
    ctx->phase += ctx->freq + ctx->alpha * error;
    ctx->phase = clampf_range(ctx->phase, ctx->min_phase, ctx->max_phase);
    ctx->freq = clampf_range(ctx->freq, ctx->min_freq, ctx->max_freq);

    *out_r = det_r;
    *out_j = det_j;
}

static inline void
costas_store_metrics(demod_state* d, const costas_metrics_acc_t* metrics, int pairs) {
    if (pairs <= 0) {
        d->costas_err_avg_q14 = 0;
        d->costas_err_raw_avg_q14 = 0;
        d->costas_conf_avg_q14 = 0;
        d->costas_zero_conf_pct = 0;
        return;
    }

    const float inv_pairs = 1.0f / (float)pairs;
    d->costas_err_avg_q14 = q14_from_ratio(metrics->err_abs_acc * inv_pairs, 32767);
    d->costas_err_raw_avg_q14 = q14_from_ratio(metrics->err_raw_abs_acc * inv_pairs, 32767);
    d->costas_conf_avg_q14 = q14_from_ratio(metrics->confidence_acc * inv_pairs, 16384);
    d->costas_zero_conf_pct = percent_from_count(metrics->zero_conf_count, pairs);
}

static inline void
costas_commit_loop(dsd_costas_loop_state_t* c, const costas_loop_context_t* ctx) {
    c->phase = ctx->phase;
    c->freq = ctx->freq;
    c->error = ctx->last_error;
    c->error_smooth = ctx->error_smooth;
}

static inline int
fll_need_reinit(const dsd_fll_band_edge_state_t* f, int sps, int* is_first_init) {
    const int first_init = !f->initialized;
    if (is_first_init) {
        *is_first_init = first_init;
    }
    if (first_init) {
        return 1;
    }
    return (f->sps > 0 && f->sps != sps) ? 1 : 0;
}

static inline void
fll_configure_loop_params(dsd_fll_band_edge_state_t* f, int sps) {
    const float loop_bw = kTwoPi / (float)sps / 350.0f;
    const float damping = 0.70710678118654752440f;
    const float denom = 1.0f + 2.0f * damping * loop_bw + loop_bw * loop_bw;
    f->loop_bw = loop_bw;
    f->alpha = (4.0f * damping * loop_bw) / denom;
    f->beta = (4.0f * loop_bw * loop_bw) / denom;
    f->max_freq = 1.0f;
    f->min_freq = -1.0f;
}

static inline void
fll_reinit_state(const demod_state* d, dsd_fll_band_edge_state_t* f, int sps, int is_first_init) {
    const float excess_bw = 0.2f;
    const int filter_size = 2 * sps + 1;
    fll_band_edge_design_filter(f, sps, excess_bw, filter_size);
    fll_configure_loop_params(f, sps);

    f->phase = 0.0f;
    if (is_first_init) {
        f->freq = 0.0f;
    }
    f->delay_idx = 0;
    fll_band_edge_clear_delay(f);

    if (debug_cqpsk_enabled()) {
        const float sample_rate = (float)(d->rate_out > 0 ? d->rate_out : 24000);
        const float freq_hz = f->freq * (sample_rate / kTwoPi);
        if (is_first_init) {
            DSD_FPRINTF(stderr, "[FLL] init: sps=%d filter_size=%d loop_bw=%.6f\n", sps, filter_size, f->loop_bw);
        } else {
            DSD_FPRINTF(stderr, "[FLL] sps_change: sps=%d filter_size=%d freq=%.1fHz (preserved)\n", sps, filter_size,
                        freq_hz);
        }
    }
}

static inline void
fll_prepare_loop_context(dsd_fll_band_edge_state_t* f, fll_loop_context_t* ctx) {
    ctx->phase = f->phase;
    ctx->freq = f->freq;
    ctx->alpha = f->alpha;
    ctx->beta = f->beta;
    ctx->max_freq = f->max_freq;
    ctx->min_freq = f->min_freq;
    ctx->n_taps = f->n_taps;
    ctx->delay_r = f->delay_r;
    ctx->delay_i = f->delay_i;
    ctx->delay_idx = f->delay_idx;
}

static inline void
fll_convolve_band_edges(const dsd_fll_band_edge_state_t* f, const fll_loop_context_t* ctx, float* lower_r,
                        float* lower_i, float* upper_r, float* upper_i) {
    *lower_r = 0.0f;
    *lower_i = 0.0f;
    *upper_r = 0.0f;
    *upper_i = 0.0f;

    const int delay_base = ctx->delay_idx + ctx->n_taps;
    for (int k = 0; k < ctx->n_taps; k++) {
        const int idx = delay_base - k;
        const float dr = ctx->delay_r[idx];
        const float di = ctx->delay_i[idx];

        *lower_r += dr * f->taps_lower_r[k] - di * f->taps_lower_i[k];
        *lower_i += dr * f->taps_lower_i[k] + di * f->taps_lower_r[k];
        *upper_r += dr * f->taps_upper_r[k] - di * f->taps_upper_i[k];
        *upper_i += dr * f->taps_upper_i[k] + di * f->taps_upper_r[k];
    }
}

static inline float
fll_compute_error(float lower_r, float lower_i, float upper_r, float upper_i) {
    const float lower_mag2 = lower_r * lower_r + lower_i * lower_i;
    const float upper_mag2 = upper_r * upper_r + upper_i * upper_i;
    return clipf_limit(upper_mag2 - lower_mag2, 1.0f);
}

static inline void
fll_advance_loop(fll_loop_context_t* ctx, float error) {
    ctx->freq += ctx->beta * error;
    ctx->freq = clampf_range(ctx->freq, ctx->min_freq, ctx->max_freq);

    ctx->phase += ctx->freq + ctx->alpha * error;
    while (ctx->phase > kTwoPi) {
        ctx->phase -= kTwoPi;
    }
    while (ctx->phase < -kTwoPi) {
        ctx->phase += kTwoPi;
    }
}

static inline void
fll_process_sample(const dsd_fll_band_edge_state_t* f, fll_loop_context_t* ctx, float in_r, float in_i, float* out_r,
                   float* out_i) {
    float nco_r = 0.0f;
    float nco_i = 0.0f;
    dsd_sincosf_wrapped_two_pi(ctx->phase, &nco_i, &nco_r);

    *out_r = in_r * nco_r - in_i * nco_i;
    *out_i = in_r * nco_i + in_i * nco_r;

    ctx->delay_r[ctx->delay_idx] = *out_r;
    ctx->delay_i[ctx->delay_idx] = *out_i;
    ctx->delay_r[ctx->delay_idx + ctx->n_taps] = *out_r;
    ctx->delay_i[ctx->delay_idx + ctx->n_taps] = *out_i;

    float lower_r = 0.0f;
    float lower_i = 0.0f;
    float upper_r = 0.0f;
    float upper_i = 0.0f;
    fll_convolve_band_edges(f, ctx, &lower_r, &lower_i, &upper_r, &upper_i);

    ctx->delay_idx++;
    if (ctx->delay_idx == ctx->n_taps) {
        ctx->delay_idx = 0;
    }

    const float error = fll_compute_error(lower_r, lower_i, upper_r, upper_i);
    fll_advance_loop(ctx, error);
}

static inline void
fll_commit_loop(dsd_fll_band_edge_state_t* f, const fll_loop_context_t* ctx) {
    f->phase = ctx->phase;
    f->freq = ctx->freq;
    f->delay_idx = ctx->delay_idx;
}

} // namespace

/*
 * Reset Costas loop state for fresh carrier acquisition on retune.
 *
 * Per OP25's costas_reset() in p25_demodulator_dev.py:574-576:
 *   self.costas.set_frequency(0)
 *   self.costas.set_phase(0)
 *
 * Unlike the old combined block, we now reset BOTH phase and frequency
 * because the Costas loop operates at symbol rate after diff_phasor,
 * and there's no shared state with Gardner.
 */
extern "C" void
dsd_costas_reset(dsd_costas_loop_state_t* c) {
    if (!c) {
        return;
    }
    c->phase = 0.0f;
    c->freq = 0.0f;
    c->error = 0.0f;
    c->error_smooth = 0.0f;
    c->initialized = 0;
}

/*
 * OP25-compatible Gardner timing recovery block.
 *
 * Direct port of OP25's gardner_cc_impl::general_work() from:
 *   op25/gr-op25_repeater/lib/gardner_cc_impl.cc
 *
 * This is PURE timing recovery - NO carrier tracking, NO NCO rotation.
 * The carrier is tracked separately by the downstream Costas loop.
 *
 * Signal flow:
 *   Input: AGC'd complex samples at sample rate
 *   Processing:
 *     1. Push samples to circular delay line
 *     2. When mu accumulates past 1.0, interpolate symbol and mid-symbol
 *     3. Compute Gardner error: (last - current) * mid
 *     4. Update omega and mu
 *     5. Update lock detector (Yair Linn method)
 *   Output: Symbol-rate complex samples (timing corrected, NOT carrier corrected)
 *
 * Key differences from old combined block:
 *   - NO NCO rotation applied to input samples
 *   - NO phase error computation or Costas tracking
 *   - Output is raw symbols, not carrier-corrected
 */
extern "C" void
op25_gardner_cc(struct demod_state* d) {
    if (!d || !d->lowpassed || d->lp_len < 2 || !d->cqpsk_enable) {
        return;
    }

    ted_state_t* ted = &d->ted_state;
    if ((d->lp_len >> 1) < 4) {
        return;
    }

    const int sps = d->ted_sps > 0 ? d->ted_sps : 5;
    float omega = ted->omega;
    int is_first_init = 0;
    if (gardner_need_reinit(ted, sps, &is_first_init) && !gardner_reinit_state(d, ted, sps, is_first_init, &omega)) {
        return;
    }

    const float gain_mu = op25_gardner_gain_mu_for_state(d, ted);
    d->ted_effective_gain = gain_mu;
    const float gain_omega = 0.1f * gain_mu * gain_mu;

    gardner_loop_context_t ctx;
    gardner_init_loop_context(d, ted, gain_mu, gain_omega, &ctx);
    ctx.omega = omega;

    while (ctx.o < ctx.buf_len && ctx.i < ctx.nc) {
        if (!gardner_consume_until_ready(&ctx)) {
            break;
        }

        int half_sps = 0;
        float half_mu = 0.0f;
        gardner_compute_half_timing(ctx.mu, ctx.omega, &half_sps, &half_mu);

        float mid_r = 0.0f;
        float mid_j = 0.0f;
        float sym_r = 0.0f;
        float sym_j = 0.0f;
        if (!gardner_interpolate_symbol(&ctx, half_sps, half_mu, &mid_r, &mid_j, &sym_r, &sym_j)) {
            ctx.mu += ctx.omega;
            continue;
        }

        const float symbol_error = gardner_compute_symbol_error(ctx.last_r, ctx.last_j, sym_r, sym_j, mid_r, mid_j);
        gardner_update_lock_accum(&ctx, sym_r, sym_j, mid_r, mid_j);
        gardner_update_loop(&ctx, ted, symbol_error, sym_r, sym_j);
        ctx.last_r = sym_r;
        ctx.last_j = sym_j;
        ctx.iq_out[ctx.o++] = sym_r;
        ctx.iq_out[ctx.o++] = sym_j;
    }

    gardner_commit_loop_context(d, ted, &ctx);
}

/*
 * External differential phasor.
 *
 * y[n] = x[n] * conj(x[n-1])
 *
 * From OP25's p25_demodulator_dev.py line 408:
 *   self.diffdec = digital.diff_phasor_cc()
 *
 * This is applied AFTER Gardner timing recovery, producing raw differential
 * phase symbols for the Costas loop.
 */
extern "C" void
op25_diff_phasor_cc(struct demod_state* d) {
    if (!d || !d->lowpassed || d->lp_len < 2) {
        return;
    }

    const int pairs = d->lp_len >> 1;
    float* iq = d->lowpassed;

    float prev_r = d->cqpsk_diff_prev_r;
    float prev_j = d->cqpsk_diff_prev_j;

    for (int n = 0; n < pairs; n++) {
        const size_t nn = (size_t)n;
        float cur_r = iq[nn * 2];
        float cur_j = iq[nn * 2 + 1];

        /* y = x * conj(prev) = (cur_r + j*cur_j) * (prev_r - j*prev_j) */
        float out_r = cur_r * prev_r + cur_j * prev_j;
        float out_j = cur_j * prev_r - cur_r * prev_j;

        iq[nn * 2] = out_r;
        iq[nn * 2 + 1] = out_j;

        prev_r = cur_r;
        prev_j = cur_j;
    }

    d->cqpsk_diff_prev_r = prev_r;
    d->cqpsk_diff_prev_j = prev_j;
}

/*
 * OP25-compatible Costas loop at symbol rate.
 *
 * Direct port of OP25's costas_loop_cc_impl::work() from:
 *   op25/gr-op25_repeater/lib/costas_loop_cc_impl.cc
 *
 * This operates on DIFFERENTIALLY DECODED symbols (after diff_phasor_cc).
 * The phase detector expects OP25 differential QPSK symbols at diagonal
 * positions. dsd-neo normalizes reliable phasor magnitudes before detection,
 * weights loop updates by raw phasor magnitude, and smooths discriminator
 * error so noisy simulcast symbols do not kick the loop as hard.
 *
 * Signal flow:
 *   Input: Symbol-rate differential phasors from diff_phasor_cc
 *   Processing:
 *     1. NCO rotation: out = in * exp(-j*phase)
 *     2. Phase error detection (QPSK detector)
 *     3. Loop filter update (PI controller)
 *     4. Phase limiting to ±π/2
 *   Output: Carrier-corrected differential phasors
 *
 * Loop parameters from OP25:
 *   loop_bw = 0.008 (from p25_demodulator_dev.py line 59: _def_costas_alpha)
 *   damping = sqrt(2)/2 (critically damped)
 *   max_phase = π/2 (from p25_demodulator_dev.py line 405: TWO_PI/4)
 *
 * The update_gains() formula from costas_loop_cc_impl.cc:162-167:
 *   denom = 1.0 + 2.0*damping*loop_bw + loop_bw*loop_bw
 *   alpha = (4*damping*loop_bw) / denom
 *   beta = (4*loop_bw*loop_bw) / denom
 */
extern "C" void
op25_costas_loop_cc(struct demod_state* d) {
    if (!d || !d->lowpassed || d->lp_len < 2 || !d->cqpsk_enable) {
        return;
    }

    dsd_costas_loop_state_t* c = &d->costas_state;
    costas_init_if_needed(c);

    const int pairs = d->lp_len >> 1;
    float* iq = d->lowpassed;

    costas_loop_context_t ctx;
    costas_prepare_loop_context(c, &ctx);

    for (int n = 0; n < pairs; n++) {
        const size_t nn = (size_t)n;
        const float in_r = iq[nn * 2];
        const float in_j = iq[nn * 2 + 1];
        float out_r = 0.0f;
        float out_j = 0.0f;
        costas_process_symbol(&ctx, in_r, in_j, &out_r, &out_j);
        iq[nn * 2] = out_r;
        iq[nn * 2 + 1] = out_j;
    }

    costas_commit_loop(c, &ctx);
    costas_store_metrics(d, &ctx.metrics, pairs);
}

/*
 * Reset FLL band-edge state for fresh frequency acquisition.
 */
extern "C" void
dsd_fll_band_edge_reset(dsd_fll_band_edge_state_t* f) {
    if (!f) {
        return;
    }
    f->phase = 0.0f;
    f->freq = 0.0f;
    fll_band_edge_clear_delay(f);
    f->delay_idx = 0;
    /* Don't clear initialized or taps - they can be reused */
}

/*
 * Design band-edge filters for FLL.
 *
 * Direct port of GNU Radio's fll_band_edge_cc_impl::design_filter().
 *
 * The band-edge filters are the derivatives of the pulse shaping filter
 * at the band edges. For raised-cosine, this is a sine wave at the rolloff
 * transition, extended to a half-wave which is equivalent to the sum of
 * two sinc functions in time.
 *
 * Parameters:
 *   sps: samples per symbol
 *   rolloff: excess bandwidth (0.0 to 1.0, typically 0.2 for P25)
 *   n_taps: number of filter taps (OP25 uses 2*sps+1)
 */
static void
fll_band_edge_design_filter(dsd_fll_band_edge_state_t* f, int sps, float rolloff, int n_taps) {
    if (n_taps > FLL_BAND_EDGE_MAX_TAPS) {
        n_taps = FLL_BAND_EDGE_MAX_TAPS;
    }
    if (n_taps < 3) {
        n_taps = 3;
    }

    f->n_taps = n_taps;
    f->sps = sps;

    /* GNU Radio fll_band_edge_cc_impl::design_filter() exact port:
     *
     * The baseband filter is built using:
     *   M = round(filter_size / samps_per_sym)
     *   k = -M + i * 2.0 / samps_per_sym
     *   tap = sinc(rolloff * k - 0.5) + sinc(rolloff * k + 0.5)
     *
     * Then normalized by power, and modulated to band edges:
     *   freq = (-N + i) / (2.0 * samps_per_sym) where N = (filter_size - 1) / 2
     *   lower = tap * exp(-j * 2π * (1 + rolloff) * freq)
     *   upper = tap * exp(+j * 2π * (1 + rolloff) * freq)
     *
     * Taps are stored in REVERSE order for the FIR implementation.
     */

    float M = roundf((float)n_taps / (float)sps);
    int N = (n_taps - 1) / 2;

    /* Build baseband filter taps - use stack array since n_taps <= FLL_BAND_EDGE_MAX_TAPS */
    float bb[FLL_BAND_EDGE_MAX_TAPS];
    float power = 0.0f;

    for (int i = 0; i < n_taps; i++) {
        /* k = -M + i * 2.0 / samps_per_sym */
        float k = -M + (float)i * 2.0f / (float)sps;

        /* sinc(rolloff * k - 0.5) + sinc(rolloff * k + 0.5) */
        float arg_m = rolloff * k - 0.5f;
        float arg_p = rolloff * k + 0.5f;

        float sinc_m, sinc_p;
        if (fabsf(arg_m) < 1e-6f) {
            sinc_m = 1.0f;
        } else {
            sinc_m = sinf(kPi * arg_m) / (kPi * arg_m);
        }
        if (fabsf(arg_p) < 1e-6f) {
            sinc_p = 1.0f;
        } else {
            sinc_p = sinf(kPi * arg_p) / (kPi * arg_p);
        }

        bb[i] = sinc_m + sinc_p;
        power += bb[i] * bb[i];
    }

    /* Normalize by power (GNU Radio fll_band_edge_cc_impl::design_filter()).
     *
     * Note: GNU Radio divides by the sum of squares (power), not sqrt(power).
     * This affects the band-edge error magnitude and thus the effective loop gain. */
    if (power > 0.0f) {
        float norm = 1.0f / power;
        for (int i = 0; i < n_taps; i++) {
            bb[i] *= norm;
        }
    }

    /* Create band-edge filters by modulating to upper and lower band edges.
     * GNU Radio stores taps in REVERSE order, so we do the same.
     * freq = (-N + i) / (2.0 * samps_per_sym)
     * phase = 2π * (1 + rolloff) * freq
     */
    for (int i = 0; i < n_taps; i++) {
        float freq = (float)(-N + i) / (2.0f * (float)sps);
        float phase = kTwoPi * (1.0f + rolloff) * freq;

        /* Lower band edge: exp(-j * phase) - store in reverse order.
         * rev_idx ranges from n_taps-1 down to 0 as i goes 0 to n_taps-1. */
        size_t rev_idx = (size_t)(n_taps - 1 - i);
        f->taps_lower_r[rev_idx] = bb[i] * cosf(-phase);
        f->taps_lower_i[rev_idx] = bb[i] * sinf(-phase);

        /* Upper band edge: exp(+j * phase) - store in reverse order */
        f->taps_upper_r[rev_idx] = bb[i] * cosf(phase);
        f->taps_upper_i[rev_idx] = bb[i] * sinf(phase);
    }

    f->initialized = 1;
}

/*
 * Initialize FLL band-edge filter for a given samples-per-symbol.
 *
 * Designs the band-edge filters and sets loop parameters upfront, avoiding
 * the lazy initialization in op25_fll_band_edge_cc() that can cause poor
 * acquisition on the first few sample blocks after cold start or retune.
 *
 * This should be called during demod_reset_on_retune() when CQPSK is enabled
 * and the SPS is known.
 *
 * OP25 behavior: On SPS changes (P25p1 CC -> P25p2 VC), OP25 preserves the
 * FLL state (freq, phase, delay line) to maintain lock during transitions.
 * We adopt the same approach here.
 */
extern "C" void
dsd_fll_band_edge_init(dsd_fll_band_edge_state_t* f, int sps) {
    if (!f || sps < 1) {
        return;
    }

    int is_first_init = !f->initialized;
    int is_sps_change = f->initialized && f->sps != sps && f->sps > 0;

    /* Debug: log FLL init when DSD_NEO_DEBUG_CQPSK=1 */
    if (debug_cqpsk_enabled()) {
        if (is_first_init) {
            DSD_FPRINTF(stderr, "[FLL-INIT] first init sps=%d\n", sps);
        } else if (is_sps_change) {
            DSD_FPRINTF(stderr, "[FLL-INIT] sps change %d->%d (freq preserved)\n", f->sps, sps);
        } else {
            DSD_FPRINTF(stderr, "[FLL-INIT] retune reset sps=%d (freq preserved)\n", sps);
        }
    }

    /* Redesign filters only when SPS changes or on first init.
     * On same-SPS retune, keep existing filters but reset phase/delay. */
    if (is_first_init || is_sps_change) {
        /* OP25 parameters from p25_demodulator_dev.py line 403:
         *   self.fll = digital.fll_band_edge_cc(sps, excess_bw, 2*sps+1, TWO_PI/sps/350)
         *
         * excess_bw = 0.2 (from line ~60: _def_excess_bw = 0.2)
         * filter_size = 2*sps+1 = 11 for sps=5, 9 for sps=4
         * loop_bw = TWO_PI / sps / 350
         */
        float excess_bw = 0.2f;
        int filter_size = 2 * sps + 1;
        float loop_bw = kTwoPi / (float)sps / 350.0f;

        /* Design the band-edge filters for the new SPS.
         *
         * GNU Radio's set_samples_per_symbol() redesigns filters but preserves
         * the frequency estimate. This is critical because:
         * 1. Band-edge frequencies are SPS-dependent: ±(1+rolloff)/(2*sps)
         * 2. The LO offset (tracked by freq) is independent of symbol rate
         *
         * Filter redesign: 5 sps -> band-edges at ±0.12 normalized
         *                  4 sps -> band-edges at ±0.15 normalized
         * Using wrong filters causes garbage band-edge energy estimates. */
        fll_band_edge_design_filter(f, sps, excess_bw, filter_size);

        /* Set loop parameters using GNU Radio's control_loop update_gains() formula */
        f->loop_bw = loop_bw;
        float damping = 0.70710678118654752440f; /* sqrt(2)/2 - critically damped */
        float denom = 1.0f + 2.0f * damping * loop_bw + loop_bw * loop_bw;
        f->alpha = (4.0f * damping * loop_bw) / denom;
        f->beta = (4.0f * loop_bw * loop_bw) / denom;
        f->max_freq = 1.0f; /* rad/sample limit */
        f->min_freq = -1.0f;
    }

    /* State reset behavior - PRESERVE frequency, reset phase and delay.
     *
     * OP25 rx.py set_freq() calls demod.reset() which only resets the Costas
     * loop, NOT the FLL. The FLL frequency estimate is preserved across
     * hardware retunes.
     *
     * This works because the FLL tracks the RTL-SDR's local oscillator offset,
     * which is primarily determined by crystal PPM error. For an RTL-SDR with
     * 10 ppm error, the offset scales linearly with frequency:
     *   - 769 MHz: ~7.69 kHz offset
     *   - 771 MHz: ~7.71 kHz offset
     *   - Difference: ~20 Hz
     *
     * The FLL can easily track a 20 Hz change, but reacquiring from 0 Hz to
     * 200+ Hz is much slower and may fail on P25p2 TDMA bursts.
     *
     * Reset:
     *   - Phase: new samples have no phase relationship to old
     *   - Delay lines: must be cleared when filters are redesigned
     *   - Frequency: PRESERVED (critical for fast reacquisition)
     */
    f->phase = 0.0f;
    /* f->freq preserved - LO offset is similar across nearby frequencies */
    f->delay_idx = 0;
    fll_band_edge_clear_delay(f);
}

/*
 * OP25-compatible FLL band-edge frequency lock loop.
 *
 * Direct port of GNU Radio's digital.fll_band_edge_cc as used in OP25:
 *   self.fll = digital.fll_band_edge_cc(sps, excess_bw, 2*sps+1, TWO_PI/sps/350)
 *
 * The FLL operates BEFORE timing recovery to correct coarse frequency offset.
 * This is critical for initial acquisition after channel retunes.
 */
extern "C" void
op25_fll_band_edge_cc(struct demod_state* d) {
    if (!d || !d->lowpassed || d->lp_len < 2 || !d->cqpsk_enable) {
        return;
    }

    dsd_fll_band_edge_state_t* f = &d->fll_band_edge_state;
    const int sps = d->ted_sps > 0 ? d->ted_sps : 5;

    int is_first_init = 0;
    if (fll_need_reinit(f, sps, &is_first_init)) {
        fll_reinit_state(d, f, sps, is_first_init);
    }

    const int pairs = d->lp_len >> 1;
    float* iq = d->lowpassed;

    fll_loop_context_t ctx;
    fll_prepare_loop_context(f, &ctx);

    for (int n = 0; n < pairs; n++) {
        const size_t nn = (size_t)n;
        const float in_r = iq[nn * 2];
        const float in_i = iq[nn * 2 + 1];
        float out_r = 0.0f;
        float out_i = 0.0f;
        fll_process_sample(f, &ctx, in_r, in_i, &out_r, &out_i);
        iq[nn * 2] = out_r;
        iq[nn * 2 + 1] = out_i;
    }

    fll_commit_loop(f, &ctx);

    /* Debug: Log FLL band-edge state when DSD_NEO_DEBUG_CQPSK=1 */
    {
        static int call_count = 0;
        if (debug_cqpsk_enabled() && (++call_count % 50) == 0) {
            static float prev_freq = 0.0f;
            /* Convert freq rad/sample to Hz: f_hz = freq * Fs / (2π) */
            float Fs = (float)d->rate_out;
            float freq_hz = ctx.freq * Fs / kTwoPi;
            float delta_freq_hz = (ctx.freq - prev_freq) * Fs / kTwoPi;
            prev_freq = ctx.freq;
            /* Estimate "locked" heuristic: freq change is small */
            const char* lock_status = (fabsf(delta_freq_hz) < 10.0f) ? "locked" : "tracking";
            DSD_FPRINTF(stderr, "[FLL-BE] freq:%.1fHz delta:%.2fHz phase:%.3f alpha:%.6f beta:%.9f (%s)\n", freq_hz,
                        delta_freq_hz, ctx.phase, f->alpha, f->beta, lock_status);
        }
    }
}
