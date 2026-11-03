// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The noise the dynamic squelches see through the live I/Q rate step (issue #806).
 *
 * The auto and noise squelches are calibrated on the noise that reaches the channel filter. With no step that is
 * white noise through the half-band cascade at twice the channel rate, which their own designs model. A step between
 * the cascade and the channel filter breaks that: the cascade runs at twice the step's input rate, and the step is a
 * periodically varying filter (its phase repeats every L outputs). This model describes that noise for one step plan:
 *
 * - R_hb, the autocorrelation at hb_out of white complex noise at the capture rate through the cascade, exactly, from
 *   the half-band recursion R_{i+1}[k] = sum_j g_i[j] R_i[2k - j], g_i the autocorrelation of stage i's taps. The
 *   input's variance is 2^(passes - 1) (1 with no pass): in the passband that is unit white noise at the last stage's
 *   input, what the designs without a step assume, so noise gains are on their scale.
 * - The spectral factor of R_hb: a zero-phase FIR whose autocorrelation is R_hb, from sqrt(DFT(R_hb)) on 4096 points,
 *   cut to the shortest odd length (at most DSD_SQUELCH_STEP_MAX_FACTOR_TAPS) whose Kaiser-windowed taps (40 dB) leave
 *   out or change less than 1e-6 of its energy, and scaled to R_hb[0]. Complex Gaussian noise through it is the
 *   cascade's noise.
 * - The step itself: a design of the plan's own (dsd_iq_resampler_configure_ratio() with the plan's key, the same taps
 *   bit for bit as the live step's), never the live one, which runs on the demod thread.
 * - The step's responses to the factor started at every input phase (one input period, M phases), for the floor
 *   model's covariance.
 *
 * dsd_squelch_step_model_build() makes all of it on the controller thread while the demod thread is paused (the rate
 * chain's transaction), and keeps it while the plan stays: a build for the plan it holds does nothing. The two plan
 * designs then run on the demod thread when a squelch plan key changes, allocating nothing:
 *
 * - dsd_squelch_step_model_floor_plan(): the factor's responses through the step and the channel taps give the exact
 *   covariance E{z_n z_{n+m}} of every output phase n (it repeats every L outputs), and the auto squelch's
 *   dsd_squelch_floor_plan_design_periodic() takes N_eff, beta and rho at every 40 ms window start phase from it.
 * - dsd_squelch_step_model_noise_plan(): the noise squelch's 2 s calibration runs on fixed-seed noise through the factor
 *   and this model's step (dsd_noise_squelch_plan_design_stepped()).
 *
 * Zeroed storage is an empty model (not ready), like dsd_squelch_step_model_init().
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_DSP_SQUELCH_STEP_MODEL_H
#define DSD_NEO_INCLUDE_DSD_NEO_DSP_SQUELCH_STEP_MODEL_H

#include <dsd-neo/dsp/iq_resampler.h>
#include <dsd-neo/dsp/live_iq_step.h>
#include <dsd-neo/dsp/nfm_noise_squelch.h>
#include <dsd-neo/dsp/squelch_floor.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    /** Most lags R_hb can span: a cascade's support settles near its widest stage's taps (64 at most). */
    DSD_SQUELCH_STEP_MAX_HB_LAGS = 64,
    /** Most taps of the spectral factor. */
    DSD_SQUELCH_STEP_MAX_FACTOR_TAPS = 255,
};

/** @brief The model for one step plan: what dsd_squelch_step_model_build() makes. */
typedef struct dsd_squelch_step_model {
    int ready;                  /**< 1 once built for plan */
    dsd_live_iq_step_plan plan; /**< The step plan it models */
    int hb_lags;                /**< R_hb[0..hb_lags - 1]; 0 beyond */
    double r_hb[DSD_SQUELCH_STEP_MAX_HB_LAGS];
    int factor_len; /**< The spectral factor's taps, odd */
    double factor[DSD_SQUELCH_STEP_MAX_FACTOR_TAPS];
    dsd_iq_resampler step; /**< The plan's step design, run by the plan designs only */
    /* The factor's responses through the step, one per input phase k (started at input k x spacing, spacing coprime to
       M): resp_len outputs each from output floor(k x spacing x L / M). */
    int spacing;
    int resp_len;
    float* resp;
    /* The floor model's scratch, sized for DSD_NOISE_SQUELCH_MAX_TAPS channel taps: the covariance of every output
       phase (L rows of cov_lags), and one response through the channel taps. */
    int cov_lags;
    double* cov;
    double* chan;
} dsd_squelch_step_model;

/** Initialise @p m: empty, not ready. */
void dsd_squelch_step_model_init(dsd_squelch_step_model* m);

/** Release @p m's storage and the step design; it is empty again. */
void dsd_squelch_step_model_release(dsd_squelch_step_model* m);

/** Whether @p m is built for @p plan (dsd_live_iq_step_plan_equal()). */
int dsd_squelch_step_model_matches(const dsd_squelch_step_model* m, const dsd_live_iq_step_plan* plan);

/**
 * @brief Build @p m for the stepping @p plan behind the half-band cascade @p stages (@p stage_count, top first, as
 * full_demod_apply_halfband_decimation() runs them; plan->passes of them).
 *
 * Does nothing when @p m is already built for @p plan. Anything else is released first, so a refusal leaves @p m
 * empty. Run it on the controller thread while the demod thread is paused.
 *
 * @return 0, or -1: bad arguments (a plan that does not step, a stage count that is not the plan's passes, a stage
 * with no taps or more than 64), a spectral factor that needs more than DSD_SQUELCH_STEP_MAX_FACTOR_TAPS taps, a step
 * design the resampler refuses, or no memory.
 */
int dsd_squelch_step_model_build(dsd_squelch_step_model* m, const dsd_live_iq_step_plan* plan,
                                 const dsd_noise_squelch_stage* stages, int stage_count);

/**
 * @brief The auto squelch's plan for the channel taps @p taps (@p taps_len; NULL or 0 when no channel filter runs)
 * behind @p m's step, at the channel rate @p rate_hz (the step's target): dsd_squelch_floor_plan_design_periodic() on
 * the exact covariance of every output phase. Allocates nothing.
 *
 * @return 0, or -1 (plan not valid): @p m not ready, more than DSD_NOISE_SQUELCH_MAX_TAPS taps, or a plan the design
 * refuses.
 */
int dsd_squelch_step_model_floor_plan(dsd_squelch_step_model* m, dsd_squelch_floor_plan* out, const float* taps,
                                      int taps_len, int rate_hz);

/**
 * @brief The noise squelch's plan for the channel taps @p taps (@p taps_len; NULL or 0 when no channel filter runs)
 * behind @p m's step, at the channel rate @p rate_hz: dsd_noise_squelch_plan_design_stepped() with @p m's factor and
 * step. Allocates nothing; the step design is cleared before and after.
 *
 * @return 0, or -1 (plan not valid): @p m not ready, or a plan the design refuses.
 */
int dsd_squelch_step_model_noise_plan(dsd_squelch_step_model* m, dsd_noise_squelch_plan* out, const float* taps,
                                      int taps_len, int rate_hz);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_DSP_SQUELCH_STEP_MODEL_H */
