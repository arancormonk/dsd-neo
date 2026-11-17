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
#ifdef DSD_NEO_TEST_HOOKS
#include <dsd-neo/platform/threading.h>
#endif

/*
 * Background watchdog for the P25 trunking state machine tick. Its thread is a beat: every 200-400 ms it asks the
 * decoder thread for a tick (p25_sm_watchdog_request_tick()), and the decoder runs it at its next service point
 * (p25_sm_watchdog_service()): the frame-sync hunt's checkpoint, between synced frames, the engine loop, and a hunt
 * read waiting on a quiet input. So the hangtime, grant and control-channel recovery timers keep running between the
 * decoder's own ticks, while every tune and every write of dsd_state and dsd_opts stays on the decoder thread.
 */

#ifdef __cplusplus
extern "C" {
#endif

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
 * @brief The watchdog's beat: ask the decoder thread for a P25 tick. Touches no decoder state; any thread.
 */
void p25_sm_watchdog_request_tick(void);

/** @brief Whether a beat is waiting for the decoder thread: one atomic load. */
int p25_sm_watchdog_tick_wanted(void);

/**
 * @brief Decoder thread: run the tick a watchdog beat asked for (p25_sm_try_tick()), if one did.
 *
 * Takes the tick guard with a try: a hold already taken, by the caller or anyone, leaves the beat standing for the next
 * service point. One atomic load when no beat waits.
 *
 * @return 1 when a tick ran, else 0.
 */
int p25_sm_watchdog_service(dsd_opts* opts, dsd_state* state);

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
 * @brief Start the background watchdog thread (the beat that asks the decoder thread for ticks).
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
