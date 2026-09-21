// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Live-scanner slicer threshold refresh: the level extremes are compared
 * as truncated ints, so a move refreshes when it leaves its integer bucket and
 * not otherwise (a center written by someone else in between survives moves
 * inside the bucket). NaN, infinite and out-of-int-range extremes are skipped.
 */

#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/engine/slicer_thresholds.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static int g_fail = 0;

static void
check_float(const char* name, float expected, float actual) {
    if (!isfinite(actual) || fabsf(expected - actual) > 1e-6f * fmaxf(1.0f, fabsf(expected))) {
        printf("FAIL: %s: expected %.9g, got %.9g\n", name, (double)expected, (double)actual);
        g_fail = 1;
    }
}

static void
check_int(const char* name, int expected, int actual) {
    if (expected != actual) {
        printf("FAIL: %s: expected %d, got %d\n", name, expected, actual);
        g_fail = 1;
    }
}

static dsd_state*
make_state(float max, float min) {
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    if (state == NULL) {
        printf("FAIL: calloc\n");
        exit(1);
    }
    state->max = max;
    state->min = min;
    state->center = -12345.0f; /* sentinel: visible if the refresh does not run */
    state->umid = -12345.0f;
    state->lmid = -12345.0f;
    return state;
}

/* First call derives all three thresholds with the legacy 5/8 mid split. */
static void
test_first_call_refreshes(void) {
    dsd_state* state = make_state(12000.0f, -8000.0f);
    dsd_engine_slicer_threshold_cache cache;
    dsd_engine_slicer_threshold_cache_init(&cache);

    check_int("first call refreshed", 1, dsd_engine_slicer_thresholds_refresh(state, &cache));
    check_float("first center", 2000.0f, state->center);
    check_float("first umid", 2000.0f + (10000.0f * 5.0f / 8.0f), state->umid);
    check_float("first lmid", 2000.0f - (10000.0f * 5.0f / 8.0f), state->lmid);
    free(state);
}

/* Unchanged extremes leave the thresholds untouched. */
static void
test_unchanged_extremes_skip(void) {
    dsd_state* state = make_state(12000.0f, -8000.0f);
    dsd_engine_slicer_threshold_cache cache;
    dsd_engine_slicer_threshold_cache_init(&cache);
    (void)dsd_engine_slicer_thresholds_refresh(state, &cache);

    state->center = 777.0f;
    check_int("unchanged skipped", 0, dsd_engine_slicer_thresholds_refresh(state, &cache));
    check_float("center kept", 777.0f, state->center);
    free(state);
}

/* A move of at least one level unit in either extreme refreshes. */
static void
test_whole_unit_change_refreshes(void) {
    dsd_state* state = make_state(10000.5f, -10000.5f);
    dsd_engine_slicer_threshold_cache cache;
    dsd_engine_slicer_threshold_cache_init(&cache);
    (void)dsd_engine_slicer_thresholds_refresh(state, &cache);

    state->max = 10001.5f;
    check_int("max +1 refreshed", 1, dsd_engine_slicer_thresholds_refresh(state, &cache));
    check_float("max +1 center", (10001.5f + -10000.5f) / 2, state->center);

    state->min = -10001.5f;
    check_int("min -1 refreshed", 1, dsd_engine_slicer_thresholds_refresh(state, &cache));
    check_float("min -1 center", 0.0f, state->center);
    check_float("min -1 umid", 10001.5f * 5.0f / 8.0f, state->umid);
    check_float("min -1 lmid", -10001.5f * 5.0f / 8.0f, state->lmid);
    free(state);
}

/* Moves inside the integer bucket, including a one-ulp rounding difference,
 * do not refresh. */
static void
test_move_inside_bucket_does_not_refresh(void) {
    dsd_state* state = make_state(10000.5f, -10000.5f);
    dsd_engine_slicer_threshold_cache cache;
    dsd_engine_slicer_threshold_cache_init(&cache);
    (void)dsd_engine_slicer_thresholds_refresh(state, &cache);
    const float center0 = state->center;

    state->max = 10000.25f;
    check_int("in-bucket max skipped", 0, dsd_engine_slicer_thresholds_refresh(state, &cache));
    state->min = -10000.75f;
    check_int("in-bucket min skipped", 0, dsd_engine_slicer_thresholds_refresh(state, &cache));
    state->max = nextafterf(10000.5f, 0.0f);
    check_int("one-ulp in-bucket skipped", 0, dsd_engine_slicer_thresholds_refresh(state, &cache));
    check_float("center untouched", center0, state->center);
    free(state);
}

/* The policy is truncation, not a one-unit band: a tiny move across an integer
 * boundary refreshes. This pins what is actually retained. */
static void
test_bucket_boundaries_decide(void) {
    dsd_state* state = make_state(10000.9f, -10000.5f);
    dsd_engine_slicer_threshold_cache cache;
    dsd_engine_slicer_threshold_cache_init(&cache);
    (void)dsd_engine_slicer_thresholds_refresh(state, &cache);

    state->max = 10001.0f; /* 0.1 of a unit, but 10000 -> 10001 */
    check_int("small cross-boundary move refreshed", 1, dsd_engine_slicer_thresholds_refresh(state, &cache));
    state->max = nextafterf(10001.0f, 0.0f); /* one ulp back across it */
    check_int("one-ulp cross-boundary move refreshed", 1, dsd_engine_slicer_thresholds_refresh(state, &cache));
    free(state);
}

