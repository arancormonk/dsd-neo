// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The noise the dynamic squelches see through a replay or network I/Q reader's resampler (issue #899).
 *
 * A replay or an I/Q stream whose rate the half-band cascade cannot divide down to the DSP rate is resampled by its
 * reader first (dsd_iq_reader_plan, issues #701 and #743). The noise that reaches the channel filter is then not white
 * noise through the cascade: the reader band-limits it to its passband edge (0.45 of the source rate when it
 * upsamples), and its L-phase bank makes it periodic, its statistics repeating every L outputs. This model describes
 * that noise for one reader plan and pass count:
 *
 * - The reader itself: a design of the plan's own (dsd_iq_reader_plan_configure(), the same bank bit for bit as the
 *   reader's), never the reader's, which runs on the IO thread.
 * - White complex noise at the source rate, variance 2^(passes - 1) (1 with no pass; the convention of the plans
 *   without a reader, so noise gains are on one scale), through the bank: the covariance of every one of the L output
 *   phases, from the taps.
 * - Then the half-band cascade, phase by phase (dsd_squelch_cascade_noise(), src/dsp/squelch_cascade_noise.h), with the
 *   reader's output phases lined up on the samples the real decimators keep: each stage's output n is centred on its
 *   input 2n, counted from the stream's start, which the reader and the cascade start over at together. The period
 *   left at the channel filter is P = L / gcd(L, 2^passes) (40 kHz to 32 kHz by 4/5, then one pass: 2 phases).
 * - The covariance averaged over that period, and the largest distance of any phase's from it.
 * - For every output phase in full: the response to an input at each of the Q = M 2^passes / gcd(L, 2^passes) input
 *   phases (shifting an input by Q shifts its response by P outputs), through the reader's taps and the half-band
 *   stages as the decimators run them. With the channel taps these give every output phase's covariance at a cost of
 *   about Q x (responses' span)^2 / 2, not P x lags x taps^2.
 *
 * dsd_squelch_reader_model_build() makes all of it on the controller thread while the demod thread is idle (the rate
 * plan's commit, dsd_demod_squelch_reader_model_sync()), and keeps it while the plan and passes stay: a build for the
 * model it holds does nothing, and a channel width change reuses it. The plan designs then run on the demod thread
 * when a squelch plan key changes, allocating nothing:
 *
 * - dsd_squelch_reader_model_floor_plan(): the averaged covariance through the channel taps (exact for the mean: the
 *   taps are time-invariant) with the bound the step model uses on how far an output phase's covariance can stray from
 *   it after the taps (the phases' largest distance times (sum |c|)^2, over the output power). Under 1e-2 the plan is
 *   designed at a period of one; past it, from every output phase in full
 *   (dsd_squelch_reader_model_floor_plan_exact()). An upsampling reader's bank strays by about 2 % (8 kHz to 16 kHz:
 *   2.03 %), so it takes the full periodic design.
 * - dsd_squelch_reader_model_noise_plan(): the noise squelch's design with the reader's passband edge and magnitude
 *   (dsd_noise_squelch_plan_design_reader()).
 *
 * The reader and the live I/Q rate step never run together: a replay keeps its recorded chain, live step included,
 * only when it reads the capture at its own rate (the reader then passes it through), and every chain the reader
 * resamples runs no step. Zeroed storage is an empty model (not ready), like dsd_squelch_reader_model_init().
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_DSP_SQUELCH_READER_MODEL_H
#define DSD_NEO_INCLUDE_DSD_NEO_DSP_SQUELCH_READER_MODEL_H

#include <dsd-neo/dsp/iq_resampler.h>
#include <dsd-neo/dsp/nfm_noise_squelch.h>
#include <dsd-neo/dsp/squelch_floor.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief The model for one reader plan behind one cascade: what dsd_squelch_reader_model_build() makes. */
typedef struct dsd_squelch_reader_model {
    int ready;               /**< 1 once built for plan and passes */
    dsd_iq_reader_plan plan; /**< The reader it models */
    int passes;              /**< The half-band passes after it */
    dsd_iq_resampler reader; /**< The plan's reader design, run by the plan designs only */
    int period;              /**< P: the output phases at the channel filter */
    int lags;                /**< The covariance's lags there, 0 included; 0 beyond */
    double* mean;            /**< The covariance averaged over the period (lags of it) */
    double cyclo_dev;        /**< The largest absolute distance of any phase's covariance from it */
    /* The responses: one per input phase k (an input at k), resp_len outputs from an output at phase resp_phase[k] of
       the period. */
    int inputs; /**< Q: the input phases */
    int resp_len;
    int* resp_phase;
    double* resp;
    /* The exact plan's scratch, sized for DSD_NOISE_SQUELCH_MAX_TAPS channel taps: every output phase's covariance
       (period rows of cov_lags), and every response through the channel taps (inputs rows of cov_lags). */
    int cov_lags;
    double* cov;
    double* chan;
    /* The averaged plan's scratch: the mean covariance through the channel taps (lags +
       DSD_NOISE_SQUELCH_MAX_TAPS - 1), and the taps' autocorrelation. */
    double* mean_chan;
    double rc[DSD_NOISE_SQUELCH_MAX_TAPS];
} dsd_squelch_reader_model;

/** Initialise @p m: empty, not ready. */
void dsd_squelch_reader_model_init(dsd_squelch_reader_model* m);

/** Release @p m's storage and the reader design; it is empty again. */
void dsd_squelch_reader_model_release(dsd_squelch_reader_model* m);

/** Whether @p m is built for @p plan (dsd_iq_reader_plan_equal(), enabled) behind @p passes half-band passes. */
int dsd_squelch_reader_model_matches(const dsd_squelch_reader_model* m, const dsd_iq_reader_plan* plan, int passes);

/**
 * @brief Build @p m for the reader @p plan ahead of the half-band cascade @p stages (@p stage_count, top first, as
 * full_demod_apply_halfband_decimation() runs them).
 *
 * Does nothing when @p m is already built for @p plan and @p stage_count passes. Anything else is released first, so a
 * refusal leaves @p m empty. Run it on the controller thread while the demod thread is idle.
 *
 * @return 0, or -1: bad arguments (a disabled plan, a stage with no taps or more than 64), a design the resampler
 * refuses, a model past the size limit (8 Mi doubles for any one table), or no memory.
 */
int dsd_squelch_reader_model_build(dsd_squelch_reader_model* m, const dsd_iq_reader_plan* plan,
                                   const dsd_noise_squelch_stage* stages, int stage_count);

/**
 * @brief The auto squelch's plan for the channel taps @p taps (@p taps_len; NULL or 0 when no channel filter runs)
 * behind @p m's reader and cascade, at the channel rate @p rate_hz (the reader's output rate over 2^passes):
 * dsd_squelch_floor_plan_design_periodic_mean() on the covariance averaged over the period while the bound on how far
 * an output phase's strays from it stays under 1e-2, else on every output phase's (see the file comment). Allocates
 * nothing.
 *
 * @return 0, or -1 (plan not valid): @p m not ready, another rate, more than DSD_NOISE_SQUELCH_MAX_TAPS taps, or a plan
 * the design refuses.
 */
int dsd_squelch_reader_model_floor_plan(dsd_squelch_reader_model* m, dsd_squelch_floor_plan* out, const float* taps,
                                        int taps_len, int rate_hz);

/**
 * @brief dsd_squelch_reader_model_floor_plan() from every output phase's covariance in full, whatever the bound: the
 * reference the averaged path is held to, and its fallback. Allocates nothing.
 *
 * @return As dsd_squelch_reader_model_floor_plan().
 */
int dsd_squelch_reader_model_floor_plan_exact(dsd_squelch_reader_model* m, dsd_squelch_floor_plan* out,
                                              const float* taps, int taps_len, int rate_hz);

/**
 * @brief The noise squelch's plan for the channel taps @p taps (@p taps_len; NULL or 0 when no channel filter runs)
 * behind @p m's reader and the half-band @p stages (@p stage_count), at the channel rate @p rate_hz:
 * dsd_noise_squelch_plan_design_reader() with @p m's reader design. Allocates nothing.
 *
 * @return 0, or -1 (plan not valid): @p m not ready, or a plan the design refuses (a band the reader's passband edge
 * leaves narrower than the noise squelch takes, among others).
 */
int dsd_squelch_reader_model_noise_plan(const dsd_squelch_reader_model* m, dsd_noise_squelch_plan* out,
                                        const float* taps, int taps_len, const dsd_noise_squelch_stage* stages,
                                        int stage_count, int rate_hz);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_DSP_SQUELCH_READER_MODEL_H */
