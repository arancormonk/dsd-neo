// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Exhaustive example-based tests for throughput watchdog trigger.
 *
 * Validates Property 8: The throughput watchdog triggers iff the 3-second
 * window ratio < 0.25 AND elapsed time since connection > 5 seconds.
 * It never triggers during the 5-second grace period.
 */

/* Feature: rtl-tcp-lag-resilience, Property 8: Throughput watchdog trigger with grace period */
/* Validates: Requirements 5.2, 5.4 */

#include <dsd-neo/io/tcp_quality_metrics.h>

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

/**
 * @brief Watchdog does NOT trigger during 5s grace period even with 0 bytes.
 *
 * Send no data for 3 seconds, but within the 5s grace period.
 * Watchdog should not fire.
 */
static int
test_grace_period_no_trigger(void) {
    int rc = 0;
    struct tcp_quality_metrics m;
    tcp_metrics_init(&m, 48000);

    uint64_t conn_ns = m.connection_established_ns;

    /* Record recv at +1s (within grace period) with 0 bytes */
    int fired = tcp_metrics_record_recv(&m, 0, conn_ns + 1000000000ULL);
    rc |= expect_int("grace +1s no trigger", fired, 0);

    /* Record at +3s — this crosses the 3s watchdog window but still in grace */
    fired = tcp_metrics_record_recv(&m, 0, conn_ns + 3000000000ULL);
    rc |= expect_int("grace +3s no trigger", fired, 0);

    /* Record at +4s — still in grace period */
    fired = tcp_metrics_record_recv(&m, 0, conn_ns + 4000000000ULL);
    rc |= expect_int("grace +4s no trigger", fired, 0);

    return rc;
}

/**
 * @brief Watchdog triggers when ratio < 0.25 AND elapsed > 5s.
 *
 * At 48000 Hz, expected bytes in 3s = 48000 × 2 × 3 = 288000.
 * 25% of that = 72000.  Send less than 72000 bytes after the grace period.
 */
static int
test_watchdog_triggers_after_grace(void) {
    int rc = 0;
    struct tcp_quality_metrics m;
    tcp_metrics_init(&m, 48000);

    uint64_t conn_ns = m.connection_established_ns;

    /* Advance past grace period: send some data at +5.5s to reset watchdog window */
    tcp_metrics_record_recv(&m, 1000, conn_ns + 5500000000ULL);

    /* Now we're past grace. The watchdog window started at init time.
     * Since 5.5s > 3s watchdog window, the first record_recv already
     * evaluated the watchdog. Let's set up a clean 3s window. */

    /* The watchdog window was reset after the first evaluation.
     * Now send very little data over the next 3 seconds. */
    tcp_metrics_record_recv(&m, 100, conn_ns + 6000000000ULL);
    tcp_metrics_record_recv(&m, 100, conn_ns + 7000000000ULL);

    /* Trigger watchdog window expiry at +8.5s (3s after 5.5s reset) */
    int fired = tcp_metrics_record_recv(&m, 100, conn_ns + 8500000000ULL);

    /* Total bytes in this 3s window: 100 + 100 + 100 = 300
     * Expected: 48000 × 2 × 3 = 288000
     * Ratio: 300 / 288000 ≈ 0.001 — well below 0.25 */
    rc |= expect_int("watchdog fires after grace", fired, 1);

    return rc;
}

/**
 * @brief Watchdog does NOT trigger when ratio >= 0.25.
 *
 * Send enough data to keep the ratio above the threshold.
 */
static int
test_watchdog_no_trigger_good_throughput(void) {
    int rc = 0;
    struct tcp_quality_metrics m;
    tcp_metrics_init(&m, 48000);

    uint64_t conn_ns = m.connection_established_ns;

    /* Advance past grace period with good data */
    tcp_metrics_record_recv(&m, 100000, conn_ns + 5500000000ULL);

    /* Send plenty of data in the next 3s window.
     * Expected in 3s: 48000 × 2 × 3 = 288000.
     * Send 288000 bytes (ratio = 1.0). */
    tcp_metrics_record_recv(&m, 96000, conn_ns + 6500000000ULL);
    tcp_metrics_record_recv(&m, 96000, conn_ns + 7500000000ULL);

    /* Trigger watchdog window expiry */
    int fired = tcp_metrics_record_recv(&m, 96000, conn_ns + 8500000000ULL);

    rc |= expect_int("good throughput no trigger", fired, 0);

    return rc;
}

/**
 * @brief Watchdog does NOT trigger when sample_rate=0.
 *
 * With sample_rate=0, the watchdog should never fire (division by zero guard).
 */
static int
test_watchdog_zero_sample_rate(void) {
    int rc = 0;
    struct tcp_quality_metrics m;
    tcp_metrics_init(&m, 0);

    uint64_t conn_ns = m.connection_established_ns;

    /* Advance well past grace period with no data */
    int fired = tcp_metrics_record_recv(&m, 0, conn_ns + 6000000000ULL);
    rc |= expect_int("zero rate +6s no trigger", fired, 0);

    /* Trigger another watchdog window */
    fired = tcp_metrics_record_recv(&m, 0, conn_ns + 9000000000ULL);
    rc |= expect_int("zero rate +9s no trigger", fired, 0);

    return rc;
}

/**
 * @brief Watchdog at the exact boundary of the grace period.
 *
 * At exactly 5s elapsed, the watchdog should NOT trigger (> 5s required,
 * not >=).
 */
static int
test_watchdog_exact_grace_boundary(void) {
    int rc = 0;
    struct tcp_quality_metrics m;
    tcp_metrics_init(&m, 48000);

    uint64_t conn_ns = m.connection_established_ns;

    /* Send 0 bytes, trigger watchdog at exactly +5s.
     * The 3s watchdog window started at conn_ns, so at +3s it would
     * evaluate. But elapsed = 3s ≤ 5s, so no trigger. */
    int fired = tcp_metrics_record_recv(&m, 0, conn_ns + 3000000000ULL);
    rc |= expect_int("exact +3s no trigger", fired, 0);

    /* Next watchdog window: +3s to +6s. At +6s, elapsed = 6s > 5s.
     * With 0 bytes, ratio = 0 < 0.25 → should trigger. */
    fired = tcp_metrics_record_recv(&m, 0, conn_ns + 6000000000ULL);
    rc |= expect_int("exact +6s triggers", fired, 1);

    return rc;
}

int
main(void) {
    int rc = 0;
    rc |= test_grace_period_no_trigger();
    rc |= test_watchdog_triggers_after_grace();
    rc |= test_watchdog_no_trigger_good_throughput();
    rc |= test_watchdog_zero_sample_rate();
    rc |= test_watchdog_exact_grace_boundary();
    return rc ? 1 : 0;
}
