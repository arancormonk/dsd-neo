// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Regression test for OP25/GNU Radio-compatible band-edge FLL filter design. */

#include <dsd-neo/dsp/costas.h>
#include <dsd-neo/dsp/demod_state.h>
#include <dsd-neo/runtime/mem.h>
#include <stdint.h>
#include <stdio.h>
#include "dsd-neo/core/safe_api.h"

static int
closef(float a, float b, float tol) {
    float d = a - b;
    if (d < 0.0f) {
        d = -d;
    }
    return (d <= tol) ? 1 : 0;
}

static int
check_convolution(void) {
    demod_state* d = static_cast<demod_state*>(dsd_neo_aligned_malloc(sizeof(demod_state)));
    if (!d) {
        return 1;
    }
    DSD_MEMSET(d, 0, sizeof(*d));
    d->lowpassed = d->input_cb_buf;
    d->lp_len = 2;
    const int rates[] = {1, 4, 5, 8, 10, 20, 23, 24};
    const float amplitudes[] = {0.125f, 1.0f, 16.0f};
    uint32_t seed = 0x1234abcdU;
    int rc = 0;
    for (int sps : rates) {
        d->cqpsk_enable = 1;
        d->ted_sps = sps;
        dsd_fll_band_edge_state_t* f = &d->fll_band_edge_state;
        dsd_fll_band_edge_init(f, sps);
        f->alpha = 0.0f;
        f->beta = 1.0f;
        for (int start = 0; start < f->n_taps; start++) {
            for (float amplitude : amplitudes) {
                for (int i = 0; i < f->n_taps; i++) {
                    seed = seed * 1664525U + 1013904223U;
                    f->delay_r[i] = amplitude * (static_cast<float>(seed >> 16) / 65536.0f - 0.5f);
                    seed = seed * 1664525U + 1013904223U;
                    f->delay_i[i] = amplitude * (static_cast<float>(seed >> 16) / 65536.0f - 0.5f);
                    f->delay_r[i + f->n_taps] = f->delay_r[i];
                    f->delay_i[i + f->n_taps] = f->delay_i[i];
                }
                d->input_cb_buf[0] = 0.3f * amplitude;
                d->input_cb_buf[1] = -0.2f * amplitude;
                f->phase = 0.0f;
                f->freq = 0.0f;
                f->delay_idx = start;
                op25_fll_band_edge_cc(d);

                /* Independently convolve both filters in double precision over
                 * the updated history. A zero starting phase makes the NCO an
                 * identity; beta=1 exposes the clipped error as frequency. */
                double lr = 0.0, li = 0.0, ur = 0.0, ui = 0.0;
                for (int k = 0; k < f->n_taps; k++) {
                    const int idx = start + f->n_taps - k;
                    const double dr = f->delay_r[idx];
                    const double di = f->delay_i[idx];
                    lr += dr * f->taps_lower_r[k] - di * f->taps_lower_i[k];
                    li += dr * f->taps_lower_i[k] + di * f->taps_lower_r[k];
                    ur += dr * f->taps_upper_r[k] - di * f->taps_upper_i[k];
                    ui += dr * f->taps_upper_i[k] + di * f->taps_upper_r[k];
                }
                double error = ur * ur + ui * ui - lr * lr - li * li;
                error = (error > 1.0) ? 1.0 : ((error < -1.0) ? -1.0 : error);
                if (!closef(f->freq, static_cast<float>(error), 1e-5f) || f->delay_idx != (start + 1) % f->n_taps) {
                    DSD_FPRINTF(stderr, "BE-FLL convolution: sps=%d start=%d frequency=%.8f expected=%.8f\n", sps,
                                start, f->freq, error);
                    rc = 1;
                }
            }
        }
    }
    dsd_neo_aligned_free(d);
    return rc;
}

static int
check_upper_taps(const dsd_fll_band_edge_state_t* f, const float* exp_r, const float* exp_i, int n, float tol) {
    for (int i = 0; i < n; i++) {
        float ur = f->taps_upper_r[i];
        float ui = f->taps_upper_i[i];
        if (!closef(ur, exp_r[i], tol) || !closef(ui, exp_i[i], tol)) {
            DSD_FPRINTF(stderr, "BE-FLL taps: mismatch at i=%d got=(%.8f,%.8f) exp=(%.8f,%.8f)\n", i, ur, ui, exp_r[i],
                        exp_i[i]);
            return 0;
        }
        /* Lower band-edge is complex conjugate of upper band-edge. */
        float lr = f->taps_lower_r[i];
        float li = f->taps_lower_i[i];
        if (!closef(lr, ur, tol) || !closef(li, -ui, tol)) {
            DSD_FPRINTF(stderr, "BE-FLL taps: lower != conj(upper) at i=%d lower=(%.8f,%.8f) upper=(%.8f,%.8f)\n", i,
                        lr, li, ur, ui);
            return 0;
        }
    }
    return 1;
}

int
main(void) {
    const float tol = 1e-5f;

    /* Expected values taken from GNU Radio digital.fll_band_edge_cc::print_taps()
     * for rolloff=0.2 and filter_size=2*sps+1. */
    {
        dsd_fll_band_edge_state_t f;
        DSD_MEMSET(&f, 0, sizeof(f));
        dsd_fll_band_edge_init(&f, 5);
        if (!(f.initialized && f.sps == 5 && f.n_taps == 11)) {
            DSD_FPRINTF(stderr, "BE-FLL init: unexpected state sps=%d n_taps=%d initialized=%d\n", f.sps, f.n_taps,
                        f.initialized);
            return 1;
        }

        const float exp_r[11] = {-5.5667e-02f, -7.2177e-02f, -4.8399e-02f, +4.9139e-03f, +5.8086e-02f, +8.0161e-02f,
                                 +5.8086e-02f, +4.9139e-03f, -4.8399e-02f, -7.2177e-02f, -5.5667e-02f};
        const float exp_i[11] = {-4.0445e-02f, +9.1181e-03f, +5.8504e-02f, +7.8105e-02f, +5.4546e-02f, +0.0000e+00f,
                                 -5.4546e-02f, -7.8105e-02f, -5.8504e-02f, -9.1181e-03f, +4.0445e-02f};
        if (!check_upper_taps(&f, exp_r, exp_i, 11, tol)) {
            return 1;
        }
    }

    {
        dsd_fll_band_edge_state_t f;
        DSD_MEMSET(&f, 0, sizeof(f));
        dsd_fll_band_edge_init(&f, 4);
        if (!(f.initialized && f.sps == 4 && f.n_taps == 9)) {
            DSD_FPRINTF(stderr, "BE-FLL init: unexpected state sps=%d n_taps=%d initialized=%d\n", f.sps, f.n_taps,
                        f.initialized);
            return 1;
        }

        const float exp_r[9] = {-6.8359e-02f, -8.5981e-02f, -2.9297e-02f, +5.7321e-02f, +9.8437e-02f,
                                +5.7321e-02f, -2.9297e-02f, -8.5981e-02f, -6.8359e-02f};
        const float exp_i[9] = {-4.9666e-02f, +2.7937e-02f, +9.0166e-02f, +7.8895e-02f, +0.0000e+00f,
                                -7.8895e-02f, -9.0166e-02f, -2.7937e-02f, +4.9666e-02f};
        if (!check_upper_taps(&f, exp_r, exp_i, 9, tol)) {
            return 1;
        }
    }

    return check_convolution();
}
