// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Multi-consumer observers for published decoder telemetry.
 *
 * The snapshot accessors in @ref snapshot.h are single-consumer: exactly one
 * thread may call them. A third-party API server, however, has to read the same
 * live data while a terminal or Qt frontend is also attached, so it cannot be
 * that one consumer.
 *
 * These observers are the alternative. They fire on the decode thread, at the
 * same points the single-consumer snapshots are published, and receive the live
 * @c dsd_state / @c dsd_opts pointers. An observer must be cheap and must not
 * block: it copies what it needs under its own lock and returns. Dispatch costs
 * one atomic load when no observer is registered (the common case).
 *
 * Callbacks run under the registry lock, so dsd_app_telemetry_observer_remove()
 * returns only after any callback of the removed observer has finished. A
 * callback must therefore never add or remove an observer.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_TELEMETRY_OBSERVERS_H_
#define DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_TELEMETRY_OBSERVERS_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Observer callback table. Either callback may be NULL.
 *
 * Callbacks run on the decode thread, outside the snapshot locks but under the
 * observer registry lock: they must not block, re-enter the command queue, or
 * add/remove observers. The state/opts pointers are borrowed for the duration
 * of the call.
 */
typedef struct dsd_app_telemetry_observer {
    void (*state)(const dsd_state* state, void* user);
    void (*opts)(const dsd_opts* opts, void* user);
    void* user;
} dsd_app_telemetry_observer;

/**
 * @brief Register an observer. @p observer and its @c user must outlive removal.
 *
 * Returns 0 on success, -1 when the table is full, @p observer is already
 * registered, or the arguments are invalid.
 */
int dsd_app_telemetry_observer_add(const dsd_app_telemetry_observer* observer);

/**
 * @brief Remove a previously added observer. Returns 0 on success, -1 if not found.
 *
 * Synchronous: once it returns, no callback of @p observer is running or will
 * run, so its storage may be released. Never call it from a callback.
 */
int dsd_app_telemetry_observer_remove(const dsd_app_telemetry_observer* observer);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_TELEMETRY_OBSERVERS_H_ */
