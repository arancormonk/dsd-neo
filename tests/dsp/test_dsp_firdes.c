// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/dsp/firdes.h>
#include <math.h>
#include <stdio.h>
#include "dsd-neo/core/safe_api.h"

static int
expect_close(const char* tag, float got, float want, float tol) {
    float diff = fabsf(got - want);
    if (diff > tol) {
        DSD_FPRINTF(stderr, "%s: got %.6f want %.6f (diff=%.6f)\n", tag, got, want, diff);
        return 1;
    }
    return 0;
}

static int
expect_double_close(const char* tag, double got, double want, double tol) {
    double diff = fabs(got - want);
    if (diff > tol) {
        DSD_FPRINTF(stderr, "%s: got %.6f want %.6f (diff=%.6f)\n", tag, got, want, diff);
        return 1;
    }
    return 0;
}

static int
expect_symmetric_finite_window(const char* tag, const float* taps, int ntaps) {
    for (int i = 0; i < ntaps; i++) {
        if (!isfinite(taps[i])) {
            DSD_FPRINTF(stderr, "%s: tap %d is not finite: %f\n", tag, i, taps[i]);
            return 1;
        }
    }
    for (int i = 0; i < ntaps / 2; i++) {
        if (expect_close(tag, taps[i], taps[ntaps - 1 - i], 1e-5f) != 0) {
            return 1;
        }
    }
    return 0;
}

