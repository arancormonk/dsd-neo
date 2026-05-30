// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_SRC_DSP_FRAME_SYNC_LEVEL_H_
#define DSD_NEO_SRC_DSP_FRAME_SYNC_LEVEL_H_

#ifdef __cplusplus
extern "C" {
#endif

void dsd_frame_sync_estimate_sorted_window_levels(const float* sorted_levels, int count, float* out_min,
                                                  float* out_max);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_SRC_DSP_FRAME_SYNC_LEVEL_H_ */
