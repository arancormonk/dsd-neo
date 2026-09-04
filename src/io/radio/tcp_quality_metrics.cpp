// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/io/tcp_quality_metrics.h>
#include <dsd-neo/platform/timing.h>

static const uint64_t WATCHDOG_WINDOW_NS = 3000000000ULL;
static const uint64_t GRACE_PERIOD_NS = 5000000000ULL;
static const float WATCHDOG_THRESHOLD = 0.25f;

void
tcp_metrics_init(struct tcp_quality_metrics* metrics, uint32_t sample_rate) {
    if (!metrics) {
        return;
    }
    *metrics = {};
    metrics->sample_rate = sample_rate;
    uint64_t now = dsd_time_monotonic_ns();
    metrics->watchdog_start_ns = now;
    metrics->connection_established_ns = now;
}

void
tcp_metrics_reset(struct tcp_quality_metrics* metrics, uint32_t sample_rate) {
    int watchdog_latched = (metrics && metrics->watchdog_trigger_latched) ? 1 : 0;
    tcp_metrics_init(metrics, sample_rate);
    if (metrics) {
        metrics->watchdog_trigger_latched = watchdog_latched;
    }
}

int
tcp_metrics_record_recv(struct tcp_quality_metrics* metrics, uint32_t bytes_received, uint64_t now_ns) {
    if (!metrics) {
        return 0;
    }

    metrics->watchdog_bytes += bytes_received;
    uint64_t watchdog_elapsed = now_ns - metrics->watchdog_start_ns;
    if (watchdog_elapsed < WATCHDOG_WINDOW_NS) {
        return 0;
    }

    int watchdog_fired = 0;
    uint64_t since_connect = now_ns - metrics->connection_established_ns;
    if (since_connect > GRACE_PERIOD_NS && metrics->sample_rate > 0U) {
        double duration_seconds = (double)watchdog_elapsed / 1e9;
        double expected_bytes = (double)metrics->sample_rate * 2.0 * duration_seconds;
        float throughput_ratio = (float)((double)metrics->watchdog_bytes / expected_bytes);
        watchdog_fired = throughput_ratio < WATCHDOG_THRESHOLD ? 1 : 0;
        metrics->watchdog_trigger_latched = watchdog_fired;
    }
    metrics->watchdog_bytes = 0U;
    metrics->watchdog_start_ns = now_ns;
    return watchdog_fired;
}
