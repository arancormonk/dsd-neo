// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Compute total SNR bias for C4FM (4-level FSK) given DSP parameters.
 *
 * @param rate_out Output sample rate in Hz.
 * @param ted_sps Samples per symbol.
 * @param lpf_profile Channel LPF profile (DSD_CH_LPF_PROFILE_*).
 * @return Total bias in dB to subtract from raw SNR estimate.
 */
double dsd_snr_bias_c4fm_db(int rate_out, int ted_sps, int lpf_profile);

/**
 * @brief Compute C4FM SNR bias using an explicit post-demod noise bandwidth.
 *
 * Use this for SDR++-style FM paths where the estimator sees discriminator
 * samples after an audio low-pass rather than complex samples after the
 * channel LPF profile.
 */
double dsd_snr_bias_c4fm_bw_db(int rate_out, int ted_sps, double noise_bw_hz);

/**
 * @brief Compute total SNR bias for EVM/GFSK/QPSK given DSP parameters.
 *
 * @param rate_out Output sample rate in Hz.
 * @param ted_sps Samples per symbol.
 * @param lpf_profile Channel LPF profile (DSD_CH_LPF_PROFILE_*).
 * @return Total bias in dB to subtract from raw SNR estimate.
 */
double dsd_snr_bias_evm_db(int rate_out, int ted_sps, int lpf_profile);

/**
 * @brief Compute EVM/GFSK/QPSK SNR bias using an explicit post-demod noise bandwidth.
 */
double dsd_snr_bias_evm_bw_db(int rate_out, int ted_sps, double noise_bw_hz);

#ifdef __cplusplus
}
#endif
