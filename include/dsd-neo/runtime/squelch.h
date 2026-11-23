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
#include <dsd-neo/runtime/scan_options.h>
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
 * stays, for a switch back). Nothing is pushed to the receiver. An applied analog default is no longer in the fields
 * afterwards (dsd_squelch_analog_default_note_written()).
 */
void dsd_squelch_setting_store(dsd_opts* opts, const dsd_squelch_setting* setting);

/**
 * @brief Apply an input spec's `sql` field in the squelch grammar, which dsd_spec_squelch_field_parse()
 * (`<dsd-neo/runtime/input_spec.h>`) has read.
 *
 * A field other than `0` is an explicit squelch: it ends the analog default (dsd_squelch_analog_default_end()).
 *
 * @return 0 when it set the squelch, 1 when `--squelch` already did (the command line wins), -1 when the field does not
 * read as a setting (nothing changes; the spec's reader refuses such a field first).
 */
int dsd_squelch_spec_field_apply(dsd_opts* opts, const char* text);

/**
 * @brief The whole dB the `[input] rtl_sql` key saves @p level (mean-power units) as (issue #899 B15): 0 for off (at or
 * below 0, or not a number), and for an active level the nearest whole dB held to -120..-1, never 0 (off): -0.25 dB and
 * a linear 1 (0 dB) are -1, a level under -120 dB is -120.
 *
 * @return 0 with the dB in @p out_db, -1 when @p out_db is NULL.
 */
int dsd_squelch_level_whole_db(double level, int* out_db);

/**
 * @brief Whether @p level is exactly what a whole dB from -120 to -1 makes (dsd_squelch_level_from_sql() of it, within
 * 1e-9 dB), or off: the levels dsd_squelch_level_whole_db() writes without rounding. The dB (0 for off) goes to
 * @p out_db when it is not NULL and the level has that form.
 *
 * @return 1 when it has the form, else 0.
 */
int dsd_squelch_level_is_whole_db(double level, int* out_db);

/** @brief Where the analog default squelch stands (dsd_opts::squelch_default_state, issue #899). */
typedef enum {
    DSD_SQUELCH_DEFAULT_IDLE = 0,    /**< not in the squelch fields: absent, or not in force on the channel now */
    DSD_SQUELCH_DEFAULT_APPLIED = 1, /**< the squelch fields run it; the base fields keep what they held before */
    DSD_SQUELCH_DEFAULT_ENDED = 2,   /**< a squelch was set explicitly this session, so it stays out */
} dsd_squelch_default_state;

/**
 * @brief Read `--squelch-analog-default`'s @p text into @p opts (issue #899): `auto[+N]` or `noise[+N]` (N 3..30, the
 * squelch grammar). No default is the option left out: `off` (or `0`) is refused, as a level is (the default opens on
 * what each channel shows, never at a fixed threshold).
 *
 * @return 0, or -1 with a short reason in @p err (when not NULL), @p opts untouched.
 */
int dsd_squelch_analog_default_parse(dsd_opts* opts, const char* text, char* err, size_t err_size);

/**
 * @brief Whether @p opts has an analog default that can still come in: one was given (not off) and no squelch was set
 * explicitly this session (DSD_SQUELCH_DEFAULT_ENDED). 0 for NULL.
 */
int dsd_squelch_analog_default_pending(const dsd_opts* opts);

/** @brief The analog default @p opts holds as a setting (off when it has none). */
dsd_squelch_setting dsd_squelch_analog_default_setting(const dsd_opts* opts);

/**
 * @brief Whether the analog default belongs in force on @p opts's channel now: it is pending
 * (dsd_squelch_analog_default_pending()), the input is a radio (RTL-SDR, rtl_tcp, SoapySDR, Airspy, I/Q replay), the
 * channel is the analog monitor (dsd_opts_is_analog_family(): a -fA/-fM session or an analog scan row; not EDACS nor
 * the M17 encoder) and the scan row on air, if any, sets no squelch of its own. Not while a scan scope is suspended
 * for a command (its resume decides). 0 for NULL @p opts.
 */
