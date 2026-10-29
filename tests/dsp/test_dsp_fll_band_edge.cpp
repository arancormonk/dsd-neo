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
    return check_convolution();
}
