// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_IO_TCP_QUALITY_METRICS_H_
#define DSD_NEO_INCLUDE_DSD_NEO_IO_TCP_QUALITY_METRICS_H_

/**
 * @file
 * @brief Throughput watchdog for rtl_tcp reconnect decisions.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct tcp_quality_metrics {
    uint32_t sample_rate;
    uint64_t watchdog_bytes;
    uint64_t watchdog_start_ns;
    uint64_t connection_established_ns;
    int watchdog_trigger_latched;
};

/** Initialize a watchdog window for a newly established connection. */
void tcp_metrics_init(struct tcp_quality_metrics* metrics, uint32_t sample_rate);

/** Restart the grace/window timing while preserving a latched reconnect event. */
void tcp_metrics_reset(struct tcp_quality_metrics* metrics, uint32_t sample_rate);

/**
 * Record received bytes and evaluate the current watchdog window.
 *
 * @return 1 when sustained throughput is below the reconnect threshold, otherwise 0.
 */
int tcp_metrics_record_recv(struct tcp_quality_metrics* metrics, uint32_t bytes_received, uint64_t now_ns);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_IO_TCP_QUALITY_METRICS_H_ */
