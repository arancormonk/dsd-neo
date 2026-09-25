// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */

/**
 * @file
 * @brief Private engine helpers the -Y scanner and the trunk-scan coordinator share for analog rows (issue #526).
 *
 * Defined in channel_scan.c; only channel_scan.c and trunk_scan.c call them, and trunk_tuning.c reads the channel
 * filter override inline.
 */
#ifndef DSD_NEO_SRC_ENGINE_SCAN_ANALOG_INTERNAL_H_
#define DSD_NEO_SRC_ENGINE_SCAN_ANALOG_INTERNAL_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/scan_options.h>
#include <stddef.h>

/** What dsd_engine_scan_warn_analog_width() found about a row's width. */
enum {
    DSD_ENGINE_SCAN_WIDTH_OK = 0,        /**< Nothing to say. */
    DSD_ENGINE_SCAN_WIDTH_NO_EFFECT = 1, /**< Warned: audio input has no demodulator to apply the width. */
    DSD_ENGINE_SCAN_WIDTH_SKIPPED = 2,   /**< Warned: the front end refuses the width, so the row is skipped. */
};

/** Whether DSD_NEO_CHANNEL_LPF=0 turns off the channel filter every explicit analog width needs
 * (dsd_analog_channel_lpf_off_check()): the RTL front end then refuses any such width, at any rate. */
static inline int
dsd_engine_scan_channel_lpf_forced_off(void) {
    const dsdneoRuntimeConfig* env = dsd_neo_get_config();
    return (env && env->channel_lpf_is_set && env->channel_lpf_enable == 0) ? 1 : 0;
}

/** Open the audio sink the row now on air plays through: the analog monitor's for an analog row, the digital voice
 * output otherwise. A list mixing the families needs whichever the session did not open; each is opened once,
 * idempotently, and a failure is logged once and leaves that row silent. Call after the row's options. */
void dsd_engine_scan_ensure_output(dsd_opts* opts);

/** Warn, as one WARNING line beginning with @p label ("Scan channel 2 (154.430000 MHz)", "Trunk scan target 'fire'"),
 * about an analog (nfm) scan row or target whose squelch -- its own --squelch-db, else the configured one -- is off or
 * at -100 dB or below: noise then holds it on air until the visit cap or a manual advance or avoid moves on. Said once
 * when a scan (or a new map or target list) starts; it does not depend on the DSP rate. @p row may be NULL (no
 * options). Returns 1 when it warned, else 0. */
int dsd_engine_scan_warn_analog_squelch(const dsd_opts* opts, const dsd_state* state, const dsd_scan_option_values* row,
                                        const char* label);

/** Whether the RTL front end refuses analog width @p width_hz (explicit, > 0) of demodulator @p kind on this input
 * (issue #526): DSD_NEO_CHANNEL_LPF=0 turns off the channel filter it needs, at any rate, or DSP rate @p dsp_rate_hz
 * (> 0; 0 skips the rate check) cannot filter it. A scan row with such a width is skipped at every visit. @p why, when
 * given, receives the validator's text with the fix this input allows (dsd_analog_width_check_at()), as the stream's
 * own refusal words it. 0 on audio input, which applies no width. */
int dsd_engine_scan_width_refused(const dsd_opts* opts, int kind, int width_hz, int dsp_rate_hz, char* why,
                                  size_t why_size);

/** Warn, in the same form, about the width an analog row runs, which is then skipped at every visit: its own
 * --nfm-bandwidth-hz on an input with no demodulator to apply it, or one the front end refuses
 * (dsd_engine_scan_width_refused(): DSD_NEO_CHANNEL_LPF=0, or @p dsp_rate_hz cannot filter it); or, for a row that
 * sets none, the configured NFM width it runs (dsd_engine_scan_configured_nfm_width_hz()) where the front end refuses
 * that. Said when a scan starts, again whenever the DSP rate changes, and for the rows without a width of their own
 * whenever the configured NFM width does; @p dsp_rate_hz 0 skips the rate check. @p row may be NULL (no options).
 * Returns DSD_ENGINE_SCAN_WIDTH_OK, _NO_EFFECT or _SKIPPED; for _SKIPPED, @p brief (when given) receives a short
 * reason for the status line ("NFM 20 kHz does not fit the 16 kHz DSP rate"). */
int dsd_engine_scan_warn_analog_width(const dsd_opts* opts, const dsd_state* state, const dsd_scan_option_values* row,
                                      int dsp_rate_hz, const char* label, char* brief, size_t brief_size);

/** Put the rows a width check found skipped at every visit on the status line every frontend shows (the terminal,
 * Qt and Android), as the input-level advisories are: @p label and @p brief name the first of @p skipped rows; the
 * WARNING lines name each of them in the log. Nothing when @p skipped is 0. */
void dsd_engine_scan_note_skipped_rows(dsd_state* state, int skipped, const char* label, const char* brief);

/** The explicit NFM width an analog row without one of its own runs: the configured width, from the scan scope's
 * configured view while one is live and dsd_opts otherwise; 0 for the unset default, which no DSP rate refuses, and
 * when @p opts is NULL. */
int dsd_engine_scan_configured_nfm_width_hz(const dsd_opts* opts, const dsd_state* state);

#endif /* DSD_NEO_SRC_ENGINE_SCAN_ANALOG_INTERNAL_H_ */
