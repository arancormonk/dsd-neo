// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Regression test for OP25/GNU Radio-compatible band-edge FLL filter design. */

#include <dsd-neo/dsp/costas.h>
#include <dsd-neo/dsp/demod_state.h>
#include <dsd-neo/runtime/mem.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
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

/* P25 (every non-TETRA chain) band-edge design, pinned to the values the design produced before it took per-chain
 * parameters (rolloff 0.2, loop bandwidth 2*pi/sps/350, filter size 2*sps+1). Upper taps in stored (reversed) order.
 *
 * Tolerances: on an IEEE build both are met exactly. A fast-math build with FMA contraction moves the design by up to
 * 4.5e-8 absolute in a tap and 3.5e-7 relative in a gain (measured over sps 1..24), so the bounds allow a few times
 * that and stay far below any parameter change: rolloff 0.2 -> 0.35 moves the taps by ~1e-2 and a loop-bandwidth
 * scale moves the gains by whole factors. */
struct p25_pin {
    int sps;
    float loop_bw;
    float alpha;
    float beta;
    float upper_r[21];
    float upper_i[21];
};

static const p25_pin kP25Pins[] = {
    {2,
     8.975978941e-03f,
     2.506767213e-02f,
     3.182078362e-04f,
     {-1.497986913e-01f, -5.940934271e-02f, 1.851614267e-01f, -5.099582300e-02f, -1.092343852e-01f},
     {-1.088352129e-01f, 1.828430593e-01f, 0.000000000e+00f, -1.569489092e-01f, 7.936348766e-02f}},
    {4,
     4.487989470e-03f,
     1.261363830e-02f,
     8.005846030e-05f,
     {-6.835893542e-02f, -8.598110080e-02f, -2.929671109e-02f, 5.732072145e-02f, 9.843689948e-02f, 5.732072145e-02f,
      -2.929671109e-02f, -8.598110080e-02f, -6.835893542e-02f},
     {-4.966571555e-02f, 2.793693356e-02f, 9.016595036e-02f, 7.889521867e-02f, 0.000000000e+00f, -7.889521867e-02f,
      -9.016595036e-02f, -2.793693356e-02f, 4.966571555e-02f}},
    {5,
     3.590391716e-03f,
     1.010372769e-02f,
     5.130249701e-05f,
     {-5.566707626e-02f, -7.217736542e-02f, -4.839866236e-02f, 4.913912620e-03f, 5.808572844e-02f, 8.016061783e-02f,
      5.808573216e-02f, 4.913912620e-03f, -4.839866236e-02f, -7.217735797e-02f, -5.566707626e-02f},
     {-4.044453055e-02f, 9.118102491e-03f, 5.850390717e-02f, 7.810457796e-02f, 5.454614013e-02f, 0.000000000e+00f,
      -5.454614386e-02f, -7.810457796e-02f, -5.850390717e-02f, -9.118101560e-03f, 4.044453055e-02f}},
    {8,
     2.243994735e-03f,
     6.326865871e-03f,
     2.007823241e-05f,
     {-3.576643765e-02f, -4.528295994e-02f, -4.498662055e-02f, -3.433579579e-02f, -1.532848552e-02f, 7.888782769e-03f,
      2.999107353e-02f, 4.578296840e-02f, 5.150368437e-02f, 4.578296840e-02f, 2.999107353e-02f, 7.888782769e-03f,
      -1.532848552e-02f, -3.433579579e-02f, -4.498662055e-02f, -4.528295994e-02f, -3.576643765e-02f},
     {-2.598585747e-02f, -7.172134239e-03f, 1.461702865e-02f, 3.433578089e-02f, 4.717620090e-02f, 4.980784655e-02f,
      4.127917811e-02f, 2.332758904e-02f, 0.000000000e+00f, -2.332758904e-02f, -4.127917811e-02f, -4.980784655e-02f,
      -4.717620090e-02f, -3.433578089e-02f, -1.461702865e-02f, 7.172134239e-03f, 2.598585747e-02f}},
    {10,
     1.795195858e-03f,
     5.064705852e-03f,
     1.285822691e-05f,
     {-2.888575383e-02f, -3.561896086e-02f, -3.745297715e-02f, -3.384960443e-02f, -2.511416189e-02f, -1.237961929e-02f,
      2.549838740e-03f,  1.747326367e-02f,  3.014079854e-02f,  3.861670569e-02f,  4.159549996e-02f,  3.861670569e-02f,
      3.014080226e-02f,  1.747326367e-02f,  2.549838740e-03f,  -1.237961929e-02f, -2.511416189e-02f, -3.384960815e-02f,
      -3.745297343e-02f, -3.561896086e-02f, -2.888575383e-02f},
     {-2.098674513e-02f, -9.145402350e-03f, 4.731400870e-03f,  1.860897243e-02f,  3.035779297e-02f,  3.810052946e-02f,
      4.052861780e-02f,  3.713258728e-02f,  2.830410004e-02f,  1.528943609e-02f,  0.000000000e+00f,  -1.528943609e-02f,
      -2.830410376e-02f, -3.713258728e-02f, -4.052861780e-02f, -3.810052946e-02f, -3.035779297e-02f, -1.860897429e-02f,
      -4.731400404e-03f, 9.145402350e-03f,  2.098674513e-02f}},
};