int dsd_squelch_analog_default_wanted(const dsd_opts* opts, const dsd_state* state);

/**
 * @brief The squelch gate a receiver runs on a channel (issue #899), the one resolver every stream open, retune and push
 * goes by: @p row's own squelch option (DSD_SCAN_OPT_SQUELCH, a scan row's, target's or signal-search range's) when it
 * has one; else the session's configured squelch (a scan scope's configured settings while one is in force on @p state,
 * else dsd_squelch_configured_fields()), with the analog default in its
 * place when the channel is the analog monitor (@p analog: its decode mode and family, from the profile being opened or
 * landed) on a radio input and the session still owns the default (dsd_squelch_analog_default_pending()). Nothing is
 * cached: each call resolves from @p opts as they stand. @p row may be NULL (no row); off for NULL @p opts.
 */
dsd_squelch_setting dsd_squelch_channel_gate(const dsd_opts* opts, const dsd_state* state,
                                             const dsd_scan_option_values* row, int analog);

/**
 * @brief Put the analog default in or out of @p opts's squelch fields as dsd_squelch_analog_default_wanted() says, on
 * the decoder thread, at each point the engine hands the receiver its squelch (the start, a decode or input change, a
 * scan row's commit, leave and resume): the caller pushes the fields to the receiver when this returns 1. Coming in it
 * keeps the fields' setting as the base (the level stays beneath, for a switch back); going out it puts the base back.
 * Also logs the AM notice each time a NOISE setting starts running as AUTO on a standalone AM monitor of a radio input,
 * whatever made it so.
 *
 * @return 1 when the squelch fields changed, or were written over an applied default since the last sync (the
 * receiver may still hold the default), else 0.
 */
int dsd_squelch_analog_default_sync(dsd_opts* opts, const dsd_state* state);

/**
 * @brief The squelch fields of a dsd_opts and where the analog default stands, whole: what a step that puts the
 * options back exactly as they were saves and restores (a staged scan row, a restart on the profile that ran). A
 * dsd_scan_settings capture holds the configured squelch instead (dsd_squelch_configured_fields()).
 */
typedef struct {
    double level;
    double base_level;
    int mode;
    int margin_db;
    int default_state;
    int default_moved;
    int base_mode;
    int base_margin_db;
} dsd_squelch_standing;

/** @brief Save @p opts's squelch standing into @p out (zeroed for NULL @p opts). */
void dsd_squelch_standing_capture(const dsd_opts* opts, dsd_squelch_standing* out);

/** @brief Put a dsd_squelch_standing_capture() back into @p opts exactly; nothing is pushed. */
void dsd_squelch_standing_restore(dsd_opts* opts, const dsd_squelch_standing* in);

/**
 * @brief Take an applied analog default out of @p opts's squelch fields, putting the base back (nothing pushed).
 *
 * @return 1 when the fields changed, else 0 (nothing was applied).
 */
int dsd_squelch_analog_default_release(dsd_opts* opts);

/**
 * @brief A squelch was set explicitly (`--squelch`, a spec's sql field other than `0`, a config's squelch keys, a live
 * edit from any frontend): the analog default ends for the session. While it was applied the base comes back first,
 * so the caller's own write lands over the session's setting.
 */
void dsd_squelch_analog_default_end(dsd_opts* opts);

/**
 * @brief The squelch fields of @p opts were just written with a setting of their own (a snapshot put back, a config's
 * value): an applied default is no longer in them. The next dsd_squelch_analog_default_sync() decides again.
 */
void dsd_squelch_analog_default_note_written(dsd_opts* opts);

/**
 * @brief The squelch @p opts runs apart from the analog default: the rtl_squelch_* fields, or the base while the
 * default is applied. What snapshots of the configured squelch and saves take. Any pointer may be NULL.
 */
