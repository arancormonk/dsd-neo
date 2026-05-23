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

float dmr_filter(float sample, int samples_per_symbol);
float nxdn_filter(float sample, int samples_per_symbol);
float dpmr_filter(float sample, int samples_per_symbol);
float m17_filter(float sample, int samples_per_symbol);
float p25_filter(float sample, int samples_per_symbol);
void init_rrc_filter_memory(void);

#ifdef __cplusplus
}
#endif
#endif /* DSD_NEO_INCLUDE_DSD_NEO_DSP_SPS_FILTERS_H_H */
