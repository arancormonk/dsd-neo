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
 * @brief Tap count @p kind uses at @p samples_per_symbol, designing it if needed.
 * @return Taps, or 0 when the filter cannot be designed for that rate.
 */
int dsd_sps_filter_taps_len(dsd_sps_filter_kind kind, int samples_per_symbol);

/**
 * @brief Group delay of @p kind at @p samples_per_symbol, in samples.
 *
 * The taps are symmetric and odd-length, so this is exactly `(taps - 1) / 2`:
 * the filter's output when sample n has just been fed describes the signal at
 * n - delay.
 *
 * @return Delay in samples, or 0 when the filter is inactive at that rate.
 */
int dsd_sps_filter_group_delay(dsd_sps_filter_kind kind, int samples_per_symbol);

/**
 * @brief Push @p count samples through @p kind's history, discarding the output.
 *
 * Used to hand a filter the samples that ran past while it was switched off, so
 * its first real output is computed from signal rather than from whatever the
 * previous transmission left behind.
 */
void dsd_sps_filter_prime(dsd_sps_filter_kind kind, const float* samples, int count, int samples_per_symbol);

/**
 * @brief Record one raw, unfiltered sample the symbol grid has consumed.
 *
 * Kept so a filter switching on mid-stream can be primed with the history it
 * missed. Cheap enough to call per sample.
 */
void dsd_sps_filter_note_raw(float sample);

/** @brief Drop the recorded raw history, e.g. across a stream discontinuity. */
void dsd_sps_filter_forget_raw(void);

/**
 * @brief Prime @p kind from the recorded raw history and report its delay.
 *
 * The caller must then push @p delay further samples through the filter and
 * discard the outputs: those describe positions the grid has already read.
 *
 * @return The filter's group delay in samples, or 0 when it is inactive.
 */
int dsd_sps_filter_prime_from_history(dsd_sps_filter_kind kind, int samples_per_symbol);

/**
 * @brief Copy @p kind's designed taps at @p samples_per_symbol into @p out.
 * @return Taps written, or 0 when they do not fit or the filter is inactive.
 */
int dsd_sps_filter_copy_taps(dsd_sps_filter_kind kind, int samples_per_symbol, float* out, int cap);

#ifdef __cplusplus
}
#endif
#endif /* DSD_NEO_INCLUDE_DSD_NEO_DSP_SPS_FILTERS_H_H */
