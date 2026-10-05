// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */

/**
 * @file
 * @brief Private engine helpers the -Y scanner and the trunk-scan coordinator share for analog rows (issue #526).
 *
 * Defined in channel_scan.c; only channel_scan.c and trunk_scan.c call them, and trunk_tuning.c reads the channel
 * filter override and the held width inline and asks for the row being tuned (dsd_engine_scan_tuning_row_options()).
 */
#ifndef DSD_NEO_SRC_ENGINE_SCAN_ANALOG_INTERNAL_H_
#define DSD_NEO_SRC_ENGINE_SCAN_ANALOG_INTERNAL_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/runtime/analog_channel.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/scan_options.h>
#include <stddef.h>

/** What dsd_engine_scan_warn_analog_width() found about a row's width. */
enum {
    DSD_ENGINE_SCAN_WIDTH_OK = 0,        /**< Nothing to say. */
    DSD_ENGINE_SCAN_WIDTH_NO_EFFECT = 1, /**< Warned: audio input with no rigctl peer to take the width. */
    DSD_ENGINE_SCAN_WIDTH_SKIPPED = 2,   /**< Warned: the front end refuses the width, or nothing demodulates the am
                                              row's AM, so the row is skipped. */
};

/** Whether DSD_NEO_CHANNEL_LPF=0 turns off the channel filter every explicit analog width needs
 * (dsd_analog_channel_lpf_off_check()): the RTL front end then refuses any such width, at any rate. */
static inline int
dsd_engine_scan_channel_lpf_forced_off(void) {
    const dsdneoRuntimeConfig* env = dsd_neo_get_config();
    return (env && env->channel_lpf_is_set && env->channel_lpf_enable == 0) ? 1 : 0;
}

/** The width a DSP rate holds of analog demodulator @p kind at width @p width_hz (0: the kind's default): an explicit
 * width, or the AM default, which always runs its channel filter (issue #524); 0 for the unset NFM default, which keeps
 * its historical filter rule and is never refused. */
static inline int
dsd_engine_scan_held_width_hz(int kind, int width_hz) {
    if (width_hz > 0) {
        return width_hz;
    }
    return kind == DSD_ANALOG_DEMOD_AM ? DSD_ANALOG_AM_WIDTH_DEFAULT_HZ : 0;
}

/** Open the audio sink the row now on air plays through: the analog monitor's for an analog row, the digital voice
 * output otherwise. A list mixing the families needs whichever the session did not open; each is opened once,
 * idempotently, and a failure is logged once and leaves that row silent. Call after the row's options. */
void dsd_engine_scan_ensure_output(dsd_opts* opts);

/** Warn, as one WARNING line beginning with @p label ("Scan channel 2 (154.430000 MHz)", "Trunk scan target 'fire'"),
 * about an analog (nfm or am, @p am) scan row or target whose squelch -- its own --squelch-db, else the configured one
 * -- is off or at -100 dB or below, or is a dynamic one that is off there (auto off a radio input, noise on AM audio):
 * noise then holds it on air until the visit cap or a manual advance or avoid moves on. Said once when a scan (or a
 * new map or target list) starts; it does not depend on the DSP rate. @p row may be NULL (no options). Returns 1 when
 * it warned, else 0. */
int dsd_engine_scan_warn_analog_squelch(const dsd_opts* opts, const dsd_state* state, const dsd_scan_option_values* row,
                                        int am, const char* label);

/** Whether the RTL front end refuses analog width @p width_hz (0: the kind's default) of demodulator @p kind on this
 * input (issue #526): DSD_NEO_CHANNEL_LPF=0 turns off the channel filter it needs, at any rate, or DSP rate
 * @p dsp_rate_hz (> 0; 0 skips the rate check) cannot filter it. An explicit width can be refused, and so can the AM
 * default (6 kHz), which always runs its channel filter (issue #524); the unset NFM default keeps its historical filter
 * rule and never is. A scan row with such a width is skipped at every visit. @p why, when given, receives the
 * validator's text with the fix this input allows (dsd_analog_width_check_at()), as the stream's own refusal words it.
 * 0 on audio input, which applies no width. */
int dsd_engine_scan_width_refused(const dsd_opts* opts, int kind, int width_hz, int dsp_rate_hz, char* why,
                                  size_t why_size);

