// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Unit test for tcp_metrics_get_snapshot() readability.
 *
 * Verifies that after recording some recv events and updating ring fill,
 * get_snapshot returns a readable copy with expected field values.
 */

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

static int
expect_true(const char* label, int cond) {
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", label);
        return 1;
    }
    return 0;
}

int
main(void) {
    int rc = 0;

    /* --- snapshot reflects ring fill after update --- */
    {
        struct tcp_quality_metrics m;
        tcp_metrics_init(&m, 1536000);

        /* Update ring fill */
        tcp_metrics_update_ring_fill(&m, 250, 1000);

        /* Manually set some snapshot fields to simulate metrics collection */
        m.snapshot.audio_underrun_count = 5;
        m.snapshot.producer_drops = 2;

        struct tcp_quality_snapshot snap = tcp_metrics_get_snapshot(&m);

        /* Verify the snapshot is a readable copy with expected values */
        rc |= expect_true("snapshot fill_pct > 0", snap.input_ring_fill_pct > 0.0f);
        rc |=
            expect_true("snapshot fill_pct ~25%", snap.input_ring_fill_pct > 24.0f && snap.input_ring_fill_pct < 26.0f);
        rc |= expect_true("snapshot audio_underrun_count", snap.audio_underrun_count == 5);
        rc |= expect_true("snapshot producer_drops", snap.producer_drops == 2);
    }

    /* --- snapshot from NULL returns zeroed struct --- */
    {
        struct tcp_quality_snapshot snap = tcp_metrics_get_snapshot(NULL);
        rc |= expect_int("null snapshot watchdog_triggered", snap.watchdog_triggered, 0);
        rc |= expect_true("null snapshot throughput_ratio == 0", snap.throughput_ratio == 0.0f);
    }

    /* --- snapshot after recording recv events within a window --- */
    {
        struct tcp_quality_metrics m;
        tcp_metrics_init(&m, 1536000);

        /* Record some recv events within the same 1s window.
         * Use timestamps that don't cross the 1s boundary so the
         * window hasn't expired yet — snapshot should still be at defaults. */
        uint64_t base_ns = m.window_start_ns;
        tcp_metrics_record_recv(&m, 1000, base_ns + 100000000ULL); /* +100ms */
        tcp_metrics_record_recv(&m, 2000, base_ns + 200000000ULL); /* +200ms */
        tcp_metrics_record_recv(&m, 3000, base_ns + 300000000ULL); /* +300ms */

        struct tcp_quality_snapshot snap = tcp_metrics_get_snapshot(&m);

        /* Window hasn't expired, so throughput_ratio should still be 0 (initial) */
        rc |= expect_true("pre-window snapshot ratio == 0", snap.throughput_ratio == 0.0f);

        /* Now trigger window expiry by recording at +1.1s */
        tcp_metrics_record_recv(&m, 500, base_ns + 1100000000ULL);

        snap = tcp_metrics_get_snapshot(&m);

        /* After window expiry, throughput_ratio should be computed and > 0 */
        rc |= expect_true("post-window snapshot ratio > 0", snap.throughput_ratio > 0.0f);
        /* Jitter should also be computed (we had 3 inter-recv deltas) */
        rc |= expect_true("post-window snapshot jitter >= 0", snap.jitter_us >= 0.0f);
    }

    return rc ? 1 : 0;
}
