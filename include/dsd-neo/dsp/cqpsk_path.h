// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#pragma once

#include <stdint.h>

struct demod_state; /* fwd decl */

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize CQPSK path state associated with a demod_state. */
void cqpsk_init(struct demod_state* s);

/*
 * Process one block of interleaved I/Q baseband for QPSK/LSM robustness.
 *
 * This runs after FLL mixing and before FM discrimination. Initial
 * implementation is a safe pass-through; subsequent versions will apply a
 * tiny decision-directed equalizer.
 */
void cqpsk_process_block(struct demod_state* s);

/* Runtime parameter adjustment for CQPSK path (best-effort). Any arg <0 leaves it unchanged. */
void cqpsk_runtime_set_params(int lms_enable, int taps, int mu_q15, int update_stride, int wl_enable, int dfe_enable,
                              int dfe_taps, int cma_warmup_samples);

/* Snapshot current CQPSK params. Returns 0 on success. Any out ptr may be NULL. */
int cqpsk_runtime_get_params(int* lms_enable, int* taps, int* mu_q15, int* update_stride, int* wl_enable,
                             int* dfe_enable, int* dfe_taps, int* cma_warmup_remaining);

/* Toggle DQPSK-aware decision mode (0=off, 1=on). */
void cqpsk_runtime_set_dqpsk(int enable);

/* Get DQPSK-aware decision mode (returns 0 on success). */
int cqpsk_runtime_get_dqpsk(int* enable);

/* Reset helpers for runtime toggles */
void cqpsk_reset_all(void);
void cqpsk_reset_runtime(void);
void cqpsk_reset_wl(void);

#ifdef __cplusplus
}
#endif