/** Warn, in the same form, about the width an analog row of demodulator @p kind (dsd_analog_demod: an nfm row's FM, an
 * am row's AM) runs, which is then skipped at every visit: its own --nfm-bandwidth-hz or --am-bandwidth-hz on audio
 * input with no rigctl peer to hand it to, or one the front end refuses (dsd_engine_scan_width_refused():
 * DSD_NEO_CHANNEL_LPF=0, or @p dsp_rate_hz cannot filter it); or, for a row that sets none, the configured width of its
 * kind it runs (dsd_scan_mode_configured_analog_width()) where the front end refuses that. An am row on audio input with
 * no rigctl peer, which nothing demodulates as AM, is named as skipped at every visit instead, whatever its width (issue
 * #526). Said when a scan starts, again whenever the DSP rate changes, and for the rows without a width of their own
 * whenever the configured width of their kind does; @p dsp_rate_hz 0 skips the rate check. @p row may be NULL (no
 * options). Returns DSD_ENGINE_SCAN_WIDTH_OK, _NO_EFFECT or _SKIPPED; for _SKIPPED, @p brief (when given) receives a
 * short reason for the status line ("NFM 20 kHz does not fit the 16 kHz DSP rate"). */
int dsd_engine_scan_warn_analog_width(const dsd_opts* opts, const dsd_state* state, const dsd_scan_option_values* row,
                                      int kind, int dsp_rate_hz, const char* label, char* brief, size_t brief_size);

/** The options -Y channel map @p row runs: its profile's with the operator's session edit laid over them (issue #518);
 * NULL for a row with neither. @p scratch holds a merged copy. channel_scan.c. */
const dsd_scan_option_values* dsd_engine_channel_scan_row_values(const dsd_state* state, int row,
                                                                 dsd_scan_option_values* scratch);

/** Whether the width an analog row of demodulator @p kind runs -- its own width, else the configured width of its kind
 * -- is one the front end refuses at @p dsp_rate_hz (dsd_engine_scan_width_refused()), or the row is an am row nothing
 * demodulates as AM (audio input with no rigctl peer), either of which skips the row at every visit. Quiet:
 * dsd_engine_scan_warn_analog_width() is what names it in the log. For a skipped row, @p brief (when given) receives
 * the status-line reason. @p row may be NULL (no options). */
int dsd_engine_scan_analog_width_skipped(const dsd_opts* opts, const dsd_state* state,
                                         const dsd_scan_option_values* row, int kind, int dsp_rate_hz, char* brief,
                                         size_t brief_size);

/** Whether a width check that found the configured widths changed must name the row of demodulator @p kind with
 * @p row's options again, rather than only count it: every row after the DSP rate changed (@p configured_changed 0),
 * else a row without a width of its own whose kind's configured width changed (@p configured_changed holds
 * 1 << dsd_analog_demod for each kind whose configured width changed). */
static inline int
dsd_engine_scan_row_named_again(const dsd_scan_option_values* row, int kind, unsigned configured_changed) {
    if (configured_changed == 0U) {
        return 1;
    }
    return !(row && (row->present & DSD_SCAN_OPT_BANDWIDTH)) && kind >= 0
           && (configured_changed & (1U << (unsigned)kind)) != 0U;
}

/** The nonsecret options of the scan row a scanner is tuning or has on air (issue #526): the -Y scanner's staged row,
 * whose options are installed only once its tune lands, else the row options the scan scope has installed (a trunk-scan
 * target's, applied before its retune). NULL outside a scan and for a row without options. The rigctl leg of a tune
 * reads a row's own channel width from here, which the settings in force cannot tell from the configured one. */
const dsd_scan_option_values* dsd_engine_scan_tuning_row_options(const dsd_opts* opts, const dsd_state* state);

/** The rows (or trunk-scan targets) a width check found skipped at every visit: how many, and the first one's label and
 * status-line reason. Zero-initialize it, add each skipped row in order, then put it on the status line. */
typedef struct {
    int count;
    char label[96];
    char brief[DSD_ANALOG_ERROR_TEXT_MAX];
} dsd_engine_scan_skipped;

/** Count one more skipped row, keeping @p label and @p brief when it is the first. */
void dsd_engine_scan_skipped_add(dsd_engine_scan_skipped* skipped, const char* label, const char* brief);

/** Put the rows a width check found skipped at every visit on the status line every frontend shows (the terminal,
 * Qt and Android), as the input-level advisories are: the first one's label and reason, and how many more; the WARNING
 * lines name each of them in the log. Nothing when none was skipped. */
void dsd_engine_scan_note_skipped_rows(dsd_state* state, const dsd_engine_scan_skipped* skipped);

#endif /* DSD_NEO_SRC_ENGINE_SCAN_ANALOG_INTERNAL_H_ */
