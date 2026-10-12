// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The digital voice chain (issue #653): one high-pass and one gain per slot, run once on each decoded 8 kHz
 * voice frame before anything consumes it.
 *
 * A decoder that synthesizes a voice frame (the vocoder, YSF V/D2, MBE file, SDRTrunk JSON playback and M17 Codec2) runs
 * dsd_voice_chain_run() on it in place, whatever the output format, output type or mute say. Every consumer then takes
 * the same samples: the per-call WAV and the shared short staging (dsd_voice_stage_short_left/right()). Mixers route
 * those samples in both output formats; stdout and UDP convert the resulting s16 blocks to f32 under -y. Every
 * consumer starts from one level and one frequency response. The chain is verdict-neutral: crypto, talkgroup and
 * slot gates still decide who receives a frame. Decoder thread only.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_CORE_VOICE_CHAIN_H
#define DSD_NEO_INCLUDE_DSD_NEO_CORE_VOICE_CHAIN_H

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** The digital voice high-pass (`-v 0x8`): a 2nd-order Butterworth high-pass at this corner on 8 kHz frames. */
#define DSD_VOICE_HPF_CORNER_HZ 300.0
/** The rate the digital voice frames, and so the high-pass, run at. */
#define DSD_VOICE_HPF_RATE_HZ   8000.0
/** Samples per voice frame. */
#define DSD_VOICE_FRAME_SAMPLES 160

/**
 * @brief Run the chain on one frame of slot @p slot (0 or 1) in place: the high-pass when `opts->use_hpf_d` is 1, then
 * the slot's gain, then saturation to the int16 range.
 *
 * @p slot picks the filter and gain state, whatever buffer holds @p frame (X2-TDMA synthesizes both timeslots into the
 * left buffer). The gain is the fixed `-g` multiplier (`audio_gain`, slot 2 `audio_gainR`) when it is above 0, unity
 * when it is below 0, and otherwise the auto gain: a peak normalizer to 30000 over the last 25 frames, at most x50,
 * attacking at once and releasing at most 5% per frame, its level carried across calls. The slot's filter state starts
 * over when the slot's call (a new nonzero epoch) or the frame's protocol family changes, and while the high-pass is
 * off.
 *
 * @param frame DSD_VOICE_FRAME_SAMPLES int16-scale samples.
 */
void dsd_voice_chain_run(const dsd_opts* opts, dsd_state* state, int slot, float* frame);

/**
 * @brief Stage the chained left frame (`audio_out_temp_buf`) for the short mixers: `s_l`, the output ring and, above 8
 * kHz, the upsampled `s_lu`. Runs no gain or filter.
 */
void dsd_voice_stage_short_left(const dsd_opts* opts, dsd_state* state);
/** @brief Stage the chained right frame (`audio_out_temp_bufR`) for the short mixers: `s_r`, the ring, `s_ru`. */
void dsd_voice_stage_short_right(const dsd_opts* opts, dsd_state* state);

/**
 * @brief Clear slot @p slot's high-pass state and the reception it ran for (both slots when @p slot is negative): the
 * next frames start a reception of their own.
 */
void dsd_voice_hpf_reset(dsd_state* state, int slot);

/** @brief High-pass @p n samples of @p buf in place on slot @p slot's state. A non-finite sample comes out as 0. */
void dsd_voice_hpf_process(dsd_state* state, int slot, float* buf, size_t n);

/** @brief The high-pass's designed magnitude response at @p hz, in dB. */
double dsd_voice_hpf_gain_db(double hz);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_CORE_VOICE_CHAIN_H */
