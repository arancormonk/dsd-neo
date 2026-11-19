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
/* The staged outputs not read yet and the FIR history go (issue #834): they came from input a retune's drop cut away.
   The next input sample primes the history again, as at a stream's start. */
void dsd_pcm_input_forget_staged(dsd_opts* opts);
/* The last sample read came after a retune's mark, and samples before it may still shape its staged outputs and the FIR
   history (issue #834, a confirmation taken after the decoder read past the mark): both start again from that sample
   alone, as at a stream's start, so no sample from before the mark reaches the new channel's outputs. */
void dsd_pcm_input_restart_staged(dsd_opts* opts);

#endif /* DSD_NEO_SRC_DSP_PCM_INPUT_STAGING_H_ */
