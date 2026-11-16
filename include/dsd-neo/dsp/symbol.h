// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Symbol/sample acquisition helpers.
 *
 * Declares symbol acquisition entrypoints implemented in `src/dsp/dsd_symbol.c`.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_DSP_SYMBOL_H_
#define DSD_NEO_INCLUDE_DSD_NEO_DSP_SYMBOL_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#ifdef __cplusplus
extern "C" {
#endif

float getSymbol(dsd_opts* opts, dsd_state* state, int have_sync);

/**
 * @brief Carry out a pending input drop (dsd_state::input_drop_pending, issue #834): a rigctl peer confirmed a retune
 * while the audio input still held the frequency left's samples. Drops what the input held at the confirmation
 * (dsd_state::input_drop_cut), as an RTL retune's flush does, and keeps what arrived after it: the UDP ring and the TCP
 * connection up to the count marked on them (the network-audio hooks), stdin up to its count when it is a pipe or
 * socket (dsd_pcm_stream_discard_through()), and PulseAudio, PortAudio or AAudio input but for the audio captured since
 * the mark (dsd_audio_discard_input()). Files and RTL streams hold nothing to drop. Every input reader runs it before
 * it reads (getSymbol(), EDACS analog voice), on the decoder thread, outside the tune. No-op when nothing is pending; a
 * request asked for on an input since replaced (dsd_state::input_drop_generation is not
 * dsd_opts::pcm_input_generation), or marked on another queue than the one read now, drops nothing.
 *
 * @param opts Decoder options (the input).
 * @param state Decoder state holding the request.
 */
void dsd_symbol_apply_input_drop(dsd_opts* opts, dsd_state* state);

/**
 * @brief Forget which matched filter the symbol grid was reading through.
 *
 * Pairs with init_rrc_filter_memory(): a cleared filter that the grid still
 * believes it has primed would run from an empty history at the next sample,
 * which is the transient the seam exists to prevent (issue #444).
 */
void dsd_symbol_matched_filter_reset(dsd_state* state);

/**
 * @brief Drop the analog monitor block the decoder has part-collected.
 *
 * getSymbol() collects the unsynced samples it reads into `analog_out_f`, in a
 * digital session too, and plays or records the block once it is full. The
 * samples in it belong to the receive family they were read under, or on the
 * analog monitor to its FM or AM detector, so a change between the analog and
 * digital families, or between FM and AM, calls this on the decoder thread; the
 * next block then starts with the new family's or kind's samples. On an RTL
 * front end getSymbol() also drops the block itself, and collects nothing, while
 * the front end has yet to switch to the monitor an analog decoder is configured
 * for (issue #582).
 */
void dsd_symbol_analog_block_reset(dsd_state* state);

/**
 * @brief Tell the RTL output rescale which rate the symbol timing is in now (issue #634).
 *
 * On an RTL monitor output the decoder rescales samplesPerSymbol and symbolCenter whenever the output rate changes,
 * from the rate it last rescaled them to. An input switch back to the radio from a PCM input calls this with the PCM
 * input's timing rate, so the next rescale starts from the rate the timing is really in. A no-op in a build without
 * radio support.
 */
void dsd_symbol_note_timing_rate(int rate_hz);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_DSP_SYMBOL_H_ */
