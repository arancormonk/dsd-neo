// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */

/**
 * @file
 * @brief Session edits of one scan row's own settings ("this channel"), laid over the row the list gives.
 *
 * A scanner row (a --trunk-scan target or a typed -Y channel map row) carries the settings its `options` cell sets
 * (dsd_scan_option_values). While a scan runs the operator can change the squelch, channel width and tone policy a
 * row runs, and a trunk-scan target's tuner gain, for the rest of the session without touching the list or the
 * configured defaults. Each field of an edit is in one of three states: no edit (the row runs what the list says),
 * set (the row runs the edit's value) or inherited (the row follows the configured default, as if the list set
 * nothing). Pure data: no allocation, no global state.
 */
#ifndef DSD_NEO_RUNTIME_SCAN_ROW_EDIT_H
#define DSD_NEO_RUNTIME_SCAN_ROW_EDIT_H

#include <dsd-neo/core/analog_tone.h>
#include <dsd-neo/runtime/scan_options.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** The fields a session edit can change. */
enum {
    DSD_SCAN_ROW_FIELD_SQUELCH = 1U << 0, /**< squelch, the row's --squelch-db */
    DSD_SCAN_ROW_FIELD_WIDTH = 1U << 1,   /**< channel width, the row's --nfm-bandwidth-hz / --am-bandwidth-hz */
    DSD_SCAN_ROW_FIELD_TONE = 1U << 2,    /**< CTCSS/DCS policy, the row's --tone-allow / --tone-block / ... */
    DSD_SCAN_ROW_FIELD_GAIN = 1U << 3,    /**< tuner gain, a trunk-scan target's rtl_gain column */
    DSD_SCAN_ROW_FIELDS_ALL = DSD_SCAN_ROW_FIELD_SQUELCH | DSD_SCAN_ROW_FIELD_WIDTH | DSD_SCAN_ROW_FIELD_TONE
        | DSD_SCAN_ROW_FIELD_GAIN,
};

/** What a change does to a field. */
typedef enum {
    DSD_SCAN_ROW_EDIT_SET = 1,     /**< run the given value for the rest of the session */
    DSD_SCAN_ROW_EDIT_INHERIT = 2, /**< follow the configured default, whatever the list sets */
    DSD_SCAN_ROW_EDIT_RESET = 3,   /**< drop the edit: back to what the list sets */
} dsd_scan_row_edit_action;

/** The values a set field runs. Each is meaningful only while its field is set. */
typedef struct {
    /** Whole dB from -100 to 0 in the rtl_sql convention; 0 = off. A level squelch's. */
    int squelch_db;
    /** The squelch's mode (dsd_squelch_mode): LEVEL (squelch_db) or AUTO, a margin of squelch_margin_db (3..30) over the
     * floor the demodulator learns, which an nfm or am row alone takes (issue #518 follow-up). */
    int squelch_mode;
    int squelch_margin_db;
    /** Full RF channel width in Hz, of the analog demodulator the row's class runs (runtime/analog_channel.h ranges). */
    int width_hz;
    /** dsd_tone_filter_mode, with its list (empty for OFF). */
    int tone_filter;
    dsd_tone_set tone_set;
    /** Tuner gain in whole dB, 1..49, or 0 for the tuner's AGC. */
    int gain_db;
} dsd_scan_row_edit_value;

/** One row's session edits. A field is in at most one of @p set and @p inherit. Zeroed means no edits. */
typedef struct {
    uint32_t set;
    uint32_t inherit;
    dsd_scan_row_edit_value value;
} dsd_scan_row_edit;

/** Largest tuner gain an edit takes, in whole dB (the RTL gain controls' range). */
#define DSD_SCAN_ROW_EDIT_GAIN_MAX_DB 49

/** The scanner a row belongs to (dsd_state::scan_row_scanner). */
typedef enum {
    DSD_SCAN_ROW_SCANNER_NONE = 0,
    DSD_SCAN_ROW_SCANNER_TRUNK_SCAN = 1,   /**< a --trunk-scan target */
    DSD_SCAN_ROW_SCANNER_CHANNEL_SCAN = 2, /**< a row of the -Y channel map */
} dsd_scan_row_scanner;

/** What became of a session edit. */
typedef enum {
    DSD_SCAN_ROW_EDIT_APPLIED = 1,      /**< the row is on air and runs the edit now */
    DSD_SCAN_ROW_EDIT_STORED = 2,       /**< the row is not on air; it runs the edit from its next visit */
    DSD_SCAN_ROW_EDIT_REFUSED = -1,     /**< the row cannot take that field or value (the result's err says why) */
    DSD_SCAN_ROW_EDIT_STALE = -2,       /**< no such row in the scan running now: the scan or its list changed */
    DSD_SCAN_ROW_EDIT_UNAVAILABLE = -3, /**< no such scanner runs */
    DSD_SCAN_ROW_EDIT_BUSY = -4,        /**< the scan was busy (the P25 SM tick guard was held); nothing changed */
} dsd_scan_row_edit_status;

