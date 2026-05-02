// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief TCP connection quality metrics implementation.
 *
 * Collects throughput ratio, jitter variance, and ring fill metrics from
 * the TCP reader thread.  Implements a 3-second throughput watchdog with
 * a 5-second grace period for proactive reconnect on throughput collapse.
 *
 * See tcp_quality_metrics.h for the full API contract.
 */

#include <dsd-neo/io/tcp_quality_metrics.h>
#include <dsd-neo/platform/timing.h>

/* 1-second window in nanoseconds */
static const uint64_t WINDOW_NS = 1000000000ULL;

/* 3-second watchdog window in nanoseconds */
static const uint64_t WATCHDOG_WINDOW_NS = 3000000000ULL;

/* 5-second grace period after connection establishment */
static const uint64_t GRACE_PERIOD_NS = 5000000000ULL;

/* Watchdog trigger threshold: ratio below this fires reconnect */
static const float WATCHDOG_THRESHOLD = 0.25f;

/*============================================================================
 * Public API
 *============================================================================*/

void
tcp_metrics_init(struct tcp_quality_metrics* m, uint32_t sample_rate) {
    if (!m) {
        return;
    }

    *m = (struct tcp_quality_metrics){0};
    m->sample_rate = sample_rate;

    uint64_t now = dsd_time_monotonic_ns();
    m->window_start_ns = now;
    m->watchdog_start_ns = now;
    m->connection_established_ns = now;
}

void
tcp_metrics_reset(struct tcp_quality_metrics* m, uint32_t sample_rate) {
    /* Reset is identical to init — fresh connection state. */
    tcp_metrics_init(m, sample_rate);
}

int
tcp_metrics_record_recv(struct tcp_quality_metrics* m, uint32_t bytes_received, uint64_t now_ns) {
    if (!m) {
        return 0;
    }

    /* Accumulate bytes in the 1-second throughput window */
    m->window_bytes += bytes_received;

    /* Accumulate bytes in the 3-second watchdog window */
    m->watchdog_bytes += bytes_received;

    /* Track jitter: compute inter-recv delta */
    if (m->last_recv_ns > 0 && now_ns > m->last_recv_ns) {
        double delta_us = (double)(now_ns - m->last_recv_ns) / 1000.0;
        m->jitter_sum += delta_us;
        m->jitter_sum_sq += delta_us * delta_us;
        m->jitter_count++;
    }
    m->last_recv_ns = now_ns;

    int watchdog_fired = 0;

    /* Check if the 1-second window has expired */
    uint64_t window_elapsed = now_ns - m->window_start_ns;
    if (window_elapsed >= WINDOW_NS) {
        double window_duration_s = (double)window_elapsed / 1e9;

        /* Compute throughput ratio */
        if (m->sample_rate > 0) {
            double expected_bytes = (double)m->sample_rate * 2.0 * window_duration_s;
            m->snapshot.throughput_ratio = (float)((double)m->window_bytes / expected_bytes);
        } else {
            /* Guard against division by zero: report 0 throughput */
            m->snapshot.throughput_ratio = 0.0f;
        }

        /* Compute jitter variance: Var(Δ) = E[Δ²] − E[Δ]² */
        if (m->jitter_count > 0) {
            double mean = m->jitter_sum / (double)m->jitter_count;
            double mean_sq = m->jitter_sum_sq / (double)m->jitter_count;
            double variance = mean_sq - (mean * mean);
            /* Clamp to zero in case of floating-point rounding */
            if (variance < 0.0) {
                variance = 0.0;
            }
            m->snapshot.jitter_us = (float)variance;
        } else {
            m->snapshot.jitter_us = 0.0f;
        }

        /* Reset 1-second window */
        m->window_bytes = 0;
        m->window_start_ns = now_ns;
        m->jitter_sum = 0.0;
        m->jitter_sum_sq = 0.0;
        m->jitter_count = 0;
    }

    /* Check 3-second watchdog window */
    uint64_t watchdog_elapsed = now_ns - m->watchdog_start_ns;
    if (watchdog_elapsed >= WATCHDOG_WINDOW_NS) {
        /* Only evaluate watchdog if past the 5-second grace period
         * and sample_rate is valid (non-zero). */
        uint64_t since_connect = now_ns - m->connection_established_ns;
        if (since_connect > GRACE_PERIOD_NS && m->sample_rate > 0) {
            double wd_duration_s = (double)watchdog_elapsed / 1e9;
            double expected = (double)m->sample_rate * 2.0 * wd_duration_s;
            float wd_ratio = (float)((double)m->watchdog_bytes / expected);
            if (wd_ratio < WATCHDOG_THRESHOLD) {
                watchdog_fired = 1;
                m->snapshot.watchdog_triggered = 1;
            } else {
                m->snapshot.watchdog_triggered = 0;
            }
        } else {
            m->snapshot.watchdog_triggered = 0;
        }

        /* Reset watchdog window */
        m->watchdog_bytes = 0;
        m->watchdog_start_ns = now_ns;
    }

    return watchdog_fired;
}

void
tcp_metrics_update_ring_fill(struct tcp_quality_metrics* m, size_t used, size_t capacity) {
    if (!m) {
        return;
    }

    if (capacity > 0) {
        m->snapshot.input_ring_fill_pct = (float)((double)used * 100.0 / (double)capacity);
    } else {
        /* Guard against division by zero */
        m->snapshot.input_ring_fill_pct = 0.0f;
    }
}

struct tcp_quality_snapshot
tcp_metrics_get_snapshot(const struct tcp_quality_metrics* m) {
    if (!m) {
        struct tcp_quality_snapshot empty = {0};
        return empty;
    }
    return m->snapshot;
}
