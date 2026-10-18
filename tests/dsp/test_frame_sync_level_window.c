// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "frame_sync_level.h"

#include <dsd-neo/core/safe_api.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

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

    dsd_frame_sync_estimate_window_levels(sorted_levels, 12, &lmin, &lmax);

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

    dsd_frame_sync_estimate_window_levels(sorted_levels, 24, &lmin, &lmax);

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

    dsd_frame_sync_estimate_window_levels(sorted_levels, 3, NULL, &lmax);
    dsd_frame_sync_estimate_window_levels(sorted_levels, 3, &lmin, NULL);

    int rc = 0;
    rc |= expect_close("missing out min unchanged", 12.0f, lmin);
    rc |= expect_close("missing out max unchanged", 34.0f, lmax);
    return rc;
}

static int
test_empty_input_clears_outputs(void) {
    float lmin = 12.0f;
    float lmax = 34.0f;

    dsd_frame_sync_estimate_window_levels(NULL, 3, &lmin, &lmax);
    int rc = 0;
    rc |= expect_close("null input lmin", 0.0f, lmin);
    rc |= expect_close("null input lmax", 0.0f, lmax);

    lmin = 12.0f;
    lmax = 34.0f;
    const float sorted_levels[1] = {9.0f};
    dsd_frame_sync_estimate_window_levels(sorted_levels, 0, &lmin, &lmax);
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

    dsd_frame_sync_estimate_window_levels(one_level, 1, &lmin, &lmax);
    rc |= expect_close("one lmin", 7.0f, lmin);
    rc |= expect_close("one lmax", 7.0f, lmax);

    dsd_frame_sync_estimate_window_levels(two_levels, 2, &lmin, &lmax);
    rc |= expect_close("two lmin", 3.0f, lmin);
    rc |= expect_close("two lmax", 3.0f, lmax);
    return rc;
}

static int
test_unsorted_input_matches_sorted(void) {
    const float sorted_levels[12] = {-100.0f, -4.0f, -3.0f, -2.0f, -1.0f, 0.0f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 100.0f};
    const float shuffled_levels[12] = {3.0f, -100.0f, 0.0f, 5.0f, -3.0f, 1.0f, 4.0f, -4.0f, -1.0f, 100.0f, -2.0f, 2.0f};
    float lmin_sorted = 0.0f;
    float lmax_sorted = 0.0f;
    float lmin_shuffled = 0.0f;
    float lmax_shuffled = 0.0f;

    dsd_frame_sync_estimate_window_levels(sorted_levels, 12, &lmin_sorted, &lmax_sorted);
    dsd_frame_sync_estimate_window_levels(shuffled_levels, 12, &lmin_shuffled, &lmax_shuffled);

    int rc = 0;
    rc |= expect_close("unsorted lmin", lmin_sorted, lmin_shuffled);
    rc |= expect_close("unsorted lmax", lmax_sorted, lmax_shuffled);
    return rc;
}

static int
compare_float(const void* a, const void* b) {
    const float x = *(const float*)a;
    const float y = *(const float*)b;
    return (x > y) - (x < y);
}

// Same ranks, in the same summation order, from a full sort of the window.
static void
sorted_reference(const float* levels, int count, float* out_min, float* out_max) {
    float sorted[64];
    for (int i = 0; i < count; i++) {
        sorted[i] = levels[i];
    }
    qsort(sorted, (size_t)count, sizeof(sorted[0]), compare_float);
    if (count < 3) {
        const float sum = (count == 1) ? sorted[0] : (sorted[0] + sorted[count - 1]);
        *out_min = sum / (float)count;
        *out_max = *out_min;
        return;
    }
    const int lo = (count >= 13) ? 2 : 0;
    const int hi = (count >= 13) ? count - 5 : count - 3;
    *out_min = (sorted[lo] + sorted[lo + 1] + sorted[lo + 2]) / 3.0f;
    *out_max = (sorted[hi] + sorted[hi + 1] + sorted[hi + 2]) / 3.0f;
}

static int
same_level(float expected, float actual) {
#if defined(__FAST_MATH__)
    // Fast-math builds may contract the reference differently from the library.
    return fabsf(expected - actual) <= 1e-6f * fmaxf(1.0f, fabsf(expected));
#else
    uint32_t a = 0;
    uint32_t b = 0;
    DSD_MEMCPY(&a, &expected, sizeof a);
    DSD_MEMCPY(&b, &actual, sizeof b);
    return a == b;
#endif
}

static int
test_windows_match_sorted_reference(void) {
    uint32_t rng = 0x61c88647U;
    float storage[64];
    for (int offset = 0; offset < 4; offset++) {
        for (int count = 1; count <= 48; count++) {
            float* levels = storage + offset;
            for (int shape = 0; shape < 4; shape++) {
                for (int i = 0; i < count; i++) {
                    rng = rng * 1664525U + 1013904223U;
                    const float rail = (float)((int)(rng >> 30) * 2 - 3);
                    switch (shape) {
                        case 0: levels[i] = (float)((int)(rng >> 16) - 32768) / 4096.0f; break;
                        case 1: levels[i] = rail; break;
                        case 2: levels[i] = rail + (float)((rng >> 8) & 0xffU) / 1024.0f; break;
                        default: levels[i] = (float)(count - i) * ((i & 1) ? 1.0f : -1.0f); break;
                    }
                }
                float want_min = 0.0f;
                float want_max = 0.0f;
                float lmin = 0.0f;
                float lmax = 0.0f;
                sorted_reference(levels, count, &want_min, &want_max);
                dsd_frame_sync_estimate_window_levels(levels, count, &lmin, &lmax);
                if (!same_level(want_min, lmin) || !same_level(want_max, lmax)) {
                    DSD_FPRINTF(stderr, "window mismatch: count=%d offset=%d shape=%d\n", count, offset, shape);
                    return 1;
                }
            }
        }
    }
    return 0;
}

int
main(void) {
    int rc = 0;
    rc |= test_missing_output_is_noop();
    rc |= test_empty_input_clears_outputs();
    rc |= test_tiny_window_uses_average();
    rc |= test_short_window_uses_edges();
    rc |= test_long_window_drops_outer_outliers();
    rc |= test_unsorted_input_matches_sorted();
    rc |= test_windows_match_sorted_reference();
    return rc;
}
