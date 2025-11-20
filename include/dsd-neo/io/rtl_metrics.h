// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * RTL-SDR metrics and auto-PPM helpers.
 *
 * Exposes auto-PPM supervision state and a small helper used by the
 * RTL stream read path to perform spectrum/SNR-based PPM adjustments.
 */

#pragma once

#include <dsd-neo/core/opts_fwd.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Auto-PPM status/query helpers (C ABI preserved). */
int dsd_rtl_stream_auto_ppm_get_status(int* enabled, double* snr_db, double* df_hz, double* est_ppm, int* last_dir,
                                       int* cooldown, int* locked);

int dsd_rtl_stream_auto_ppm_training_active(void);

int dsd_rtl_stream_auto_ppm_get_lock(int* ppm, double* snr_db, double* df_hz);

void dsd_rtl_stream_set_auto_ppm(int onoff);

int dsd_rtl_stream_get_auto_ppm(void);

/* Spectrum update helper used by the demod thread. */
void rtl_metrics_update_spectrum_from_iq(const int16_t* iq_interleaved, int len_interleaved, int out_rate_hz);

#ifdef __cplusplus
}
#endif
