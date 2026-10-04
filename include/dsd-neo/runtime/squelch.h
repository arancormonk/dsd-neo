// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The RTL squelch setting's grammar, text and resolution (issue #518 follow-up).
 *
 * One grammar for every place a squelch is written -- `--squelch`, the `sql` field of an `rtl:`/`rtltcp:`/`soapy:`
 * input, the `[input]` INI keys and a scan row's `--squelch` option:
 *
 *   off | 0 | <negative dB> | <positive linear power> | auto | auto+N | noise | noise+N
 *
 * A negative number is a threshold in dB, zero (or `off`) switches the squelch off, and a positive number is a linear
 * mean power taken as given (the legacy contract of dsd_squelch_level_from_sql()). `auto` opens a margin of N whole dB
 * (3..30, default 10) above the noise floor the demodulator learns for the channel. `noise` opens an FM channel when
 * the discriminator's output above the voice band quiets by N whole dB (3..30, default 10). Keywords are
 * case-insensitive and surrounding spaces are ignored. Diagnostics never repeat the text they were given.
 *
 * The dynamic squelches (AUTO, NOISE) need a radio input carrying an analog channel: on audio input FM noise is louder
 * than voice, so a floor plus a margin would read backwards (the noise squelch has no audio-input form yet), and a
 * digital channel never shows its noise (its CRC/FEC gates it anyway). There they resolve to off. NOISE needs FM: on an
 * AM channel it resolves to AUTO with the same N (dsd_squelch_setting_resolve()).
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_SQUELCH_H_
#define DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_SQUELCH_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/power.h>
#include <dsd-neo/core/state_fwd.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Room for any text dsd_squelch_setting_format() writes ("noise +10 dB", "-120.0 dB"). */
enum { DSD_SQUELCH_TEXT_SIZE = 24 };

/** @brief Why a setting resolved as it did (dsd_squelch_setting_resolve()). */
typedef enum {
    DSD_SQUELCH_RESOLVED_AS_SET = 0, /**< the setting runs as written */
    DSD_SQUELCH_RESOLVED_NO_RADIO,   /**< a dynamic setting on an input that is not a radio: off */
    DSD_SQUELCH_RESOLVED_DIGITAL,    /**< a dynamic setting on a digital channel: off */
    DSD_SQUELCH_RESOLVED_AM_AUTO,    /**< NOISE on an AM channel: AUTO with the same N */
} dsd_squelch_resolution;

/** @brief A LEVEL setting with @p level (mean-power units; 0 or below is off) and the default AUTO margin. */
dsd_squelch_setting dsd_squelch_setting_of_level(double level);

/** @brief The AUTO setting with @p margin_db, clamped into DSD_SQUELCH_MARGIN_MIN_DB..DSD_SQUELCH_MARGIN_MAX_DB. */
dsd_squelch_setting dsd_squelch_setting_auto(int margin_db);

/** @brief The NOISE setting with @p threshold_db of quieting, clamped as dsd_squelch_setting_auto() clamps. */
dsd_squelch_setting dsd_squelch_setting_noise(int threshold_db);

/** @brief The NOISE setting for @p mode NOISE, the AUTO one otherwise, with @p margin_db (clamped). */
dsd_squelch_setting dsd_squelch_setting_dynamic(int mode, int margin_db);

/** @brief Whether @p s is dynamic (AUTO, NOISE) rather than a fixed threshold; 0 for NULL. */
int dsd_squelch_setting_is_dynamic(const dsd_squelch_setting* s);

/** @brief Whether @p s gates nothing: a LEVEL setting at or below 0; never a dynamic one. 1 for NULL. */
int dsd_squelch_setting_is_off(const dsd_squelch_setting* s);

/**
 * @brief Whether @p a and @p b decide the same way: the same mode, and the same margin (AUTO, NOISE) or level (LEVEL,
 * equal within a relative 1e-9, or both off). 0 when either is NULL.
 */
int dsd_squelch_setting_equal(const dsd_squelch_setting* a, const dsd_squelch_setting* b);

/**
 * @brief Parse @p text in the squelch grammar (see the file comment) into @p out.
 *
 * @return 0, or -1 with a short reason in @p err (when not NULL), @p out untouched.
 */
