// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Runtime hook table for optional RTL stream metrics.
 *
 * Some DSP/protocol code wants to query RTL stream metrics without directly
 * depending on IO backends. The engine installs real hook functions at
 * startup; the runtime provides safe wrappers and fallback behavior when
 * hooks are not installed.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    unsigned int (*output_rate_hz)(void);
    int (*dsp_get)(int* out_cqpsk_enable, int* out_fll_enable, int* out_ted_enable);
    double (*snr_bias_evm)(void);
} dsd_rtl_stream_metrics_hooks;

void dsd_rtl_stream_metrics_hooks_set(dsd_rtl_stream_metrics_hooks hooks);

unsigned int dsd_rtl_stream_metrics_hook_output_rate_hz(void);
int dsd_rtl_stream_metrics_hook_dsp_get(int* out_cqpsk_enable, int* out_fll_enable, int* out_ted_enable);
double dsd_rtl_stream_metrics_hook_snr_bias_evm(void);

#ifdef __cplusplus
}
#endif
