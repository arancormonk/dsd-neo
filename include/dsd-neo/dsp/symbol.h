// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Symbol/sample acquisition helpers.
 *
 * Declares symbol acquisition entrypoints implemented in `src/dsp/dsd_symbol.c`.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_DSP_SYMBOL_H_
#define DSD_NEO_INCLUDE_DSD_NEO_DSP_SYMBOL_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#ifdef __cplusplus
extern "C" {
#endif

float getSymbol(dsd_opts* opts, dsd_state* state, int have_sync);

/**
 * @brief Forget which matched filter the symbol grid was reading through.
 *
 * Pairs with init_rrc_filter_memory(): a cleared filter that the grid still
 * believes it has primed would run from an empty history at the next sample,
 * which is the transient the seam exists to prevent (issue #444).
 */
void dsd_symbol_matched_filter_reset(dsd_state* state);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_DSP_SYMBOL_H_ */
