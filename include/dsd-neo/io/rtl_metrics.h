// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief RTL-SDR metrics and auto-PPM helpers.
 *
 * Exposes auto-PPM supervision state and a helper used by the RTL stream
 * read path to perform spectrum/SNR-based PPM adjustments.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_IO_RTL_METRICS_H_
#define DSD_NEO_INCLUDE_DSD_NEO_IO_RTL_METRICS_H_

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Return 1 if auto-PPM training window is active. */
int dsd_rtl_stream_auto_ppm_training_active(void);

/**
 * @brief Update spectrum/SNR metrics from a block of interleaved I/Q.
 *
 * Used by the demod thread to feed the auto-PPM estimator.
 */
void rtl_metrics_update_spectrum_from_iq(const float* iq_interleaved, int len_interleaved, int out_rate_hz);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_IO_RTL_METRICS_H_ */
