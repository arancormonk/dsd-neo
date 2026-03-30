// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Polyphase rational resampler public API.
 */

#ifndef DSP_RESAMPLER_H
#define DSP_RESAMPLER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declaration of demod_state structure */
struct demod_state;

/**
 * @brief Reusable polyphase rational resampler state.
 *
 * This state is shared by both the RTL demodulator path and lower-rate PCM
 * ingest paths that need FIR-based rate conversion before symbol timing
 * consumes the samples.
 */
typedef struct dsd_resampler_state {
    int enabled;
    int target_hz;
    int L;
    int M;
    int phase;
    int taps_len;
    int taps_per_phase;
    int hist_head;
    float* taps;
    float* hist;
    uint64_t internal_cookie; /* internal lifetime tag; callers must not modify */
} dsd_resampler_state;

/**
 * @brief Free any allocated taps/history and reset the resampler state.
 *
 * Safe to call on first-use storage before the state has been designed.
 *
 * @param state Resampler state to clear.
 */
void dsd_resampler_reset(dsd_resampler_state* state);

/**
 * @brief Zero the history/phase of an already-designed resampler.
 *
 * @param state Resampler state to rewind.
 */
void dsd_resampler_clear_history(dsd_resampler_state* state);

/**
 * @brief Design a windowed-sinc prototype for a reusable resampler state.
 *
 * Safe on first-use storage. If allocation fails while redesigning an existing
 * state, the prior taps/history remain intact.
 *
 * @param state Resampler state to configure.
 * @param L Upsampling factor.
 * @param M Downsampling factor.
 * @return 1 on success, 0 on allocation or argument failure.
 */
int dsd_resampler_design(dsd_resampler_state* state, int L, int M);

/**
 * @brief Process a block of samples through a reusable resampler state.
 *
 * @param state Resampler state containing taps/history.
 * @param in Input sample block.
 * @param in_len Number of input samples.
 * @param out Output sample buffer.
 * @param out_cap Capacity of @p out in samples.
 * @return Number of output samples written, or -1 on invalid arguments/capacity.
 *
 * On capacity failure the resampler state is left unchanged so callers may retry
 * with a larger output buffer.
 */
int dsd_resampler_process_block(dsd_resampler_state* state, const float* in, int in_len, float* out, int out_cap);

/**
 * @brief Design windowed-sinc low-pass prototype for polyphase upfirdn (runs at L*Fs_in).
 *
 * Taps are stored as contiguous per-phase blocks with oldest-to-newest sample
 * order inside each block. The function allocates aligned storage for taps and
 * mirrored history inside the provided demod_state and initializes the
 * resampler bookkeeping fields.
 *
 * @param s Demodulator state to receive resampler taps/history.
 * @param L Upsampling factor.
 * @param M Downsampling factor.
 */
void resamp_design(struct demod_state* s, int L, int M);

/**
 * @brief Process one block using polyphase upfirdn with history.
 *
 * @param s      Demodulator state containing resampler state.
 * @param in     Pointer to input samples.
 * @param in_len Number of input samples.
 * @param out    Pointer to output buffer (sized to hold produced samples).
 * @return Number of output samples written.
 */
int resamp_process_block(struct demod_state* s, const float* in, int in_len, float* out);

#ifdef __cplusplus
}
#endif

#endif /* DSP_RESAMPLER_H */
