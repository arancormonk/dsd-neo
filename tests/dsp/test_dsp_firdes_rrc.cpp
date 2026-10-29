// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* The root-raised-cosine designer (a port of GNU Radio's firdes::root_raised_cosine()) against the closed-form
 * impulse response, and the TETRA matched filter built on it: length, energy normalization and the inter-symbol
 * interference of the cascade. */

#include <cmath>
#include <dsd-neo/dsp/firdes.h>
#include <stdio.h>
#include "dsd-neo/core/safe_api.h"

static const double kPi = 3.14159265358979323846;

/* The RRC impulse response in closed form, t in symbols: the general expression, its t = 0 limit, and its limit where
 * |4 alpha t| = 1 (the expression's 0/0 points). */
static double
rrc_closed_form(double t, double alpha) {
    if (std::fabs(t) < 1e-12) {
        return 1.0 - alpha + 4.0 * alpha / kPi;
    }
    const double x = 4.0 * alpha * t;
    if (std::fabs(1.0 - x * x) < 1e-12) {
        return (alpha / std::sqrt(2.0))
               * ((1.0 + 2.0 / kPi) * std::sin(kPi / (4.0 * alpha))
                  + (1.0 - 2.0 / kPi) * std::cos(kPi / (4.0 * alpha)));
    }
    return (std::sin(kPi * t * (1.0 - alpha)) + x * std::cos(kPi * t * (1.0 + alpha))) / (kPi * t * (1.0 - x * x));
}

/* ntaps reference taps, normalized as GNU Radio normalizes them: the taps sum to gain. */
static void
rrc_reference(double gain, double sps, double alpha, int ntaps, double* out) {
    const int m = ntaps / 2;
    double sum = 0.0;
    for (int i = 0; i < ntaps; i++) {
        out[i] = rrc_closed_form((double)(i - m) / sps, alpha);
        sum += out[i];
    }
    for (int i = 0; i < ntaps; i++) {
        out[i] *= gain / sum;
    }
}

static int
expect_symmetric(const char* tag, const float* taps, int n) {
    for (int i = 0; i < n / 2; i++) {
        if (std::fabs((double)taps[i] - (double)taps[n - 1 - i]) > 1e-9) {
            DSD_FPRINTF(stderr, "%s: tap %d = %.9e but tap %d = %.9e\n", tag, i, (double)taps[i], n - 1 - i,
                        (double)taps[n - 1 - i]);
            return 1;
        }
    }
    return 0;
}

static int
expect_matches_reference(const char* tag, const float* taps, const double* ref, int n, double tol) {
    for (int i = 0; i < n; i++) {
        if (std::fabs((double)taps[i] - ref[i]) > tol) {
            DSD_FPRINTF(stderr, "%s: tap %d = %.9e, closed form %.9e\n", tag, i, (double)taps[i], ref[i]);
            return 1;
        }
    }
    return 0;
}

/* GNU Radio's design at sps 4, alpha 0.35, 65 taps: symmetric, the closed form normalized to the gain. */
static int
test_rrc_matches_closed_form(void) {
    static float taps[DSD_TETRA_RRC_MAX_TAPS];
    static double ref[DSD_TETRA_RRC_MAX_TAPS];
    const int n = dsd_firdes_root_raised_cosine(1.0, 4.0, 1.0, 0.35, 65, taps, DSD_TETRA_RRC_MAX_TAPS);
    if (n != 65) {
        DSD_FPRINTF(stderr, "RRC sps 4 alpha 0.35: %d taps, want 65\n", n);
        return 1;
    }
    rrc_reference(1.0, 4.0, 0.35, n, ref);
    if (expect_symmetric("RRC sps 4 alpha 0.35", taps, n) != 0
        || expect_matches_reference("RRC sps 4 alpha 0.35", taps, ref, n, 1e-6) != 0) {
        return 1;
    }
    /* The design is the same in absolute rates: 72 kHz at 18 ksym/s is 4 samples per symbol, and gain scales it. */
    static float scaled[DSD_TETRA_RRC_MAX_TAPS];
    if (dsd_firdes_root_raised_cosine(2.5, 72000.0, 18000.0, 0.35, 65, scaled, DSD_TETRA_RRC_MAX_TAPS) != 65) {
        DSD_FPRINTF(stderr, "RRC at 72 kHz / 18 ksym/s did not design\n");
        return 1;
    }
    for (int i = 0; i < n; i++) {
        if (std::fabs((double)scaled[i] - 2.5 * ref[i]) > 1e-6) {
            DSD_FPRINTF(stderr, "RRC gain 2.5 tap %d = %.9e, want %.9e\n", i, (double)scaled[i], 2.5 * ref[i]);
            return 1;
        }
    }
    return 0;
}

