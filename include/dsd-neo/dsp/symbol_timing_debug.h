// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Runtime observability for the decoder's own symbol grid.
 *
 * The decoder's sampling phase is fixed when a sync is acquired and held for the
 * whole call, but nothing printed it: the sub-symbol offset the grid settled on
 * was only ever measurable with a throwaway build (issue #404). This module
 * measures that offset from the samples the grid actually consumed and reports
 * it once per accepted frame sync.
 *
 * Everything here is gated on DSD_NEO_DEBUG_SYMBOL_TIMING: with it unset nothing
 * is recorded, measured, or printed, and the decode path is the one it would have
 * taken anyway.
 */

#ifndef DSD_NEO_DSP_SYMBOL_TIMING_DEBUG_H
#define DSD_NEO_DSP_SYMBOL_TIMING_DEBUG_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct dsd_state;

/**
 * Symbols of received sync the offset is measured over.
 *
 * Every protocol's sync word is at least this long (M17's eight is the
 * shortest), so the trailing eight decided dibits at an accept are always inside
 * the sync word whatever matched. That keeps the measurement free of a
 * per-protocol window-length table, which would silently rot as matchers change.
 */
#define DSD_SYMBOL_TIMING_TEMPLATE_SYMS 8

/** Largest samplesPerSymbol the decoder produces (dsd_opts_compute_sps_rate clamps to 64). */
#define DSD_SYMBOL_TIMING_MAX_SPAN      64

/** Samples a measurement reads: the template, plus a symbol of room to shift it over. */
#define DSD_SYMBOL_TIMING_SPAN_SAMPLES  (DSD_SYMBOL_TIMING_MAX_SPAN * (DSD_SYMBOL_TIMING_TEMPLATE_SYMS + 1))

/**
 * @brief Current DSD_NEO_DEBUG_SYMBOL_TIMING level.
 * @return 0 off, 1 one line per accepted sync, 2 additionally the per-sample trace.
 */
int dsd_symbol_timing_debug_level(void);

/**
 * @brief Record one post-filter sample the symbol grid consumed.
 *
 * The buffers belong to decoder-state setup and teardown, as the symbol history's
 * do; this is a no-op until they exist. Safe to call with a NULL state.
 */
void dsd_symbol_timing_trace_push_sample(struct dsd_state* state, float sample);

/**
 * @brief Record how many samples the symbol just completed consumed.
 * @param samples_consumed Sample count for that symbol; ignored when out of range.
 */
void dsd_symbol_timing_trace_push_span(struct dsd_state* state, int samples_consumed);

/**
 * @brief Drop the trace contents without releasing the buffers.
 *
 * Called wherever the sample stream is discontinuous (a retune, a cache flush, a
 * symbol-rate change), so a measurement never correlates across the seam.
 */
void dsd_symbol_timing_trace_reset(struct dsd_state* state);

/**
 * @brief Measure which sub-symbol offset the samples best support.
 *
 * Correlates whole-symbol integrals of @p samples against the levels @p window
 * says were received, once per candidate offset, and returns the offset scoring
 * highest. Whole-symbol rather than the slicer's narrow window on purpose: a
 * narrow window scores flat across the middle of a symbol and localises nothing.
 *
 * The last sum(spans) entries of @p samples are the template's symbols; anything
 * before them is room to shift the template earlier.
 *
 * @param samples       Samples in the order the grid consumed them.
 * @param sample_count  Entries in @p samples.
 * @param spans         Samples each template symbol consumed, oldest first.
 * @param span_count    Entries in @p spans, matching @p window's length.
 * @param window        Decided dibits ('0'..'3') for those symbols, oldest first.
 * @param out_offset    Receives the winning offset, in samples before the grid's boundary.
 * @return 1 when a measurement was made, 0 when the input cannot support one.
 */
int dsd_symbol_timing_measure_sync_offset(const float* samples, int sample_count, const uint8_t* spans, int span_count,
                                          const char* window, int* out_offset);

/**
 * @brief Print one measurement line for an accepted frame sync.
 *
 * @param state    Decoder state carrying the trace and the grid's timing.
 * @param synctype Accepted sync type, reported numerically.
 * @param window   Trailing decided dibits; at least DSD_SYMBOL_TIMING_TEMPLATE_SYMS of them.
 */
void dsd_symbol_timing_report_sync(const struct dsd_state* state, int synctype, const char* window);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_DSP_SYMBOL_TIMING_DEBUG_H */
