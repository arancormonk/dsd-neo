// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Exhaustive example-based tests for watermark pause/resume logic.
 *
 * Validates Property 6: Watermark pause/resume with disabled passthrough.
 *
 * For various fill levels and enabled/disabled states, verifies that
 * watermark_should_consume() returns the correct pause/resume decision.
 */

/* Feature: rtl-tcp-lag-resilience, Property 6 */
/* Validates: Requirements 3.3, 3.7, 7.1 */

#include <dsd-neo/runtime/input_ring_watermark.h>

#include <stdio.h>

static int
expect_int(const char* label, int got, int want) {
    if (got != want) {
        fprintf(stderr, "FAIL: %s: got=%d want=%d\n", label, got, want);
        return 1;
    }
    return 0;
}

/**
 * @brief Disabled mode: always returns 1 regardless of fill level.
 *
 * When enabled=0 (non-TCP mode), the watermark system is a no-op
 * passthrough — demod always consumes.
 */
static int
test_disabled_always_consumes(void) {
    int rc = 0;
    struct input_ring_watermark wm;
    watermark_init(&wm, 0, 1536000);

    /* Empty ring */
    rc |= expect_int("disabled, empty ring", watermark_should_consume(&wm, 0, 1000000), 1);

    /* Partially filled */
    rc |= expect_int("disabled, partial fill", watermark_should_consume(&wm, 500000, 1000000), 1);

    /* Full ring */
    rc |= expect_int("disabled, full ring", watermark_should_consume(&wm, 999999, 1000000), 1);

    /* Very small fill (below what would be low_watermark if enabled) */
    rc |= expect_int("disabled, tiny fill", watermark_should_consume(&wm, 1, 1000000), 1);

    return rc;
}

/**
 * @brief Enabled, fill drops below low_watermark → returns 0 (pause).
 *
 * At sample_rate=1536000, low_watermark = 614400 elements.
 * When fill < 614400, should_consume returns 0 and sets paused=1.
 */
static int
test_enabled_below_low_pauses(void) {
    int rc = 0;
    struct input_ring_watermark wm;
    watermark_init(&wm, 1, 1536000);

    /* Ring capacity large enough to hold all watermarks */
    size_t capacity = 10000000;

    /* Fill just below low_watermark (614400) */
    int result = watermark_should_consume(&wm, 614399, capacity);
    rc |= expect_int("below low → pause", result, 0);
    rc |= expect_int("paused flag set", wm.paused, 1);

    return rc;
}

/**
 * @brief Enabled, paused, fill reaches target_watermark → returns 1 (resume).
 *
 * At sample_rate=1536000, target_watermark = 1536000 elements.
 * When paused and fill >= target, should_consume returns 1 and clears paused.
 */
static int
test_enabled_paused_reaches_target_resumes(void) {
    int rc = 0;
    struct input_ring_watermark wm;
    watermark_init(&wm, 1, 1536000);

    size_t capacity = 10000000;

    /* First, trigger a pause by going below low_watermark */
    watermark_should_consume(&wm, 100, capacity);
    rc |= expect_int("pre: paused", wm.paused, 1);

    /* Now fill reaches target_watermark (1536000) → resume */
    int result = watermark_should_consume(&wm, 1536000, capacity);
    rc |= expect_int("at target → resume", result, 1);
    rc |= expect_int("paused cleared", wm.paused, 0);

    return rc;
}

/**
 * @brief Enabled, not paused, fill above low_watermark → returns 1 (continue).
 *
 * Normal operation: ring has enough data, demod keeps consuming.
 */
static int
test_enabled_above_low_continues(void) {
    int rc = 0;
    struct input_ring_watermark wm;
    watermark_init(&wm, 1, 1536000);

    size_t capacity = 10000000;

    /* Fill well above low_watermark */
    int result = watermark_should_consume(&wm, 2000000, capacity);
    rc |= expect_int("above low → continue", result, 1);
    rc |= expect_int("not paused", wm.paused, 0);

    return rc;
}

/**
 * @brief Enabled, paused, fill still below target → returns 0 (stay paused).
 *
 * The ring is refilling but hasn't reached the target yet.
 */
static int
test_enabled_paused_below_target_stays_paused(void) {
    int rc = 0;
    struct input_ring_watermark wm;
    watermark_init(&wm, 1, 1536000);

    size_t capacity = 10000000;

    /* Trigger pause */
    watermark_should_consume(&wm, 100, capacity);
    rc |= expect_int("pre: paused", wm.paused, 1);

    /* Fill is above low but below target (1536000) */
    int result = watermark_should_consume(&wm, 1000000, capacity);
    rc |= expect_int("below target → stay paused", result, 0);
    rc |= expect_int("still paused", wm.paused, 1);

    return rc;
}

/**
 * @brief Full transition sequence: normal → pause → resume → normal.
 *
 * Simulates a TCP stall and recovery cycle:
 * 1. Normal consumption (fill above low)
 * 2. Fill drops below low → pause
 * 3. Ring refills to target → resume
 * 4. Normal consumption continues
 */
static int
test_full_transition_sequence(void) {
    int rc = 0;
    struct input_ring_watermark wm;
    watermark_init(&wm, 1, 1536000);

    size_t capacity = 10000000;

    /* Step 1: Normal — fill well above low_watermark */
    int r1 = watermark_should_consume(&wm, 3000000, capacity);
    rc |= expect_int("step1: normal consume", r1, 1);
    rc |= expect_int("step1: not paused", wm.paused, 0);

    /* Step 2: Fill drops below low_watermark → pause */
    int r2 = watermark_should_consume(&wm, 500000, capacity);
    rc |= expect_int("step2: pause", r2, 0);
    rc |= expect_int("step2: paused", wm.paused, 1);

    /* Step 3: Ring refills to target → resume */
    int r3 = watermark_should_consume(&wm, 1536000, capacity);
    rc |= expect_int("step3: resume", r3, 1);
    rc |= expect_int("step3: not paused", wm.paused, 0);

    /* Step 4: Normal consumption continues */
    int r4 = watermark_should_consume(&wm, 2000000, capacity);
    rc |= expect_int("step4: normal", r4, 1);
    rc |= expect_int("step4: not paused", wm.paused, 0);

    return rc;
}

/**
 * @brief Exactly at low_watermark boundary — should NOT pause.
 *
 * The condition is fill < low_watermark, so fill == low_watermark
 * should continue consuming.
 */
static int
test_exactly_at_low_watermark(void) {
    int rc = 0;
    struct input_ring_watermark wm;
    watermark_init(&wm, 1, 1536000);

    size_t capacity = 10000000;

    /* Fill exactly at low_watermark (614400) — not below, so continue */
    int result = watermark_should_consume(&wm, 614400, capacity);
    rc |= expect_int("at low boundary → continue", result, 1);
    rc |= expect_int("not paused", wm.paused, 0);

    return rc;
}

int
main(void) {
    int rc = 0;
    rc |= test_disabled_always_consumes();
    rc |= test_enabled_below_low_pauses();
    rc |= test_enabled_paused_reaches_target_resumes();
    rc |= test_enabled_above_low_continues();
    rc |= test_enabled_paused_below_target_stays_paused();
    rc |= test_full_transition_sequence();
    rc |= test_exactly_at_low_watermark();
    return rc ? 1 : 0;
}
