// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#pragma once

#include <stdint.h>

struct demod_state; /* fwd decl */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file
 * @brief CQPSK DSP path entry points and runtime tuning helpers.
 */

/** @brief Initialize CQPSK path state associated with a demod_state. */
void cqpsk_init(struct demod_state* s);

/**
 * @brief Process one block of interleaved I/Q baseband for QPSK robustness.
 *
 * Runs after FLL mixing and before FM discrimination.
 */
void cqpsk_process_block(struct demod_state* s);

/**
 * @brief Runtime parameter adjustment for CQPSK path (best-effort).
 * @param lms_enable Enable NLMS equalizer (0/1) or -1 to keep.
 * @param taps Number of taps or -1 to keep.
 * @param mu_q15 Step size (Q15) or -1 to keep.
 * @param update_stride Update stride or -1 to keep.
 * @param wl_enable Enable widely-linear branch (0/1 or -1 to keep).
 * @param dfe_enable Enable DFE branch (0/1 or -1 to keep).
 * @param dfe_taps Number of DFE taps or -1 to keep.
 * @param cma_warmup_samples CMA warmup sample budget or -1 to keep.
 */
void cqpsk_runtime_set_params(int lms_enable, int taps, int mu_q15, int update_stride, int wl_enable, int dfe_enable,
                              int dfe_taps, int cma_warmup_samples);

/**
 * @brief Snapshot current CQPSK params.
 * @return 0 on success. Any out pointer may be NULL.
 */
int cqpsk_runtime_get_params(int* lms_enable, int* taps, int* mu_q15, int* update_stride, int* wl_enable,
                             int* dfe_enable, int* dfe_taps, int* cma_warmup_remaining);

/** @brief Toggle DQPSK-aware decision mode (0=off, 1=on). */
void cqpsk_runtime_set_dqpsk(int enable);

/** @brief Get DQPSK-aware decision mode (returns 0 on success). */
int cqpsk_runtime_get_dqpsk(int* enable);

/** @brief Reset all CQPSK runtime state (FFE/DFE/WL). */
void cqpsk_reset_all(void);
/** @brief Reset counters/histories; keep taps. */
void cqpsk_reset_runtime(void);
/** @brief Reset widely-linear branch taps/history. */
void cqpsk_reset_wl(void);

/**
 * @brief Debug snapshot of CQPSK EQ internals.
 *
 * Any out pointer may be NULL. Returns 0 on success.
 */
int cqpsk_runtime_get_debug(int* updates, int* adapt_mode, int* c0_i, int* c0_q, int* taps, int* isi_ratio_q15,
                            int* wl_improp_q15, int* cma_warmup, int* mu_q15, int* sym_stride, int* dfe_taps,
                            int* err_ema_q14);

#ifdef __cplusplus
}
#endif
