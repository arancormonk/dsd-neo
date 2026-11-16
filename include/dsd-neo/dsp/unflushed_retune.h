// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The input-replacement side of the unflushed-retune count (issue #834, src/dsp/frame_sync_unflushed.c).
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_DSP_UNFLUSHED_RETUNE_H_
#define DSD_NEO_INCLUDE_DSD_NEO_DSP_UNFLUSHED_RETUNE_H_

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief The input was replaced (a switch, a reopened or reconnected stream): every unflushed retune posted off the
 * decoder thread so far is taken as seen, unrun. Such a retune moved the peer before the new stream began, so the new
 * stream holds nothing of the channel it left; a retune posted from here on is taken at the next read as usual
 * (dsd_frame_sync_note_off_thread_unflushed_retune()). Decoder thread.
 */
void dsd_frame_sync_forget_unflushed_retunes(void);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_DSP_UNFLUSHED_RETUNE_H_ */
