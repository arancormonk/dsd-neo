// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Unit tests for tcp_metrics_update_ring_fill().
 *
 * Verifies fill percentage computation for boundary values and
 * division-by-zero guard when capacity is 0.
 */

#include <dsd-neo/io/tcp_quality_metrics.h>

#include <math.h>
#include <stdio.h>

static int
expect_float_approx(const char* label, float got, float want, float eps) {
    if (fabsf(got - want) > eps) {
        fprintf(stderr, "FAIL: %s: got=%f want=%f (eps=%f)\n", label, got, want, eps);
        return 1;
    }
    return 0;
}

int
main(void) {
    int rc = 0;

    /* --- fill_pct = 0 when used=0 --- */
    {
        struct tcp_quality_metrics m;
        tcp_metrics_init(&m, 48000);

        tcp_metrics_update_ring_fill(&m, 0, 1000);
        rc |= expect_float_approx("fill 0/1000", m.snapshot.input_ring_fill_pct, 0.0f, 0.01f);
    }

    /* --- fill_pct = 50 when used=capacity/2 --- */
    {
        struct tcp_quality_metrics m;
        tcp_metrics_init(&m, 48000);

        tcp_metrics_update_ring_fill(&m, 500, 1000);
        rc |= expect_float_approx("fill 500/1000", m.snapshot.input_ring_fill_pct, 50.0f, 0.01f);
    }

    /* --- fill_pct = 100 when used=capacity --- */
    {
        struct tcp_quality_metrics m;
        tcp_metrics_init(&m, 48000);

        tcp_metrics_update_ring_fill(&m, 1000, 1000);
        rc |= expect_float_approx("fill 1000/1000", m.snapshot.input_ring_fill_pct, 100.0f, 0.01f);
    }

    /* --- fill_pct = 0 when capacity=0 (guard div by zero) --- */
    {
        struct tcp_quality_metrics m;
        tcp_metrics_init(&m, 48000);

        tcp_metrics_update_ring_fill(&m, 0, 0);
        rc |= expect_float_approx("fill 0/0", m.snapshot.input_ring_fill_pct, 0.0f, 0.01f);
    }

    /* --- fill_pct = 0 when capacity=0 but used>0 (guard div by zero) --- */
    {
        struct tcp_quality_metrics m;
        tcp_metrics_init(&m, 48000);

        tcp_metrics_update_ring_fill(&m, 100, 0);
        rc |= expect_float_approx("fill 100/0", m.snapshot.input_ring_fill_pct, 0.0f, 0.01f);
    }

    /* --- NULL metrics pointer is safe --- */
    tcp_metrics_update_ring_fill(NULL, 50, 100);

    return rc ? 1 : 0;
}
