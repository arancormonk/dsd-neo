// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef DSD_NEO_SRC_DSP_SIMD_FIR_INTERNAL_H_
#define DSD_NEO_SRC_DSP_SIMD_FIR_INTERNAL_H_

void simd_fir_complex_apply_scalar(const float* in, int in_len, float* out, float* hist_i, float* hist_q,
                                   const float* taps, int taps_len);
int simd_hb_decim2_complex_scalar(const float* in, int in_len, float* out, float* hist_i, float* hist_q,
                                  const float* taps, int taps_len);
int simd_hb_decim2_real_scalar(const float* in, int in_len, float* out, float* hist, const float* taps, int taps_len);

#endif /* DSD_NEO_SRC_DSP_SIMD_FIR_INTERNAL_H_ */
