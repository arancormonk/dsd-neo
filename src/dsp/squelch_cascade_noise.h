// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The covariance of the noise the dynamic squelches see, through the half-band cascade and the channel taps
 * (module-private; issue #899). One implementation of the half-band recursion for both users: the step model's R_hb
 * (src/dsp/squelch_step_model.c) and the auto squelch's plan on a chain with no step (squelch_floor_ensure_plan() in
 * src/dsp/demod_pipeline.cpp). Pure: no allocation, the caller owns every buffer.
 *
 * Noise is described as dsd_squelch_floor_periodic_noise: its covariance E{z_n z_{n+m}} at every phase n of its period
 * and every lag m from 0 up to lags - 1 (zero beyond), row n at cov[n * lags]. It is real (real taps on circular
 * noise), so a negative lag is the other sample's row: R(n, n - m) = R(n - m, n). Stationary noise has a period of one.
 *
 * A half-band stage takes y_q = sum_i h_i x_{2q - i}: output q is the stage's sum at input 2q (input phase 0 is output
 * phase 0), so R'(q, q + m) = sum_i sum_j h_i h_j R(2q - i, 2q + 2m - j), and a period P becomes P / gcd(P, 2). On
 * stationary noise that is R'[m] = sum_j g[j] R[2m - j], g the taps' autocorrelation: what the step model has always
 * run. A stage of len taps maps a support of s lags beyond 0 to (s + len - 1) / 2: from white noise, under 64 lags
 * for any cascade of stages of at most 64 taps.
 */

#ifndef DSD_NEO_SRC_DSP_SQUELCH_CASCADE_NOISE_H_
#define DSD_NEO_SRC_DSP_SQUELCH_CASCADE_NOISE_H_

#include <dsd-neo/dsp/nfm_noise_squelch.h>
#include <dsd-neo/dsp/squelch_floor.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    /** Most taps a half-band stage may have. */
    DSD_SQUELCH_CASCADE_MAX_STAGE_TAPS = 64,
    /** Most lags white noise through any cascade of such stages spans (its support settles under 63). */
    DSD_SQUELCH_CASCADE_WHITE_LAGS = 64,
};

/**
 * @brief The doubles of workspace dsd_squelch_cascade_noise() needs for noise of @p in_period phases and @p in_lags
 * lags (1 and 1 for white noise) through @p stages (@p count of them): twice the largest of the input and every
 * stage's output (period x lags), the two halves it alternates between. 0 for bad arguments (among them @p in_lags past
 * INT_MAX - DSD_SQUELCH_CASCADE_MAX_STAGE_TAPS, where a stage's lag count could overflow).
 */
size_t dsd_squelch_cascade_noise_work_len(int in_period, int in_lags, const dsd_noise_squelch_stage* stages, int count);

/**
 * @brief The covariance of noise after the half-band @p stages (@p count, top first, as
 * full_demod_apply_halfband_decimation() runs them), each decimating by 2.
 *
 * @p in is the noise ahead of the first stage (any period), or NULL for white complex noise of variance 2^(count - 1)
 * (1 with no stage): the scale the step model and the floor plan use, unit white noise at the last stage's input in
 * the passband. The result lands in @p work (@p work_len doubles, dsd_squelch_cascade_noise_work_len()), and @p out
 * describes it (its cov points into @p work; with no stage it is a copy of the input).
 *
 * @return 0, or -1: bad arguments, a stage with no taps or more than DSD_SQUELCH_CASCADE_MAX_STAGE_TAPS, or too
 * little workspace.
 */
int dsd_squelch_cascade_noise(const dsd_squelch_floor_periodic_noise* in, const dsd_noise_squelch_stage* stages,
                              int count, double* work, size_t work_len, dsd_squelch_floor_periodic_noise* out);

/**
 * @brief The covariance of noise @p in after the channel taps (@p taps, @p taps_len; no decimation): R'(n, n + m) =
 * sum_i sum_j c_i c_j R(n - i, n + m - j), the input's period and in->lags + taps_len - 1 lags, into @p out_cov
 * (@p out_len doubles: at least period x those lags), which @p out describes.
 *
 * Stationary noise takes the taps' autocorrelation, written to @p rc (@p rc_len doubles, at least @p taps_len; may be
 * NULL and 0 for periodic noise): R'[m] = sum_d R_c(d) R[m - d].
 *
 * @return 0, or -1: bad arguments, too small an rc for stationary noise, or too small an output.
 */
int dsd_squelch_cascade_noise_through_taps(const dsd_squelch_floor_periodic_noise* in, const float* taps, int taps_len,
                                           double* rc, size_t rc_len, double* out_cov, size_t out_len,
                                           dsd_squelch_floor_periodic_noise* out);

/**
 * @brief The auto squelch's plan on a chain with no live step (issue #899): white complex noise of variance
 * 2^(count - 1) at the capture rate through the half-band @p stages and the channel taps (@p taps, @p taps_len; NULL or
 * 0 when no channel filter runs), designed at a period of one with the tracker's mean 40 ms window,
 * dsd_squelch_floor_plan_design_periodic_mean(). The lag follows the channel taps; noise_gain is the output power on
 * that scale, the step model's, so plans with and without a step for the same chain agree. Allocates nothing (its
 * workspace, about 6 KB, is on the stack).
 *
 * @return 0, or -1 (plan not valid): more than DSD_NOISE_SQUELCH_MAX_TAPS channel taps, a cascade
 * dsd_squelch_cascade_noise() refuses, or a plan the design refuses.
 */
int dsd_squelch_cascade_noise_floor_plan(dsd_squelch_floor_plan* out, const float* taps, int taps_len,
                                         const dsd_noise_squelch_stage* stages, int count, int rate_hz);

/**
 * @brief Whether periodic noise whose phases' covariances stray from their period mean by at most @p cyclo_dev (the
 * largest absolute distance at any phase and lag) may be designed from that mean after the channel taps (@p taps,
 * @p taps_len): the bound on how far any output phase's covariance can then stray, cyclo_dev x (sum |c|)^2, stays
 * under 1e-2 of the mean output power @p power. The per-phase results then average to the mean's within about the
 * square of it. The rule the step model and the reader model both apply (issues #806, #899).
 *
 * @return 1 when the mean suffices, 0 when every output phase must be taken in full (a bound past 1e-2, no power, or
 * no taps).
 */
int dsd_squelch_cascade_mean_suffices(double cyclo_dev, const float* taps, int taps_len, double power);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_SRC_DSP_SQUELCH_CASCADE_NOISE_H_ */