/* Alpha 0.25 at sps 4 puts taps exactly on |4 alpha t| = 1 (t = +-1 symbol), GNU Radio's second branch. */
static int
test_rrc_singular_points(void) {
    static float taps[DSD_TETRA_RRC_MAX_TAPS];
    static double ref[DSD_TETRA_RRC_MAX_TAPS];
    const int n = dsd_firdes_root_raised_cosine(1.0, 4.0, 1.0, 0.25, 33, taps, DSD_TETRA_RRC_MAX_TAPS);
    if (n != 33) {
        DSD_FPRINTF(stderr, "RRC sps 4 alpha 0.25: %d taps, want 33\n", n);
        return 1;
    }
    rrc_reference(1.0, 4.0, 0.25, n, ref);
    return (expect_symmetric("RRC alpha 0.25", taps, n) != 0
            || expect_matches_reference("RRC alpha 0.25", taps, ref, n, 1e-6) != 0)
               ? 1
               : 0;
}

/* GNU Radio forces an odd length; bad arguments and a short buffer are refused without writing. */
static int
test_rrc_arguments(void) {
    static float taps[DSD_TETRA_RRC_MAX_TAPS];
    if (dsd_firdes_root_raised_cosine(1.0, 4.0, 1.0, 0.35, 64, taps, DSD_TETRA_RRC_MAX_TAPS) != 65) {
        DSD_FPRINTF(stderr, "RRC with 64 taps requested did not design 65\n");
        return 1;
    }
    for (int i = 0; i < DSD_TETRA_RRC_MAX_TAPS; i++) {
        taps[i] = 7.0f;
    }

    struct bad_case {
        double gain;
        double fs;
        double rs;
        double alpha;
        int ntaps;
        int max_taps;
        const char* name;
    };

    const bad_case cases[] = {
        {1.0, 0.0, 1.0, 0.35, 65, 129, "zero sampling rate"},
        {1.0, 4.0, 0.0, 0.35, 65, 129, "zero symbol rate"},
        {1.0, -4.0, 1.0, 0.35, 65, 129, "negative sampling rate"},
        {1.0, 4.0, 1.0, 0.0, 65, 129, "alpha 0"},
        {1.0, 4.0, 1.0, 1.5, 65, 129, "alpha above 1"},
        {1.0, 4.0, 1.0, 0.35, 0, 129, "no taps"},
        {1.0, 4.0, 1.0, 0.35, 65, 64, "taps beyond the buffer"},
        {1.0, 4.0, 1.0, 0.35, 64, 64, "an even request made odd beyond the buffer"},
    };
    for (const bad_case& c : cases) {
        if (dsd_firdes_root_raised_cosine(c.gain, c.fs, c.rs, c.alpha, c.ntaps, taps, c.max_taps) != -1) {
            DSD_FPRINTF(stderr, "RRC accepted %s\n", c.name);
            return 1;
        }
    }
    if (dsd_firdes_root_raised_cosine(1.0, 4.0, 1.0, 0.35, 65, NULL, 129) != -1) {
        DSD_FPRINTF(stderr, "RRC accepted a null buffer\n");
        return 1;
    }
    if (std::fabs((double)taps[0] - 7.0) > 1e-9) {
        DSD_FPRINTF(stderr, "a refused RRC design wrote taps\n");
        return 1;
    }
    return 0;
}