static const float kPinTapTol = 2e-7f;
static const float kPinGainRelTol = 2e-6f;

static int
close_rel(float got, float want, float rel) {
    float tol = want * rel;
    if (tol < 0.0f) {
        tol = -tol;
    }
    return closef(got, want, tol);
}

static int
check_p25_pin(const char* path, const dsd_fll_band_edge_state_t* f, const p25_pin& pin) {
    const int n = 2 * pin.sps + 1;
    if (!f->initialized || f->sps != pin.sps || f->n_taps != n) {
        DSD_FPRINTF(stderr, "BE-FLL P25 pin (%s): sps=%d state sps=%d n_taps=%d initialized=%d\n", path, pin.sps,
                    f->sps, f->n_taps, f->initialized);
        return 0;
    }
    if (!close_rel(f->loop_bw, pin.loop_bw, kPinGainRelTol) || !close_rel(f->alpha, pin.alpha, kPinGainRelTol)
        || !close_rel(f->beta, pin.beta, kPinGainRelTol) || !closef(f->max_freq, 1.0f, 0.0f)
        || !closef(f->min_freq, -1.0f, 0.0f)) {
        DSD_FPRINTF(stderr,
                    "BE-FLL P25 pin (%s): sps=%d loop_bw=%.9e alpha=%.9e beta=%.9e limits=(%f,%f) want %.9e %.9e "
                    "%.9e\n",
                    path, pin.sps, f->loop_bw, f->alpha, f->beta, f->min_freq, f->max_freq, pin.loop_bw, pin.alpha,
                    pin.beta);
        return 0;
    }
    if (!check_upper_taps(f, pin.upper_r, pin.upper_i, n, kPinTapTol)) {
        DSD_FPRINTF(stderr, "BE-FLL P25 pin (%s): sps=%d taps differ\n", path, pin.sps);
        return 0;
    }
    return 1;
}

/* Runs the FLL block once on a fresh demod so it designs lazily, the way a stream's first CQPSK block does. */
static demod_state*
lazy_fll_demod(int sps, int tetra_chain) {
    demod_state* d = static_cast<demod_state*>(dsd_neo_aligned_malloc(sizeof(demod_state)));
    if (!d) {
        return nullptr;
    }
    DSD_MEMSET(d, 0, sizeof(*d));
    d->cqpsk_enable = 1;
    d->tetra_chain = tetra_chain;
    d->ted_sps = sps;
    d->rate_out = 24000;
    d->lowpassed = d->input_cb_buf;
    d->lp_len = 2;
    op25_fll_band_edge_cc(d);
    return d;
}

static int
check_p25_pins(void) {
    for (const p25_pin& pin : kP25Pins) {
        dsd_fll_band_edge_state_t f;
        DSD_MEMSET(&f, 0, sizeof(f));
        dsd_fll_band_edge_init(&f, pin.sps);
        if (!check_p25_pin("init", &f, pin)) {
            return 1;
        }
        demod_state* d = lazy_fll_demod(pin.sps, 0);
        if (!d) {
            return 1;
        }
        const int ok = check_p25_pin("lazy", &d->fll_band_edge_state, pin);
        dsd_neo_aligned_free(d);
        if (!ok) {
            return 1;
        }
    }
    return 0;
}

static int
same_bits(float a, float b) {
    uint32_t ua = 0;
    uint32_t ub = 0;
    DSD_MEMCPY(&ua, &a, sizeof(ua));
    DSD_MEMCPY(&ub, &b, sizeof(ub));
    return ua == ub;
}

/* The design outputs (taps, tap count, loop gains, limits and the parameters they came from), bit for bit. */
static int
same_design(const dsd_fll_band_edge_state_t* a, const dsd_fll_band_edge_state_t* b) {
    if (a->n_taps != b->n_taps || a->sps != b->sps || !same_bits(a->rolloff, b->rolloff)
        || !same_bits(a->loop_bw_scale, b->loop_bw_scale) || !same_bits(a->loop_bw, b->loop_bw)
        || !same_bits(a->alpha, b->alpha) || !same_bits(a->beta, b->beta) || !same_bits(a->max_freq, b->max_freq)
        || !same_bits(a->min_freq, b->min_freq)) {
        return 0;
    }
    for (int i = 0; i < a->n_taps; i++) {
        if (!same_bits(a->taps_upper_r[i], b->taps_upper_r[i]) || !same_bits(a->taps_upper_i[i], b->taps_upper_i[i])
            || !same_bits(a->taps_lower_r[i], b->taps_lower_r[i])
            || !same_bits(a->taps_lower_i[i], b->taps_lower_i[i])) {
            return 0;
        }
    }
    return 1;
}

