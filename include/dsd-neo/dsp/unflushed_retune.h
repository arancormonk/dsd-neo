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
 * @brief The input was replaced (a switch, a reopened or reconnected stream): an unflushed retune posted off the
 * decoder thread and not yet taken is retired, unrun, unless it was measured on the queue attached now
 * (dsd_input_backlog_forget()). One posted before the new stream existed moved the peer before it began, so the new
 * stream holds nothing of the channel it left; one marked once the new stream was opening (dsd_input_backlog_opening())
 * is that stream's drop (measured on its connection, it stands on the reader that attached), and one posted from here
 * on is taken at the next read as usual (dsd_frame_sync_note_off_thread_unflushed_retune()). Decoder thread.
 */
void dsd_frame_sync_forget_unflushed_retunes(void);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_DSP_UNFLUSHED_RETUNE_H_ */
