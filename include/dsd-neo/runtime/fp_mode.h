// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The floating-point mode (SSE flush-to-zero and denormals-are-zero) of a run's float threads.
 *
 * `DSD_NEO_FTZ_DAZ` is a start-time setting. The engine resolves it once when a run starts
 * (dsd_bootstrap_apply_fp_mode()) and every dsd-neo thread that does float work applies it first thing
 * (dsd_runtime_fp_mode_apply_thread()): a new thread does not inherit MXCSR everywhere (macOS starts each thread at
 * the default, and so, by every available source, does Windows). Unset leaves the mode each thread inherited alone,
 * 0 clears both bits and 1 sets both. The mode covers dsd-neo's own computation: a callback a library calls on its own
 * thread applies it around dsd-neo's work and puts the library's mode back before it returns
 * (dsd_runtime_fp_mode_scope_enter()). x86 and x86-64 only; elsewhere the calls change nothing.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_FP_MODE_H_H
#define DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_FP_MODE_H_H

/* MSVC defines neither __SSE__ nor __SSE2__, only _M_X64 and _M_IX86_FP. */
#if defined(__SSE__) || defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 1)
#define DSD_RUNTIME_FP_MODE_HAVE_MXCSR 1
#else
#define DSD_RUNTIME_FP_MODE_HAVE_MXCSR 0
#endif

/** MXCSR's flush-to-zero (bit 15) and denormals-are-zero (bit 6) bits. */
#define DSD_RUNTIME_FP_MODE_FTZ_DAZ_BITS ((1u << 15) | (1u << 6))

#ifdef __cplusplus
extern "C" {
#endif

struct dsdneoRuntimeConfig;

/** The mode a run's float threads use. */
typedef enum dsd_fp_mode {
    DSD_FP_MODE_INHERIT = 0, /**< DSD_NEO_FTZ_DAZ unset: each thread keeps the mode it inherited */
    DSD_FP_MODE_OFF = 1,     /**< a falsey DSD_NEO_FTZ_DAZ: FTZ and DAZ cleared */
    DSD_FP_MODE_ON = 2,      /**< any other DSD_NEO_FTZ_DAZ: FTZ and DAZ set */
} dsd_fp_mode;

/** @brief 1 when this build can set the mode (x86/x86-64), else 0. */
int dsd_runtime_fp_mode_supported(void);

/** @brief The mode @p cfg asks for (DSD_FP_MODE_INHERIT for NULL). */
dsd_fp_mode dsd_runtime_fp_mode_from_config(const struct dsdneoRuntimeConfig* cfg);

/** @brief Make @p mode the run's mode, for the threads that start from now on. Called once per run at its start; a
 * value outside dsd_fp_mode counts as DSD_FP_MODE_INHERIT. */
void dsd_runtime_fp_mode_publish(dsd_fp_mode mode);

/** @brief The run's mode, as last published (DSD_FP_MODE_INHERIT before any run). */
dsd_fp_mode dsd_runtime_fp_mode_current(void);

/**
 * @brief Apply the run's mode to the calling thread: the first statement of every dsd-neo thread that does float work.
 *
 * Costs one MXCSR read, and a write when the thread's mode differs. @p thread_tag names the thread for the observer.
 */
void dsd_runtime_fp_mode_apply_thread(const char* thread_tag);

/** The MXCSR a scope saved, and whether it has to be put back. */
typedef struct dsd_fp_mode_scope {
    unsigned int saved_csr;
    int restore;
} dsd_fp_mode_scope;

/**
 * @brief Apply the run's mode for dsd-neo's work in a callback a library runs on its own thread.
 *
 * Pair each call with dsd_runtime_fp_mode_scope_leave() before the callback returns: both the System V and the
 * Windows x64 ABI expect MXCSR's control bits back as they were. Costs nothing while the mode is DSD_FP_MODE_INHERIT;
 * otherwise one MXCSR read, plus a write here and one at leave only when the thread's mode differs.
 */
void dsd_runtime_fp_mode_scope_enter(dsd_fp_mode_scope* scope);

/** @brief Put back the MXCSR dsd_runtime_fp_mode_scope_enter() saved, if it changed it. */
void dsd_runtime_fp_mode_scope_leave(const dsd_fp_mode_scope* scope);

/** Called by dsd_runtime_fp_mode_apply_thread() with the thread's tag and its MXCSR after the apply (0 where the
 * build has no MXCSR). */
typedef void (*dsd_runtime_fp_mode_observer_fn)(const char* thread_tag, unsigned int csr);

/**
 * @brief Tests: observe every thread's apply.
 *
 * Built into every build, not only test builds: the tests that use it run the production engine, stream and protocol
 * threads, and a test-only observer would need hook-enabled copies of the runtime, engine and IO libraries, which the
 * Windows DLL builds cannot take. Unset, it costs one relaxed load per thread start. Set it before the threads it watches start and clear it (NULL) after they are joined; the slot is atomic, so
 * a set that breaks that rule is a missed or extra observation, never undefined behavior.
 */
void dsd_runtime_fp_mode_set_observer(dsd_runtime_fp_mode_observer_fn observer);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_FP_MODE_H_H */