/* The TETRA matched filter: 2 x 8 x sps + 1 taps, the alpha 0.35 shape, energy 1/sps, and a cascade with itself whose
 * symbol-spaced samples are under 1% of its peak. */
static int
test_tetra_rrc_design(void) {
    if (DSD_TETRA_RRC_MAX_TAPS != 129) {
        DSD_FPRINTF(stderr, "DSD_TETRA_RRC_MAX_TAPS is %d, want 129\n", DSD_TETRA_RRC_MAX_TAPS);
        return 1;
    }
    for (int sps = 2; sps <= 8; sps++) {
        static float taps[DSD_TETRA_RRC_MAX_TAPS];
        static double ref[DSD_TETRA_RRC_MAX_TAPS];
        const int n = dsd_tetra_rrc_design(sps, taps, DSD_TETRA_RRC_MAX_TAPS);
        if (n != 2 * 8 * sps + 1) {
            DSD_FPRINTF(stderr, "TETRA RRC at sps %d: %d taps, want %d\n", sps, n, 2 * 8 * sps + 1);
            return 1;
        }
        double energy = 0.0;
        for (int i = 0; i < n; i++) {
            energy += (double)taps[i] * (double)taps[i];
        }
        if (std::fabs(energy - 1.0 / (double)sps) > 1e-6) {
            DSD_FPRINTF(stderr, "TETRA RRC at sps %d: energy %.9f, want %.9f\n", sps, energy, 1.0 / (double)sps);
            return 1;
        }
        /* The closed form at alpha 0.35, scaled to the same energy. */
        rrc_reference(1.0, (double)sps, 0.35, n, ref);
        double ref_energy = 0.0;
        for (int i = 0; i < n; i++) {
            ref_energy += ref[i] * ref[i];
        }
        const double k = std::sqrt((1.0 / (double)sps) / ref_energy);
        for (int i = 0; i < n; i++) {
            ref[i] *= k;
        }
        if (expect_symmetric("TETRA RRC", taps, n) != 0 || expect_matches_reference("TETRA RRC", taps, ref, n, 1e-6)) {
            DSD_FPRINTF(stderr, "  at sps %d\n", sps);
            return 1;
        }
        /* Normalized ISI of the cascade: (h * h)[c + k sps] / (h * h)[c], c the cascade's centre. */
        static double cascade[2 * DSD_TETRA_RRC_MAX_TAPS];
        const int clen = 2 * n - 1;
        for (int m = 0; m < clen; m++) {
            double acc = 0.0;
            for (int j = 0; j < n; j++) {
                const int i = m - j;
                if (i >= 0 && i < n) {
                    acc += (double)taps[j] * (double)taps[i];
                }
            }
            cascade[m] = acc;
        }
        const int c = n - 1;
        for (int off = sps; c + off < clen; off += sps) {
            const double isi = std::fabs(cascade[c + off]) / std::fabs(cascade[c]);
            if (isi >= 0.01) {
                DSD_FPRINTF(stderr, "TETRA RRC at sps %d: ISI %.5f at %d symbols\n", sps, isi, off / sps);
                return 1;
            }
        }
    }
    static float small[DSD_TETRA_RRC_MAX_TAPS];
    if (dsd_tetra_rrc_design(1, small, DSD_TETRA_RRC_MAX_TAPS) != -1
        || dsd_tetra_rrc_design(9, small, DSD_TETRA_RRC_MAX_TAPS) != -1 || dsd_tetra_rrc_design(8, small, 128) != -1
        || dsd_tetra_rrc_design(4, NULL, DSD_TETRA_RRC_MAX_TAPS) != -1) {
        DSD_FPRINTF(stderr, "TETRA RRC accepted an sps outside 2..8, a short buffer or a null buffer\n");
        return 1;
    }
    return 0;
}

int
main(void) {
    int rc = 0;
    rc |= test_rrc_matches_closed_form();
    rc |= test_rrc_singular_points();
    rc |= test_rrc_arguments();
    rc |= test_tetra_rrc_design();
    return rc;
}
