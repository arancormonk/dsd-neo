// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/dsp/firdes.h>
#include <math.h>
#include <stdio.h>

static int
expect_close(const char* tag, float got, float want, float tol) {
    float diff = fabsf(got - want);
    if (diff > tol) {
        fprintf(stderr, "%s: got %.6f want %.6f (diff=%.6f)\n", tag, got, want, diff);
        return 1;
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
        fprintf(stderr, "Kaiser window unexpectedly matches rectangular window\n");
        return 1;
    }

    for (int i = 0; i < kNtaps / 2; i++) {
        rc |= expect_close("kaiser symmetry", kaiser[i], kaiser[kNtaps - 1 - i], 1e-5f);
    }
    rc |= expect_close("kaiser center gain", kaiser[kNtaps / 2], 1.0f, 1e-5f);

    if (!(kaiser[0] < 0.2f && kaiser[kNtaps - 1] < 0.2f)) {
        fprintf(stderr, "Kaiser edges are not attenuated enough: first=%f last=%f\n", kaiser[0], kaiser[kNtaps - 1]);
        rc = 1;
    }

    float taps_kaiser[512];
    float taps_rect[512];
    int ntaps_kaiser = dsd_firdes_low_pass(1.0, 48000.0, 7000.0, 1200.0, DSD_WIN_KAISER, taps_kaiser,
                                           (int)(sizeof(taps_kaiser) / sizeof(float)));
    int ntaps_rect = dsd_firdes_low_pass(1.0, 48000.0, 7000.0, 1200.0, DSD_WIN_RECTANGULAR, taps_rect,
                                         (int)(sizeof(taps_rect) / sizeof(float)));

    if (ntaps_kaiser <= 0 || ntaps_rect <= 0) {
        fprintf(stderr, "firdes low_pass returned invalid tap count (kaiser=%d rect=%d)\n", ntaps_kaiser, ntaps_rect);
        return 1;
    }

    if (ntaps_kaiser <= ntaps_rect) {
        fprintf(stderr, "Kaiser should require more taps than rectangular (kaiser=%d rect=%d)\n", ntaps_kaiser,
                ntaps_rect);
        rc = 1;
    }

    return rc;
}
