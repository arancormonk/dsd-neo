// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Minimal 2-thread worker pool API for intra-block demodulation tasks.
 *
 * Exposes functions keyed by `demod_state*` to initialize/destroy a tiny
 * env-gated pool and to run up to two tasks in parallel per processing block.
 */

#ifndef RUNTIME_WORKER_POOL_H
#define RUNTIME_WORKER_POOL_H

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declaration to avoid including heavy headers here */
struct demod_state;

typedef void (*demod_mt_task_fn)(void* arg);

typedef struct demod_mt_task {
    demod_mt_task_fn run;
    void* arg;
} demod_mt_task;

/* Minimal 2-thread worker pool API, enabled by DSD_NEO_MT. */

/**
 * @brief Initialize the minimal worker pool when `DSD_NEO_MT=1`.
 *
 * Safe to call multiple times per demodulator instance.
 * @param s Demodulator state used as a key for the worker context.
 * @note No-op when multithreading is disabled via environment.
 */
void demod_mt_init(struct demod_state* s);

/**
 * @brief Tear down worker threads created by `demod_mt_init`.
 *
 * @param s Demodulator state used as a key for the worker context.
 * @note Safe no-op if the pool was never enabled/initialized.
 */
void demod_mt_destroy(struct demod_state* s);

/**
 * @brief Post up to two tasks and wait for completion.
 *
 * Runs synchronously in the caller thread when the pool is disabled.
 * @param s Demodulator state key for the worker context.
 * @param f0 Function pointer for the first task (may be NULL).
 * @param a0 Argument for the first task.
 * @param f1 Function pointer for the second task (may be NULL).
 * @param a1 Argument for the second task.
 */
void demod_mt_run_two_impl(struct demod_state* s, demod_mt_task task0, demod_mt_task task1);

#ifdef __cplusplus
#define demod_mt_run_two(s, f0, a0, f1, a1)                                                                            \
    demod_mt_run_two_impl((s), demod_mt_task{(f0), (a0)}, demod_mt_task{(f1), (a1)})
#else
#define demod_mt_run_two(s, f0, a0, f1, a1)                                                                            \
    demod_mt_run_two_impl((s), (demod_mt_task){(f0), (a0)}, (demod_mt_task){(f1), (a1)})
#endif

#ifdef __cplusplus
}
#endif

#endif /* RUNTIME_WORKER_POOL_H */