/* Truncation is toward zero, so the zero bucket is two units wide. */
static void
test_zero_bucket_spans_two_units(void) {
    dsd_state* state = make_state(-0.9f, -10000.5f);
    dsd_engine_slicer_threshold_cache cache;
    dsd_engine_slicer_threshold_cache_init(&cache);
    (void)dsd_engine_slicer_thresholds_refresh(state, &cache);

    state->max = 0.9f; /* 1.8 units, but (int)-0.9 == (int)0.9 == 0 */
    check_int("zero-bucket move skipped", 0, dsd_engine_slicer_thresholds_refresh(state, &cache));
    free(state);
}

/* A center written by another writer between two refreshes survives a blend
 * of the extremes that stays inside the bucket and is replaced by one that
 * leaves it. */
static void
test_foreign_center_survives_sub_unit_blend(void) {
    dsd_state* state = make_state(10000.5f, -10000.5f);
    dsd_engine_slicer_threshold_cache cache;
    dsd_engine_slicer_threshold_cache_init(&cache);
    (void)dsd_engine_slicer_thresholds_refresh(state, &cache);

    state->center = 137.0f;
    state->max = (state->max + 10000.9f) / 2; /* matcher-style blend, stays within the unit */
    state->min = (state->min + -10000.1f) / 2;
    check_int("blend within unit skipped", 0, dsd_engine_slicer_thresholds_refresh(state, &cache));
    check_float("foreign center survives in-bucket blend", 137.0f, state->center);

    state->max = 10002.0f; /* a whole-unit move still wins */
    check_int("unit move refreshed", 1, dsd_engine_slicer_thresholds_refresh(state, &cache));
    check_float("center recomputed", (10002.0f + state->min) / 2, state->center);
    free(state);
}

/* NaN, infinite and out-of-int-range extremes are skipped and the last good
 * thresholds kept. */
static void
test_unconvertible_extremes_skip(void) {
    dsd_state* state = make_state(10000.5f, -10000.5f);
    dsd_engine_slicer_threshold_cache cache;
    dsd_engine_slicer_threshold_cache_init(&cache);
    (void)dsd_engine_slicer_thresholds_refresh(state, &cache);
    const float center0 = state->center;
    const float umid0 = state->umid;

    state->max = NAN;
    check_int("nan max skipped", 0, dsd_engine_slicer_thresholds_refresh(state, &cache));
    state->max = 10000.5f;
    state->min = -INFINITY;
    check_int("inf min skipped", 0, dsd_engine_slicer_thresholds_refresh(state, &cache));
    state->min = -10000.5f;
    state->max = 2147483648.0f; /* 2^31: finite, but not an int */
    check_int("above int range skipped", 0, dsd_engine_slicer_thresholds_refresh(state, &cache));
    state->max = 10000.5f;
    state->min = nextafterf(-2147483648.0f, -INFINITY); /* just below -2^31 */
    check_int("below int range skipped", 0, dsd_engine_slicer_thresholds_refresh(state, &cache));
    check_float("center kept through unconvertible", center0, state->center);
    check_float("umid kept through unconvertible", umid0, state->umid);

    state->min = -10000.5f; /* back to the cached bucket: the cache survived the rejects */
    check_int("cache kept through unconvertible", 0, dsd_engine_slicer_thresholds_refresh(state, &cache));

    state->min = -10001.5f; /* sane again and in a new bucket: refreshes */
    check_int("convertible again refreshed", 1, dsd_engine_slicer_thresholds_refresh(state, &cache));
    free(state);
}

/* Zero-initialised cache behaves like an initialised one. */
static void
test_zeroed_cache_is_unprimed(void) {
    dsd_state* state = make_state(0.0f, 0.0f);
    dsd_engine_slicer_threshold_cache cache = {0, 0, 0};

    check_int("zeroed cache first call refreshed", 1, dsd_engine_slicer_thresholds_refresh(state, &cache));
    check_float("zeroed center", 0.0f, state->center);
    check_int("zeroed cache second call skipped", 0, dsd_engine_slicer_thresholds_refresh(state, &cache));
    free(state);
}

static void
test_null_arguments(void) {
    dsd_state* state = make_state(1.0f, -1.0f);
    dsd_engine_slicer_threshold_cache cache;
    dsd_engine_slicer_threshold_cache_init(&cache);
    check_int("null state", -1, dsd_engine_slicer_thresholds_refresh(NULL, &cache));
    check_int("null cache", -1, dsd_engine_slicer_thresholds_refresh(state, NULL));
    check_float("null cache left thresholds alone", -12345.0f, state->center);
    dsd_engine_slicer_threshold_cache_init(NULL); /* must not crash */
    free(state);
}

int
main(void) {
    test_first_call_refreshes();
    test_unchanged_extremes_skip();
    test_whole_unit_change_refreshes();
    test_move_inside_bucket_does_not_refresh();
    test_bucket_boundaries_decide();
    test_zero_bucket_spans_two_units();
    test_foreign_center_survives_sub_unit_blend();
    test_unconvertible_extremes_skip();
    test_zeroed_cache_is_unprimed();
    test_null_arguments();
    if (g_fail) {
        printf("FAILED\n");
        return 1;
    }
    printf("OK\n");
    return 0;
}
