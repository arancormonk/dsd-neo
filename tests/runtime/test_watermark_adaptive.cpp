// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Exhaustive example-based tests for watermark adaptive target adjustment.
 *
 * Validates Property 7: Watermark adaptive target adjustment.
 *
 * Verifies that the target watermark increases when low-watermark events
 * are frequent, decreases during quiet periods, and always stays within
 * the [300ms, 1500ms] bounds.
 */

/* Feature: rtl-tcp-lag-resilience, Property 7 */
/* Validates: Requirements 3.5, 3.6 */

#include <dsd-neo/runtime/input_ring_watermark.h>

#include <stdint.h>
#include <stdio.h>

static int
expect_u32(const char* label, uint32_t got, uint32_t want) {
    if (got != want) {
        fprintf(stderr, "FAIL: %s: got=%u want=%u\n", label, got, want);
        return 1;
    }
    return 0;
}

static int
expect_true(const char* label, int cond) {
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", label);
        return 1;
    }
    return 0;
}

/** Nanoseconds per second — used to construct test timestamps. */
static const uint64_t NS_PER_S = 1000000000ULL;

/**
 * @brief 3 low events in 60s → target increases by 100ms.
 *
 * WATERMARK_ADAPT_UP_THRESHOLD is 2, so >2 events (i.e. 3) triggers
 * an increase of WATERMARK_ADAPT_UP_MS (100ms).
 *
 * Default target_ms = 500, so after 3 events → 600ms.
 */
static int
test_three_events_increases_target(void) {
    int rc = 0;
    struct input_ring_watermark wm;
    watermark_init(&wm, 1, 1536000);

    rc |= expect_u32("initial target_ms", wm.target_ms, 500);

    uint64_t base = 1 * NS_PER_S;

    /* 3 low events within 60s window */
    watermark_on_low_event(&wm, base + 0 * NS_PER_S);
    watermark_on_low_event(&wm, base + 10 * NS_PER_S);
    watermark_on_low_event(&wm, base + 20 * NS_PER_S);

    /* After 3 events (>2 threshold), target should increase by 100ms */
    rc |= expect_u32("target after 3 events", wm.target_ms, 600);

    return rc;
}

/**
 * @brief Target capped at 1500ms.
 *
 * Repeatedly trigger low events to push target_ms up to the cap.
 */
static int
test_target_capped_at_max(void) {
    int rc = 0;
    struct input_ring_watermark wm;
    watermark_init(&wm, 1, 1536000);

    /* Start at 500ms. Each batch of 3 events adds 100ms.
     * Need 10 batches to go from 500 → 1500, then one more to verify cap. */
    uint64_t t = 1 * NS_PER_S;

    for (int batch = 0; batch < 12; batch++) {
        watermark_on_low_event(&wm, t);
        t += 1 * NS_PER_S;
        watermark_on_low_event(&wm, t);
        t += 1 * NS_PER_S;
        watermark_on_low_event(&wm, t);
        t += 1 * NS_PER_S;
    }

    rc |= expect_u32("target capped at max", wm.target_ms, WATERMARK_TARGET_MAX_MS);

    return rc;
}

/**
 * @brief No events for 30s → target decreases by 50ms.
 *
 * After a low event, if 30 seconds pass with no further events,
 * watermark_periodic_adjust() decreases target_ms by 50ms.
 */
static int
test_quiet_period_decreases_target(void) {
    int rc = 0;
    struct input_ring_watermark wm;
    watermark_init(&wm, 1, 1536000);

    /* First, push target up to 600ms with 3 events */
    uint64_t t = 1 * NS_PER_S;
    watermark_on_low_event(&wm, t);
    t += 5 * NS_PER_S;
    watermark_on_low_event(&wm, t);
    t += 5 * NS_PER_S;
    watermark_on_low_event(&wm, t);
    rc |= expect_u32("target after increase", wm.target_ms, 600);

    /* Now wait 30 seconds with no events */
    t += 30 * NS_PER_S;
    watermark_periodic_adjust(&wm, t);

    rc |= expect_u32("target after 30s quiet", wm.target_ms, 550);

    return rc;
}