static int
check_params_for(void) {
    const dsd_fll_band_edge_params p25 = dsd_fll_band_edge_params_for(0);
    const dsd_fll_band_edge_params tetra = dsd_fll_band_edge_params_for(1);
    if (!closef(p25.rolloff, 0.2f, 1e-9f) || !closef(p25.loop_bw_scale, 1.0f, 1e-9f)
        || !closef(tetra.rolloff, 0.35f, 1e-9f) || !closef(tetra.loop_bw_scale, 4.0f, 1e-9f)) {
        DSD_FPRINTF(stderr, "BE-FLL params: P25 (%f, %f) TETRA (%f, %f)\n", p25.rolloff, p25.loop_bw_scale,
                    tetra.rolloff, tetra.loop_bw_scale);
        return 1;
    }
    return 0;
}

/* The P25 wrapper, the parameter init and the lazy design in the FLL block design the same filter for the same
 * parameters, at every sps the taps hold. */
static int
check_init_paths_agree(void) {
    for (int tetra_chain = 0; tetra_chain <= 1; tetra_chain++) {
        const dsd_fll_band_edge_params params = dsd_fll_band_edge_params_for(tetra_chain);
        for (int sps = 1; sps <= 23; sps++) {
            dsd_fll_band_edge_state_t f;
            DSD_MEMSET(&f, 0, sizeof(f));
            dsd_fll_band_edge_init_params(&f, sps, params);
            if (!closef(f.rolloff, params.rolloff, 1e-9f) || !closef(f.loop_bw_scale, params.loop_bw_scale, 1e-9f)) {
                DSD_FPRINTF(stderr, "BE-FLL paths: tetra=%d sps=%d stored params (%f, %f)\n", tetra_chain, sps,
                            f.rolloff, f.loop_bw_scale);
                return 1;
            }
            demod_state* d = lazy_fll_demod(sps, tetra_chain);
            if (!d) {
                return 1;
            }
            const int lazy_ok = same_design(&f, &d->fll_band_edge_state);
            dsd_neo_aligned_free(d);
            if (!lazy_ok) {
                DSD_FPRINTF(stderr, "BE-FLL paths: tetra=%d sps=%d init and lazy designs differ\n", tetra_chain, sps);
                return 1;
            }
            if (!tetra_chain) {
                dsd_fll_band_edge_state_t w;
                DSD_MEMSET(&w, 0, sizeof(w));
                dsd_fll_band_edge_init(&w, sps);
                if (!same_design(&f, &w)) {
                    DSD_FPRINTF(stderr, "BE-FLL paths: sps=%d P25 wrapper differs from the P25 parameters\n", sps);
                    return 1;
                }
            }
        }
    }
    return 0;
}

/* The TETRA parameters: GNU Radio's band-edge design at rolloff 0.35 (computed here independently, in double), filter
 * size 2*sps+1, and loop bandwidth 4 * 2*pi/sps/350 with control_loop's critically damped gains. */
static int
check_tetra_design(void) {
    const double pi = 3.14159265358979323846;
    const double rolloff = 0.35;
    const int rates[] = {2, 4, 8};
    for (int sps : rates) {
        dsd_fll_band_edge_state_t f;
        DSD_MEMSET(&f, 0, sizeof(f));
        dsd_fll_band_edge_init_params(&f, sps, dsd_fll_band_edge_params_for(1));
        const int n = 2 * sps + 1;
        if (!f.initialized || f.sps != sps || f.n_taps != n) {
            DSD_FPRINTF(stderr, "BE-FLL TETRA: sps=%d state sps=%d n_taps=%d\n", sps, f.sps, f.n_taps);
            return 1;
        }

        const double loop_bw = 4.0 * 2.0 * pi / (double)sps / 350.0;
        const double damping = 0.70710678118654752440;
        const double denom = 1.0 + 2.0 * damping * loop_bw + loop_bw * loop_bw;
        const double alpha = 4.0 * damping * loop_bw / denom;
        const double beta = 4.0 * loop_bw * loop_bw / denom;
        if (!closef(f.loop_bw, (float)loop_bw, 1e-7f) || !closef(f.alpha, (float)alpha, 1e-6f)
            || !closef(f.beta, (float)beta, 1e-7f)) {
            DSD_FPRINTF(stderr, "BE-FLL TETRA: sps=%d loop_bw=%.9e alpha=%.9e beta=%.9e want %.9e %.9e %.9e\n", sps,
                        f.loop_bw, f.alpha, f.beta, loop_bw, alpha, beta);
            return 1;
        }

        double bb[FLL_BAND_EDGE_MAX_TAPS];
        double power = 0.0;
        const double m = (double)(int)((double)n / (double)sps + 0.5);
        for (int i = 0; i < n; i++) {
            const double k = -m + (double)i * 2.0 / (double)sps;
            const double am = rolloff * k - 0.5;
            const double ap = rolloff * k + 0.5;
            const double sm = (am > -1e-9 && am < 1e-9) ? 1.0 : sin(pi * am) / (pi * am);
            const double sp = (ap > -1e-9 && ap < 1e-9) ? 1.0 : sin(pi * ap) / (pi * ap);
            bb[i] = sm + sp;
            power += bb[i] * bb[i];
        }
        float exp_r[FLL_BAND_EDGE_MAX_TAPS];
        float exp_i[FLL_BAND_EDGE_MAX_TAPS];
        const int half = (n - 1) / 2;
        for (int i = 0; i < n; i++) {
            const double freq = (double)(-half + i) / (2.0 * (double)sps);
            const double phase = 2.0 * pi * (1.0 + rolloff) * freq;
            exp_r[n - 1 - i] = (float)(bb[i] / power * cos(phase));
            exp_i[n - 1 - i] = (float)(bb[i] / power * sin(phase));
        }
        if (!check_upper_taps(&f, exp_r, exp_i, n, 1e-6f)) {
            DSD_FPRINTF(stderr, "BE-FLL TETRA: sps=%d taps differ from the rolloff 0.35 design\n", sps);
            return 1;
        }
    }
    return 0;
}

