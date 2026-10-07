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

/** The taps per phase dsd_resampler_design() uses. */
#define DSD_RESAMPLER_DEFAULT_TAPS_PER_PHASE 16
/** The range of taps per phase dsd_resampler_design_taps() accepts. */
#define DSD_RESAMPLER_MIN_TAPS_PER_PHASE     8
#define DSD_RESAMPLER_MAX_TAPS_PER_PHASE     256

/**
 * @brief Whether a resampler of ratio @p L / @p M with @p taps_per_phase taps per phase can be designed at all.
 *
 * The prototype's taps_per_phase x L taps must fit an int and their byte size a size_t, and the phase recurrence (a
 * phase below L plus M) must fit an int. Whether the memory is there is only known when a design allocates it.
 *
 * @return 1 when the ratio can be designed, 0 otherwise (also for any term below 1 or taps per phase out of range).
 */
int dsd_resampler_ratio_designable(int L, int M, int taps_per_phase);

/**
 * @brief Design a windowed-sinc prototype for a reusable resampler state.
 *
 * Safe on first-use storage. If allocation fails while redesigning an existing
 * state, the prior taps/history remain intact. A ratio that cannot be designed
 * (dsd_resampler_ratio_designable()) is an argument failure, which resets the
 * state to an undesigned pass-through.
 *
 * @param state Resampler state to configure.
 * @param L Upsampling factor.
 * @param M Downsampling factor.
 * @return 1 on success, 0 on allocation or argument failure.
 */
int dsd_resampler_design(dsd_resampler_state* state, int L, int M);

/**
 * @brief dsd_resampler_design() with @p taps_per_phase taps per phase instead of
 * DSD_RESAMPLER_DEFAULT_TAPS_PER_PHASE.
 *
 * The filter spans taps_per_phase input samples, so a decimating ratio needs more of them for the same transition band
 * relative to the output rate (issue #633).
 *
 * @param taps_per_phase DSD_RESAMPLER_MIN_TAPS_PER_PHASE..DSD_RESAMPLER_MAX_TAPS_PER_PHASE.
 * @return 1 on success, 0 on allocation or argument failure.
 */
int dsd_resampler_design_taps(dsd_resampler_state* state, int L, int M, int taps_per_phase);

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
 * @return 1 when @p s holds the designed taps, 0 when the design failed (an allocation, or a ratio that cannot be
 *         designed): the caller must not publish the resampled rate then, since the samples pass through.
 */
int resamp_design(struct demod_state* s, int L, int M);

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

/**
 * @brief resamp_process_block() with the auto squelch's per-sample flags (issue #518 follow-up).
 *
 * Each output takes the flag of the input at or just before its filter's centre, half the taps per phase back (a
 * passthrough copies them). @p in_flags and @p out_flags may be NULL, and then this is resamp_process_block().
 *
 * @return Number of output samples written, with as many flags.
 */
int resamp_process_block_flags(struct demod_state* s, const float* in, const uint8_t* in_flags, int in_len, float* out,
                               uint8_t* out_flags);

#ifdef __cplusplus
}
#endif

#endif /* DSP_RESAMPLER_H */
