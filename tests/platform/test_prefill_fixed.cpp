// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Unit tests for fixed pre-fill mode (non-TCP).
 *
 * Tests the pre-fill depth selection logic from dsd_audio_open_output().
 * Since g_prefill_depth_frames is a static global in audio_android.c and
 * not directly testable from outside, we test the clamping/selection logic
 * as a standalone function that mirrors the implementation.
 *
 * When the connection mode is not rtl_tcp, the pre-fill depth should use
 * a fixed value.  When the depth is invalid (0, negative, or exceeds the
 * ring size), the fallback to ring_size / 2 is used.
 *
 * _Requirements: 4.5, 7.5_
 */

#include <stdio.h>

/*============================================================================
 * Test helper — mirrors the pre-fill depth selection logic from
 * dsd_audio_open_output() in audio_android.c.
 *============================================================================*/

/**
 * @brief Select the pre-fill frame count based on the configured depth
 *        and the ring buffer capacity.
 *
 * @param depth      The adaptive pre-fill depth in frames (from g_prefill_depth_frames).
 * @param ring_size  The total ring buffer capacity in frames.
 * @return The number of frames to pre-fill.
 *
 * When depth is positive and strictly less than ring_size, it is used directly.
 * Otherwise, the fallback of ring_size / 2 is returned.
 */
static size_t
select_prefill_frames(int depth, size_t ring_size) {
    if (depth > 0 && (size_t)depth < ring_size) {
        return (size_t)depth;
    }
    return ring_size / 2;
}

/*============================================================================
 * Helpers
 *============================================================================*/

static int g_fail_count = 0;

static void
expect_size(const char* label, size_t got, size_t want) {
    if (got != want) {
        fprintf(stderr, "FAIL: %s: got=%zu want=%zu\n", label, got, want);
        g_fail_count++;
    }
}

/*============================================================================
 * Tests
 *============================================================================*/

/**
 * @brief Valid depth within ring capacity is used directly.
 *
 * depth=2400, ring_size=16000 → 2400
 */
static void
test_valid_depth(void) {
    expect_size("valid depth=2400 ring=16000", select_prefill_frames(2400, 16000), 2400);
}

/**
 * @brief Depth of zero triggers fallback to ring_size / 2.
 *
 * depth=0, ring_size=16000 → 8000
 */
static void
test_zero_depth(void) {
    expect_size("zero depth ring=16000", select_prefill_frames(0, 16000), 8000);
}

/**
 * @brief Negative depth triggers fallback to ring_size / 2.
 *
 * depth=-1, ring_size=16000 → 8000
 */
static void
test_negative_depth(void) {
    expect_size("negative depth=-1 ring=16000", select_prefill_frames(-1, 16000), 8000);
}

/**
 * @brief Depth exceeding ring capacity triggers fallback.
 *
 * depth=20000, ring_size=16000 → 8000
 */
static void
test_depth_exceeds_ring(void) {
    expect_size("depth exceeds ring depth=20000 ring=16000", select_prefill_frames(20000, 16000), 8000);
}

/**
 * @brief Depth equal to ring_size triggers fallback (not strictly less).
 *
 * depth=16000, ring_size=16000 → 8000
 */
static void
test_depth_equals_ring(void) {
    expect_size("depth equals ring depth=16000 ring=16000", select_prefill_frames(16000, 16000), 8000);
}

/**
 * @brief Depth just below ring_size is accepted.
 *
 * depth=15999, ring_size=16000 → 15999
 */
static void
test_depth_just_below_ring(void) {
    expect_size("depth just below ring depth=15999 ring=16000", select_prefill_frames(15999, 16000), 15999);
}

/**
 * @brief Minimum valid depth (1 frame) is accepted.
 *
 * depth=1, ring_size=16000 → 1
 */
static void
test_minimum_valid_depth(void) {
    expect_size("minimum valid depth=1 ring=16000", select_prefill_frames(1, 16000), 1);
}

/**
 * @brief Large negative depth triggers fallback.
 *
 * depth=-10000, ring_size=16000 → 8000
 */
static void
test_large_negative_depth(void) {
    expect_size("large negative depth=-10000 ring=16000", select_prefill_frames(-10000, 16000), 8000);
}

int
main(void) {
    test_valid_depth();
    test_zero_depth();
    test_negative_depth();
    test_depth_exceeds_ring();
    test_depth_equals_ring();
    test_depth_just_below_ring();
    test_minimum_valid_depth();
    test_large_negative_depth();

    if (g_fail_count > 0) {
        fprintf(stderr, "\n%d test(s) FAILED\n", g_fail_count);
        return 1;
    }

    return 0;
}
