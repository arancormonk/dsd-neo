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
 * real hook functions at startup. Tune requests fail when no backend is
 * installed.
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
    /* Hooks must use request_id when publishing asynchronous completion. */
    dsd_trunk_tune_result (*tune_to_freq_request)(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps,
                                                  uint64_t request_id);
    dsd_trunk_tune_result (*tune_to_cc_request)(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps,
                                                uint64_t request_id);
    dsd_trunk_tune_result (*return_to_cc_request)(dsd_opts* opts, dsd_state* state, uint64_t request_id);
    /* Whether a protocol state machine may move the tuner on its own initiative now (a TETRA move to the main carrier
       its SYSINFO names): no manual or correlated tune is pending and no scan owns the tuner. NULL reads as free. */
    int (*tuner_free_for_protocol)(const dsd_opts* opts, const dsd_state* state);
} dsd_trunk_tuning_hooks;

void dsd_trunk_tuning_hooks_set(dsd_trunk_tuning_hooks hooks);

/**
 * @brief Return the generation of accepted trunk-tuning boundaries.
 *
 * Frame decoders can snapshot this value before collecting a frame and verify
 * it again before dispatch, preventing work from an earlier trunk target from
 * mutating state restored for a newer target. Hook wrappers and direct
 * trunk-scan retunes publish this generation only after completion.
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
 * @brief Return the newest request ID dsd_trunk_tuning_request_begin() handed out, or zero before the first.
 *
 * Monotonic for the process, across dsd_trunk_tuning_requests_reset(): a caller that remembers where the radio was
 * left compares it to tell whether any correlated tune has been issued since.
 */
uint64_t dsd_trunk_tuning_last_request(void);

/**
 * @brief Whether a correlated tune begun after @p request_id may have moved the receiver.
 *
 * Every request begun since counts except one its owner completed as refused or deferred before the backend took it
 * (dsd_trunk_tuning_request_complete() with a failed or deferred result, before any readiness or backend answer): such
 * a request moved nothing. A request whose record the bounded history no longer holds counts, so the answer errs
 * towards "moved". For an owner that puts the receiver back after its own request failed, unless another tune decided
 * where it is since.
 *
 * @return 1 when one may have, 0 when none did.
 */
int dsd_trunk_tuning_retuned_since(uint64_t request_id);

/**
 * @brief Retire terminal failed requests after correlated recovery is abandoned.
 *
 * Mode-exit paths call this after trunking ownership has been released so a
 * failed request cannot keep the process-wide frame gate closed indefinitely.
 * In-flight requests are left untouched; a later successful completion still
 * establishes a tuner boundary, while a later failure may be retired by a
 * subsequent call.
 */
void dsd_trunk_tuning_retire_failed_requests(void);

/**
 * @brief Retire the terminal failed requests up to and including @p request_id.
 *
 * For a caller that reconciles decoder state with a failure before retiring it: a request newer than the one it
 * inspected, or one that fails after it inspected it, stays gated for a later call.
 */
void dsd_trunk_tuning_retire_failed_requests_through(uint64_t request_id);

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
 * later successful tune or explicit mode-exit retirement. Stale and duplicate
 * request IDs are ignored.
 *
 * @param request_id Request ID returned by dsd_trunk_tuning_request_begin().
 * @param result Terminal result; PENDING is ignored.
 */
void dsd_trunk_tuning_request_complete(uint64_t request_id, dsd_trunk_tune_result result);

/**
 * @brief Let go of a tune transition whose owner keeps nothing staged for it any more.
 *
 * An owner that drops the state it staged for a request in flight (a protocol state machine starting over, handing
 * the tuner over, or freed with its decoder state by a decode-mode change) hands the request's terminal cleanup to
 * this layer, which outlives every owner. A request the backend has not answered stays in flight: the frame gate
 * keeps waiting for it, and a successful completion still advances the generation as the tuner boundary. A failure,
 * already published or published later, has nothing left to roll back and is retired as rolled back at once, so it
 * does not keep the frame gate closed while trunking stays on (a safety gate inherited from bounded-history recovery
 * stays closed, as for any rolled-back failure). Stale and duplicate request IDs are ignored.
 *
 * @param request_id Request ID returned by dsd_trunk_tuning_request_begin().
 */
void dsd_trunk_tuning_request_disown(uint64_t request_id);

/**
 * @brief The radio stream a restart reopened is on the frequency @p request_id asked for (issue #644): the request
 * landed, whatever the stop published for it.
 *
 * A request still in flight, or failed and not yet rolled back, finishes as a success (OK, completed now): the tuning
 * generation advances as for any landing, older unresolved failures retire, and every reader of its status (the trunk
 * scan, the state machines awaiting it) takes the landing. One already finished is left as it is.
 */
void dsd_trunk_tuning_request_land_at_reopen(uint64_t request_id);

/** @brief Return the newest unresolved request ID (in flight or awaiting recovery), or zero when ready. */
uint64_t dsd_trunk_tuning_pending_request(void);

/**
 * @brief Lock-free peek: whether @p request_id is the request whose success finished last (issue #834).
 *
 * One atomic load, for a caller that may not take the ledger lock (the frame-sync hunt's scan quiet service): 1 once
 * the request completed successfully and no other request has completed successfully since, else 0 (pending, failed,
 * superseded by a later landing, or 0).
 */
int dsd_trunk_tuning_request_landed(uint64_t request_id);

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

/**
 * @brief Test whether a collected frame may be dispatched for the active mode.
 *
 * When no trunking or trunk-scan tuner owns frame dispatch, terminal failed
 * requests are retired before checking the generation. In-flight requests
 * remain gated until their backend publishes a terminal result. The engine
 * reconciles its tuner state before it retires a failure
 * (dsd_engine_retire_inactive_tune_failures()) and then checks the generation
 * with dsd_trunk_tuning_frame_is_current().
 *
 * @param frame_generation Tune generation captured before frame sync.
 * @param tune_owner_active Non-zero while trunking or trunk scan owns tuning.
 */
int dsd_trunk_tuning_frame_is_dispatchable(uint64_t frame_generation, int tune_owner_active);

dsd_trunk_tune_result dsd_trunk_tuning_hook_tune_to_freq(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps,
                                                         uint64_t* out_request_id);
dsd_trunk_tune_result dsd_trunk_tuning_hook_tune_to_cc(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps,
                                                       uint64_t* out_request_id);
dsd_trunk_tune_result dsd_trunk_tuning_hook_return_to_cc(dsd_opts* opts, dsd_state* state, uint64_t* out_request_id);

/** @brief The installed tuner_free_for_protocol() answer; 1 (free) when no hook is installed (headless and test builds). */
int dsd_trunk_tuning_hook_tuner_free(const dsd_opts* opts, const dsd_state* state);

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
