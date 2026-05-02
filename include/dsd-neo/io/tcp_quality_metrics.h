// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#pragma once

/**
 * @file
 * @brief TCP connection quality metrics for rtl_tcp lag resilience.
 *
 * Collects throughput, jitter, and ring fill metrics from the TCP reader
 * thread.  Also implements a throughput watchdog that triggers proactive
 * reconnect when sustained throughput drops below 25% of expected for 3
 * seconds (with a 5-second grace period after connection establishment).
 *
 * All functions are designed to be called from a single writer thread
 * (the TCP reader).  The snapshot struct can be read from a different
 * thread (JNI poll) without locking — fields may be slightly stale but
 * never torn on aligned 32/64-bit platforms.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*============================================================================
 * Types
 *============================================================================*/

/**
 * @brief Snapshot of TCP connection quality metrics.
 *
 * Read by the JNI poll thread; written by the TCP reader thread.
 * All fields are plain types safe for single-writer / single-reader access.
 */
struct tcp_quality_snapshot {
    float throughput_ratio;        /**< bytes_received / expected_bytes over 1s window.  */
    float jitter_us;               /**< Variance of inter-recv times in microseconds.    */
    float input_ring_fill_pct;     /**< Input ring fill level as percentage (0.0–100.0). */
    uint64_t audio_underrun_count; /**< Cumulative audio underruns.                      */
    uint64_t producer_drops;       /**< Cumulative input ring producer drops.             */
    int watchdog_triggered;        /**< 1 if throughput watchdog fired this window.       */
};

/**
 * @brief Internal state for TCP quality metrics collection.
 *
 * Owned by the TCP reader thread; not shared directly.
 */
struct tcp_quality_metrics {
    /* 1-second sliding window for throughput */
    uint64_t window_bytes;    /**< Bytes accumulated in current 1s window.         */
    uint64_t window_start_ns; /**< Monotonic timestamp when current window opened. */
    uint32_t sample_rate;     /**< Configured sample rate for expected-bytes calc.  */

    /* Jitter tracking: variance of inter-recv intervals */
    uint64_t last_recv_ns; /**< Timestamp of previous recv() call.              */
    double jitter_sum;     /**< Sum of inter-recv deltas (for mean).            */
    double jitter_sum_sq;  /**< Sum of squared deltas (for variance).           */
    uint32_t jitter_count; /**< Number of inter-recv deltas recorded.           */

    /* Throughput watchdog: 3-second window */
    uint64_t watchdog_bytes;            /**< Bytes accumulated in current 3s watchdog window.*/
    uint64_t watchdog_start_ns;         /**< Monotonic timestamp when watchdog window opened.*/
    uint64_t connection_established_ns; /**< Timestamp of connection establishment.      */
    int watchdog_active;                /**< 1 if watchdog is armed (past grace period).     */

    /* Latest snapshot for JNI consumption */
    struct tcp_quality_snapshot snapshot; /**< Most recent computed metrics.              */
};

/*============================================================================
 * Public API
 *============================================================================*/

/**
 * @brief Initialise metrics state.
 *
 * Zeroes all fields, sets sample_rate, and records the current monotonic
 * time as connection_established_ns.  Call when a TCP connection is first
 * established.
 *
 * @param m           Metrics state to initialise (must not be NULL).
 * @param sample_rate Configured sample rate in Hz (e.g. 1536000).
 */
void tcp_metrics_init(struct tcp_quality_metrics* m, uint32_t sample_rate);

/**
 * @brief Reset metrics on reconnect.
 *
 * Re-initialises all fields as if a fresh connection was established.
 * Equivalent to calling tcp_metrics_init() again.
 *
 * @param m           Metrics state to reset (must not be NULL).
 * @param sample_rate Configured sample rate in Hz.
 */
void tcp_metrics_reset(struct tcp_quality_metrics* m, uint32_t sample_rate);

/**
 * @brief Record bytes received from a recv() call.
 *
 * Accumulates bytes in a 1-second window.  When the window expires:
 * - Computes throughput_ratio = window_bytes / (sample_rate × 2 × duration_s)
 * - Computes jitter as variance of inter-recv deltas: E[Δ²] − E[Δ]²
 * - Resets the window for the next period
 *
 * Also runs a 3-second throughput watchdog.  Returns 1 if the watchdog
 * triggers (ratio < 0.25 AND elapsed > 5s grace period).
 *
 * @param m              Metrics state.
 * @param bytes_received Number of bytes from this recv() call.
 * @param now_ns         Current monotonic timestamp in nanoseconds.
 * @return 1 if the throughput watchdog triggered, 0 otherwise.
 */
int tcp_metrics_record_recv(struct tcp_quality_metrics* m, uint32_t bytes_received, uint64_t now_ns);

/**
 * @brief Update the input ring fill percentage in the snapshot.
 *
 * @param m        Metrics state.
 * @param used     Number of elements currently in the ring.
 * @param capacity Total ring capacity in elements.
 */
void tcp_metrics_update_ring_fill(struct tcp_quality_metrics* m, size_t used, size_t capacity);

/**
 * @brief Get a copy of the latest metrics snapshot.
 *
 * Safe to call from a different thread (e.g. JNI poll thread).
 *
 * @param m  Metrics state (must not be NULL).
 * @return Copy of the latest snapshot.
 */
struct tcp_quality_snapshot tcp_metrics_get_snapshot(const struct tcp_quality_metrics* m);

#ifdef __cplusplus
}
#endif
