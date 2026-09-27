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
 * DCS (issue #523): the standard 104-code set (023 ... 754), each code a 9-bit value written as
 * three octal digits and held as that value (023 octal = 19). A code's 23-bit word is the Golay
 * (23,12) code word of the 12 data bits "code, then 100": in the dsd_dcs_word() layout bits 0-8
 * are the code (least significant first), bits 9-11 are 0, 0, 1 and bits 12-22 the 11 check
 * bits, and bit 0 is the first bit sent. As a polynomial with bit i the coefficient of x^i the
 * word is a multiple of g(x) = x^11 + x^10 + x^6 + x^5 + x^4 + x^2 + 1. The transmitter repeats
 * the word at 134.4 bit/s, a one as positive deviation in normal (N) polarity and as negative
 * deviation in inverted (I) polarity, which sends the word's complement. Every supported code's
 * word carries 11 or 12 ones, so its level averages to within 1/23 of zero over any 23 bits,
 * in either polarity (the DCS detector's balance slicer relies on it).
 *
 * A receiver cannot tell where a word starts, so every rotation of a word is the same signal,
 * and every complement is a code word too: each supported code shares its waveform with other
 * codes, in the standard set always with exactly one other code of the opposite polarity (D023N
 * is D047I). dsd_dcs_canonical() names such a class by one member, normal polarity first, then
 * the lowest code, so every inverted code is named by its normal alias (D023I by D047N), and
 * dsd_dcs_alias() by the other standard spelling. Nothing in the signal says which of the two
 * a transmitter was set to, so a received code is shown as both, canonical first:
 * "DCS D047N / D023I" (dsd_dcs_format_label()).
 *
 * The tables live in runtime rather than DSP because the frontends format these values and the
 * receive policy (#527) parses them, and neither may depend on the DSP module.
 *
 * Tone lists (issue #527): the configured receive policy's list, written the same way on the CLI,
 * in the INI and in a scan row's options: entries separated by '/', each a standard CTCSS tone
 * ("100" or "100.0") or a standard DCS code ("D023", "D023N" or "D023I", any case; a bare code is
 * N), for example 67.0/100.0/D023N. Commas are refused (the channel-map splitter would cut a
 * list at them). A list holds a signal once: a tone written twice, or both spellings of one DCS
 * signal (D023N and D047I), are refused. Diagnostics name entries by position and never repeat
 * the text they were given. A list is kept as written (D023I stays D023I) and matched by the
 * signal, so a listed D023I matches a received D047N.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_ANALOG_TONES_H_
#define DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_ANALOG_TONES_H_

#include <dsd-neo/core/analog_tone.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <stddef.h>
#include <stdint.h>

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

/** @brief Number of supported DCS codes (the standard 104-code set). */
enum { DSD_DCS_CODE_COUNT = 104 };

/** @brief Bits in a DCS word. */
enum { DSD_DCS_WORD_BITS = 23 };

/** @brief Highest DCS code value: nine bits, octal 777. */
enum { DSD_DCS_CODE_MAX = 0777 };

/** @brief Room for any text dsd_dcs_format_label() writes, terminator included ("DCS D023N / D047I"). */
enum { DSD_DCS_LABEL_SIZE = 24 };

/** @brief Number of supported DCS codes; always DSD_DCS_CODE_COUNT. */
int dsd_dcs_code_count(void);

/**
 * @brief The supported DCS code at @p index, as its value (023 octal = 19).
 *
 * The table is ascending, so index 0 is 023 and the last index is 754.
 *
 * @return The code, or -1 when @p index is out of range.
 */
int dsd_dcs_code(int index);

/**
 * @brief Table index of the supported DCS code @p code.
 *
 * @return The index, or -1 when @p code is not in the standard set.
 */
int dsd_dcs_code_index(int code);

/**
 * @brief The 23-bit word a transmitter repeats for @p code in the given polarity.
 *
 * Any 9-bit code (0 to DSD_DCS_CODE_MAX) has a word, supported or not. Bit 0 is sent first;
 * see the file comment for the layout. @p inverted nonzero gives the complement.
 *
 * @return The word, or 0 when @p code is out of range (no word is 0).
 */
uint32_t dsd_dcs_word(int code, int inverted);

/**
 * @brief Name the DCS signal in 23 consecutive received bits.
 *
 * @p window holds the bits in the order they arrived, the earliest in bit 0 (bits above 22 are
 * ignored). When some rotation of it, in either polarity, is the word of a supported code, the
 * signal is that code's class, and the class's canonical member (dsd_dcs_canonical()) is
 * written to @p code and @p inverted (either may be NULL).
 *
 * @return 1 when the window is a supported code's signal, 0 otherwise (outputs untouched).
 */
int dsd_dcs_match(uint32_t window, int* code, int* inverted);

/**
 * @brief The name a receiver gives the signal of @p code in the given polarity.
 *
 * Every rotation of a word, and of its complement, is one signal. Of the supported codes that
 * send it, the canonical member is the normal-polarity one if there is one, then the lowest
 * code: D023I is the signal of D047N and is named D047N, and every supported normal code names
 * its own signal. Accepts any 9-bit code; an unsupported one is named only when its signal is a
 * supported code's.
 *
 * @return 0 with the member written to @p canon_code and @p canon_inverted (either may be
 *         NULL), or -1 when @p code is out of range or its signal is no supported code's.
 */
int dsd_dcs_canonical(int code, int inverted, int* canon_code, int* canon_inverted);

/**
 * @brief The other standard spelling of the signal of @p code in the given polarity.
 *
 * Of the standard codes, exactly two send each supported signal, one in each polarity: the
 * canonical normal member (dsd_dcs_canonical()) and one inverted member. D023N and D047I are one
 * signal, D047N and D023I another. This names the inverted member, whichever spelling @p code is
 * (D023N and D047I both give D047I). Accepts any 9-bit code whose signal is a supported code's.
 *
 * @return 0 with the member written to @p alias_code and @p alias_inverted (either may be NULL),
 *         or -1 when @p code is out of range or its signal is no supported code's (outputs
 *         untouched).
 */
int dsd_dcs_alias(int code, int inverted, int* alias_code, int* alias_inverted);

/**
 * @brief Write a DCS code as "D023N" or "D023I": three octal digits with leading zeros.
 *
 * Accepts any 9-bit code, supported or not.
 *
 * @return Characters written (terminator excluded), or -1 for a NULL/too-small buffer or a code
 *         outside 0..DSD_DCS_CODE_MAX. The buffer holds an empty string on failure.
 */
int dsd_dcs_format(int code, int inverted, char* buf, size_t buf_size);

/**
 * @brief Write the display and log label for a received DCS code: "DCS D023N / D047I".
 *
 * Names the signal of @p code in the given polarity by both of its standard spellings, the
 * canonical one first (dsd_dcs_canonical(), then dsd_dcs_alias()), whichever of them @p code
 * is: D023N and D047I both write "DCS D023N / D047I", D023I "DCS D047N / D023I".
 *
 * @return Characters written (terminator excluded), or -1 for a NULL/too-small buffer, a code
 *         outside 0..DSD_DCS_CODE_MAX, or one whose signal is no supported code's. The buffer
 *         holds an empty string on failure.
 */
int dsd_dcs_format_label(int code, int inverted, char* buf, size_t buf_size);

/** @brief Longest tone-list text, terminator excluded: the INI line, a CLI argument and a row option all fit it. */
enum { DSD_TONE_LIST_TEXT_MAX = 1023 };

/** @brief Room for any diagnostic dsd_tone_set_parse() writes, terminator included. */
enum { DSD_TONE_LIST_ERROR_SIZE = 96 };

/**
 * @brief Parse a '/'-separated tone list (see the file comment) into @p out.
 *
 * Empty text, a comma anywhere, an empty entry, an entry that is no standard CTCSS tone or DCS
 * code, a tone or DCS signal listed twice and text longer than DSD_TONE_LIST_TEXT_MAX are
 * refused. On failure @p out is untouched and @p err (when not NULL) says why by entry number,
 * naming a duplicate by its canonical spelling, never by the text given.
 *
 * @return 0 on success, -1 on failure (and for NULL @p text or @p out).
 */
int dsd_tone_set_parse(const char* text, dsd_tone_set* out, char* err, size_t err_size);

/**
 * @brief Write @p set as the list dsd_tone_set_parse() reads back: tones ascending ("67.0"), then
 * codes ascending, each as written ("D023N"), '/' between them. The empty set writes "".
 *
 * @return Characters written (terminator excluded), or -1 for a NULL set or buffer or a buffer
 *         too small for the whole list (the buffer then holds an empty string).
 */
int dsd_tone_set_format(const dsd_tone_set* set, char* buf, size_t buf_size);

/**
 * @brief Write @p set for display: the order of dsd_tone_set_format(), tones with their unit
 * ("100.0 Hz/D023N"). Entries that do not fit are summarised by a count ("67.0 Hz/…+12"), so
 * the text always fits a buffer of at least 24 bytes.
 *
 * @return Characters written (terminator excluded), or -1 for a NULL set or buffer or a buffer
 *         smaller than 24 bytes (the buffer then holds an empty string).
 */
int dsd_tone_set_format_display(const dsd_tone_set* set, char* buf, size_t buf_size);

/** @brief Entries in @p set (0 for NULL). */
int dsd_tone_set_count(const dsd_tone_set* set);

/** @brief 1 when @p set holds at least one DCS code, 0 otherwise (and for NULL). */
int dsd_tone_set_has_dcs(const dsd_tone_set* set);

/** @brief 1 when @p a and @p b hold the same entries, each spelled the same (both NULL counts as equal). */
int dsd_tone_set_equal(const dsd_tone_set* a, const dsd_tone_set* b);

/** @brief 1 when @p set lists the CTCSS tone @p tenths_hz, 0 otherwise (and for NULL). */
int dsd_tone_set_contains_ctcss(const dsd_tone_set* set, int tenths_hz);

/**
 * @brief 1 when @p set lists the DCS signal of @p code in the given polarity, under either of
 * its standard spellings (a listed D023I matches D047N), 0 otherwise (and for NULL).
 */
int dsd_tone_set_contains_dcs(const dsd_tone_set* set, int code, int inverted);

/** @brief "off", "allow" or "block" for a dsd_tone_filter_mode, NULL for anything else. */
const char* dsd_tone_filter_mode_name(int mode);

/**
 * @brief Parse "off", "allow" or "block", ignoring ASCII case.
 *
 * @return 0 with the dsd_tone_filter_mode in @p mode, -1 for anything else (@p mode untouched).
 */
int dsd_tone_filter_mode_parse(const char* text, int* mode);

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

/**
 * @brief The CTCSS/DCS receive policy's verdict in force on the reception the tap last read (issue #527): a
 * dsd_analog_tone_gate.
 *
 * OFF whenever received-tone detection does not run (dsd_analog_tone_detection_active(): the AM monitor, a digital
 * mode, the -8 source monitor), whatever an earlier session published; otherwise dsd_state::analog_rx.gate, with a
 * value this build does not know read as PENDING. It fails closed: a published OFF while dsd_opts holds a list policy
 * with a list (--tone-allow/--tone-block) is no verdict of that policy -- the tap has no session to judge with, having
 * failed to allocate one, or has not read since the policy came on -- and reads as PENDING until the tap's own verdict
 * replaces it. The monitor output plays only OFF and ALLOWED, and a REJECTED carrier
 * is no scan activity. The verdict is only as fresh as the tap's last read: a scanner pairs it with the carrier it
 * holds a row on (dsd_scan_analog_carrier_open()). Read-only; OFF for NULL.
 */
int dsd_analog_tone_gate_in_force(const dsd_opts* opts, const dsd_state* state);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_ANALOG_TONES_H_ */