int dsd_squelch_setting_parse(const char* text, dsd_squelch_setting* out, char* err, size_t err_size);

/**
 * @brief Write @p s as the grammar reads it back: `off`, `-60.0 dB` (a level, to one place), `auto +10 dB`,
 * `noise +10 dB`.
 *
 * A positive linear level is written as its dB value, which parses to the same threshold within the display rounding.
 *
 * @return 0, or -1 for NULL arguments or a zero size (@p out then holds an empty string when it has room).
 */
int dsd_squelch_setting_format(const dsd_squelch_setting* s, char* out, size_t out_size);

/** @brief Whether @p s's level is a finite number, as a frontend's request must be: 1 for a dynamic setting, whose
 * level is not in force; 0 for NULL. Built with IEEE semantics, so it holds in a fast-math build. */
int dsd_squelch_setting_level_finite(const dsd_squelch_setting* s);

/**
 * @brief The setting that runs for @p configured on a session or row with @p radio_input (an RTL-family input),
 * @p digital (a digital mode or row) and @p am (an AM channel): a dynamic setting resolves to off without a radio
 * input or on a digital channel, NOISE resolves to AUTO with the same N on an AM channel, and every other setting runs
 * as written. Writes the result to @p out and returns why (dsd_squelch_resolution).
 */
int dsd_squelch_setting_resolve(const dsd_squelch_setting* configured, int radio_input, int digital, int am,
                                dsd_squelch_setting* out);

/** @brief The setting @p opts holds (rtl_squelch_mode, rtl_squelch_level, rtl_squelch_margin_db); off for NULL. */
dsd_squelch_setting dsd_squelch_setting_of_opts(const dsd_opts* opts);

/**
 * @brief Store @p setting in @p opts: its mode, and under LEVEL its level, under AUTO or NOISE its margin (the level
 * stays, for a switch back). Nothing is pushed to the receiver.
 */
void dsd_squelch_setting_store(dsd_opts* opts, const dsd_squelch_setting* setting);

/**
 * @brief Apply an input spec's `sql` field (`rtl:`, `rtltcp:`, `soapy:`, Airspy) in the squelch grammar.
 *
 * @return 0 when it set the squelch, 1 when `--squelch` already did (the command line wins), -1 when the field does not
 * read as a setting (nothing changes: a field that is not a squelch says nothing about it).
 */
int dsd_squelch_spec_field_apply(dsd_opts* opts, const char* text);

/**
 * @brief Publish the auto squelch's status into @p state for the frontends (dsd_state::squelch_auto_*), from the RTL
 * stream's (dsd_rtl_stream_io_hook_squelch_status()); without a stream it reads as not running. The floor goes on
 * rtl_squelch_level's scale (half the mean |z|^2: the channel power the level squelch compares), in hundredths of a dB.
 */
void dsd_squelch_publish_status(dsd_state* state);

/**
 * @brief Whether a dynamic squelch gates @p opts's monitor per sample: an AUTO or NOISE setting on an RTL-family input
 * (`rtl:`, `rtltcp:`, `soapy:`, Airspy, I/Q replay). Each sample then carries its gate (the RTL stream's flags), and
 * the level comparisons are off. Elsewhere the dynamic settings resolve to off.
 */
int dsd_squelch_dynamic_in_force(const dsd_opts* opts);

/** @brief The level the level comparisons use: rtl_squelch_level under LEVEL, 0 (off) under AUTO and NOISE. 0 for
 * NULL. */
double dsd_squelch_level_in_force(const dsd_opts* opts);

/** @brief Whether the level squelch is open: rtl_pwr above the level in force (always, under AUTO and NOISE). */
int dsd_squelch_level_open(const dsd_opts* opts);

/**
 * @brief Whether a monitor sample carrying @p flag is heard by the squelch: its own flag (DSD_SQUELCH_FLAG_CLOSED)
 * under a dynamic squelch (dsd_squelch_dynamic_in_force()), the level squelch otherwise.
 */
int dsd_squelch_gate_open(const dsd_opts* opts, uint8_t flag);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_SQUELCH_H_ */