int
main(void) {
    int rc = 0;

    enum { kNtaps = 33 };

    float kaiser[kNtaps];
    float rect[kNtaps];

    dsd_window_build(DSD_WIN_KAISER, kNtaps, kaiser);
    dsd_window_build(DSD_WIN_RECTANGULAR, kNtaps, rect);

    int differs_from_rect = 0;
    for (int i = 0; i < kNtaps; i++) {
        if (fabsf(kaiser[i] - rect[i]) > 1e-3f) {
            differs_from_rect = 1;
            break;
        }
    }
    if (!differs_from_rect) {
        DSD_FPRINTF(stderr, "Kaiser window unexpectedly matches rectangular window\n");
        return 1;
    }

    for (int i = 0; i < kNtaps / 2; i++) {
        rc |= expect_close("kaiser symmetry", kaiser[i], kaiser[kNtaps - 1 - i], 1e-5f);
    }
    rc |= expect_close("kaiser center gain", kaiser[kNtaps / 2], 1.0f, 1e-5f);

    if (!(kaiser[0] < 0.2f && kaiser[kNtaps - 1] < 0.2f)) {
        DSD_FPRINTF(stderr, "Kaiser edges are not attenuated enough: first=%f last=%f\n", kaiser[0],
                    kaiser[kNtaps - 1]);
        rc = 1;
    }

    rc |= expect_double_close("hamming attenuation", dsd_window_max_attenuation(DSD_WIN_HAMMING), 53.0, 1e-9);
    rc |= expect_double_close("hann attenuation", dsd_window_max_attenuation(DSD_WIN_HANN), 44.0, 1e-9);
    rc |= expect_double_close("blackman attenuation", dsd_window_max_attenuation(DSD_WIN_BLACKMAN), 74.0, 1e-9);
    rc |= expect_double_close("rectangular attenuation", dsd_window_max_attenuation(DSD_WIN_RECTANGULAR), 21.0, 1e-9);
    rc |= expect_double_close("blackman-harris attenuation", dsd_window_max_attenuation(DSD_WIN_BLACKMAN_HARRIS), 92.0,
                              1e-9);
    rc |= expect_double_close("bartlett attenuation", dsd_window_max_attenuation(DSD_WIN_BARTLETT), 27.0, 1e-9);
    rc |= expect_double_close("flattop attenuation", dsd_window_max_attenuation(DSD_WIN_FLATTOP), 93.0, 1e-9);
    rc |= expect_double_close("default attenuation", dsd_window_max_attenuation((dsd_window_type_t)99), 53.0, 1e-9);

    {
        enum { kWindowNtaps = 17 };

        static const struct {
            dsd_window_type_t type;
            const char* tag;
        } window_cases[] = {
            {DSD_WIN_HAMMING, "hamming"},         {DSD_WIN_HANN, "hann"},
            {DSD_WIN_BLACKMAN, "blackman"},       {DSD_WIN_BLACKMAN_HARRIS, "blackman-harris"},
            {DSD_WIN_BARTLETT, "bartlett"},       {DSD_WIN_FLATTOP, "flattop"},
            {DSD_WIN_RECTANGULAR, "rectangular"}, {(dsd_window_type_t)99, "default-rectangular"},
        };

        for (size_t c = 0; c < sizeof window_cases / sizeof window_cases[0]; c++) {
            float taps[kWindowNtaps];
            dsd_window_build(window_cases[c].type, kWindowNtaps, taps);
            rc |= expect_symmetric_finite_window(window_cases[c].tag, taps, kWindowNtaps);
            if (window_cases[c].type == DSD_WIN_RECTANGULAR || window_cases[c].type == (dsd_window_type_t)99) {
                for (int i = 0; i < kWindowNtaps; i++) {
                    rc |= expect_close(window_cases[c].tag, taps[i], 1.0f, 1e-6f);
                }
            } else if (!(taps[kWindowNtaps / 2] > taps[0])) {
                DSD_FPRINTF(stderr, "%s window center should exceed edge: edge=%f center=%f\n", window_cases[c].tag,
                            taps[0], taps[kWindowNtaps / 2]);
                rc = 1;
            }
        }
    }

    {
        float one = 0.0f;
        dsd_window_build(DSD_WIN_KAISER, 0, &one);
        dsd_window_build(DSD_WIN_KAISER, 1, &one);
        rc |= expect_close("kaiser single tap", one, 1.0f, 1e-6f);
    }

    float taps_kaiser[512];
    float taps_rect[512];
    int ntaps_kaiser = dsd_firdes_low_pass(1.0, 48000.0, 7000.0, 1200.0, DSD_WIN_KAISER, taps_kaiser,
                                           (int)(sizeof(taps_kaiser) / sizeof(float)));
    int ntaps_rect = dsd_firdes_low_pass(1.0, 48000.0, 7000.0, 1200.0, DSD_WIN_RECTANGULAR, taps_rect,
                                         (int)(sizeof(taps_rect) / sizeof(float)));

    if (ntaps_kaiser <= 0 || ntaps_rect <= 0) {
        DSD_FPRINTF(stderr, "firdes low_pass returned invalid tap count (kaiser=%d rect=%d)\n", ntaps_kaiser,
                    ntaps_rect);
        return 1;
    }

    if (ntaps_kaiser <= ntaps_rect) {
        DSD_FPRINTF(stderr, "Kaiser should require more taps than rectangular (kaiser=%d rect=%d)\n", ntaps_kaiser,
                    ntaps_rect);
        rc = 1;
    }

    if ((ntaps_kaiser & 1) == 0 || dsd_firdes_compute_ntaps(48000.0, 1200.0, (dsd_window_type_t)99) <= 0) {
        DSD_FPRINTF(stderr, "computed tap counts should be positive and odd\n");
        rc = 1;
    }

    if (dsd_firdes_low_pass(1.0, 0.0, 7000.0, 1200.0, DSD_WIN_HAMMING, taps_rect, 512) != -1
        || dsd_firdes_low_pass(1.0, 48000.0, 0.0, 1200.0, DSD_WIN_HAMMING, taps_rect, 512) != -1
        || dsd_firdes_low_pass(1.0, 48000.0, 25000.0, 1200.0, DSD_WIN_HAMMING, taps_rect, 512) != -1
        || dsd_firdes_low_pass(1.0, 48000.0, 7000.0, 0.0, DSD_WIN_HAMMING, taps_rect, 512) != -1
        || dsd_firdes_low_pass(1.0, 48000.0, 7000.0, 1200.0, DSD_WIN_HAMMING, taps_rect, 4) != -1
        || dsd_firdes_low_pass(1.0, 48000.0, 7000.0, 10.0, DSD_WIN_HAMMING, taps_rect, 4096) != -1) {
        DSD_FPRINTF(stderr, "firdes low_pass validation guards failed\n");
        rc = 1;
    }

    return rc;
}
