// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The terminal's Unit Activity section without curses: the newest rows of the unit activity log, read from a UI
 * snapshot through the app-control view (<dsd-neo/app_control/unit_activity_view.h>), and the text of each row's lines,
 * so the terminal shows the same labels, names and reasons as the Qt Activity screen.
 *
 * The panel holds the UI_UNIT_ACTIVITY_SHOWN rows with the highest seq (the top of the newest-first list the Qt screen
 * shows), newest first. A merge updates its row in place by seq, so a repeated message raises the row's count and never
 * moves it. A row the ring dropped before it was read is older than the 1024 rows the ring keeps, far more than the
 * section shows, so it falls below them and the section shows no gap row. When a row's name could change
 * (dsd_app_unit_activity_name_key()), the panel reads the log again from its start and updates the rows it holds.
 *
 * Threading: the UI thread, from snapshots; the panel never reads the live decoder state.
 */

#ifndef DSD_NEO_SRC_UI_TERMINAL_UNIT_ACTIVITY_PANEL_H_
#define DSD_NEO_SRC_UI_TERMINAL_UNIT_ACTIVITY_PANEL_H_

#include <dsd-neo/app_control/unit_activity_view.h>
#include <dsd-neo/core/state_fwd.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    /** Rows the section shows. */
    UI_UNIT_ACTIVITY_SHOWN = 8,
    /** Rows read per view call. */
    UI_UNIT_ACTIVITY_PAGE = 16,
    /** A buffer for ui_unit_activity_summary() or ui_unit_activity_detail(): holds the longest line of any row. */
    UI_UNIT_ACTIVITY_LINE_SIZE = 512,
};

typedef struct {
    dsd_app_unit_activity_cursor cursor;
    uint64_t name_key;
    int count;                                              /**< Rows held in rows[]. */
    dsd_app_unit_activity_row rows[UI_UNIT_ACTIVITY_SHOWN]; /**< Highest seq first. */
    dsd_app_unit_activity_row page[UI_UNIT_ACTIVITY_PAGE];  /**< Scratch for one read. */
} ui_unit_activity_panel;

/** Forget every row and the read position (a zeroed panel is the same). */
void ui_unit_activity_panel_reset(ui_unit_activity_panel* panel);

/** Bring @p panel up to @p snapshot's log: new rows, merges and, after a name change, renamed rows. A snapshot holding no
 * log leaves the panel as it was; a new log (another ring instance) replaces the rows it held. */
void ui_unit_activity_panel_update(ui_unit_activity_panel* panel, const dsd_state* snapshot);

/**
 * The row's first line: its local time (HH:MM:SS), the headline (dsd_app_unit_activity_headline()), the subject with
 * its name in brackets, the target text, and the repeat count when the row holds more than one message:
 * "12:34:56 Joined talkgroup; 1234567 [Engine 5]; TG 100 (x3)". Parts the row lacks are left out.
 */
void ui_unit_activity_summary(const dsd_app_unit_activity_row* row, char* out, size_t cap);

/**
 * The row's second line: the reason when the headline does not already carry it, and, when @p with_system, the
 * protocol and the system the row was heard on ("Target Unit Not Valid (0x20); P25 Phase 1, WACN BEE00 SYS 123").
 * "" when there is nothing to say.
 */
void ui_unit_activity_detail(const dsd_app_unit_activity_row* row, int with_system, char* out, size_t cap);

/** Whether @p row was heard on another protocol or system than @p newer (the row shown above it); 1 for a NULL
 * @p newer. The section names the system on a row's second line only then. */
int ui_unit_activity_system_changes(const dsd_app_unit_activity_row* row, const dsd_app_unit_activity_row* newer);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_SRC_UI_TERMINAL_UNIT_ACTIVITY_PANEL_H_ */
