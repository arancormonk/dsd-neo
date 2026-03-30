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
#pragma once

#include <dsd-neo/core/opts_fwd.h>

int dsd_pcm_input_uses_staged_resampler(const dsd_opts* opts);
void dsd_pcm_input_stage_resample(dsd_opts* opts, float invalue);
int dsd_pcm_input_begin_resampler_tail(dsd_opts* opts);
int dsd_pcm_input_stage_resampler_tail(dsd_opts* opts);
