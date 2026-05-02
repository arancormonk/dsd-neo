// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Unit tests for watermark init and default thresholds.
 *
 * Validates that watermark_init() correctly converts ms thresholds to
 * element counts, sets defaults, and handles edge cases (sample_rate=0,
 * enabled=0).
 */

#include <dsd-neo/runtime/input_ring_watermark.h>

#include <stdint.h>
#include <stdio.h>

static int
expect_int(const char* label, int got, int want) {
    if (got != want) {
        fprintf(stderr, "FAIL: %s: got=%d want=%d\n", label, got, want);
        return 1;
    }
    return 0;
}

static int
expect_size(const char* label, size_t got, size_t want) {
    if (got != want) {
        fprintf(stderr, "FAIL: %s: got=%zu want=%zu\n", label, got, want);
        return 1;
    }
    return 0;
}

static int
expect_u32(const char* label, uint32_t got, uint32_t want) {
    if (got != want) {
        fprintf(stderr, "FAIL: %s: got=%u want=%u\n", label, got, want);
        return 1;
    }
    return 0;
}

static int
expect_float(const char* label, float got, float want) {
    if (got != want) {
        fprintf(stderr, "FAIL: %s: got=%f want=%f\n", label, got, want);
        return 1;
    }
    return 0;
}

/**
 * @brief Test default thresholds after init with sample_rate=1536000.
 *
 * low_watermark  = 200 × 1536000 × 2 / 1000 = 614400 elements
 * target_watermark = 500 × 1536000 × 2 / 1000 = 1536000 elements
 */
static int
test_defaults_at_1536000(void) {
    int rc = 0;
    struct input_ring_watermark wm;
    watermark_init(&wm, 1, 1536000);

    rc |= expect_int("enabled", wm.enabled, 1);
    rc |= expect_u32("sample_rate", wm.sample_rate, 1536000);
    rc |= expect_u32("target_ms", wm.target_ms, WATERMARK_TARGET_DEFAULT_MS);
    rc |= expect_int("paused", wm.paused, 0);
    rc |= expect_float("fill_ema", wm.fill_ema, 0.0f);
    rc |= expect_float("ema_alpha", wm.ema_alpha, 0.1f);

    /* ms→elements: 200 × 1536000 × 2 / 1000 = 614400 */
    rc |= expect_size("low_watermark", wm.low_watermark, 614400U);

    /* ms→elements: 500 × 1536000 × 2 / 1000 = 1536000 */
    rc |= expect_size("target_watermark", wm.target_watermark, 1536000U);

    return rc;
}

/**
 * @brief Test ms→elements conversion correctness.
 *
 * 200ms at 1536000 Hz = 200 × 1536000 × 2 / 1000 = 614400 elements.
 * This verifies the interleaved I/Q factor of 2 is applied.
 */
static int
test_ms_to_elements_conversion(void) {
    int rc = 0;
    struct input_ring_watermark wm;
    watermark_init(&wm, 1, 1536000);

    /* Verify the low watermark conversion explicitly */
    size_t expected_low = (size_t)((uint64_t)200 * 1536000 * 2 / 1000);
    rc |= expect_size("low_watermark elements", wm.low_watermark, expected_low);

    /* Verify the target watermark conversion explicitly */
    size_t expected_target = (size_t)((uint64_t)500 * 1536000 * 2 / 1000);
    rc |= expect_size("target_watermark elements", wm.target_watermark, expected_target);

    return rc;
}

/**
 * @brief Test init with sample_rate=0 forces enabled=0.
 *
 * A zero sample rate makes ms→elements conversion meaningless, so the
 * watermark system must disable itself to avoid nonsensical thresholds.
 */
static int
test_sample_rate_zero_disables(void) {
    int rc = 0;
    struct input_ring_watermark wm;

    /* Even if caller requests enabled=1, sample_rate=0 forces disabled. */
    watermark_init(&wm, 1, 0);
    rc |= expect_int("enabled with sr=0", wm.enabled, 0);

    return rc;
}

/**
 * @brief Test init with enabled=0 sets passthrough mode.
 *
 * When the connection mode is not rtl_tcp, the watermark system should
 * be disabled so the demod thread consumes without pausing.
 */
static int
test_disabled_passthrough(void) {
    int rc = 0;
    struct input_ring_watermark wm;
    watermark_init(&wm, 0, 1536000);

    rc |= expect_int("enabled=0 passthrough", wm.enabled, 0);

    /* Even though sample_rate is valid, watermark_should_consume should
     * always return 1 when disabled. */
    int consume = watermark_should_consume(&wm, 0, 1000000);
    rc |= expect_int("disabled always consumes", consume, 1);

    return rc;
}

int
main(void) {
    int rc = 0;
    rc |= test_defaults_at_1536000();
    rc |= test_ms_to_elements_conversion();
    rc |= test_sample_rate_zero_disables();
    rc |= test_disabled_passthrough();
    return rc ? 1 : 0;
}
