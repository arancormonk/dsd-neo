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
 * The dynamic squelches (AUTO, NOISE) need an analog channel: a digital channel never shows its noise (its CRC/FEC
 * gates it anyway), and there they resolve to off. AUTO needs a radio input: on audio input FM noise is louder than
 * voice, so a floor plus a margin would read backwards. NOISE needs FM: on a radio input's AM channel it resolves to
 * AUTO with the same N; on audio input (issue #628) it runs on the FM monitor through the PCM noise squelch, which
 * learns the input's noise (src/dsp/pcm_noise_squelch.c), and is off on AM audio, where no AUTO can stand in
 * (dsd_squelch_setting_resolve()).
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
    DSD_SQUELCH_RESOLVED_NO_RADIO,   /**< AUTO off audio input, or a dynamic setting on neither input kind: off */
    DSD_SQUELCH_RESOLVED_DIGITAL,    /**< a dynamic setting on a digital channel: off */
    DSD_SQUELCH_RESOLVED_AM_AUTO,    /**< NOISE on a radio input's AM channel: AUTO with the same N */
    DSD_SQUELCH_RESOLVED_AUDIO_AM,   /**< NOISE on AM audio input: off */
} dsd_squelch_resolution;

/** @brief The input a setting runs on (dsd_squelch_input_kind()). */
typedef enum {
    DSD_SQUELCH_INPUT_OTHER = 0, /**< neither: a symbol file, no input */
    DSD_SQUELCH_INPUT_RADIO = 1, /**< an RTL-family input: DSD-neo runs the discriminator */
    DSD_SQUELCH_INPUT_AUDIO = 2, /**< PCM audio input (Pulse, a file, stdin, TCP, UDP): the discriminator ran outside */
} dsd_squelch_input;

/** @brief What the PCM noise squelch knows (dsd_state::squelch_noise_state; 0 on radio input and when it is not
 * running). */
typedef enum {
    DSD_SQUELCH_NOISE_STATE_NONE = 0,
    DSD_SQUELCH_NOISE_STATE_LEARNING = 1,    /**< no reference yet: closed */
    DSD_SQUELCH_NOISE_STATE_PROVISIONAL = 2, /**< a reference from the first steady stretch */
    DSD_SQUELCH_NOISE_STATE_KNOWN = 3,       /**< a reference a transition confirmed as noise */
    DSD_SQUELCH_NOISE_STATE_NO_BAND = 4,     /**< nothing above voice to measure: off */
    DSD_SQUELCH_NOISE_STATE_NO_ROOM = 5,     /**< the input's rate leaves no band: off */
} dsd_squelch_noise_state;

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
 * @brief The setting that runs for @p configured on a session or row with @p input (dsd_squelch_input: 1 a radio
 * input, 2 audio input, 0 neither), @p digital (a digital mode or row) and @p am (an AM channel): a dynamic setting
 * resolves to off on a digital channel or on neither input kind; AUTO resolves to off on audio input; NOISE resolves
 * to AUTO with the same N on a radio input's AM channel and to off on AM audio; every other setting runs as written.
 * Writes the result to @p out and returns why (dsd_squelch_resolution).
 */
int dsd_squelch_setting_resolve(const dsd_squelch_setting* configured, int input, int digital, int am,
                                dsd_squelch_setting* out);

/** @brief The input kind @p opts runs (dsd_squelch_input): DSD_SQUELCH_INPUT_OTHER for NULL. */
int dsd_squelch_input_kind(const dsd_opts* opts);

/**
 * @brief Whether @p opts runs the PCM noise squelch (issue #628): a NOISE setting on audio input whose FM analog
 * monitor reads it (dsd_analog_tone_detection_active()). Whether it gates is dsd_squelch_dynamic_in_force(): not while
 * it has no band or no room. 0 for NULL.
 */
int dsd_squelch_pcm_noise_in_force(const dsd_opts* opts);

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
 * @brief Publish the dynamic squelch's status into @p state for the frontends (dsd_state::squelch_auto_*,
 * squelch_noise_*). On radio input (or with @p opts NULL) it is the RTL stream's
 * (dsd_rtl_stream_io_hook_squelch_status()), not running without a stream; the floor goes on rtl_squelch_level's scale
 * (half the mean |z|^2: the channel power the level squelch compares), in hundredths of a dB. While the PCM noise
 * squelch is in force (dsd_squelch_pcm_noise_in_force()) the fields are its own, which it writes on the decoder thread,
 * and are left alone. Everywhere else they are cleared.
 */
void dsd_squelch_publish_status(const dsd_opts* opts, dsd_state* state);

/**
 * @brief Whether a dynamic squelch gates @p opts's monitor per sample: an AUTO or NOISE setting on an RTL-family input
 * (`rtl:`, `rtltcp:`, `soapy:`, Airspy, I/Q replay), whose stream's flags carry each sample's gate; or the PCM noise
 * squelch in force (dsd_squelch_pcm_noise_in_force()) and available, as @p state publishes it (learning, or holding a
 * reference; not with no band or no room, nor with @p state NULL), whose flags the monitor's capture attaches. The
 * level comparisons are then off. Elsewhere the dynamic settings resolve to off.
 */
int dsd_squelch_dynamic_in_force(const dsd_opts* opts, const dsd_state* state);

/**
 * @brief Whether @p opts runs the AM monitor on its own (-fM, no -Y or trunk scan): a session where a NOISE setting
 * has no FM channel to run on. A --squelch noise is refused there, and so is a frontend's request for it; a scan's AM
 * rows run a NOISE default as AUTO instead. 0 for NULL.
 */
int dsd_squelch_noise_has_no_fm(const dsd_opts* opts);

/** @brief The level the level comparisons use: rtl_squelch_level under LEVEL, 0 (off) under AUTO and NOISE. 0 for
 * NULL. */
double dsd_squelch_level_in_force(const dsd_opts* opts);

/** @brief Whether the level squelch is open: rtl_pwr above the level in force (always, under AUTO and NOISE). */
int dsd_squelch_level_open(const dsd_opts* opts);

/**
 * @brief Whether the monitor's per-sample flags carry the gate: a dynamic squelch in force
 * (dsd_squelch_dynamic_in_force()), or the PCM noise squelch running at all (dsd_squelch_pcm_noise_in_force()), whose
 * flags read open while it has no band or no room. So a block the squelch closed until its last sample stays closed
 * when the decision that follows is "no band", rather than playing whole.
 */
int dsd_squelch_flags_in_force(const dsd_opts* opts, const dsd_state* state);

/**
 * @brief Whether a monitor sample carrying @p flag is heard by the squelch: its own flag (DSD_SQUELCH_FLAG_CLOSED)
 * while the flags carry the gate (dsd_squelch_flags_in_force()), the level squelch otherwise.
 */
int dsd_squelch_gate_open(const dsd_opts* opts, const dsd_state* state, uint8_t flag);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_SQUELCH_H_ */