void dsd_squelch_configured_fields(const dsd_opts* opts, double* level, int* mode, int* margin_db);

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
 * @brief Whether @p opts runs the AM monitor on its own (-fM, no -Y or trunk scan running): a session where a NOISE
 * setting has no FM channel to run on. On a radio input it runs as AUTO with the same N there, whatever set it (issue
 * #899 D4); on audio input it is off, and a frontend's request for it is refused. A scan's AM rows run a NOISE default
 * as AUTO too. A paused trunk scan runs none of its targets (@p state's
 * dsd_state::trunk_scan_paused, issue #671), so its manual AM session takes it as one with no scan does; a NULL
 * @p state reads as not paused. 0 for a NULL @p opts.
 */
int dsd_squelch_noise_has_no_fm(const dsd_opts* opts, const dsd_state* state);

/**
 * @brief Whether the session follows EDACS analog voice calls (issue #625): a selected EDACS session
 * (dsd_edacs_session(), so not Auto merely enabling ProVoice) with trunking on, off the analog family and the M17
 * encoder. On a radio input the auto squelch runs on the 9600 bit/s FSK path EDACS reads (a NOISE setting runs as AUTO
 * there, the FSK discriminator's output being in hertz on a fixed scale), and EDACS ends an analog call on its gate; on audio
 * input no dynamic squelch runs for it. 0 for NULL @p opts; a NULL @p state counts as no EDACS addressing selected.
 */
int dsd_squelch_edacs_analog_voice(const dsd_opts* opts, const dsd_state* state);

/**
 * @brief Whether a dynamic setting has no analog channel to run on (issue #625): neither the analog family, the M17
 * encoder's monitor nor EDACS analog voice (dsd_squelch_edacs_analog_voice()). CRC and FEC decide a digital channel's
 * traffic. 1 for NULL @p opts.
 */
int dsd_squelch_channel_digital(const dsd_opts* opts, const dsd_state* state);

/**
 * @brief The input kind a dynamic setting resolves against: dsd_squelch_input_kind(), except that the M17 encoder and
 * EDACS analog voice read as DSD_SQUELCH_INPUT_OTHER (no radio input) on audio input, where neither runs a dynamic
 * squelch: the PCM noise squelch serves the FM monitor only (issue #625).
 */
int dsd_squelch_dynamic_input_kind(const dsd_opts* opts, const dsd_state* state);

/**
 * @brief Whether the RTL stream's per-sample flags carry a dynamic squelch's gate for a reader of its own (EDACS analog
 * voice, the M17 encoder; issue #625): an AUTO or NOISE setting on a radio input, and the stream's status says its
 * last block ran the tracker or the noise squelch with a valid plan. A squelch without a plan keeps every flag open,
 * and so does a stream that runs none, so a reader falls back to what it does with no squelch. 0 for NULL.
 */
int dsd_squelch_stream_gate_running(const dsd_opts* opts, const dsd_state* state);

/**
 * @brief Whether an analog reception on @p opts's input may become a call, with its history row and recording (issue
 * #667 D2). Not on a radio input whose squelch lets noise through: no dynamic squelch in force
 * (dsd_squelch_dynamic_in_force()), a level threshold at or below the startup default (DSD_SQUELCH_STARTUP_DB, off
 * included; dsd_squelch_level_in_force()), and no CTCSS/DCS allow list in force on the FM monitor
 * (dsd_analog_tone_detection_active()), which only a listed tone passes. Such a squelch opens for the receiver's own
 * noise, which would hold one reception open all session. Audio input (and any input that is not a radio) is always
 * allowed: its silence closes the carrier, and another receiver may squelch it. Reads @p opts as it stands, a scan
 * row's own squelch and tone policy included. 0 for NULL @p opts.
 */
int dsd_squelch_analog_calls_allowed(const dsd_opts* opts, const dsd_state* state);

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
