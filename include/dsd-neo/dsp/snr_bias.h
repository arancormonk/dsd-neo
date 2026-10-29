// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_DSP_SNR_BIAS_H_
#define DSD_NEO_INCLUDE_DSD_NEO_DSP_SNR_BIAS_H_

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Noise equivalent bandwidth (Hz) of the receive chain a channel profile runs.
 *
 * The TETRA profile's bandwidth follows the chain that runs: 18000 Hz (its RRC
 * matched filter) while the TETRA chain runs, the WIDE plan's otherwise.
 *
 * @param lpf_profile Channel LPF profile (DSD_CH_LPF_PROFILE_*).
 * @param tetra_chain demod_state::tetra_chain (nonzero while the TETRA chain runs).
 * @return Noise bandwidth in Hz; the WIDE bandwidth for unknown profiles.
 */
double dsd_snr_noise_bandwidth_hz(int lpf_profile, int tetra_chain);

/**
 * @brief Compute total SNR bias for C4FM (4-level FSK) given DSP parameters.
 *
 * @param rate_out Output sample rate in Hz.
 * @param ted_sps Samples per symbol.
 * @param lpf_profile Channel LPF profile (DSD_CH_LPF_PROFILE_*).
 * @param tetra_chain demod_state::tetra_chain (dsd_snr_noise_bandwidth_hz()).
 * @return Total bias in dB to subtract from raw SNR estimate.
 */
double dsd_snr_bias_c4fm_db(int rate_out, int ted_sps, int lpf_profile, int tetra_chain);

/**
 * @brief Compute total SNR bias for EVM/GFSK/QPSK given DSP parameters.
 *
 * @param rate_out Output sample rate in Hz.
 * @param ted_sps Samples per symbol.
 * @param lpf_profile Channel LPF profile (DSD_CH_LPF_PROFILE_*).
 * @param tetra_chain demod_state::tetra_chain (dsd_snr_noise_bandwidth_hz()).
 * @return Total bias in dB to subtract from raw SNR estimate.
 */
double dsd_snr_bias_evm_db(int rate_out, int ted_sps, int lpf_profile, int tetra_chain);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_DSP_SNR_BIAS_H_ */
