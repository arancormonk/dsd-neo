// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Feature: rtl-tcp-lag-resilience, Property 9 */

/**
 * @file
 * @brief Exhaustive example-based tests for adaptive pre-fill depth adjustment.
 *
 * Validates Property 9: Adaptive pre-fill depth adjustment.
 *
 * For any current Pre_Fill_Depth value within [200ms, 1500ms] and any
 * cumulative underrun count:
 * - When underrun count > 3, the next depth = min(current + 200ms, 1500ms).
 * - When underrun count == 0, the next depth = max(current - 100ms, 200ms).
 * - When 1 <= underrun count <= 3, the depth is unchanged.
 * - The depth always remains within [200ms, 1500ms].
 *
 * **Validates: Requirements 4.1, 4.2, 4.3**
 */

#include <stdio.h>

/*============================================================================
 * Constants — mirror the adaptive pre-fill policy from the design.
 *============================================================================*/

/** Minimum allowed pre-fill depth in milliseconds. */
#define PREFILL_MIN_MS             200

/** Maximum allowed pre-fill depth in milliseconds. */
#define PREFILL_MAX_MS             1500

/** Amount to increase depth when underruns exceed the threshold. */
#define PREFILL_INCREASE_MS        200

/** Amount to decrease depth when a session has zero underruns. */
#define PREFILL_DECREASE_MS        100

/** Underrun count threshold above which depth is increased. */
#define PREFILL_UNDERRUN_THRESHOLD 3

/*============================================================================
 * Test helper — mirrors the adaptive pre-fill adjustment logic.
 *
 * This is the logic that the JNI/Flutter layer would execute based on
 * underrun counts reported at the end of a session.
 *============================================================================*/

/**
 * @brief Simulate the adaptive pre-fill adjustment.
 *
 * @param current_depth_ms  Current pre-fill depth in milliseconds.
 * @param underrun_count    Cumulative underrun count for the session.
 * @return Adjusted pre-fill depth in milliseconds, clamped to [200, 1500].
 */
static int
prefill_adjust(int current_depth_ms, int underrun_count) {
    if (underrun_count > PREFILL_UNDERRUN_THRESHOLD) {
        int new_depth = current_depth_ms + PREFILL_INCREASE_MS;
        return (new_depth > PREFILL_MAX_MS) ? PREFILL_MAX_MS : new_depth;
    }
    if (underrun_count == 0) {
        int new_depth = current_depth_ms - PREFILL_DECREASE_MS;
        return (new_depth < PREFILL_MIN_MS) ? PREFILL_MIN_MS : new_depth;
    }
    return current_depth_ms; /* No change for 1-3 underruns */
}

/*============================================================================
 * Helpers
 *============================================================================*/

static int g_fail_count = 0;

static void
expect_int(const char* label, int got, int want) {
    if (got != want) {
        fprintf(stderr, "FAIL: %s: got=%d want=%d\n", label, got, want);
        g_fail_count++;
    }
}

/*============================================================================
 * Tests
 *============================================================================*/

/**
 * @brief Increase: 300ms + 4 underruns → 500ms.
 */
static void
test_increase_basic(void) {
    expect_int("increase 300+4", prefill_adjust(300, 4), 500);
}

/**
 * @brief Increase capped: 1400ms + 4 underruns → 1500ms (not 1600).
 */
static void
test_increase_capped(void) {
    expect_int("increase capped 1400+4", prefill_adjust(1400, 4), 1500);
}

/**
 * @brief Increase at max: 1500ms + 10 underruns → 1500ms (already at max).
 */
static void
test_increase_at_max(void) {
    expect_int("increase at max 1500+10", prefill_adjust(1500, 10), 1500);
}

/**
 * @brief Decrease: 500ms + 0 underruns → 400ms.
 */
static void
test_decrease_basic(void) {
    expect_int("decrease 500+0", prefill_adjust(500, 0), 400);
}

/**
 * @brief Decrease floored: 250ms + 0 underruns → 200ms (not 150).
 */
static void
test_decrease_floored(void) {
    expect_int("decrease floored 250+0", prefill_adjust(250, 0), 200);
}

/**
 * @brief Decrease at min: 200ms + 0 underruns → 200ms (already at min).
 */
static void
test_decrease_at_min(void) {
    expect_int("decrease at min 200+0", prefill_adjust(200, 0), 200);
}

/**
 * @brief No change: 500ms + 2 underruns → 500ms.
 */
static void
test_no_change_mid(void) {
    expect_int("no change 500+2", prefill_adjust(500, 2), 500);
}

/**
 * @brief No change: 500ms + 1 underrun → 500ms.
 */
static void
test_no_change_one(void) {
    expect_int("no change 500+1", prefill_adjust(500, 1), 500);
}

/**
 * @brief No change: 500ms + 3 underruns → 500ms (threshold is >3, not >=3).
 */
static void
test_no_change_at_threshold(void) {
    expect_int("no change 500+3", prefill_adjust(500, 3), 500);
}

/**
 * @brief Bounds sweep: for every valid depth in [200, 1500] stepped by 100ms,
 *        and for underrun counts {0, 1, 3, 4, 10}, verify the result is
 *        always within [200, 1500].
 */
static void
test_bounds_sweep(void) {
    int underrun_counts[] = {0, 1, 2, 3, 4, 5, 10, 50, 100};
    int num_counts = (int)(sizeof(underrun_counts) / sizeof(underrun_counts[0]));

    for (int depth = PREFILL_MIN_MS; depth <= PREFILL_MAX_MS; depth += 50) {
        for (int c = 0; c < num_counts; c++) {
            int result = prefill_adjust(depth, underrun_counts[c]);
            if (result < PREFILL_MIN_MS || result > PREFILL_MAX_MS) {
                fprintf(stderr,
                        "FAIL: bounds depth=%d underruns=%d result=%d "
                        "(expected [%d, %d])\n",
                        depth, underrun_counts[c], result, PREFILL_MIN_MS, PREFILL_MAX_MS);
                g_fail_count++;
            }
        }
    }
}

/**
 * @brief Multi-step convergence: starting from 300ms, repeated sessions with
 *        4 underruns should converge to 1500ms.
 */
static void
test_convergence_up(void) {
    int depth = 300;
    for (int i = 0; i < 20; i++) {
        depth = prefill_adjust(depth, 4);
    }
    expect_int("converge up", depth, PREFILL_MAX_MS);
}

/**
 * @brief Multi-step convergence: starting from 1500ms, repeated sessions with
 *        0 underruns should converge to 200ms.
 */
static void
test_convergence_down(void) {
    int depth = 1500;
    for (int i = 0; i < 20; i++) {
        depth = prefill_adjust(depth, 0);
    }
    expect_int("converge down", depth, PREFILL_MIN_MS);
}

int
main(void) {
    test_increase_basic();
    test_increase_capped();
    test_increase_at_max();
    test_decrease_basic();
    test_decrease_floored();
    test_decrease_at_min();
    test_no_change_mid();
    test_no_change_one();
    test_no_change_at_threshold();
    test_bounds_sweep();
    test_convergence_up();
    test_convergence_down();

    if (g_fail_count > 0) {
        fprintf(stderr, "\n%d test(s) FAILED\n", g_fail_count);
        return 1;
    }

    return 0;
}