/* A same-sps change of parameters redesigns the filter and gains and keeps the frequency estimate; a same-sps,
 * same-parameter init keeps the filter. Both through the parameter init and through the FLL block. */
static int
check_same_sps_param_switch(void) {
    const int sps = 4;
    const float kept_freq = 0.0125f;

    dsd_fll_band_edge_state_t want;
    DSD_MEMSET(&want, 0, sizeof(want));
    dsd_fll_band_edge_init_params(&want, sps, dsd_fll_band_edge_params_for(1));

    dsd_fll_band_edge_state_t f;
    DSD_MEMSET(&f, 0, sizeof(f));
    dsd_fll_band_edge_init(&f, sps);
    f.freq = kept_freq;
    f.phase = 0.5f;
    f.delay_idx = 3;
    f.delay_r[3] = 0.25f;
    dsd_fll_band_edge_init_params(&f, sps, dsd_fll_band_edge_params_for(1));
    if (!same_design(&f, &want) || !closef(f.freq, kept_freq, 0.0f) || !closef(f.phase, 0.0f, 0.0f) || f.delay_idx != 0
        || !closef(f.delay_r[3], 0.0f, 0.0f)) {
        DSD_FPRINTF(stderr, "BE-FLL switch (init): redesigned=%d freq=%.9f phase=%f delay_idx=%d\n",
                    same_design(&f, &want), f.freq, f.phase, f.delay_idx);
        return 1;
    }

    /* Same sps, same parameters: the filter stays as it is (a corrupted tap survives), the frequency too. */
    f.taps_upper_r[0] = 9.0f;
    dsd_fll_band_edge_init_params(&f, sps, dsd_fll_band_edge_params_for(1));
    if (!closef(f.taps_upper_r[0], 9.0f, 0.0f) || !closef(f.freq, kept_freq, 0.0f)) {
        DSD_FPRINTF(stderr, "BE-FLL switch (same params): tap0=%f freq=%.9f\n", f.taps_upper_r[0], f.freq);
        return 1;
    }

    /* Back to P25: the P25 design returns. */
    dsd_fll_band_edge_init_params(&f, sps, dsd_fll_band_edge_params_for(0));
    dsd_fll_band_edge_state_t p25;
    DSD_MEMSET(&p25, 0, sizeof(p25));
    dsd_fll_band_edge_init(&p25, sps);
    if (!same_design(&f, &p25) || !closef(f.freq, kept_freq, 0.0f)) {
        DSD_FPRINTF(stderr, "BE-FLL switch (back to P25): redesigned=%d freq=%.9f\n", same_design(&f, &p25), f.freq);
        return 1;
    }

    /* The FLL block: a P25-designed loop whose demod turns to the TETRA chain redesigns on its next block. A zero
     * input gives zero band-edge error, so the kept frequency passes through the block unchanged. */
    demod_state* d = lazy_fll_demod(sps, 0);
    if (!d) {
        return 1;
    }
    d->fll_band_edge_state.freq = kept_freq;
    d->tetra_chain = 1;
    d->input_cb_buf[0] = 0.0f;
    d->input_cb_buf[1] = 0.0f;
    op25_fll_band_edge_cc(d);
    const int lazy_ok = same_design(&d->fll_band_edge_state, &want);
    const float lazy_freq = d->fll_band_edge_state.freq;
    dsd_neo_aligned_free(d);
    if (!lazy_ok || !closef(lazy_freq, kept_freq, 0.0f)) {
        DSD_FPRINTF(stderr, "BE-FLL switch (block): redesigned=%d freq=%.9f\n", lazy_ok, lazy_freq);
        return 1;
    }
    return 0;
}

