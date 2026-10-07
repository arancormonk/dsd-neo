// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Streaming sample-rate converter from any audio input rate to a sink rate (issue #633).
 *
 * The analog sinks run at 48 kHz (the local raw stream, the UDP analog socket, the -6 raw WAV), whatever rate the
 * input arrives at. This converts a stream to such a rate with the polyphase rational resampler: an exact L/M where the
 * rates allow one, otherwise the closest L/M with both terms up to DSD_RATE_CONVERTER_MAX_TERM, which is within
 * DSD_RATE_CONVERTER_TOLERANCE_PPM of the true ratio for every supported rate. Equal rates, or a best ratio of 1/1, are
 * an identity: samples pass through untouched, bit for bit.
 *
 * Decoder-thread only; a converter is plain storage the caller owns. Its output depends only on the samples fed since
 * the last clear, never on how they were cut into calls.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_DSP_RATE_CONVERTER_H
#define DSD_NEO_INCLUDE_DSD_NEO_DSP_RATE_CONVERTER_H

#include <dsd-neo/dsp/resampler.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** The lowest input rate converted: one input then makes at most 48 outputs at 48 kHz. */
#define DSD_RATE_CONVERTER_MIN_IN_HZ      1000
/** The highest decimation converted: past it, the 256-tap cap no longer filters the band above the output's Nyquist. */
#define DSD_RATE_CONVERTER_MAX_DECIMATION 16
/** The largest L or M an approximated ratio uses. */
#define DSD_RATE_CONVERTER_MAX_TERM       4096
/** How far an approximated ratio may be from the true one: 250 ppm, which terms up to 4096 always meet. */
#define DSD_RATE_CONVERTER_TOLERANCE_PPM  250
/** Room for the outputs one input makes, which the pull API keeps between calls. */
#define DSD_RATE_CONVERTER_CARRY          64

/** What a converter does with its rates. */
typedef enum {
    /** Equal rates, or a best ratio of 1/1: samples pass through untouched. */
    DSD_RATE_CONVERTER_IDENTITY = 0,
    /** Samples are resampled at L/M. */
    DSD_RATE_CONVERTER_CONVERTING = 1,
    /** No conversion the converter can do well (a rate of 0 or less, an input below DSD_RATE_CONVERTER_MIN_IN_HZ, or a
        decimation over DSD_RATE_CONVERTER_MAX_DECIMATION): the caller must not write the samples to the sink. */
    DSD_RATE_CONVERTER_UNSUPPORTED = -1,
    /** The filter could not be allocated: as unsupported, for now. */
    DSD_RATE_CONVERTER_FAILED = -2,
} dsd_rate_converter_mode;

/** A streaming converter: initialise it with dsd_rate_converter_init(), release it with dsd_rate_converter_free(). */
typedef struct dsd_rate_converter {
    int configured;
    int in_hz;
    int out_hz;
    int mode; /**< dsd_rate_converter_mode */
    int L;
    int M;
    dsd_resampler_state rs;
    float carry[DSD_RATE_CONVERTER_CARRY];
    int carry_len;
    int carry_pos;
    uint8_t carry_tag; /**< The tag of the input the carried outputs were made from. */
} dsd_rate_converter;

/** Reads the next input sample into @p sample, and its tag into @p tag, which starts at 0 for a reader with none: 1, or
    0 when the input ended. */
typedef int (*dsd_rate_converter_read_fn)(void* ctx, float* sample, uint8_t* tag);

/** Initialise @p c, unconfigured. Safe on any storage. */
void dsd_rate_converter_init(dsd_rate_converter* c);

/** Release @p c's filter; it is unconfigured again. */
void dsd_rate_converter_free(dsd_rate_converter* c);

/**
 * @brief The ratio a converter from @p from_hz to @p to_hz runs: L / M, written when either pointer is not NULL.
 *
 * @return DSD_RATE_CONVERTER_IDENTITY (L = M = 1), DSD_RATE_CONVERTER_CONVERTING or DSD_RATE_CONVERTER_UNSUPPORTED.
 */
int dsd_rate_converter_ratio(int from_hz, int to_hz, int* L, int* M);

/**
 * @brief Convert from @p from_hz to @p to_hz from now on.
 *
 * The same rates keep the converter as it is (a failed allocation is tried again). Other rates design it anew, which
 * clears it.
 *
 * @return The dsd_rate_converter_mode now in force.
 */
int dsd_rate_converter_configure(dsd_rate_converter* c, int from_hz, int to_hz);

/** The rate the samples leave at: the output rate while converting, the input rate otherwise. */
int dsd_rate_converter_output_hz(const dsd_rate_converter* c);

/** Start over from silence: the filter's history, its phase and anything the pull API kept. */
void dsd_rate_converter_clear(dsd_rate_converter* c);

/** The most outputs one input makes (ceil(L / M)); 1 for an identity. */
int dsd_rate_converter_max_out_per_input(const dsd_rate_converter* c);

/**
 * @brief Push up to @p in_len samples, writing at most @p out_cap outputs.
 *
 * Consumes as many inputs as fit the output, at least one when @p in_len is positive. A converter is driven either by
 * this or by dsd_rate_converter_fill(), never both.
 *
 * @param consumed Set to the inputs consumed.
 * @return The outputs written, or -1 on an argument error, an unusable mode, or @p out_cap below
 *         dsd_rate_converter_max_out_per_input() (so a caller loop can never stall).
 */
int dsd_rate_converter_process(dsd_rate_converter* c, const float* in, size_t in_len, float* out, size_t out_cap,
                               size_t* consumed);

/**
 * @brief Pull exactly @p count outputs, reading inputs from @p read as they are needed.
 *
 * Outputs an input makes beyond @p count are kept for the next call. With @p tags (@p count of them, or NULL), each
 * output gets the tag @p read gave the newest input it was made from, so a per-sample flag (the squelch's gate, say)
 * follows the samples through the conversion.
 *
 * @return @p count, or -1 on an argument error, an unusable mode, or when @p read ends the input.
 */
int dsd_rate_converter_fill(dsd_rate_converter* c, float* out, uint8_t* tags, size_t count,
                            dsd_rate_converter_read_fn read, void* ctx);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_DSP_RATE_CONVERTER_H */
