// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Analog receive channel contract shared by the CLI, config, app commands, scan rows and the demodulator.
 *
 * The channel width is the full RF passband, centred on the tuned frequency, that the analog channel filter protects:
 * the filter's cutoff sits at width/2 + DSD_ANALOG_CHANNEL_GUARD_HZ with a fixed DSD_ANALOG_CHANNEL_TRANSITION_HZ
 * Blackman transition outside it. It is neither the tuner bandwidth nor the audio bandwidth. A width of 16000 Hz
 * reproduces the historical analog (WIDE) channel filter exactly wherever that design succeeded: from about 19.1 kHz,
 * where its cutoff stops being held to 0.9 x Nyquist, up to about 51.4 kHz, where it no longer fits its 144 taps. Above
 * that the historical filter was the 63-tap fallback prototype, and 16000 Hz gets a full design instead (219 taps at
 * 78,125 Hz) up to the analog tap capacity at about 102.7 kHz. Below about 19.1 kHz and above that capacity 16000 Hz is
 * not realizable.
 *
 * The transition is the window-method design parameter, not a stopband guarantee: the response is about -0.3 dB at
 * width/2, about -30 dB at width/2 + 1200 Hz, and -50 dB or better only from about width/2 + 1450 Hz.
 *
 * Everything here is pure integer arithmetic so that runtime callers can validate a width without linking the DSP
 * module, which mirrors the design constants below. `src/dsp/demod_pipeline.cpp` takes its transition from
 * DSD_ANALOG_CHANNEL_TRANSITION_HZ and static-asserts the guard (half that transition) and the tap capacity. The rest
 * is pinned at run time by DSP_CHANNEL_FILTERS: the Blackman attenuation the tap count uses (firdes), the tap count at
 * every rate, and that every width the validator accepts designs while every width it rejects does not (the analog
 * design calls dsd_analog_width_realizable() itself, which holds the 9/20 cutoff ratio).
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_ANALOG_CHANNEL_H_
#define DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_ANALOG_CHANNEL_H_

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Analog demodulator kind. Zero-initialised state means FM. */
typedef enum dsd_analog_demod {
    DSD_ANALOG_DEMOD_FM = 0, /**< Narrowband FM (phase-difference discriminator). */
    DSD_ANALOG_DEMOD_AM = 1, /**< AM envelope detection. */
} dsd_analog_demod;

/** @brief Receive family a radio front end is asked to run. */
typedef enum dsd_rx_family {
    DSD_RX_FAMILY_DIGITAL = 0, /**< Symbol/discriminator output for the digital decoders. */
    DSD_RX_FAMILY_ANALOG = 1,  /**< Analog monitor audio. */
} dsd_rx_family;

/* Per-kind channel width ranges and defaults, in Hz. */
#define DSD_ANALOG_NFM_WIDTH_MIN_HZ              8000
#define DSD_ANALOG_NFM_WIDTH_MAX_HZ              25000
#define DSD_ANALOG_NFM_WIDTH_DEFAULT_HZ          16000
#define DSD_ANALOG_AM_WIDTH_MIN_HZ               5000
#define DSD_ANALOG_AM_WIDTH_MAX_HZ               20000
#define DSD_ANALOG_AM_WIDTH_DEFAULT_HZ           6000

/* Channel filter design constants. The DSP takes the transition from here and static-asserts the guard and the tap
   capacity; DSP_CHANNEL_FILTERS checks the window attenuation and tap counts against firdes, and that the analog
   design accepts exactly the widths the validator does. */
#define DSD_ANALOG_CHANNEL_TRANSITION_HZ         1200 /**< Blackman transition width. */
#define DSD_ANALOG_CHANNEL_GUARD_HZ              600  /**< Cutoff = width/2 + guard (half the transition). */
#define DSD_ANALOG_CHANNEL_MAX_TAPS              288  /**< Analog channel-filter tap capacity. */
#define DSD_ANALOG_CHANNEL_WINDOW_ATTENUATION_DB 74   /**< GNU Radio Blackman attenuation for the tap count. */
/* The cutoff must stay at or below 9/20 of the DSP rate (0.9 x Nyquist). */
#define DSD_ANALOG_CHANNEL_CUTOFF_RATE_NUM       9
#define DSD_ANALOG_CHANNEL_CUTOFF_RATE_DEN       20

/** @brief Buffer size that always holds dsd_analog_width_format() output. */
#define DSD_ANALOG_WIDTH_TEXT_MAX                24
/** @brief Buffer size that holds every validator/parser message untruncated. */
#define DSD_ANALOG_ERROR_TEXT_MAX                256
/** @brief Longest prefix of rejected input the parser echoes (longer input is shown with a trailing "..."). */
#define DSD_ANALOG_PARSE_ECHO_MAX                32

