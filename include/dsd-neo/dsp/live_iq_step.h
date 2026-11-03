// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The live I/Q rate step after the half-band chain (issue #806).
 *
 * A device that forces its own rate (an Airspy's 2.5 MS/s, say) leaves the half-band chain at hb_out, a rate that is
 * not the DSP bandwidth rate (78,125 Hz for 48 kHz). The step resamples the complex stream from hb_out to the target
 * exactly, with a dsd_iq_resampler designed for the plan's L/M (dsd_iq_resampler_configure_ratio()). full_demod() runs
 * it right after the half-band decimation, on the demod thread, in passes of DSD_LIVE_IQ_STEP_PASS_PAIRS inputs; a
 * chain with no step pays one branch a block.
 *
 * The plan comes from the IO rate planner (dsd_rate_plan_compute(), <dsd-neo/io/rate_plan.h>), as plain numbers: DSP
 * does not depend on IO.
 *
 * Designs change in a transaction, on the controller thread while the demod thread is paused: prepare a candidate
 * from the final plan, then commit (the running design is retained, the candidate runs) or roll back (the retained
 * design runs again), and finish (the retained design is released). A prepare that fails leaves the running and the
 * retained designs as they were. Nothing is allocated or freed while samples flow.
 *
 * Zeroed storage is an empty step (no design, off), like dsd_live_iq_step_init().
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_DSP_LIVE_IQ_STEP_H
#define DSD_NEO_INCLUDE_DSD_NEO_DSP_LIVE_IQ_STEP_H

#include <dsd-neo/dsp/iq_resampler.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** The complex inputs the step filters per pass, so the bank and the history it works on stay in cache. */
#define DSD_LIVE_IQ_STEP_PASS_PAIRS 512

/** Why dsd_live_iq_step_prepare() or dsd_live_iq_step_commit() refused. */
typedef enum {
    DSD_LIVE_IQ_STEP_OK = 0,
    /** A NULL argument; a plan whose passes or hb_out are out of range, or a stepping plan whose L/M does not take
        hb_out to the target within DSD_IQ_RESAMPLER_MAX_APPROX_PPM; or a commit with nothing prepared. */
    DSD_LIVE_IQ_STEP_ERR_ARGS = -1,
    /** The plan would upsample (L above M; the live chain never does), or a maximum block's outputs would not fit the
        buffer the step writes. */
    DSD_LIVE_IQ_STEP_ERR_CAPACITY = -2,
    /** The resampler refused the design; dsd_live_iq_step::design_status says why. */
    DSD_LIVE_IQ_STEP_ERR_DESIGN = -3,
} dsd_live_iq_step_status;

/** A step plan, as the rate planner worked it out. Its fields are the key: two plans are the same design when every
    field matches (dsd_live_iq_step_plan_equal()). */
typedef struct {
    int enabled; /**< 1: the chain steps; 0: it runs no step, and only passes and hb_out describe it. */
    int passes;  /**< p, the half-band passes ahead of the step */
    /** hb_out = hb_out_num / 2^hb_out_shift Hz, the rate the step takes in. */
    int64_t hb_out_num;
    unsigned hb_out_shift;
    int L;
    int M;
    int target_hz;  /**< The DSP bandwidth rate the step makes. */
    int protect_hz; /**< The protected band: 0.45 x target. */
    int stop_hz;    /**< The stopband edge: target less the protected band. */
} dsd_live_iq_step_plan;

/** The step: the running design and the two a transaction holds next to it. */
typedef struct dsd_live_iq_step {
    /** 1 while the active design runs: the one field full_demod() reads before a block's step. */
    int enabled;
    /** The status of the last prepare's design: DSD_IQ_RESAMPLER_OK, or why the resampler refused. */
    int design_status;
    int retained_set;            /**< 1 after a commit, until the transaction finishes or rolls back */
    int candidate_set;           /**< 1 after a prepare, until the commit, a rollback or the finish */
    int candidate_reuses_active; /**< The prepared plan is the active one: the commit keeps its design. */
    dsd_live_iq_step_plan plan;  /**< The active plan */
    dsd_live_iq_step_plan retained_plan;
    dsd_live_iq_step_plan candidate_plan;
    dsd_iq_resampler active;    /**< Runs on the demod thread while enabled */
    dsd_iq_resampler retained;  /**< The design the transaction's first commit replaced */
    dsd_iq_resampler candidate; /**< Designed by prepare, run only after the commit */
} dsd_live_iq_step;

/** Initialise @p s: no design, off. */
void dsd_live_iq_step_init(dsd_live_iq_step* s);

/** Whether @p a and @p b are the same design: every field equal. Two NULLs are not. */
int dsd_live_iq_step_plan_equal(const dsd_live_iq_step_plan* a, const dsd_live_iq_step_plan* b);

/** The complex rate after the step for @p plan, in whole Hz: the target when it steps, else hb_out when that is a
    whole number of Hz; 0 otherwise or for NULL. */
int dsd_live_iq_step_plan_out_hz(const dsd_live_iq_step_plan* plan);

/**
 * @brief Prepare @p plan as the candidate the next commit runs.
 *
 * A plan equal to the active one keeps the active design (nothing is designed). Another stepping plan is designed into
 * the candidate, cleared; a plan that does not step needs no design. An earlier candidate not yet committed is released
 * first. Refused: a plan that does not agree with itself, one whose block could not fit (L above M, or
 * ceil(@p max_block_in x L / M) above @p out_cap), and a design the resampler refuses (its status in design_status).
 * A refusal leaves no candidate and the active and retained designs untouched.
 *
 * @param max_block_in The most complex inputs a block brings to the step.
 * @param out_cap The room, in complex samples, of the buffer the step writes a block to.
 * @return DSD_LIVE_IQ_STEP_OK, or a negative dsd_live_iq_step_status.
 */
int dsd_live_iq_step_prepare(dsd_live_iq_step* s, const dsd_live_iq_step_plan* plan, size_t max_block_in,
                             size_t out_cap);

/**
 * @brief Run the prepared candidate.
 *
 * The first commit of a transaction keeps the active design as the retained one; a later commit releases the design
 * it replaces, so the retained design stays the one the transaction began with. A new design starts cleared: a plan
 * change starts the step over. A candidate that reuses the active design changes nothing.
 *
 * @return DSD_LIVE_IQ_STEP_OK, or DSD_LIVE_IQ_STEP_ERR_ARGS with nothing prepared (nothing changes).
 */
int dsd_live_iq_step_commit(dsd_live_iq_step* s);

/** Put the retained design back, cleared, and release the committed one and any candidate. With nothing retained (no
    commit since the transaction began) only the candidate goes. */
void dsd_live_iq_step_rollback(dsd_live_iq_step* s);

/** End the transaction: release the retained design and any candidate not committed. The active design runs on. */
void dsd_live_iq_step_finish(dsd_live_iq_step* s);

/** Turn the step off and release every design (stream cleanup, and a chain that stops stepping outside a
    transaction). */
void dsd_live_iq_step_disable(dsd_live_iq_step* s);

/** Start the active design over from silence: its history and phase. dsd_demod_reset_filter_state() calls it with
    the half-band cascade's reset. */
void dsd_live_iq_step_clear(dsd_live_iq_step* s);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_DSP_LIVE_IQ_STEP_H */