/* The TETRA mixer bound: 2*pi*8000/rate_out rad/sample, and 0 for a rate that is not positive. */
static int
check_pre_limit(void) {
    const int rates[] = {36000, 72000, 144000, 48000};
    for (int rate : rates) {
        const double want = 2.0 * 3.14159265358979323846 * 8000.0 / (double)rate;
        const float got = dsd_tetra_pre_limit_rad(rate);
        if (fabs((double)got - want) > 1e-6) {
            DSD_FPRINTF(stderr, "TETRA pre limit: rate=%d got=%.9f want=%.9f\n", rate, got, want);
            return 1;
        }
    }
    if (!closef(dsd_tetra_pre_limit_rad(0), 0.0f, 0.0f) || !closef(dsd_tetra_pre_limit_rad(-72000), 0.0f, 0.0f)) {
        DSD_FPRINTF(stderr, "TETRA pre limit: a non-positive rate gives a nonzero bound\n");
        return 1;
    }
    return 0;
}

static void
mixer_state(dsd_fll_band_edge_state_t* f, float pre, float freq) {
    DSD_MEMSET(f, 0, sizeof(*f));
    dsd_fll_band_edge_init_params(f, 4, dsd_fll_band_edge_params_for(1));
    f->pre_freq = pre;
    f->freq = freq;
    f->pre_re = 0.6f;
    f->pre_im = 0.8f;
}

/* The combined estimate is pre_freq + freq, and reading it changes nothing. */
static int
check_total_accessor(void) {
    dsd_fll_band_edge_state_t f;
    mixer_state(&f, 0.3f, -0.1f);
    f.phase = 0.25f;
    dsd_fll_band_edge_state_t before;
    DSD_MEMCPY(&before, &f, sizeof(f));
    const float a = dsd_fll_band_edge_total_freq(&f);
    const float b = dsd_fll_band_edge_total_freq(&f);
    if (!closef(a, 0.2f, 1e-7f) || !closef(a, b, 0.0f) || memcmp(&before, &f, sizeof(f)) != 0) {
        DSD_FPRINTF(stderr, "FLL total: a=%.9f b=%.9f mutated=%d\n", a, b, memcmp(&before, &f, sizeof(f)) != 0);
        return 1;
    }
    /* Off the TETRA chain the total is the FLL frequency exactly. */
    mixer_state(&f, 0.0f, 0.123456f);
    if (!closef(dsd_fll_band_edge_total_freq(&f), f.freq, 0.0f)) { /* exact: 0 + x == x */
        DSD_FPRINTF(stderr, "FLL total: pre 0 changes the value\n");
        return 1;
    }
    if (!closef(dsd_fll_band_edge_total_freq(nullptr), 0.0f, 0.0f)) {
        DSD_FPRINTF(stderr, "FLL total: null state\n");
        return 1;
    }
    return 0;
}

/* Transfer moves what fits under the bound into the mixer and conserves the total; the clamp leaves the excess in
 * the FLL. */
static int
check_transfer(void) {
    const float limit = dsd_tetra_pre_limit_rad(72000); /* ~0.698 rad */

    const struct {
        float pre;
        float freq;
    } cases[] = {
        {0.0f, 0.1f}, {0.2f, 0.3f}, {-0.4f, 0.05f}, {0.0f, -0.65f}, {0.6f, 0.5f}, {-0.6f, -0.9f}, {0.698f, 0.0f},
    };

    for (const auto& c : cases) {
        dsd_fll_band_edge_state_t f;
        mixer_state(&f, c.pre, c.freq);
        const double total = (double)c.pre + (double)c.freq;
        dsd_fll_band_edge_transfer(&f, limit);
        const double after = (double)dsd_fll_band_edge_total_freq(&f);
        double want_pre = total;
        if (want_pre > limit) {
            want_pre = limit;
        } else if (want_pre < -limit) {
            want_pre = -limit;
        }
        if (fabs(after - total) > 1e-6 || fabs((double)f.pre_freq - want_pre) > 1e-6
            || fabs((double)f.freq - (total - want_pre)) > 1e-6) {
            DSD_FPRINTF(stderr, "FLL transfer: pre=%.6f freq=%.6f -> pre=%.9f freq=%.9f total %.9f->%.9f\n", c.pre,
                        c.freq, f.pre_freq, f.freq, total, after);
            return 1;
        }
    }
    /* Repeated transfers with the FLL still adding converge on the bound and keep the total. */
    dsd_fll_band_edge_state_t f;
    mixer_state(&f, 0.0f, 0.0f);
    double total = 0.0;
    for (int i = 0; i < 1000; i++) {
        f.freq += 0.003f;
        total += 0.003;
        dsd_fll_band_edge_transfer(&f, limit);
    }
    if (fabs((double)dsd_fll_band_edge_total_freq(&f) - total) > 1e-4 || !closef(f.pre_freq, limit, 1e-6f)) {
        DSD_FPRINTF(stderr, "FLL transfer drift: total=%.9f want=%.9f pre=%.9f\n", dsd_fll_band_edge_total_freq(&f),
                    total, f.pre_freq);
        return 1;
    }
    return 0;
}

