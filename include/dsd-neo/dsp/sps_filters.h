// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Samples-per-symbol FIR helpers for per-protocol shaping.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_DSP_SPS_FILTERS_H_H
#define DSD_NEO_INCLUDE_DSD_NEO_DSP_SPS_FILTERS_H_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Which per-protocol matched filter a sample is being shaped by.
 *
 * `DSD_SPS_FILTER_NONE` is the unfiltered stream the sync search runs on before
 * a protocol is known, and is what makes the switch to a filter a discontinuity
 * the symbol grid has to be told about.
 */
typedef enum {
    DSD_SPS_FILTER_NONE = 0,
    DSD_SPS_FILTER_DMR,
    DSD_SPS_FILTER_P25,
    DSD_SPS_FILTER_NXDN,
    DSD_SPS_FILTER_DPMR,
} dsd_sps_filter_kind;

/** Largest history any of these filters keeps, and so the most a prime can use. */
#define DSD_SPS_FILTER_MAX_TAPS 1024

float dmr_filter(float sample, int samples_per_symbol);
float nxdn_filter(float sample, int samples_per_symbol);
float dpmr_filter(float sample, int samples_per_symbol);
float p25_filter(float sample, int samples_per_symbol);
void init_rrc_filter_memory(void);

/**
 * @brief Filter one sample through @p kind, or return it unchanged for NONE.
 */
float dsd_sps_filter_apply(dsd_sps_filter_kind kind, float sample, int samples_per_symbol);

/**
 * @brief Group delay of @p kind at @p samples_per_symbol, in samples.
 *
 * The taps are symmetric and odd in length, so this is exactly `(taps - 1) / 2`:
 * the filter's output when sample n has just been fed describes the signal at
 * n - delay. A query only: it neither designs nor disturbs a running filter, so
 * the symbolizer can ask about the filter it is switching to while the one it
 * is leaving still runs.
 *
 * @return Delay in samples, or 0 when the filter is inactive at that rate.
 */
int dsd_sps_filter_group_delay(dsd_sps_filter_kind kind, int samples_per_symbol);

/**
 * @brief Push one sample into @p kind's history without computing an output.
 *
 * How a filter switching on mid-stream is handed the samples that ran past
 * while it was off, oldest first, so its first real output is computed from
 * signal rather than from whatever the previous transmission left behind.
 * Designs the filter for @p samples_per_symbol if it is not already; a no-op
 * for NONE.
 */
void dsd_sps_filter_prime(dsd_sps_filter_kind kind, float sample, int samples_per_symbol);

#ifdef __cplusplus
}
#endif
#endif /* DSD_NEO_INCLUDE_DSD_NEO_DSP_SPS_FILTERS_H_H */
