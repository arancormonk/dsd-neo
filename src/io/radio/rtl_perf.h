// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_IO_RADIO_RTL_PERF_H
#define DSD_NEO_IO_RADIO_RTL_PERF_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int rtl_perf_enabled(void);
uint64_t rtl_perf_now_ns(void);

void rtl_perf_record_ingest(uint64_t elapsed_ns, size_t input_samples, uint64_t dropped_samples);
void rtl_perf_record_demod_block(uint64_t full_demod_ns, uint64_t post_metrics_ns, uint64_t output_write_ns,
                                 size_t input_samples, size_t output_samples);
void rtl_perf_record_consumer_read(uint64_t elapsed_ns, size_t output_samples);

typedef struct {
    const char* source;
    uint32_t sample_rate_hz;
    int output_kind;
    size_t input_used;
    size_t input_capacity;
    uint64_t input_drops;
    size_t output_used;
    size_t output_capacity;
    int symbol_cache_pending;
    double snr_db;
    double cfo_hz;
    int carrier_lock;
} rtl_perf_log_snapshot;

void rtl_perf_maybe_log(const rtl_perf_log_snapshot* snapshot);
void rtl_perf_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_IO_RADIO_RTL_PERF_H */