#define DSD_SCAN_ROW_EDIT_ERROR_SIZE 160

/** What an accepted edit leaves the caller to do, and what to put back should that fail. */
typedef struct {
    /** Nonzero when the width in force on air changed: the caller asks the front end for it. */
    int publish_width;
    /** Nonzero when the tuner gain in force changed: the caller reopens the stream, which applies it. */
    int restart_gain;
    /** The row's edits before this change (dsd_engine_*_restore_*() puts them back). */
    dsd_scan_row_edit previous;
    /** Why an edit was refused; empty otherwise. */
    char err[DSD_SCAN_ROW_EDIT_ERROR_SIZE];
} dsd_scan_row_edit_result;

/** A new scan session number for a scanner starting, never 0 (decoder thread). */
uint32_t dsd_scan_row_edit_new_session(void);

/**
 * @brief The fields a row of class @p mode (dsd_scan_mode) can take a session edit of: the ones its `options` cell
 * could set -- squelch on every class, the channel width on nfm and am, the tone policy on nfm -- and the tuner gain
 * when @p gain_editable says the row runs a gain of its own (a trunk-scan target on an input whose gain the scan
 * sets per target). A legacy untyped -Y row (DSD_SCAN_MODE_INHERIT) takes none.
 */
uint32_t dsd_scan_row_edit_fields(unsigned int mode, int gain_editable);

/**
 * @brief Whether @p value is valid for @p field on a row of class @p mode: squelch in -100..0, a width in the range of
 * the class's demodulator, a tone mode of the three with the list it needs (allow and block need one), a gain in
 * 0..DSD_SCAN_ROW_EDIT_GAIN_MAX_DB. Returns 1 when valid, else 0 with a short reason in @p err (when not NULL), never
 * echoing the value's text.
 */
int dsd_scan_row_edit_value_valid(unsigned int mode, uint32_t field, const dsd_scan_row_edit_value* value, char* err,
                                  size_t err_size);

/**
 * @brief Apply @p action to one @p field of @p edit: set it to @p value's (SET; @p value is not checked here, see
 * dsd_scan_row_edit_value_valid()), inherit the default (INHERIT) or drop the edit (RESET).
 *
 * @return 0, or -1 when @p edit is NULL, @p field is not exactly one field, @p action is unknown or SET has no
 * @p value (@p edit untouched).
 */
int dsd_scan_row_edit_change(dsd_scan_row_edit* edit, uint32_t field, dsd_scan_row_edit_action action,
                             const dsd_scan_row_edit_value* value);

/**
 * @brief Make the @p fields of @p edit those of @p from -- set, inherited or neither, and the value a set one carries --
 * leaving its other fields as they are: a rollback of one field that keeps edits made to the others since. No-op for
 * NULL arguments.
 */
void dsd_scan_row_edit_take_fields(dsd_scan_row_edit* edit, const dsd_scan_row_edit* from, uint32_t fields);

/**
 * @brief Whether a row's options @p before and @p after (NULL = none) ask a receiver for a different channel width: a
 * width of their own that differs in value or kind, or one where the other sets none. A rigctl peer that demodulates
 * audio input is asked for the row's own width, else the configured width of its kind (NFM: -B, or the peer's own,
 * when unset; issue #621), so the width the decoder runs can stay the same while the peer's request changes -- from
 * the source if not the value (a row's own 12.5 kHz to a configured 12.5 kHz), which the peer's cache deduplicates.
 */
int dsd_scan_row_edit_width_request_differs(const dsd_scan_option_values* before, const dsd_scan_option_values* after);

/** The fields a list row sets itself: those of @p row's options (NULL = none) an edit can change, and the gain when
 * @p list_gain_is_set (a trunk-scan target's rtl_gain column). */
uint32_t dsd_scan_row_edit_fields_listed(const dsd_scan_option_values* row, int list_gain_is_set);

/** The fields @p edit sets or inherits (0 for NULL). */
uint32_t dsd_scan_row_edit_fields_edited(const dsd_scan_row_edit* edit);

/**
 * @brief The options a row of class @p mode runs: @p row (NULL = none) with @p edit (NULL = none) laid over its
 * squelch, width and tone policy. A set field takes the edit's value and its option bit; an inherited one loses the
 * bit, so the row follows the configured default. Every other field is @p row's. @p out may not alias @p row.
 */
void dsd_scan_row_edit_apply(const dsd_scan_option_values* row, const dsd_scan_row_edit* edit, unsigned int mode,
                             dsd_scan_option_values* out);

/**
 * @brief The tuner gain a trunk-scan target runs: its list gain (@p list_is_set, @p list_db; 0 dB = AGC) with
 * @p edit's gain field over it. Writes whether the target runs a gain of its own to @p out_is_set and the gain to
 * @p out_db.
 */
void dsd_scan_row_edit_gain(int list_is_set, int list_db, const dsd_scan_row_edit* edit, int* out_is_set, int* out_db);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_RUNTIME_SCAN_ROW_EDIT_H */
