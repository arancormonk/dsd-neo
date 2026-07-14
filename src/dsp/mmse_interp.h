// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef DSD_NEO_SRC_DSP_MMSE_INTERP_H
#define DSD_NEO_SRC_DSP_MMSE_INTERP_H

constexpr int DSD_MMSE_INTERP_TAPS = 8;

void dsd_mmse_interp_complex_8tap(const float* samples, float mu, float* out_real, float* out_imag);

#endif /* DSD_NEO_SRC_DSP_MMSE_INTERP_H */
