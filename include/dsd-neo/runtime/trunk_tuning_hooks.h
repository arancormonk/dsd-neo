// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Runtime hook table for trunking tune side effects.
 *
 * Protocol state machines may need to request retunes without depending on
 * IO/control headers or linking IO backends. The engine (or tests) installs
 * real hook functions at startup; the runtime provides safe wrappers and
 * fallback behavior when hooks are not installed.
 */
#ifndef DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_TRUNK_TUNING_HOOKS_H_
#define DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_TRUNK_TUNING_HOOKS_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DSD_TRUNK_TUNE_RESULT_OK = 0,
    /* No hardware retune was scheduled yet; callers should retry without advancing state. */
    DSD_TRUNK_TUNE_RESULT_DEFERRED = 1,
    /* Retune was accepted but remains in flight; intended state may be staged, but is not completed. */
    DSD_TRUNK_TUNE_RESULT_PENDING = 2,
    DSD_TRUNK_TUNE_RESULT_FAILED = -1,
    DSD_TRUNK_TUNE_RESULT_TIMEOUT = -2,
} dsd_trunk_tune_result;

typedef struct {
    void (*tune_to_freq)(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps);
    void (*tune_to_cc)(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps);
    void (*return_to_cc)(dsd_opts* opts, dsd_state* state);
    /* Legacy result hooks cannot receive a request ID. A PENDING result remains
     * accepted but uncorrelated: it advances the generation without gating. */
    dsd_trunk_tune_result (*tune_to_freq_result)(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps);
    dsd_trunk_tune_result (*tune_to_cc_result)(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps);
    dsd_trunk_tune_result (*return_to_cc_result)(dsd_opts* opts, dsd_state* state);
    /* Request-aware hooks must use request_id when publishing asynchronous completion. */
    dsd_trunk_tune_result (*tune_to_freq_request)(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps,
                                                  uint64_t request_id);
    dsd_trunk_tune_result (*tune_to_cc_request)(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps,
                                                uint64_t request_id);
    dsd_trunk_tune_result (*return_to_cc_request)(dsd_opts* opts, dsd_state* state, uint64_t request_id);
} dsd_trunk_tuning_hooks;

void dsd_trunk_tuning_hooks_set(dsd_trunk_tuning_hooks hooks);

/**
 * @brief Return the generation of accepted trunk-tuning boundaries.
 *
 * Frame decoders can snapshot this value before collecting a frame and verify
 * it again before dispatch, preventing work from an earlier trunk target from
 * mutating state restored for a newer target. Correlated hook wrappers and
 * direct trunk-scan retunes publish this generation only after completion.
 * Legacy result hooks also publish on an accepted PENDING result because they
 * have no completion channel; those hooks remain deliberately ungated.
 */
uint64_t dsd_trunk_tuning_generation(void);

/**
 * @brief Advance the tuner generation after a completed direct retune.
 *
 * Hook wrappers do this automatically for completed results. Direct retune
 * paths that do not pass through a hook wrapper must call this only after the
 * tuner and output pipeline have completed the request.
 */
void dsd_trunk_tuning_generation_advance(void);

/**
 * @brief Begin a correlated tune transition and close the frame-dispatch gate.
 *
 * Hook wrappers call this automatically. Direct asynchronous tune paths may
 * use the returned request ID to publish their exact completion later.
 *
 * @return Non-zero request ID for the new transition.
 */
uint64_t dsd_trunk_tuning_request_begin(void);

/**
 * @brief Clear correlated request history at a quiescent runtime boundary.
 *
 * Engine lifecycle code calls this after the previous tuner backend has
 * stopped and before a new decode run begins. Stale completion callbacks are
 * ignored after the reset. Do not call while an active run owns tune requests.
 */
void dsd_trunk_tuning_requests_reset(void);

/**
 * @brief Publish backend completion for an exact tune transition.
 *
 * Successful completion commits the generation after the owner has marked its
 * staged decoder state ready. A failed asynchronous completion remains gated
 * until the owner rolls back the state or a newer successful tune replaces it.
 *
 * @param request_id Request ID returned by dsd_trunk_tuning_request_begin().
 * @param result Terminal backend result; PENDING is ignored.
 */
void dsd_trunk_tuning_request_publish(uint64_t request_id, dsd_trunk_tune_result result);

/**
 * @brief Mark staged decoder state ready for asynchronous tune completion.
 *
 * A backend completion that races ahead of its requesting thread remains
 * gated until this call confirms that the matching decoder state has been
 * committed. Hook wrappers call this automatically for PENDING results.
 *
 * @param request_id Request ID returned by dsd_trunk_tuning_request_begin().
 */
void dsd_trunk_tuning_request_mark_ready(uint64_t request_id);

/**
 * @brief Publish the terminal result for an exact tune transition.
 *
 * A successful completion advances the completed generation exactly once.
 * Failure normally clears the transition without publishing a completed
 * generation; callers must first restore or replace any staged decoder state.
 * A safety gate inherited from bounded-history recovery remains closed until a
 * later successful tune. Stale and duplicate request IDs are ignored.
 *
 * @param request_id Request ID returned by dsd_trunk_tuning_request_begin().
 * @param result Terminal result; PENDING is ignored.
 */
void dsd_trunk_tuning_request_complete(uint64_t request_id, dsd_trunk_tune_result result);

/** @brief Return the newest unresolved request ID (in flight or awaiting recovery), or zero when ready. */
uint64_t dsd_trunk_tuning_pending_request(void);

/**
 * @brief Query the correlated state of a tune request.
 *
 * @param request_id Request ID to query.
 * @param out_completed_m Optional completed monotonic timestamp in seconds.
 * @return PENDING while in flight or awaiting owner readiness, the terminal
 *         result when known, or FAILED when the request is no longer retained.
 */
dsd_trunk_tune_result dsd_trunk_tuning_request_status(uint64_t request_id, double* out_completed_m);

/**
 * @brief Test whether a collected frame still belongs to a completed tune.
 *
 * Equal generations are deliberately rejected while any retune is pending.
 */
int dsd_trunk_tuning_frame_is_current(uint64_t frame_generation);

dsd_trunk_tune_result dsd_trunk_tuning_hook_tune_to_freq(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps);
dsd_trunk_tune_result dsd_trunk_tuning_hook_tune_to_cc(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps);
dsd_trunk_tune_result dsd_trunk_tuning_hook_return_to_cc(dsd_opts* opts, dsd_state* state);
/* The _with_id wrappers return zero when dispatching through a legacy result hook. */
dsd_trunk_tune_result dsd_trunk_tuning_hook_tune_to_freq_with_id(dsd_opts* opts, dsd_state* state, long int freq,
                                                                 int ted_sps, uint64_t* out_request_id);
dsd_trunk_tune_result dsd_trunk_tuning_hook_tune_to_cc_with_id(dsd_opts* opts, dsd_state* state, long int freq,
                                                               int ted_sps, uint64_t* out_request_id);
dsd_trunk_tune_result dsd_trunk_tuning_hook_return_to_cc_with_id(dsd_opts* opts, dsd_state* state,
                                                                 uint64_t* out_request_id);

static inline int
dsd_trunk_tune_result_is_ok(dsd_trunk_tune_result result) {
    return result == DSD_TRUNK_TUNE_RESULT_OK || result == DSD_TRUNK_TUNE_RESULT_PENDING;
}

static inline int
dsd_trunk_tune_result_is_complete(dsd_trunk_tune_result result) {
    return result == DSD_TRUNK_TUNE_RESULT_OK;
}

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_TRUNK_TUNING_HOOKS_H_ */
