// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file
 * @brief P25 state-machine watchdog helpers.
 */
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_P25_P25_SM_WATCHDOG_H_H
#define DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_P25_P25_SM_WATCHDOG_H_H

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <time.h>
#ifdef DSD_NEO_TEST_HOOKS
#include <dsd-neo/platform/threading.h>
#endif

/*
 * Background watchdog for the P25 trunking state machine tick.
 * Ensures hangtime/CC-return logic continues even if the main
 * DSP loop is temporarily stalled by upstream I/O.
 */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief What frame acquisition found, as the watchdog's tick reads it.
 *
 * The frame-sync hunt writes these dsd_state fields at every accepted sync, outside the P25 SM tick guard. The decoder
 * thread (the one that started the watchdog) copies them here each time it leaves the guard, after the dispatch that
 * follows every accept, so the watchdog's tick reads this copy and never the fields. A stamp the tick writes is merged
 * back into dsd_state, newest wins, the next time the decoder takes the guard. Read and written under the guard only.
 */
typedef struct {
    int lastsynctype;            /**< dsd_state::lastsynctype */
    time_t last_cc_sync_time;    /**< dsd_state::last_cc_sync_time */
    double last_cc_sync_time_m;  /**< dsd_state::last_cc_sync_time_m */
    int cc_sync_written_by_tick; /**< the tick wrote the stamp: the decoder merges it at its next guard entry */
} p25_sm_acquisition;

/**
 * @brief The acquisition copy on the watchdog's thread; NULL on every other thread, which reads dsd_state itself.
 *
 * Only meaningful under the tick guard.
 */
p25_sm_acquisition* p25_sm_watchdog_acquisition(void);

/**
 * @brief Attempt to run one P25 state-machine tick when idle.
 *
 * Safe to call frequently; a tick is skipped when one is already in progress.
 *
 * @param opts Decoder options.
 * @param state Decoder state.
 */
void p25_sm_try_tick(dsd_opts* opts, dsd_state* state);

/**
 * @brief Try to enter the critical section shared with P25 SM ticks.
 *
 * Callers that temporarily restore or swap trunking state shared with the P25
 * watchdog can use this to avoid racing an in-flight tick. Returns 1 when the
 * caller owns the guard and must call p25_sm_tick_guard_leave().
 */
int p25_sm_tick_guard_try_enter(void);

/**
 * @brief Enter the critical section shared with P25 SM and trunk-scan ticks.
 *
 * This blocking form is intended for work that must not overlap a tick, such
 * as frame dispatch against one tuner target or a tuner lifecycle transition.
 */
void p25_sm_tick_guard_enter(void);

/** @brief Leave a critical section entered by either guard entry function. */
void p25_sm_tick_guard_leave(void);

/** @brief Return 1 when the calling thread holds the tick guard (entered and not yet left). */
int p25_sm_tick_guard_held_by_caller(void);

/** @brief Return 1 while any thread holds the tick guard. A look only: it takes nothing, and the answer can change at
 * once. */
int p25_sm_tick_guard_busy(void);

/**
 * @brief Start the background 1 Hz watchdog thread.
 *
 * No-op if already started. A thread that cannot start leaves the watchdog off (logged); a later start tries again.
 *
 * @param opts Decoder options.
 * @param state Decoder state.
 */
void p25_sm_watchdog_start(dsd_opts* opts, dsd_state* state);
/** @brief Stop the background watchdog thread (safe to call when stopped). */
void p25_sm_watchdog_stop(void);

#ifdef DSD_NEO_TEST_HOOKS
/** Creates the watchdog thread the way dsd_thread_create() does: 0 on success. */
typedef int (*p25_sm_watchdog_thread_create_fn)(dsd_thread_t* thread, dsd_thread_fn entry, void* arg);

/** @brief Tests: create the watchdog thread through @p create (NULL: dsd_thread_create()). Set while it is stopped. */
void p25_sm_watchdog_set_thread_create_for_test(p25_sm_watchdog_thread_create_fn create);
#endif

/**
 * @brief Return 1 while a P25 SM tick is executing on the current thread.
 */
int p25_sm_in_tick(void);

#ifdef __cplusplus
}
#endif
#endif /* DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_P25_P25_SM_WATCHDOG_H_H */
