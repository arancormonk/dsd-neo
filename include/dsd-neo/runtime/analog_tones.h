// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Sub-audible signalling tables and text shared by the detectors, the views and the policy.
 *
 * CTCSS (issue #522): the standard 50-tone EIA/TIA table, 67.0-254.1 Hz, held in tenths of a
 * hertz so every consumer compares integers. 150.0 Hz is deliberately not in the table: it is
 * 1.4 Hz from 151.4 Hz, and a detector that snapped it would report the wrong tone.
 *
 * The tables live in runtime rather than DSP because the frontends format these values and the
 * receive policy (#527) parses them, and neither may depend on the DSP module.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_ANALOG_TONES_H_
#define DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_ANALOG_TONES_H_

#include <dsd-neo/core/opts_fwd.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Number of supported CTCSS tones (the standard 50-tone table). */
enum { DSD_CTCSS_TONE_COUNT = 50 };

/** @brief Longest text dsd_ctcss_format_label() writes, terminator included ("CTCSS 254.1 Hz"). */
enum { DSD_CTCSS_LABEL_SIZE = 24 };

/** @brief Number of supported CTCSS tones; always DSD_CTCSS_TONE_COUNT. */
int dsd_ctcss_tone_count(void);

/**
 * @brief The supported tone at @p index, in tenths of a hertz.
 *
 * The table is ascending, so index 0 is 67.0 Hz (670) and the last index is 254.1 Hz (2541).
 *
 * @return The tone in tenths of a hertz, or -1 when @p index is out of range.
 */
int dsd_ctcss_tone_tenths(int index);

/**
 * @brief Table index of the supported tone @p tenths_hz.
 *
 * @return The index, or -1 when @p tenths_hz is not a supported tone (150.0 Hz included).
 */
int dsd_ctcss_tone_index(int tenths_hz);

/**
 * @brief Write a tone value as "100.0" (one decimal, no unit).
 *
 * Accepts any value from 0.1 Hz to 999.9 Hz, supported or not, so a caller can also name a
 * frequency it is rejecting.
 *
 * @return Characters written (terminator excluded), or -1 for a NULL/too-small buffer or a
 *         value outside 1..9999 tenths. The buffer holds an empty string on failure.
 */
int dsd_ctcss_format(int tenths_hz, char* buf, size_t buf_size);

/**
 * @brief Write the display and log label for a received tone, "CTCSS 100.0 Hz".
 *
 * @return Characters written (terminator excluded), or -1 as for dsd_ctcss_format().
 */
int dsd_ctcss_format_label(int tenths_hz, char* buf, size_t buf_size);

/**
 * @brief Whether the decoder's analog receive tap runs: the analog monitor of either kind, FM or AM, on audio it can
 * hear.
 *
 * Analog-only decoding with input monitoring, on a PCM input (Pulse, stdin, WAV, UDP, TCP) or on an RTL-family stream
 * whose output is monitor audio (asked of the RTL stream-metrics hook, whose default answers monitor audio).
 * Symbol-file and null inputs carry no audio to hear. The -8 source monitor during digital decoding and EDACS analog
 * voice are out: neither is analog-only.
 *
 * The tap keeps the monitor's carrier, which holds an analog scan row whether or not audio plays (issue #526), and the
 * boundaries (retunes, profile changes, resets) across which the monitor output drops a block; it runs the tone
 * detectors only where dsd_analog_tone_detection_active() says so.
 *
 * @return 1 when the tap runs, 0 otherwise (and for NULL).
 */
int dsd_analog_monitor_tap_active(const dsd_opts* opts);

/**
 * @brief Whether received-tone detection runs: the analog FM monitor, on audio it can hear.
 *
 * dsd_analog_monitor_tap_active() with the FM analog kind (the Analog preset, not AM: CTCSS and DCS are FM signalling).
 * On the AM monitor the tap keeps only the carrier and its boundaries (issue #524).
 *
 * The decoder's tap and every frontend's received-tone row ask this one question, so a row is
 * never on screen for a session in which nothing is listening -- for instance while an RTL
 * stream still outputs a digital family's samples.
 *
 * @return 1 when detection runs, 0 otherwise (and for NULL).
 */
int dsd_analog_tone_detection_active(const dsd_opts* opts);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_ANALOG_TONES_H_ */
