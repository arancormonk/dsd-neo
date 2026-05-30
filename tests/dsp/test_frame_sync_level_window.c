// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "frame_sync_level.h"

#include <math.h>
#include <stdio.h>

static int
expect_close(const char* label, float expected, float actual) {
    const float delta = fabsf(expected - actual);
    if (delta > 0.0001f) {
        fprintf(stderr, "%s: expected %.6f, got %.6f\n", label, (double)expected, (double)actual);
        return 1;
    }
    return 0;
}

static int
test_short_window_uses_edges(void) {
    const float sorted_levels[12] = {-100.0f, -4.0f, -3.0f, -2.0f, -1.0f, 0.0f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 100.0f};
    float lmin = 0.0f;
    float lmax = 0.0f;

    dsd_frame_sync_estimate_sorted_window_levels(sorted_levels, 12, &lmin, &lmax);

    int rc = 0;
    rc |= expect_close("short lmin", (-100.0f - 4.0f - 3.0f) / 3.0f, lmin);
    rc |= expect_close("short lmax", (4.0f + 5.0f + 100.0f) / 3.0f, lmax);
    return rc;
}

static int
test_long_window_drops_outer_outliers(void) {
    const float sorted_levels[24] = {-100.0f, -50.0f, -4.0f, -3.0f, -2.0f, -1.0f, 0.0f,  0.0f,
                                     0.0f,    0.0f,   0.0f,  0.0f,  0.0f,  0.0f,  0.0f,  0.0f,
                                     1.0f,    2.0f,   3.0f,  4.0f,  5.0f,  6.0f,  50.0f, 100.0f};
    float lmin = 0.0f;
    float lmax = 0.0f;

    dsd_frame_sync_estimate_sorted_window_levels(sorted_levels, 24, &lmin, &lmax);

    int rc = 0;
    rc |= expect_close("long lmin", (-4.0f - 3.0f - 2.0f) / 3.0f, lmin);
    rc |= expect_close("long lmax", (4.0f + 5.0f + 6.0f) / 3.0f, lmax);
    return rc;
}

int
main(void) {
    int rc = 0;
    rc |= test_short_window_uses_edges();
    rc |= test_long_window_drops_outer_outliers();
    return rc;
}
