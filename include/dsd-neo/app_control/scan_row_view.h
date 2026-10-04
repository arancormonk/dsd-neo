// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */

/**
 * @file
 * @brief The scan row on air as the "this channel" editors see it, and the words they use (issue #518).
 *
 * Every frontend asks the same questions before it offers a session edit of the row on air -- is there one, which
 * fields can it take, what is it called, what does it run now -- and says the same thing afterwards. This view answers
 * from the published dsd_state::scan_row_* fields, on the decoder thread or a frontend snapshot alike.
 */
#ifndef DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_SCAN_ROW_VIEW_H_
#define DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_SCAN_ROW_VIEW_H_

#include <dsd-neo/app_control/commands.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/runtime/scan_row_edit.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** A short name for a row in a notice: "county-p25", "Fire Dispatch", "Ch 3 (154.430000 MHz)". */
enum { DSD_APP_SCAN_ROW_LABEL_SIZE = 64 };

/** Room for any notice the view writes (it fits dsd_state::ui_msg). */
enum { DSD_APP_SCAN_ROW_NOTICE_SIZE = 128 };

typedef struct {
    /** 1 when a scan row is on air (the rest is meaningful only then). */
    int active;
    int scanner; /**< dsd_scan_row_scanner */
    uint32_t session;
    int row;
    unsigned int mode; /**< the row's dsd_scan_mode */
    uint32_t editable; /**< DSD_SCAN_ROW_FIELD_* it can take */
    uint32_t edited;   /**< ... it runs a session edit of */
    uint32_t listed;   /**< ... its list row sets itself */
    /** 1 when the options snapshot was taken under this row's scope (dsd_opts::scan_row_scope_seq equals
        dsd_state::scan_row_scope_seq), so the values an editor reads from it to start from are this row's: the two
        snapshots are published one after the other, and a reader between them sees one row's options with the other's
        identity. */
    int opts_match;
    char target_id[64]; /**< a --trunk-scan target's id, "" for a -Y row */
    char label[DSD_APP_SCAN_ROW_LABEL_SIZE];
} dsd_app_scan_row_view;

/** Fill @p out from @p opts and @p state. Returns 0, or -1 for NULL arguments (@p out zeroed when given). */
int dsd_app_scan_row_view_get(const dsd_opts* opts, const dsd_state* state, dsd_app_scan_row_view* out);

/** Whether the row on air takes a session edit of @p field (one DSD_SCAN_ROW_FIELD_*), with the options snapshot its
 * editor starts from being this row's (opts_match). */
int dsd_app_scan_row_view_offers(const dsd_app_scan_row_view* view, uint32_t field);

/** Fill @p out with a DSD_APP_CMD_SCAN_ROW_EDIT naming @p view's row, for @p field and @p action (the values zeroed).
 * Returns 0, or -1 for NULL arguments or no row on air. */
int dsd_app_scan_row_view_payload(const dsd_app_scan_row_view* view, uint32_t field, int action,
                                  dsd_app_scan_row_edit_payload* out);

/** The label a notice uses for a row: a --trunk-scan target's id, else the -Y row's name, else
 * "Ch N (F MHz)". */
int dsd_app_scan_row_label(const dsd_state* state, int scanner, int row, const char* target_id, char* out,
                           size_t out_size);

/** A field's name in a notice: "squelch", "NFM bandwidth" or "AM bandwidth" (by @p mode), "tone filter", "RTL gain". */
const char* dsd_app_scan_row_field_name(uint32_t field, unsigned int mode);

/** The value of a SET edit as a notice says it: "-55 dB" or "off", "12.5 kHz", "allow 100.0 Hz/D023N" or "off"
 * (ASCII), "20 dB" or "AGC". */
int dsd_app_scan_row_value_text(uint32_t field, const dsd_scan_row_edit_value* value, char* out, size_t out_size);

/**
 * @brief The notice after a session edit with @p status (dsd_scan_row_edit_status) of @p field of the row @p label of
 * class @p mode:
 *
 * - "This channel (county-p25): squelch -55 dB for this session" (APPLIED) or "... from its next visit" (STORED);
 * - "This channel (county-p25): squelch follows the default" for INHERIT, "...: squelch back to the list value" for
 *   RESET, each with " from its next visit" when STORED;
 * - "Refused: this channel's squelch: <why>" (REFUSED), "Refused: the scan changed; nothing applied" (STALE),
 *   "Refused: no scan is running" (UNAVAILABLE), "Busy: the scan is retuning; try again" (BUSY).
 *
 * @p value_text is the SET value (dsd_app_scan_row_value_text()); @p why the refusal's reason. Returns 0, or -1 for a
 * NULL or empty @p out.
 */
int dsd_app_scan_row_notice(const char* label, uint32_t field, unsigned int mode, int action, const char* value_text,
                            int status, const char* why, char* out, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_SCAN_ROW_VIEW_H_ */
