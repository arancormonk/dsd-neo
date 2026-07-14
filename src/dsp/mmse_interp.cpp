// SPDX-License-Identifier: GPL-3.0-or-later

#include "mmse_interp.h"

#include <cstddef>

namespace {

constexpr int kMmseInterpSteps = 16;

/*
 * GNU Radio MMSE 8-tap polyphase interpolator coefficients.
 * From gnuradio/gr-filter/include/gnuradio/filter/interpolator_taps.h.
 * This is every eighth row of the 129-row table; coefficients are linearly
 * interpolated between rows for 1/128-step-equivalent resolution.
 */
constexpr float kMmseTaps[kMmseInterpSteps + 1][DSD_MMSE_INTERP_TAPS] = {
    {0.00000e+00f, 0.00000e+00f, 0.00000e+00f, 0.00000e+00f, 1.00000e+00f, 0.00000e+00f, 0.00000e+00f, 0.00000e+00f},
    {-1.23337e-03f, 6.84261e-03f, -2.24178e-02f, 6.57852e-02f, 9.83392e-01f, -4.04519e-02f, 9.56876e-03f,
     -1.54221e-03f},
    {-2.43121e-03f, 1.35716e-02f, -4.49929e-02f, 1.36968e-01f, 9.55956e-01f, -7.43154e-02f, 1.80759e-02f,
     -2.94361e-03f},
    {-3.55283e-03f, 1.99599e-02f, -6.70018e-02f, 2.12443e-01f, 9.18329e-01f, -1.01501e-01f, 2.53295e-02f,
     -4.16581e-03f},
    {-4.55932e-03f, 2.57844e-02f, -8.77011e-02f, 2.91006e-01f, 8.71305e-01f, -1.22047e-01f, 3.11866e-02f,
     -5.17776e-03f},
    {-5.41467e-03f, 3.08323e-02f, -1.06342e-01f, 3.71376e-01f, 8.15826e-01f, -1.36111e-01f, 3.55525e-02f,
     -5.95620e-03f},
    {-6.08674e-03f, 3.49066e-02f, -1.22185e-01f, 4.52218e-01f, 7.52958e-01f, -1.43968e-01f, 3.83800e-02f,
     -6.48585e-03f},
    {-6.54823e-03f, 3.78315e-02f, -1.34515e-01f, 5.32164e-01f, 6.83875e-01f, -1.45993e-01f, 3.96678e-02f,
     -6.75943e-03f},
    {-6.77751e-03f, 3.94578e-02f, -1.42658e-01f, 6.09836e-01f, 6.09836e-01f, -1.42658e-01f, 3.94578e-02f,
     -6.77751e-03f},
    {-6.73929e-03f, 3.95900e-02f, -1.46043e-01f, 6.92808e-01f, 5.22267e-01f, -1.33190e-01f, 3.75341e-02f,
     -6.50285e-03f},
    {-6.48585e-03f, 3.83800e-02f, -1.43968e-01f, 7.52958e-01f, 4.52218e-01f, -1.22185e-01f, 3.49066e-02f,
     -6.08674e-03f},
    {-5.95620e-03f, 3.55525e-02f, -1.36111e-01f, 8.15826e-01f, 3.71376e-01f, -1.06342e-01f, 3.08323e-02f,
     -5.41467e-03f},
    {-5.17776e-03f, 3.11866e-02f, -1.22047e-01f, 8.71305e-01f, 2.91006e-01f, -8.77011e-02f, 2.57844e-02f,
     -4.55932e-03f},
    {-4.16581e-03f, 2.53295e-02f, -1.01501e-01f, 9.18329e-01f, 2.12443e-01f, -6.70018e-02f, 1.99599e-02f,
     -3.55283e-03f},
    {-2.94361e-03f, 1.80759e-02f, -7.43154e-02f, 9.55956e-01f, 1.36968e-01f, -4.49929e-02f, 1.35716e-02f,
     -2.43121e-03f},
    {-1.54221e-03f, 9.56876e-03f, -4.04519e-02f, 9.83392e-01f, 6.57852e-02f, -2.24178e-02f, 6.84261e-03f,
     -1.23337e-03f},
    {0.00000e+00f, 0.00000e+00f, 0.00000e+00f, 1.00000e+00f, 0.00000e+00f, 0.00000e+00f, 0.00000e+00f, 0.00000e+00f},
};

float
interpolate_real_8tap(const float* samples, float mu) {
    float table_position = mu * static_cast<float>(kMmseInterpSteps);
    int lower_index = static_cast<int>(table_position);
    float fraction = table_position - static_cast<float>(lower_index);

    if (lower_index < 0) {
        lower_index = 0;
        fraction = 0.0f;
    }
    if (lower_index >= kMmseInterpSteps) {
        lower_index = kMmseInterpSteps - 1;
        fraction = 1.0f;
    }

    const float* lower_taps = kMmseTaps[lower_index];
    const float* upper_taps = kMmseTaps[lower_index + 1];
    const float lower_weight = 1.0f - fraction;

    float result = 0.0f;
    for (int i = 0; i < DSD_MMSE_INTERP_TAPS; i++) {
        const float tap = lower_weight * lower_taps[i] + fraction * upper_taps[i];
        result += tap * samples[i];
    }
    return result;
}

} // namespace

void
dsd_mmse_interp_complex_8tap(const float* samples, float mu, float* out_real, float* out_imag) {
    float real_samples[DSD_MMSE_INTERP_TAPS];
    float imag_samples[DSD_MMSE_INTERP_TAPS];

    for (int i = 0; i < DSD_MMSE_INTERP_TAPS; i++) {
        const std::size_t offset = static_cast<std::size_t>(i) * 2U;
        real_samples[i] = samples[offset];
        imag_samples[i] = samples[offset + 1U];
    }

    *out_real = interpolate_real_8tap(real_samples, mu);
    *out_imag = interpolate_real_8tap(imag_samples, mu);
}
