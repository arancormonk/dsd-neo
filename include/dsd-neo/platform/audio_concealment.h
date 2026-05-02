// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#pragma once

/**
 * @file
 * @brief Audio underrun concealment for the OpenSL ES player callback.
 *
 * When the audio ring buffer underruns (fewer frames available than needed),
 * this module replaces the hard silence gap with a fade-repeat of the last
 * successfully played buffer.  Each consecutive underrun attenuates the
 * repeated buffer by a fixed factor (default −6 dB ≈ 0.5 linear per repeat).
 * After a configurable maximum number of repeats the output fades to silence.
 *
 * All operations are lock-free and bounded-time so they are safe to call
 * from the high-priority OpenSL ES callback thread.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*============================================================================
 * Constants
 *============================================================================*/

/**
 * @brief Maximum consecutive underrun repeats before fading to silence.
 *
 * At 256 frames/buffer @ 8 kHz = 32 ms per buffer, 4 repeats ≈ 128 ms.
 */
#define AUDIO_CONCEAL_MAX_REPEATS      4

/**
 * @brief Linear attenuation factor applied per consecutive repeat.
 *
 * −6 dB ≈ 0.5 linear multiplier.  The k-th repeat is attenuated by
 * (AUDIO_CONCEAL_ATTEN_PER_REPEAT)^k relative to the original buffer.
 */
#define AUDIO_CONCEAL_ATTEN_PER_REPEAT 0.5f

/*============================================================================
 * Types
 *============================================================================*/

/**
 * @brief State for the audio underrun concealment system.
 *
 * Stored alongside the dsd_audio_stream; accessed only from the
 * OpenSL ES callback thread (no locking needed).
 */
struct audio_conceal_state {
    int16_t* last_good_buffer; /**< Copy of last successfully played buffer. */
    size_t buffer_frames;      /**< Frame count of last_good_buffer.         */
    int channels;              /**< Channel count (1 = mono, 2 = stereo).    */
    int repeat_count;          /**< Consecutive underrun repeats so far.     */
    int max_repeats;           /**< Configurable max (default: 4).           */
    float atten_per_repeat;    /**< Linear attenuation factor per repeat.    */
    uint64_t underrun_total;   /**< Cumulative underrun event count.         */
};

/*============================================================================
 * Public API
 *============================================================================*/

/**
 * @brief Initialise concealment state and allocate the last-good buffer.
 *
 * @param cs            Concealment state to initialise (must not be NULL).
 * @param buffer_frames Number of audio frames per playback buffer.
 * @param channels      Number of audio channels (1 or 2).
 * @return 0 on success, -1 on failure (NULL @p cs, zero @p buffer_frames,
 *         zero @p channels, or allocation failure).
 */
int audio_conceal_init(struct audio_conceal_state* cs, size_t buffer_frames, int channels);

/**
 * @brief Destroy concealment state and free the last-good buffer.
 *
 * Safe to call on an already-destroyed state or with a NULL pointer.
 *
 * @param cs  Concealment state to destroy (may be NULL).
 */
void audio_conceal_destroy(struct audio_conceal_state* cs);

/**
 * @brief Record a successfully played buffer.
 *
 * Copies up to @p buffer_frames frames from @p buf into the internal
 * last-good buffer and resets the underrun repeat counter to zero.
 *
 * @param cs     Concealment state.
 * @param buf    Buffer that was just played (interleaved int16 samples).
 * @param frames Number of frames in @p buf.
 */
void audio_conceal_on_good_buffer(struct audio_conceal_state* cs, const int16_t* buf, size_t frames);

/**
 * @brief Generate a concealment buffer for an underrun event.
 *
 * Behaviour depends on the current repeat count:
 * - repeat_count < max_repeats: writes an attenuated copy of the last-good
 *   buffer into @p out (attenuation = atten_per_repeat^(repeat_count+1)),
 *   then increments repeat_count and underrun_total.
 * - repeat_count >= max_repeats: zero-fills @p out and increments
 *   underrun_total (repeat_count is not incremented further).
 * - No last-good buffer available: zero-fills @p out.
 *
 * @param cs     Concealment state.
 * @param out    Output buffer to fill (interleaved int16 samples).
 * @param frames Number of frames requested.
 * @return Number of frames written into @p out.
 */
size_t audio_conceal_on_underrun(struct audio_conceal_state* cs, int16_t* out, size_t frames);

#ifdef __cplusplus
}
#endif