/* Split puts min(|total|, bound) in the mixer and the rest in the FLL, clamped to its range; fold puts the total
 * back in the FLL and resets the mixer. Both round-trip the total. */
static int
check_split_fold(void) {
    const float limit = dsd_tetra_pre_limit_rad(36000); /* ~1.396 rad */
    const float totals[] = {0.0f, 0.25f, -0.5f, 1.3f, -1.39f, 2.0f, -2.2f};
    for (float total : totals) {
        dsd_fll_band_edge_state_t f;
        mixer_state(&f, 0.4f, 0.2f);
        dsd_fll_band_edge_split_total(&f, total, limit);
        float want_pre = total > limit ? limit : (total < -limit ? -limit : total);
        if (!closef(f.pre_freq, want_pre, 1e-7f) || !closef(dsd_fll_band_edge_total_freq(&f), total, 1e-6f)) {
            DSD_FPRINTF(stderr, "FLL split: total=%.6f -> pre=%.9f freq=%.9f\n", total, f.pre_freq, f.freq);
            return 1;
        }
        dsd_fll_band_edge_fold(&f);
        /* |total| <= 1 folds whole; beyond, the FLL's own +-1 clamp holds. */
        const float want_freq = total > 1.0f ? 1.0f : (total < -1.0f ? -1.0f : total);
        if (!closef(f.pre_freq, 0.0f, 0.0f) || !closef(f.freq, want_freq, 1e-6f) || !closef(f.pre_re, 1.0f, 0.0f)
            || !closef(f.pre_im, 0.0f, 0.0f)) {
            DSD_FPRINTF(stderr, "FLL fold: total=%.6f -> pre=%.9f freq=%.9f phasor=(%f,%f)\n", total, f.pre_freq,
                        f.freq, f.pre_re, f.pre_im);
            return 1;
        }
    }
    /* Beyond bound + FLL range the excess is dropped: the FLL part clamps. */
    dsd_fll_band_edge_state_t f;
    mixer_state(&f, 0.0f, 0.0f);
    dsd_fll_band_edge_split_total(&f, 3.0f, limit);
    if (!closef(f.pre_freq, limit, 1e-7f) || !closef(f.freq, 1.0f, 0.0f)) {
        DSD_FPRINTF(stderr, "FLL split excess: pre=%.9f freq=%.9f\n", f.pre_freq, f.freq);
        return 1;
    }
    /* A zero bound (TETRA chain inactive) leaves the whole estimate in the FLL, bit for bit. */
    const float plain[] = {0.0f, 0.0123456f, -0.75f, 0.999999f};
    for (float x : plain) {
        mixer_state(&f, 0.0f, 0.0f);
        dsd_fll_band_edge_split_total(&f, x, 0.0f);
        if (!closef(f.freq, x, 0.0f) || !closef(f.pre_freq, 0.0f, 0.0f)) { /* exact by design: x - 0 == x */
            DSD_FPRINTF(stderr, "FLL split, zero bound: x=%.9f -> pre=%.9f freq=%.9f\n", x, f.pre_freq, f.freq);
            return 1;
        }
    }
    /* A non-finite total clears rather than poisoning the loop. */
    mixer_state(&f, 0.1f, 0.1f);
    dsd_fll_band_edge_split_total(&f, NAN, limit);
    if (!closef(f.pre_freq, 0.0f, 0.0f) || !closef(f.freq, 0.0f, 0.0f)) {
        DSD_FPRINTF(stderr, "FLL split NaN: pre=%f freq=%f\n", f.pre_freq, f.freq);
        return 1;
    }
    return 0;
}

/* premix_block() of a +6 kHz tone with pre_freq at minus its frequency (the FLL's sign: an estimate is minus the
 * carrier offset) leaves DC: residual power at least 60 dB under the tone. */
