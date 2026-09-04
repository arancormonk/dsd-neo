// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "frame_sync_level.h"

#include <dsd-neo/core/safe_api.h>

#include <math.h>
#include <stdio.h>

static int
expect_close(const char* label, float expected, float actual) {
    const float delta = fabsf(expected - actual);
    if (delta > 0.0001f) {
        DSD_FPRINTF(stderr, "%s: expected %.6f, got %.6f\n", label, (double)expected, (double)actual);
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

static int
test_missing_output_is_noop(void) {
    const float sorted_levels[3] = {-1.0f, 0.0f, 1.0f};
    float lmin = 12.0f;
    float lmax = 34.0f;

    dsd_frame_sync_estimate_sorted_window_levels(sorted_levels, 3, NULL, &lmax);
    dsd_frame_sync_estimate_sorted_window_levels(sorted_levels, 3, &lmin, NULL);

    int rc = 0;
    rc |= expect_close("missing out min unchanged", 12.0f, lmin);
    rc |= expect_close("missing out max unchanged", 34.0f, lmax);
    return rc;
}

static int
test_empty_input_clears_outputs(void) {
    float lmin = 12.0f;
    float lmax = 34.0f;

    dsd_frame_sync_estimate_sorted_window_levels(NULL, 3, &lmin, &lmax);
    int rc = 0;
    rc |= expect_close("null input lmin", 0.0f, lmin);
    rc |= expect_close("null input lmax", 0.0f, lmax);

    lmin = 12.0f;
    lmax = 34.0f;
    const float sorted_levels[1] = {9.0f};
    dsd_frame_sync_estimate_sorted_window_levels(sorted_levels, 0, &lmin, &lmax);
    rc |= expect_close("zero count lmin", 0.0f, lmin);
    rc |= expect_close("zero count lmax", 0.0f, lmax);
    return rc;
}

static int
test_tiny_window_uses_average(void) {
    const float one_level[1] = {7.0f};
    const float two_levels[2] = {-3.0f, 9.0f};
    float lmin = 0.0f;
    float lmax = 0.0f;
    int rc = 0;

    dsd_frame_sync_estimate_sorted_window_levels(one_level, 1, &lmin, &lmax);
    rc |= expect_close("one lmin", 7.0f, lmin);
    rc |= expect_close("one lmax", 7.0f, lmax);

    dsd_frame_sync_estimate_sorted_window_levels(two_levels, 2, &lmin, &lmax);
    rc |= expect_close("two lmin", 3.0f, lmin);
    rc |= expect_close("two lmax", 3.0f, lmax);
    return rc;
}

int
main(void) {
    int rc = 0;
    rc |= test_missing_output_is_noop();
    rc |= test_empty_input_clears_outputs();
    rc |= test_tiny_window_uses_average();
    rc |= test_short_window_uses_edges();
    rc |= test_long_window_drops_outer_outliers();
    return rc;
}
