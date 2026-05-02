// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Exhaustive example-based tests for throughput ratio computation.
 *
 * Validates Property 4: For sequences of recv events with byte counts
 * within a 1-second window and a configured sample_rate, the throughput_ratio
 * equals sum(bytes) / (sample_rate × 2 × window_duration_seconds).
 */

/* Feature: rtl-tcp-lag-resilience, Property 4: Throughput ratio computation */
/* Validates: Requirements 2.1 */

#include <dsd-neo/io/tcp_quality_metrics.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>

static int
expect_float_approx(const char* label, float got, float want, float eps) {
    if (fabsf(got - want) > eps) {
        fprintf(stderr, "FAIL: %s: got=%f want=%f (eps=%f)\n", label, got, want, eps);
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

/**
 * @brief Test throughput ratio with a known byte sequence.
 *
 * Send exactly the expected number of bytes for 1 second at 48000 Hz
 * sample rate.  Expected bytes = 48000 × 2 = 96000 bytes/s.
 * Ratio should be ~1.0.
 */
static int
test_exact_throughput(void) {
    int rc = 0;
    struct tcp_quality_metrics m;
    tcp_metrics_init(&m, 48000);

    uint64_t base_ns = m.window_start_ns;

    /* Send 96000 bytes total across 4 recv calls within 1 second */
    tcp_metrics_record_recv(&m, 24000, base_ns + 200000000ULL); /* +200ms */
    tcp_metrics_record_recv(&m, 24000, base_ns + 400000000ULL); /* +400ms */
    tcp_metrics_record_recv(&m, 24000, base_ns + 600000000ULL); /* +600ms */
    tcp_metrics_record_recv(&m, 24000, base_ns + 800000000ULL); /* +800ms */

    /* Trigger window expiry at exactly +1.0s */
    tcp_metrics_record_recv(&m, 0, base_ns + 1000000000ULL);

    struct tcp_quality_snapshot snap = tcp_metrics_get_snapshot(&m);

    /* Expected: 96000 / (48000 × 2 × 1.0) = 1.0 */
    rc |= expect_float_approx("exact throughput ratio", snap.throughput_ratio, 1.0f, 0.01f);

    return rc;
}

/**
 * @brief Test throughput ratio at half the expected rate.
 *
 * Send half the expected bytes.  Ratio should be ~0.5.
 */
static int
test_half_throughput(void) {
    int rc = 0;
    struct tcp_quality_metrics m;
    tcp_metrics_init(&m, 48000);

    uint64_t base_ns = m.window_start_ns;

    /* Send 48000 bytes (half of 96000 expected) */
    tcp_metrics_record_recv(&m, 24000, base_ns + 500000000ULL); /* +500ms */
    tcp_metrics_record_recv(&m, 24000, base_ns + 900000000ULL); /* +900ms */

    /* Trigger window expiry */
    tcp_metrics_record_recv(&m, 0, base_ns + 1000000000ULL);

    struct tcp_quality_snapshot snap = tcp_metrics_get_snapshot(&m);

    /* Expected: 48000 / (48000 × 2 × 1.0) = 0.5 */
    rc |= expect_float_approx("half throughput ratio", snap.throughput_ratio, 0.5f, 0.01f);

    return rc;
}

/**
 * @brief Test with sample_rate=0 — should return 0.0 and not crash.
 */
static int
test_zero_sample_rate(void) {
    int rc = 0;
    struct tcp_quality_metrics m;
    tcp_metrics_init(&m, 0);

    uint64_t base_ns = m.window_start_ns;

    tcp_metrics_record_recv(&m, 10000, base_ns + 500000000ULL);

    /* Trigger window expiry */
    tcp_metrics_record_recv(&m, 0, base_ns + 1000000000ULL);

    struct tcp_quality_snapshot snap = tcp_metrics_get_snapshot(&m);

    /* With sample_rate=0, ratio should be 0.0 (division by zero guard) */
    rc |= expect_float_approx("zero sample_rate ratio", snap.throughput_ratio, 0.0f, 0.001f);

    return rc;
}

/**
 * @brief Test multiple windows — verify reset between windows.
 *
 * First window: send full throughput.  Second window: send nothing.
 * Verify each window computes independently.
 */
static int
test_multiple_windows(void) {
    int rc = 0;
    struct tcp_quality_metrics m;
    tcp_metrics_init(&m, 48000);

    uint64_t base_ns = m.window_start_ns;

    /* Window 1: full throughput (96000 bytes in 1s) */
    tcp_metrics_record_recv(&m, 96000, base_ns + 500000000ULL);

    /* Trigger window 1 expiry */
    tcp_metrics_record_recv(&m, 0, base_ns + 1000000000ULL);

    struct tcp_quality_snapshot snap1 = tcp_metrics_get_snapshot(&m);
    rc |= expect_float_approx("window1 ratio ~1.0", snap1.throughput_ratio, 1.0f, 0.01f);

    /* Window 2: no data for 1 second, then trigger expiry with 0 bytes.
     * The window_start_ns was reset to base_ns + 1000000000ULL after window 1. */
    tcp_metrics_record_recv(&m, 0, base_ns + 2000000000ULL);

    struct tcp_quality_snapshot snap2 = tcp_metrics_get_snapshot(&m);

    /* Window 2 had 0 bytes received, so ratio should be 0.0 */
    rc |= expect_float_approx("window2 ratio 0.0", snap2.throughput_ratio, 0.0f, 0.01f);

    return rc;
}

/**
 * @brief Test with a high sample rate (1.536 MHz) and realistic byte counts.
 *
 * At 1536000 Hz, expected = 1536000 × 2 = 3072000 bytes/s.
 * Send 3072000 bytes → ratio ~1.0.
 */
static int
test_high_sample_rate(void) {
    int rc = 0;
    struct tcp_quality_metrics m;
    tcp_metrics_init(&m, 1536000);

    uint64_t base_ns = m.window_start_ns;

    /* Send in 16 KiB chunks, ~188 chunks for 3072000 bytes */
    uint32_t total = 0;
    uint64_t t = base_ns;
    while (total < 3072000) {
        uint32_t chunk = 16384;
        if (total + chunk > 3072000) {
            chunk = 3072000 - total;
        }
        t += 5000000ULL; /* +5ms per chunk */
        tcp_metrics_record_recv(&m, chunk, t);
        total += chunk;
    }

    /* Ensure window has expired (should have, since we sent ~940ms of data) */
    if (t - base_ns < 1000000000ULL) {
        tcp_metrics_record_recv(&m, 0, base_ns + 1000000000ULL);
    }

    struct tcp_quality_snapshot snap = tcp_metrics_get_snapshot(&m);

    /* Ratio should be close to 1.0 */
    rc |= expect_true("high rate ratio > 0.9", snap.throughput_ratio > 0.9f);
    rc |= expect_true("high rate ratio < 1.1", snap.throughput_ratio < 1.1f);

    return rc;
}

int
main(void) {
    int rc = 0;
    rc |= test_exact_throughput();
    rc |= test_half_throughput();
    rc |= test_zero_sample_rate();
    rc |= test_multiple_windows();
    rc |= test_high_sample_rate();
    return rc ? 1 : 0;
}