static int
check_premix_cancels_tone(void) {
    const int rate = 72000;
    const int n = 72000; /* one second */
    const float w = (float)(2.0 * 3.14159265358979323846 * 6000.0 / (double)rate);
    float* iq = static_cast<float*>(dsd_neo_aligned_malloc(sizeof(float) * 2U * (size_t)n));
    if (!iq) {
        return 1;
    }
    const double phi0 = 0.4;
    for (int i = 0; i < n; i++) {
        const double ph = phi0 + (double)w * (double)i;
        iq[2 * i] = (float)(0.5 * cos(ph));
        iq[2 * i + 1] = (float)(0.5 * sin(ph));
    }
    dsd_fll_band_edge_state_t f;
    DSD_MEMSET(&f, 0, sizeof(f)); /* a zeroed phasor reads as (1, 0) */
    f.pre_freq = -w;
    for (int off = 0; off < n; off += 512) {
        const int len = (n - off) < 512 ? (n - off) : 512;
        dsd_tetra_premix_block(&f, iq + 2 * off, len);
    }
    const double dc_r = 0.5 * cos(phi0);
    const double dc_i = 0.5 * sin(phi0);
    double resid = 0.0;
    double power = 0.0;
    for (int i = 0; i < n; i++) {
        const double er = iq[2 * i] - dc_r;
        const double ei = iq[2 * i + 1] - dc_i;
        resid += er * er + ei * ei;
        power += 0.25;
    }
    dsd_neo_aligned_free(iq);
    const double db = 10.0 * log10(resid / power + 1e-30);
    const double mag = sqrt((double)f.pre_re * f.pre_re + (double)f.pre_im * f.pre_im);
    if (!(db < -60.0) || fabs(mag - 1.0) > 1e-6) {
        DSD_FPRINTF(stderr, "TETRA premix: residual %.1f dB (want < -60), phasor |p|=%.9f\n", db, mag);
        return 1;
    }
    return 0;
}

/* The rotation is phase continuous across block boundaries: blocks of 1, 7, 256 and irregular pairs all give the
 * one-block rotation x[n] * exp(+j*w*n), the FLL NCO's sign. */
static int
check_premix_partitions(void) {
    const int n = 4096;
    const float w = 0.37f;
    static float in[2 * 4096];
    static float out[2 * 4096];
    uint32_t seed = 0x9e3779b9U;
    for (int i = 0; i < 2 * n; i++) {
        seed = seed * 1664525U + 1013904223U;
        in[i] = static_cast<float>(seed >> 8) / 16777216.0f - 0.5f;
    }
    const int irregular[] = {3, 1, 50, 129, 2, 400, 17, 1000, 5};
    for (int pattern = 0; pattern < 4; pattern++) {
        DSD_MEMCPY(out, in, sizeof(out));
        dsd_fll_band_edge_state_t f;
        DSD_MEMSET(&f, 0, sizeof(f));
        f.pre_re = 1.0f;
        f.pre_freq = w;
        int off = 0;
        int k = 0;
        while (off < n) {
            int len = pattern == 0 ? 1 : (pattern == 1 ? 7 : (pattern == 2 ? 256 : irregular[k++ % 9]));
            if (len > n - off) {
                len = n - off;
            }
            dsd_tetra_premix_block(&f, out + 2 * off, len);
            off += len;
        }
        double worst = 0.0;
        for (int i = 0; i < n; i++) {
            const double ph = (double)w * (double)i;
            const double c = cos(ph);
            const double s = sin(ph);
            const double wr = in[2 * i] * c - in[2 * i + 1] * s;
            const double wi = in[2 * i] * s + in[2 * i + 1] * c;
            const double e = fabs(out[2 * i] - wr) + fabs(out[2 * i + 1] - wi);
            worst = e > worst ? e : worst;
        }
        if (worst > 1e-4) {
            DSD_FPRINTF(stderr, "TETRA premix partition %d: worst error %.3g\n", pattern, worst);
            return 1;
        }
    }
    /* A phasor whose magnitude has strayed is renormalized: the rotation has unit gain, and the phasor it leaves is a
     * unit one. */
    {
        dsd_fll_band_edge_state_t g;
        DSD_MEMSET(&g, 0, sizeof(g));
        g.pre_re = 0.9f;
        g.pre_im = 1.2f; /* |p| = 1.5 */
        g.pre_freq = w;
        DSD_MEMCPY(out, in, sizeof(out));
        dsd_tetra_premix_block(&g, out, 64);
        for (int i = 0; i < 64; i++) {
            const double a = hypot((double)in[2 * i], (double)in[2 * i + 1]);
            const double b = hypot((double)out[2 * i], (double)out[2 * i + 1]);
            if (fabs(a - b) > 1e-6) {
                DSD_FPRINTF(stderr, "TETRA premix: gain %.9f at n=%d with a stray phasor\n", b / a, i);
                return 1;
            }
        }
        const double mag = hypot((double)g.pre_re, (double)g.pre_im);
        if (fabs(mag - 1.0) > 1e-6) {
            DSD_FPRINTF(stderr, "TETRA premix: left |p|=%.9f\n", mag);
            return 1;
        }
    }
    /* No pairs, no state: nothing moves. */
    dsd_fll_band_edge_state_t f;
    DSD_MEMSET(&f, 0, sizeof(f));
    f.pre_re = 0.6f;
    f.pre_im = 0.8f;
    f.pre_freq = w;
    dsd_tetra_premix_block(&f, out, 0);
    dsd_tetra_premix_block(nullptr, out, 4);
    dsd_tetra_premix_block(&f, nullptr, 4);
    if (!closef(f.pre_re, 0.6f, 0.0f) || !closef(f.pre_im, 0.8f, 0.0f)) {
        DSD_FPRINTF(stderr, "TETRA premix: an empty block moved the phasor\n");
        return 1;
    }
    return 0;
}

