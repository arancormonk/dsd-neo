// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Pure text formatters for the RadioReference import panel.
 *
 * This header is internal to src/ui/terminal/ and should NOT be installed. It
 * names no curses type and no wizard-core type, so the headless UI_RR_PANEL
 * target can compile rr_panel_format.c without a terminal or a wizard.
 */
#ifndef DSD_NEO_SRC_UI_TERMINAL_RR_PANEL_FORMAT_H_
#define DSD_NEO_SRC_UI_TERMINAL_RR_PANEL_FORMAT_H_

#include <stddef.h>

#include <dsd-neo/runtime/radioreference.h>
#include <dsd-neo/runtime/radioreference_import.h>

typedef struct {
    int kept;       /* distinct usable frequencies kept, no cap */
    int empty;      /* selected sites whose first usable frequency is 0 */
    int duplicates; /* selected sites repeating an already-kept frequency */
    char text[40];  /* "[ 3 distinct frequencies ]" */
} RrPanelCounter;

typedef int (*rr_panel_site_selected_fn)(const void* user, size_t index);

/** @brief One site list row. Returns 0 when the whole row fitted, -1 otherwise. */
int rr_panel_site_row_format(const dsd_rr_site* site, int trunked, int selected, int cursor, char* out, size_t out_sz);

/** @brief Distinct-frequency tally for the conventional counter, mirroring rr_chan_conventional. */
void rr_panel_counter_state(const dsd_rr_site* sites, size_t site_count, rr_panel_site_selected_fn is_selected,
                            const void* user, RrPanelCounter* out);

/**
 * @brief The plan summary row.
 *
 * A plan merely waiting for a site choice (dsd_rr_import_plan::awaiting_selection)
 * reports 0, not 1: the caller paints a blocked row red and bold, and an unmade
 * choice is a question rather than a refusal.
 *
 * @return 1 when the plan is blocked, 0 when not, -1 on bad output.
 */
int rr_panel_plan_line(const dsd_rr_import_plan* plan, char* out, size_t out_sz);

#endif /* DSD_NEO_SRC_UI_TERMINAL_RR_PANEL_FORMAT_H_ */
