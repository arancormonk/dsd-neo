// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The analog monitor's sinks at the sink rate (issue #633).
 *
 * The local raw stream, the UDP analog socket and the -6 raw WAV all run at dsd_opts_analog_sink_rate_hz() (48 kHz),
 * whatever rate the monitor's blocks run at. A block at another rate goes through this module's converter for its sink
 * (<dsd-neo/dsp/rate_converter.h>) and leaves in chunks of at most DSD_ANALOG_SINK_CHUNK samples, the size of a 48 kHz
 * block and of every UDP datagram the monitor sent before. A block already at the sink rate is the caller's to write as
 * it always has, untouched.
 *
 * A sink's converter starts over from silence before a block when the sink's previous block was not written, when
 * samples were dropped (dsd_analog_sink_break()), when the reception moved since its last write (a new RTL stream or
 * trunk-tuning generation, a new PCM input stream), or when the block's rate changed. The monitor also starts over at
 * every announced reception boundary (dsd_analog_sink_note_reception()); the -6 raw WAV, a capture of the input as it
 * arrives, runs on across one, so it keeps every sample of an acquisition reset in a digital session.
 *
 * Decoder-thread only: the state lives in the DSD_STATE_EXT_DSP_ANALOG_SINK slot, allocated the first time a block
 * needs converting, so a session that only ever runs at the sink rate never allocates it.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_DSP_ANALOG_SINK_H
#define DSD_NEO_INCLUDE_DSD_NEO_DSP_ANALOG_SINK_H

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** The most samples one write hands a sink: 20 ms at 48 kHz, 1920 bytes on the UDP analog socket. */
#define DSD_ANALOG_SINK_CHUNK 960

/** Which sink: each keeps its own converter. */
typedef enum {
    /** The monitor's audio: the local raw stream and the UDP analog socket, written together. */
    DSD_ANALOG_SINK_MONITOR = 0,
    /** The -6 raw WAV: the input before the monitor's filters, every block. */
    DSD_ANALOG_SINK_RAW_WAV = 1,
} dsd_analog_sink;

/** What dsd_analog_sink_write() did with a block. */
typedef enum {
    /** The block already runs at the sink rate: the caller writes it itself, as it is. */
    DSD_ANALOG_SINK_NATIVE = 0,
    /** The block was converted and written. */
    DSD_ANALOG_SINK_WROTE = 1,
    /** The block's rate cannot be converted (or the converter could not be allocated): nothing was written, and an
        error was logged once for the rate. Writing it unconverted would play it at the wrong speed. */
    DSD_ANALOG_SINK_MUTED = -1,
} dsd_analog_sink_result;

/** Hands @p count samples at the sink rate to a sink. */
typedef void (*dsd_analog_sink_write_fn)(const void* ctx, const short* samples, size_t count);

/**
 * @brief A block starts filling: a sink whose previous block was not written starts over from silence at its next
 * write.
 */
void dsd_analog_sink_block_begin(const dsd_state* state);

/** @brief Samples were dropped here (a part-collected block, a block that mixed two rates): every sink starts over at
    its next write. */
void dsd_analog_sink_break(const dsd_state* state);

/** @brief A reception boundary was announced (dsd_analog_rx_reset()): the monitor starts over at its next write, as its
    audio chain does. */
void dsd_analog_sink_note_reception(const dsd_state* state);

/**
 * @brief Write @p n samples of @p sink's block, collected at @p block_hz, to a sink at @p sink_hz.
 *
 * The samples are converted, scaled by @p gain, saturated to int16 (NaN as silence) and handed to @p write in chunks of
 * at most DSD_ANALOG_SINK_CHUNK samples. @p gain is what takes the samples to int16 scale: 1.0 for the monitor, whose
 * audio chain has set its level, and dsd_analog_audio_int16_scale() for the -6 raw WAV (issue #643). The converter is
 * linear, so scaling its output is scaling its input; @p buf is left as it is.
 *
 * @return A dsd_analog_sink_result.
 */
int dsd_analog_sink_write(const dsd_opts* opts, dsd_state* state, dsd_analog_sink sink, const float* buf, size_t n,
                          double gain, int block_hz, int sink_hz, dsd_analog_sink_write_fn write, const void* ctx);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_DSP_ANALOG_SINK_H */