/* A QPSK-like stream: random (+-1, +-j) symbols held for sps samples, carried at an offset of w rad/sample. */
struct qpsk_source {
    uint32_t seed;
    double phase;
};

static void
qpsk_block(qpsk_source* src, float* buf, int pairs, int sps, double w) {
    for (int n = 0; n < pairs; n++) {
        if (n % sps == 0) {
            src->seed = src->seed * 1664525U + 1013904223U;
        }
        const double sr = ((src->seed >> 30) & 1U) ? 1.0 : -1.0;
        const double si = ((src->seed >> 29) & 1U) ? 1.0 : -1.0;
        const double c = cos(src->phase);
        const double s = sin(src->phase);
        buf[2 * n] = (float)(0.5 * (sr * c - si * s));
        buf[2 * n + 1] = (float)(0.5 * (sr * s + si * c));
        src->phase += w;
    }
}

/* Run the band-edge FLL on the stream block by block, alone or as the TETRA chain runs it (mixer, FLL, transfer), and
 * report the mean mixer and FLL frequencies over the last quarter of the blocks, in Hz. */
static int
run_fll_loop(int with_mixer, double offset_hz, int blocks, double* out_pre_hz, double* out_freq_hz) {
    const int rate = 72000;
    const int sps = 4;
    const int pairs = 512;
    static float buf[2 * 512];
    demod_state* d = static_cast<demod_state*>(dsd_neo_aligned_malloc(sizeof(demod_state)));
    if (!d) {
        return 1;
    }
    DSD_MEMSET(d, 0, sizeof(*d));
    d->cqpsk_enable = 1;
    d->ted_sps = sps;
    d->rate_out = rate;
    d->tetra_chain = 1;
    qpsk_source src = {1U, 0.0};
    const double w = 2.0 * 3.14159265358979323846 * offset_hz / (double)rate;
    const double hz_per_rad = (double)rate / (2.0 * 3.14159265358979323846);
    double pre_sum = 0.0;
    double freq_sum = 0.0;
    int counted = 0;
    dsd_fll_band_edge_state_t* f = &d->fll_band_edge_state;
    for (int b = 0; b < blocks; b++) {
        qpsk_block(&src, buf, pairs, sps, w);
        if (with_mixer) {
            dsd_tetra_premix_block(f, buf, pairs);
        }
        d->lowpassed = buf;
        d->lp_len = pairs * 2;
        op25_fll_band_edge_cc(d);
        if (with_mixer) {
            dsd_fll_band_edge_transfer(f, dsd_tetra_pre_limit_rad(rate));
        }
        if (b >= blocks - blocks / 4) {
            pre_sum += (double)f->pre_freq * hz_per_rad;
            freq_sum += (double)f->freq * hz_per_rad;
            counted++;
        }
    }
    dsd_neo_aligned_free(d);
    *out_pre_hz = pre_sum / (double)counted;
    *out_freq_hz = freq_sum / (double)counted;
    return 0;
}

/* Closed loop: with the mixer ahead of the FLL and the residual moved into it each block, the loop settles on the
 * estimate the FLL alone reaches (minus the carrier offset), carried by the mixer, with the FLL's residual near 0. A
 * mixer of the opposite sign to the FLL's NCO runs away to its bound instead. */
static int
check_premix_closed_loop(void) {
    const double offsets[] = {600.0, -3000.0, 5000.0};
    for (double offset : offsets) {
        double alone_pre = 0.0;
        double alone_freq = 0.0;
        double mix_pre = 0.0;
        double mix_freq = 0.0;
        if (run_fll_loop(0, offset, 2000, &alone_pre, &alone_freq) != 0
            || run_fll_loop(1, offset, 2000, &mix_pre, &mix_freq) != 0) {
            return 1;
        }
        const double alone = alone_pre + alone_freq;
        const double mixed = mix_pre + mix_freq;
        if (fabs(alone + offset) > 30.0 || fabs(mixed - alone) > 30.0 || fabs(mix_freq) > 30.0
            || fabs(mix_pre - mixed) > 30.0) {
            DSD_FPRINTF(stderr,
                        "TETRA mixer loop: offset %+.0f Hz: FLL alone %.1f Hz; with mixer pre %.1f Hz freq %.1f Hz "
                        "(total %.1f Hz)\n",
                        offset, alone, mix_pre, mix_freq, mixed);
            return 1;
        }
    }
    return 0;
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

    if (check_p25_pins() != 0) {
        return 1;
    }
    if (check_params_for() != 0 || check_init_paths_agree() != 0 || check_tetra_design() != 0
        || check_same_sps_param_switch() != 0) {
        return 1;
    }
    if (check_pre_limit() != 0 || check_total_accessor() != 0 || check_transfer() != 0 || check_split_fold() != 0
        || check_premix_cancels_tone() != 0 || check_premix_partitions() != 0 || check_premix_closed_loop() != 0) {
        return 1;
    }
    return check_convolution();
}
