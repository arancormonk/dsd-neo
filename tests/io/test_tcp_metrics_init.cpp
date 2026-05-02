// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Unit tests for tcp_metrics_init() and tcp_metrics_reset().
 *
 * Verifies that init zeroes all fields and sets sample_rate correctly,
 * and that reset re-initialises while preserving sample_rate.
 */

#include <dsd-neo/io/tcp_quality_metrics.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int
expect_int(const char* label, int got, int want) {
    if (got != want) {
        fprintf(stderr, "FAIL: %s: got=%d want=%d\n", label, got, want);
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
expect_u64(const char* label, uint64_t got, uint64_t want) {
    if (got != want) {
        fprintf(stderr, "FAIL: %s: got=%llu want=%llu\n", label, (unsigned long long)got, (unsigned long long)want);
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

static int
expect_nonzero_u64(const char* label, uint64_t val) {
    if (val == 0) {
        fprintf(stderr, "FAIL: %s: expected non-zero, got 0\n", label);
        return 1;
    }
    return 0;
}

int
main(void) {
    int rc = 0;

    /* --- init zeroes all fields and sets sample_rate --- */
    {
        struct tcp_quality_metrics m;
        /* Fill with garbage first to ensure init clears everything */
        memset(&m, 0xAB, sizeof(m));

        tcp_metrics_init(&m, 1536000);

        rc |= expect_u32("init sample_rate", m.sample_rate, 1536000);
        rc |= expect_u64("init window_bytes", m.window_bytes, 0);
        rc |= expect_u64("init last_recv_ns", m.last_recv_ns, 0);
        rc |= expect_u32("init jitter_count", m.jitter_count, 0);
        rc |= expect_u64("init watchdog_bytes", m.watchdog_bytes, 0);
        rc |= expect_int("init watchdog_active", m.watchdog_active, 0);
        rc |= expect_float("init snapshot.throughput_ratio", m.snapshot.throughput_ratio, 0.0f);
        rc |= expect_float("init snapshot.jitter_us", m.snapshot.jitter_us, 0.0f);
        rc |= expect_float("init snapshot.input_ring_fill_pct", m.snapshot.input_ring_fill_pct, 0.0f);
        rc |= expect_u64("init snapshot.audio_underrun_count", m.snapshot.audio_underrun_count, 0);
        rc |= expect_u64("init snapshot.producer_drops", m.snapshot.producer_drops, 0);
        rc |= expect_int("init snapshot.watchdog_triggered", m.snapshot.watchdog_triggered, 0);

        /* Timestamps should be set to current time (non-zero) */
        rc |= expect_nonzero_u64("init window_start_ns", m.window_start_ns);
        rc |= expect_nonzero_u64("init watchdog_start_ns", m.watchdog_start_ns);
        rc |= expect_nonzero_u64("init connection_established_ns", m.connection_established_ns);
    }

    /* --- reset re-initialises with same sample_rate --- */
    {
        struct tcp_quality_metrics m;
        tcp_metrics_init(&m, 48000);

        /* Dirty some fields to simulate usage */
        m.window_bytes = 99999;
        m.jitter_count = 42;
        m.watchdog_bytes = 12345;
        m.snapshot.throughput_ratio = 0.75f;

        tcp_metrics_reset(&m, 48000);

        rc |= expect_u32("reset sample_rate", m.sample_rate, 48000);
        rc |= expect_u64("reset window_bytes", m.window_bytes, 0);
        rc |= expect_u32("reset jitter_count", m.jitter_count, 0);
        rc |= expect_u64("reset watchdog_bytes", m.watchdog_bytes, 0);
        rc |= expect_float("reset snapshot.throughput_ratio", m.snapshot.throughput_ratio, 0.0f);
    }

    /* --- reset with different sample_rate --- */
    {
        struct tcp_quality_metrics m;
        tcp_metrics_init(&m, 48000);
        tcp_metrics_reset(&m, 1536000);

        rc |= expect_u32("reset new sample_rate", m.sample_rate, 1536000);
    }

    /* --- init with NULL is safe (no crash) --- */
    tcp_metrics_init(NULL, 48000);
    tcp_metrics_reset(NULL, 48000);

    return rc ? 1 : 0;
}
