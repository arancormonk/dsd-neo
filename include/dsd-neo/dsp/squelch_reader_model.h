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
 *   A limit: where one starts over without the other, the decimators can keep another of the gcd(L, 2^passes) cosets of
 *   the reader's phases until the next common start, which the plan does not follow. The cascade alone: an FM <-> AM
 *   switch, a digital -> analog switch and an analog -> digital one on a reader chain (the last costs nothing while the
 *   digital chain runs no dynamic squelch, until the 9600 bit/s FSK path or the next analog entry runs on the alignment
 *   it left); on an I/Q stream, the blocks a reconnect's or resume's purge takes after the reader's restart, and a drop
 *   whose gap mark the 33rd pending one moved (the reader restarts, the demod does not). The reader alone: a replay's
 *   RETUNE, and a MUTE that marks no gap (a retune_mute span with no RESET at its sample). No external landing reaches
 *   a replay or I/Q stream, whose tunes the engine refuses. Each is a shift of the alignment, so the worst over every
 *   coset bounds it: 1.2e-4 of a plan over 9,530 readers the rate plans make (every pass count, sources at every 1 % of
 *   the octave), unfiltered and through the analog channel filters (60 kHz read into 48 kHz ahead of two passes,
 *   unfiltered; 5.4e-5 at 112 kHz into 64 kHz). A block an I/Q stream's full ring drops restarts the reader with the
 *   cascade (rtl_net_iq.cpp): exact for an isolated drop, fewer than 33 gap marks pending.
 * - The covariance at every one of those phases (phase_cov), averaged over the period, and the largest distance of any
 *   phase's from the average.
 * - For every output phase in full, two exact routes through the channel taps (dsd_squelch_reader_route): the
 *   response to an input at each of the Q = M 2^passes / gcd(L, 2^passes) input phases (shifting an input by Q shifts
 *   its response by P outputs), through the reader's taps and the half-band stages as the decimators run them, about
 *   Q x (responses' span)^2 / 2; or phase_cov through the taps, about P x taps x (lags + taps), whatever Q is. A design
 *   takes the cheaper, so every reader the rate plans make has one, in bounded tables.
 *
 * dsd_squelch_reader_model_build() makes all of it on the controller thread while the demod thread is idle (the rate
 * plan's commit on a chain a dynamic squelch runs on, dsd_demod_squelch_reader_model_sync(), or a switch onto one,
 * dsd_demod_squelch_reader_model_prepare()), and keeps it while the plan and passes stay: a build for the
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

#ifdef __cplusplus
extern "C" {
#endif

/** @brief The routes the every-phase design can take (dsd_squelch_reader_model_floor_plan_route()); both exact. */
typedef enum {
    /** The cheaper of the two for the channel taps, by their multiply-add counts. */
    DSD_SQUELCH_READER_ROUTE_CHEAPER = 0,
    /** Every input phase's response through the channel taps, in bounded batches: about Q x (span + taps)^2 / 2. */
    DSD_SQUELCH_READER_ROUTE_RESPONSES = 1,
    /** The covariance every output phase has ahead of the channel filter, through its taps: about P x taps x (lags +
        taps), whatever Q is. */
    DSD_SQUELCH_READER_ROUTE_COVARIANCE = 2,
} dsd_squelch_reader_route;

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
    /** Every output phase's covariance there: period rows of lags (released, NULL, with exact_ready 0) */
    double* phase_cov;
    /* The every-phase design's tables, bounded whatever the reader (exact_ready 1 once built; a reader past the size
       limit, or no memory for them, leaves the averaged plan only, and a plan its bound refuses is then not valid). */
    int exact_ready;
    int inputs;   /**< Q: the input phases */
    int resp_len; /**< The longest span a response takes, at the reader or after any stage */
    /** The lags a row of cov holds: the longer of resp_len and lags, plus DSD_NOISE_SQUELCH_MAX_TAPS - 1 */
    int cov_lags;
    double* cov; /**< The last every-phase design's covariance: period rows of exact_lags */
    int exact_lags;
    /* The responses route: the stages' taps (copied), a response's two halves, and a batch of responses through the
       channel taps (batch rows of cov_lags) with the output phase each starts at. */
    dsd_noise_squelch_stage stages[DSD_NOISE_SQUELCH_MAX_STAGES];
    float* stage_taps;
    double* resp_a;
    double* resp_b;
    int batch;
    int* resp_phase;
    double* chan;
    /* The covariance route: the rows it carries through the taps, DSD_NOISE_SQUELCH_MAX_TAPS of ring_len (an output
       phase's covariance ahead of the filter, through it, at every offset). */
    int ring_len;
    double* ring;
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
 * refuses, a reader covariance or cascade workspace past the size limit (8 Mi doubles for any one table), or no
 * memory. The every-phase tables (the covariance a design writes, a batch of responses of at most 1 MB, one row per
 * channel tap) do not grow with Q; past the limit, or with no memory for them, they leave the model ready for the
 * averaged plan only (exact_ready 0). The largest model the rate plans make retains about 35 MB (8,046 Hz read into
 * 72 kHz, 4000 output phases).
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
 * reference the averaged path is held to, and its fallback. dsd_squelch_reader_model_floor_plan_route() with
 * DSD_SQUELCH_READER_ROUTE_CHEAPER. Allocates nothing.
 *
 * @return As dsd_squelch_reader_model_floor_plan(), and -1 when the every-phase tables were not built (exact_ready 0).
 */
int dsd_squelch_reader_model_floor_plan_exact(dsd_squelch_reader_model* m, dsd_squelch_floor_plan* out,
                                              const float* taps, int taps_len, int rate_hz);

/**
 * @brief The every-phase design by @p route (see dsd_squelch_reader_route): the two routes compute the same covariance
 * by separate arithmetic, from the responses through the reader's bank and the stages as the decimators run them, and
 * from the reader's covariance carried through the cascade phase by phase. Either leaves its table in cov (period rows
 * of exact_lags). Allocates nothing; the time is the route's multiply-adds (8,046 Hz read into 72 kHz, NFM 16k: the
 * responses, about 1.2e8; 864,058 Hz read into 576 kHz ahead of three passes, 201 taps: the covariance, about 1e8).
 *
 * @return As dsd_squelch_reader_model_floor_plan_exact(), and -1 for another @p route.
 */
int dsd_squelch_reader_model_floor_plan_route(dsd_squelch_reader_model* m, dsd_squelch_floor_plan* out,
                                              const float* taps, int taps_len, int rate_hz,
                                              dsd_squelch_reader_route route);

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
