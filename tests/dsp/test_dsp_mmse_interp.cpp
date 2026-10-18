// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * MMSE 8-tap interpolator convention tests.
 *
 * GNU Radio's mmse_fir_interpolator (the reference for OP25's gardner_cc)
 * interpolates FORWARD in time between x[3] and x[4]: interpolate(x, mu) ~=
 * x[3 + mu] for mu in [0, 1]. The Gardner timing loop's stability depends on
 * that sign: mu increases must move the sampling instant later. These tests
 * pin the convention and the mu=1.0 / mu=0.0 seam consistency the loop relies
 * on when the timing phase crosses a sample boundary.
 */

#include <cmath>
#include <stdio.h>
#include <vector>

#include "dsd-neo/core/safe_api.h"

/* Private DSP helper (src/dsp/mmse_interp.h); declared here to match its C++
   linkage without reaching into src/ include paths. */
void dsd_mmse_interp_complex_8tap(const float* samples, float mu, float* out_real, float* out_imag);

namespace {

int
expect_close(const char* tag, float got, float want, float tol) {
    if (std::fabs(got - want) > tol) {
        DSD_FPRINTF(stderr, "%s: got %.6f want %.6f\n", tag, got, want);
        return 1;
    }
    return 0;
}

/* Interleave a real sequence with a distinct imaginary sequence. */
void
make_iq(const float* re, const float* im, int n, float* iq) {
    for (int i = 0; i < n; i++) {
        iq[i * 2 + 0] = re[i];
        iq[i * 2 + 1] = im[i];
    }
}

int
check_reference_phases(const float* iq) {
    /* Golden results from independent real/imaginary dot products, halfway
     * between every pair of table rows. The channels intentionally differ. */
    const float expected[16][2] = {
        {0.149544418f, 0.819211543f},  {0.047893338f, 0.851615608f},  {-0.054453757f, 0.871791959f},
        {-0.155925363f, 0.879460394f}, {-0.254952580f, 0.874541998f}, {-0.349994630f, 0.857157826f},
        {-0.439562261f, 0.827622831f}, {-0.522239983f, 0.786439002f}, {-0.600750029f, 0.730346084f},
        {-0.665816844f, 0.668067813f}, {-0.716379881f, 0.600596845f}, {-0.759613276f, 0.521179020f},
        {-0.790748358f, 0.434995323f}, {-0.809239924f, 0.343380988f}, {-0.814740062f, 0.247745961f},
        {-0.807105839f, 0.149551719f},
    };
    /* Offset the exact-sized window by one float to exercise unaligned input
     * and let sanitizers detect a read beyond its final complex sample. */
    std::vector<float> storage(17);
    DSD_MEMCPY(storage.data() + 1, iq, 16 * sizeof(float));
    int rc = 0;
    for (int row = 0; row < 16; row++) {
        float real = 0.0f, imag = 0.0f;
        const float mu = static_cast<float>(2 * row + 1) / 32.0f;
        dsd_mmse_interp_complex_8tap(storage.data() + 1, mu, &real, &imag);
        rc |= expect_close("reference real", real, expected[row][0], 1e-6f);
        rc |= expect_close("reference imag", imag, expected[row][1], 1e-6f);
    }
    return rc;
}

} // namespace

int
main(void) {
    int rc = 0;

    const float re[9] = {0.9f, -0.3f, 0.7f, 0.2f, -0.8f, 0.5f, -0.1f, 0.4f, -0.6f};
    const float im[9] = {-0.2f, 0.6f, -0.5f, 0.8f, 0.1f, -0.7f, 0.3f, -0.9f, 0.25f};
    float iq[18];
    make_iq(re, im, 9, iq);
    rc |= check_reference_phases(iq);

    float r = 0.0f;
    float j = 0.0f;

    /* mu = 0 must return x[3] exactly (GNU Radio row 0 is a delta). */
    dsd_mmse_interp_complex_8tap(iq, 0.0f, &r, &j);
    rc |= expect_close("mu=0 real", r, re[3], 1e-6f);
    rc |= expect_close("mu=0 imag", j, im[3], 1e-6f);

    /* mu = 1 must return x[4] exactly. */
    dsd_mmse_interp_complex_8tap(iq, 1.0f, &r, &j);
    rc |= expect_close("mu=1 real", r, re[4], 1e-6f);
    rc |= expect_close("mu=1 imag", j, im[4], 1e-6f);

    /* Seam: interpolating at mu=1.0 from window n must equal mu=0.0 from
       window n+1 (same time instant, different window). The Gardner loop
       crosses this seam every time the timing phase wraps a sample. */
    float r_seam0 = 0.0f, j_seam0 = 0.0f, r_seam1 = 0.0f, j_seam1 = 0.0f;
    dsd_mmse_interp_complex_8tap(iq, 1.0f, &r_seam0, &j_seam0);
    dsd_mmse_interp_complex_8tap(iq + 2, 0.0f, &r_seam1, &j_seam1);
    rc |= expect_close("seam real", r_seam0, r_seam1, 1e-6f);
    rc |= expect_close("seam imag", j_seam0, j_seam1, 1e-6f);

    /* Forward time: on a linear ramp, interpolation must move WITH mu.
       ramp[i] = i, so interp(mu) ~= 3 + mu. */
    const float ramp[8] = {0.0f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f};
    float ramp_iq[16];
    make_iq(ramp, ramp, 8, ramp_iq);
    for (int m = 1; m < 16; m++) {
        const float mu = (float)m / 16.0f;
        dsd_mmse_interp_complex_8tap(ramp_iq, mu, &r, &j);
        rc |= expect_close("ramp forward", r, 3.0f + mu, 0.05f);
    }

    return rc;
}
