// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Where an I/Q replay resumes after a run-time re-plan of its rate chain (issue #701).
 *
 * A decode-mode or DSP-bandwidth change during a replay reopens the replay at the rate the new settings need. The
 * stopped replay's reader leaves this checkpoint: the position of the last capture chunk it handed the demodulator,
 * with the reader state that goes with it. The reopened replay starts there. What was still in the input ring or the
 * decoder's symbol cache (at most one chunk plus that cache, a few ms) is not replayed again: the resume is
 * approximate. The type lives in core so dsd_opts can carry a pending resume to the stream start.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_CORE_IQ_REPLAY_CHECKPOINT_H
#define DSD_NEO_INCLUDE_DSD_NEO_CORE_IQ_REPLAY_CHECKPOINT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** The reader position an I/Q replay resumes from. */
typedef struct dsd_iq_replay_checkpoint {
    uint64_t data_offset;      /**< capture bytes read on this pass (a WAV's virtual cf32 bytes), whole samples */
    uint32_t event_index;      /**< the first capture event not applied yet: events at data_offset included */
    uint64_t timeline_samples; /**< capture samples on the media timeline so far: MUTEs and earlier passes included */
    uint64_t pass_samples;     /**< capture samples on this pass so far, MUTEs included: the realtime schedule */
    uint32_t loop_pass;        /**< passes completed under --iq-loop */
    int32_t fs4_phase;         /**< the fs/4 rotation phase (0..3) of the sample at data_offset */
} dsd_iq_replay_checkpoint;

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_CORE_IQ_REPLAY_CHECKPOINT_H */