/** @brief Return 1 for DSD_ANALOG_DEMOD_FM or DSD_ANALOG_DEMOD_AM, else 0. */
int dsd_analog_demod_is_valid(int kind);

/** @brief Short display label for a demodulator kind ("NFM", "AM"); "?" for an unknown kind. */
const char* dsd_analog_demod_label(int kind);

/** @brief Smallest accepted width for @p kind in Hz; 0 for an unknown kind. */
int dsd_analog_width_min_hz(int kind);

/** @brief Largest accepted width for @p kind in Hz; 0 for an unknown kind. */
int dsd_analog_width_max_hz(int kind);

/** @brief Default width for @p kind in Hz; 0 for an unknown kind. */
int dsd_analog_width_default_hz(int kind);

/**
 * @brief Resolve a configured width.
 *
 * @param kind          Demodulator kind.
 * @param configured_hz Explicit width in Hz, or 0 for "default, not requested".
 * @return @p configured_hz when positive, otherwise the kind's default (0 for an unknown kind).
 */
int dsd_analog_width_effective_hz(int kind, int configured_hz);

/** @brief Return 1 when @p width_hz is inside the accepted range for @p kind. 0 is never in range. */
int dsd_analog_width_in_range(int kind, int width_hz);

/**
 * @brief Strictly parse a width in whole Hz and range-check it for @p kind.
 *
 * Accepts only decimal digits: no sign, whitespace, suffix, radix prefix or fraction. Out-of-range values are
 * rejected, never clamped.
 *
 * @param kind          Demodulator kind the width is for.
 * @param text          Text to parse.
 * @param out_width_hz  Receives the width on success; untouched on failure.
 * @param err           Optional buffer for an actionable message (cleared on success).
 * @param err_size      Size of @p err in bytes.
 * @return 0 on success, -1 on invalid input.
 */
int dsd_analog_width_parse(int kind, const char* text, int* out_width_hz, char* err, size_t err_size);

/**
 * @brief Blackman tap count the channel filter needs at @p rate_hz.
 *
 * Same arithmetic as dsd_firdes_compute_ntaps() for a Blackman window and DSD_ANALOG_CHANNEL_TRANSITION_HZ.
 *
 * @return The (odd) tap count, or 0 when @p rate_hz is not positive.
 */
int dsd_analog_channel_taps_for_rate(int rate_hz);

/**
 * @brief Return 1 when a channel of @p width_hz can be designed at @p rate_hz without clamping or a fallback.
 *
 * Holds when width/2 + guard <= 0.45 x rate and the tap count fits DSD_ANALOG_CHANNEL_MAX_TAPS. This does not
 * check the per-kind range; see dsd_analog_width_check() for the full rule.
 */
int dsd_analog_width_realizable(int width_hz, int rate_hz);

/** @brief Largest width realizable at @p rate_hz in Hz, or 0 when no width fits that rate. */
int dsd_analog_width_max_for_rate(int rate_hz);

/** @brief Return 1 when @p khz is a selectable RTL DSP bandwidth (rtl_bw_khz): 4, 6, 8, 12, 16, 24 or 48 kHz. */
int dsd_analog_rtl_dsp_bw_is_selectable(int khz);

/**
 * @brief List the selectable RTL DSP bandwidths that filter @p width_hz, in kHz: "48", "24 or 48", "16, 24 or 48".
 *
 * The list dsd_analog_width_check() names as the fix. @p out is empty when no bandwidth fits.
 *
 * @return 0 on success, -1 on a NULL or empty buffer.
 */
int dsd_analog_width_fitting_rtl_bandwidths(int width_hz, char* out, size_t out_size);

/**
 * @brief Validate an explicit width for @p kind at a DSP rate.
 *
 * A width is accepted only when it is in the kind's range and realizable at @p rate_hz. The message on failure names
 * the requested width, the DSP rate, the largest width that rate fits, and the RTL DSP bandwidths that would fit the
 * request.
 *
 * @param kind     Demodulator kind.
 * @param width_hz Explicit width in Hz (0 is not a width; resolve defaults first).
 * @param rate_hz  DSP (demodulator) sample rate in Hz.
 * @param err      Optional message buffer (cleared on success).
 * @param err_size Size of @p err in bytes.
 * @return 0 when accepted, -1 otherwise.
 */
int dsd_analog_width_check(int kind, int width_hz, int rate_hz, char* err, size_t err_size);

/**
 * @brief Format a width in Hz as kHz text with trailing zeros dropped ("12.5 kHz", "16 kHz").
 *
 * @return 0 on success, -1 on a NULL/short buffer or negative width.
 */
int dsd_analog_width_format(int width_hz, char* out, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_ANALOG_CHANNEL_H_ */
