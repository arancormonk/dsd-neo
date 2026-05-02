// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Exhaustive example-based tests for jitter variance computation.
 *
 * Validates Property 5: For sequences of monotonically increasing recv
 * timestamps, the jitter metric equals the variance of inter-arrival
 * deltas, computed as E[Δ²] − E[Δ]².
 */

/* Feature: rtl-tcp-lag-resilience, Property 5: Jitter variance computation */
/* Validates: Requirements 2.2 */

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
 * @brief Test jitter with evenly spaced timestamps.
 *
 * When all inter-recv deltas are identical, variance should be ~0.
 * The window-expiry recv call is also part of the even spacing so it
 * doesn't introduce an outlier delta.
 *
 * Timestamps: 0, 100ms, 200ms, ..., 1000ms (11 calls, 10 deltas of 100ms)
 * Deltas (us): all 100000
 * Variance = E[Δ²] − E[Δ]² = 100000² − 100000² = 0
 */
static int
test_even_spacing_zero_jitter(void) {
    int rc = 0;
    struct tcp_quality_metrics m;
    tcp_metrics_init(&m, 48000);

    uint64_t base_ns = m.window_start_ns;

    /* 11 evenly spaced recv calls at 100ms intervals.
     * The last call at +1000ms triggers window expiry and is also
     * evenly spaced from the previous call at +900ms. */
    tcp_metrics_record_recv(&m, 1000, base_ns + 0ULL);
    tcp_metrics_record_recv(&m, 1000, base_ns + 100000000ULL);
    tcp_metrics_record_recv(&m, 1000, base_ns + 200000000ULL);
    tcp_metrics_record_recv(&m, 1000, base_ns + 300000000ULL);
    tcp_metrics_record_recv(&m, 1000, base_ns + 400000000ULL);
    tcp_metrics_record_recv(&m, 1000, base_ns + 500000000ULL);
    tcp_metrics_record_recv(&m, 1000, base_ns + 600000000ULL);
    tcp_metrics_record_recv(&m, 1000, base_ns + 700000000ULL);
    tcp_metrics_record_recv(&m, 1000, base_ns + 800000000ULL);
    tcp_metrics_record_recv(&m, 1000, base_ns + 900000000ULL);
    tcp_metrics_record_recv(&m, 1000, base_ns + 1000000000ULL);

    struct tcp_quality_snapshot snap = tcp_metrics_get_snapshot(&m);

    /* With perfectly even spacing, jitter variance should be ~0 */
    rc |= expect_float_approx("even spacing jitter ~0", snap.jitter_us, 0.0f, 1.0f);

    return rc;
}

/**
 * @brief Test jitter with known uneven timestamps.
 *
 * Timestamps: 0, 50ms, 200ms, 250ms, 500ms, 1000ms (window expiry)
 * Deltas (ns): 50000000, 150000000, 50000000, 250000000, 500000000
 * Deltas (us): 50000, 150000, 50000, 250000, 500000
 *
 * n = 5 deltas
 * Mean = (50000 + 150000 + 50000 + 250000 + 500000) / 5 = 200000 us
 * E[Δ²] = (50000² + 150000² + 50000² + 250000² + 500000²) / 5
 *        = (2500000000 + 22500000000 + 2500000000 + 62500000000 + 250000000000) / 5
 *        = 340000000000 / 5
 *        = 68000000000
 * E[Δ]² = 200000² = 40000000000
 * Variance = 68000000000 − 40000000000 = 28000000000 us²
 */
static int
test_known_uneven_timestamps(void) {
    int rc = 0;
    struct tcp_quality_metrics m;
    tcp_metrics_init(&m, 48000);

    uint64_t base_ns = m.window_start_ns;

    tcp_metrics_record_recv(&m, 1000, base_ns + 0ULL);
    tcp_metrics_record_recv(&m, 1000, base_ns + 50000000ULL);  /* +50ms */
    tcp_metrics_record_recv(&m, 1000, base_ns + 200000000ULL); /* +200ms */
    tcp_metrics_record_recv(&m, 1000, base_ns + 250000000ULL); /* +250ms */
    tcp_metrics_record_recv(&m, 1000, base_ns + 500000000ULL); /* +500ms */
    /* Window expiry call — also contributes a delta */
    tcp_metrics_record_recv(&m, 0, base_ns + 1000000000ULL); /* +1000ms */

    struct tcp_quality_snapshot snap = tcp_metrics_get_snapshot(&m);

    /* Expected variance = 28000000000 us² (as float) */
    float expected_variance = 28000000000.0f;

    /* Use relative tolerance since the numbers are large */
    float rel_err = fabsf(snap.jitter_us - expected_variance) / expected_variance;
    rc |= expect_true("uneven jitter variance within 1%", rel_err < 0.01f);

    return rc;
}

/**
 * @brief Test jitter with only 2 recv events producing 1 delta.
 *
 * With a single delta, variance should be 0 (no spread).
 * The second recv triggers window expiry at exactly +1s.
 */
static int
test_single_delta(void) {
    int rc = 0;
    struct tcp_quality_metrics m;
    tcp_metrics_init(&m, 48000);

    uint64_t base_ns = m.window_start_ns;

    /* Two recv calls: first sets last_recv_ns, second computes one delta
     * and triggers window expiry. Delta = 1000ms = 1000000 us. */
    tcp_metrics_record_recv(&m, 1000, base_ns + 0ULL);
    tcp_metrics_record_recv(&m, 1000, base_ns + 1000000000ULL);

    struct tcp_quality_snapshot snap = tcp_metrics_get_snapshot(&m);

    /* Single delta → variance = 0 */
    rc |= expect_float_approx("single delta jitter = 0", snap.jitter_us, 0.0f, 1.0f);

    return rc;
}

/**
 * @brief Test jitter with no recv events in window.
 *
 * If no deltas are recorded, jitter should remain 0.
 */
static int
test_no_recv_events(void) {
    int rc = 0;
    struct tcp_quality_metrics m;
    tcp_metrics_init(&m, 48000);

    uint64_t base_ns = m.window_start_ns;

    /* Single recv (no delta can be computed from just one event) */
    tcp_metrics_record_recv(&m, 0, base_ns + 500000000ULL);

    /* Trigger window expiry */
    tcp_metrics_record_recv(&m, 0, base_ns + 1000000000ULL);

    struct tcp_quality_snapshot snap = tcp_metrics_get_snapshot(&m);

    /* No deltas → jitter = 0 */
    rc |= expect_float_approx("no deltas jitter = 0", snap.jitter_us, 0.0f, 0.01f);

    return rc;
}

int
main(void) {
    int rc = 0;
    rc |= test_even_spacing_zero_jitter();
    rc |= test_known_uneven_timestamps();
    rc |= test_single_delta();
    rc |= test_no_recv_events();
    return rc ? 1 : 0;
}