/**
 * @brief Target floored at 300ms.
 *
 * Repeatedly trigger quiet-period decreases to push target_ms down
 * to the floor.
 */
static int
test_target_floored_at_min(void) {
    int rc = 0;
    struct input_ring_watermark wm;
    watermark_init(&wm, 1, 1536000);

    /* Default target is 500ms. We need a low event to set last_low_event_ns,
     * then repeated quiet periods to decrease. */
    uint64_t t = 1 * NS_PER_S;
    watermark_on_low_event(&wm, t);

    /* Each quiet period of 30s decreases by 50ms.
     * 500 → 450 → 400 → 350 → 300 (floor). Need 4 decreases + 1 more. */
    for (int i = 0; i < 6; i++) {
        t += 30 * NS_PER_S;
        watermark_periodic_adjust(&wm, t);
    }

    rc |= expect_u32("target floored at min", wm.target_ms, WATERMARK_TARGET_MIN_MS);

    return rc;
}

/**
 * @brief Target always in [300ms, 1500ms] after mixed event sequences.
 *
 * Interleave increases and decreases, verify bounds are maintained.
 */
static int
test_target_always_in_bounds(void) {
    int rc = 0;
    struct input_ring_watermark wm;
    watermark_init(&wm, 1, 1536000);

    uint64_t t = 1 * NS_PER_S;

    /* Push target up with many low events */
    for (int batch = 0; batch < 15; batch++) {
        watermark_on_low_event(&wm, t);
        t += 1 * NS_PER_S;
        watermark_on_low_event(&wm, t);
        t += 1 * NS_PER_S;
        watermark_on_low_event(&wm, t);
        t += 1 * NS_PER_S;
    }

    rc |= expect_true("target <= max after increases", wm.target_ms <= WATERMARK_TARGET_MAX_MS);
    rc |= expect_true("target >= min after increases", wm.target_ms >= WATERMARK_TARGET_MIN_MS);

    /* Now let it decrease with many quiet periods */
    for (int i = 0; i < 30; i++) {
        t += 30 * NS_PER_S;
        watermark_periodic_adjust(&wm, t);
    }

    rc |= expect_true("target <= max after decreases", wm.target_ms <= WATERMARK_TARGET_MAX_MS);
    rc |= expect_true("target >= min after decreases", wm.target_ms >= WATERMARK_TARGET_MIN_MS);

    /* Interleave: increase then decrease */
    for (int cycle = 0; cycle < 5; cycle++) {
        /* 3 events to increase */
        watermark_on_low_event(&wm, t);
        t += 1 * NS_PER_S;
        watermark_on_low_event(&wm, t);
        t += 1 * NS_PER_S;
        watermark_on_low_event(&wm, t);
        t += 1 * NS_PER_S;

        rc |= expect_true("target in bounds after increase cycle",
                          wm.target_ms >= WATERMARK_TARGET_MIN_MS && wm.target_ms <= WATERMARK_TARGET_MAX_MS);

        /* Quiet period to decrease */
        t += 30 * NS_PER_S;
        watermark_periodic_adjust(&wm, t);

        rc |= expect_true("target in bounds after decrease cycle",
                          wm.target_ms >= WATERMARK_TARGET_MIN_MS && wm.target_ms <= WATERMARK_TARGET_MAX_MS);
    }

    return rc;
}

int
main(void) {
    int rc = 0;
    rc |= test_three_events_increases_target();
    rc |= test_target_capped_at_max();
    rc |= test_quiet_period_decreases_target();
    rc |= test_target_floored_at_min();
    rc |= test_target_always_in_bounds();
    return rc ? 1 : 0;
}
