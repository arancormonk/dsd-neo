// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Unit tests for tcp_metrics_init() and tcp_metrics_reset().
 *
 * Verifies watchdog initialization and reset behavior.
 */

#include <dsd-neo/io/tcp_quality_metrics.h>
#include <stdint.h>
#include <stdio.h>
#include "dsd-neo/core/safe_api.h"

static int
expect_int(const char* label, int got, int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "FAIL: %s: got=%d want=%d\n", label, got, want);
        return 1;
    }
    return 0;
}

static int
expect_u32(const char* label, uint32_t got, uint32_t want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "FAIL: %s: got=%u want=%u\n", label, got, want);
        return 1;
    }
    return 0;
}

static int
expect_u64(const char* label, uint64_t got, uint64_t want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "FAIL: %s: got=%llu want=%llu\n", label, (unsigned long long)got, (unsigned long long)want);
        return 1;
    }
    return 0;
}

static int
expect_nonzero_u64(const char* label, uint64_t val) {
    if (val == 0) {
        DSD_FPRINTF(stderr, "FAIL: %s: expected non-zero, got 0\n", label);
        return 1;
    }
    return 0;
}

int
main(void) {
    int rc = 0;

    /* --- init zeroes watchdog counters and sets sample rate --- */
    {
        struct tcp_quality_metrics m;
        /* Fill with garbage first to ensure init clears everything */
        DSD_MEMSET(&m, 0xAB, sizeof(m));

        tcp_metrics_init(&m, 1536000);

        rc |= expect_u32("init sample_rate", m.sample_rate, 1536000);
        rc |= expect_u64("init watchdog_bytes", m.watchdog_bytes, 0);
        rc |= expect_int("init watchdog_trigger_latched", m.watchdog_trigger_latched, 0);

        /* Timestamps should be set to current time (non-zero) */
        rc |= expect_nonzero_u64("init watchdog_start_ns", m.watchdog_start_ns);
        rc |= expect_nonzero_u64("init connection_established_ns", m.connection_established_ns);
    }

    /* --- reset re-initialises with same sample_rate --- */
    {
        struct tcp_quality_metrics m;
        tcp_metrics_init(&m, 48000);

        /* Dirty some fields to simulate usage */
        m.watchdog_bytes = 12345;
        m.watchdog_trigger_latched = 1;

        tcp_metrics_reset(&m, 48000);

        rc |= expect_u32("reset sample_rate", m.sample_rate, 48000);
        rc |= expect_u64("reset watchdog_bytes", m.watchdog_bytes, 0);
        rc |= expect_int("reset preserves watchdog event", m.watchdog_trigger_latched, 1);
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
    rc |= expect_int("record recv NULL is no watchdog event", tcp_metrics_record_recv(NULL, 1000, 123456789ULL), 0);

    return rc ? 1 : 0;
}
