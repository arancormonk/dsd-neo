// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Internal helpers for staged low-rate PCM input upsampling.
 *
 * This header is internal to src/dsp/ and should NOT be installed.
 */
#ifndef DSD_NEO_SRC_DSP_PCM_INPUT_STAGING_H_
#define DSD_NEO_SRC_DSP_PCM_INPUT_STAGING_H_

#include <dsd-neo/core/opts_fwd.h>

int dsd_pcm_input_uses_staged_resampler(const dsd_opts* opts);
void dsd_pcm_input_stage_resample(dsd_opts* opts, float invalue);
int dsd_pcm_input_begin_resampler_tail(dsd_opts* opts);
int dsd_pcm_input_stage_resampler_tail(dsd_opts* opts);

#endif /* DSD_NEO_SRC_DSP_PCM_INPUT_STAGING_H_ */
