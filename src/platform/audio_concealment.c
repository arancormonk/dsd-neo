// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Audio underrun concealment implementation.
 *
 * Provides fade-repeat concealment for audio underruns on the OpenSL ES
 * callback thread.  All functions are lock-free and bounded-time.
 *
 * See audio_concealment.h for the full API contract.
 */

#include <dsd-neo/platform/audio_concealment.h>

#include <stdlib.h>
#include <string.h>

/*============================================================================
 * Public API
 *============================================================================*/

int
audio_conceal_init(struct audio_conceal_state* cs, size_t buffer_frames, int channels) {
    if (!cs || buffer_frames == 0 || channels == 0) {
        return -1;
    }

    size_t alloc_samples = buffer_frames * (size_t)channels;
    int16_t* buf = (int16_t*)calloc(alloc_samples, sizeof(int16_t));
    if (!buf) {
        return -1;
    }

    cs->last_good_buffer = buf;
    cs->buffer_frames = buffer_frames;
    cs->channels = channels;
    cs->repeat_count = 0;
    cs->max_repeats = AUDIO_CONCEAL_MAX_REPEATS;
    cs->atten_per_repeat = AUDIO_CONCEAL_ATTEN_PER_REPEAT;
    cs->underrun_total = 0;

    return 0;
}

void
audio_conceal_destroy(struct audio_conceal_state* cs) {
    if (!cs) {
        return;
    }

    free(cs->last_good_buffer);
    cs->last_good_buffer = NULL;
}

void
audio_conceal_on_good_buffer(struct audio_conceal_state* cs, const int16_t* buf, size_t frames) {
    if (!cs || !buf || frames == 0 || !cs->last_good_buffer) {
        return;
    }

    /* Copy at most buffer_frames frames into the last-good buffer. */
    size_t copy_frames = frames < cs->buffer_frames ? frames : cs->buffer_frames;
    size_t copy_samples = copy_frames * (size_t)cs->channels;

    memcpy(cs->last_good_buffer, buf, copy_samples * sizeof(int16_t));

    cs->repeat_count = 0;
}

size_t
audio_conceal_on_underrun(struct audio_conceal_state* cs, int16_t* out, size_t frames) {
    if (!cs || !out || frames == 0) {
        return 0;
    }

    size_t write_frames = frames < cs->buffer_frames ? frames : cs->buffer_frames;
    size_t write_samples = write_frames * (size_t)cs->channels;

    /* No last-good buffer available — zero-fill. */
    if (!cs->last_good_buffer) {
        memset(out, 0, write_samples * sizeof(int16_t));
        cs->underrun_total++;
        return write_frames;
    }

    if (cs->repeat_count < cs->max_repeats) {
        /*
         * Compute the attenuation factor: atten_per_repeat^(repeat_count+1).
         * This is a small bounded loop (max_repeats iterations at most) so
         * it stays O(1) and is safe for the audio callback thread.
         */
        float gain = 1.0f;
        for (int i = 0; i <= cs->repeat_count; i++) {
            gain *= cs->atten_per_repeat;
        }

        /* Write attenuated copy of the last-good buffer. */
        for (size_t i = 0; i < write_samples; i++) {
            float sample = (float)cs->last_good_buffer[i] * gain;

            /* Clamp to int16 range to avoid overflow. */
            if (sample > 32767.0f) {
                sample = 32767.0f;
            } else if (sample < -32768.0f) {
                sample = -32768.0f;
            }

            out[i] = (int16_t)sample;
        }

        cs->repeat_count++;
    } else {
        /* Exceeded max repeats — fade to silence. */
        memset(out, 0, write_samples * sizeof(int16_t));
    }

    cs->underrun_total++;

    return write_frames;
}
