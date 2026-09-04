// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_DSP_SNR_ESTIMATOR_H_
#define DSD_NEO_INCLUDE_DSD_NEO_DSP_SNR_ESTIMATOR_H_

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Estimate 4-level FSK SNR from real sample-domain discriminator output.
 *
 * Scans all possible sample phases for the best eye opening, clusters samples
 * into four rails, and returns 10*log10(signal variance / noise variance) minus
 * the supplied estimator bias. Returns -100 dB when insufficient data is
 * available.
 */
double dsd_snr_estimate_c4fm_real_db(const float* samples, int sample_count, int samples_per_symbol, int phase_window,
                                     double bias_db);

/**
 * @brief Estimate binary FSK SNR from real sample-domain discriminator output.
 *
 * Scans all possible sample phases for the best eye opening, splits samples
 * into two rails by median, and returns 10*log10(signal variance / noise
 * variance) minus the supplied estimator bias. Returns -100 dB when
 * insufficient data is available.
 */
double dsd_snr_estimate_gfsk_real_db(const float* samples, int sample_count, int samples_per_symbol, int phase_window,
                                     double bias_db);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_DSP_SNR_ESTIMATOR_H_ */
