// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* The terminal's Unit Activity rows and line text (unit_activity_panel.h). */

#include "unit_activity_panel.h"

#include <dsd-neo/app_control/unit_activity_view.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/time_format.h>
#include <dsd-neo/core/unit_activity.h>

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

/* The most pages one pass reads: the whole ring, and one more for the read that finds nothing left. */
enum { UI_UA_MAX_PAGES = DSD_UNIT_ACTIVITY_CAPACITY / UI_UNIT_ACTIVITY_PAGE + 2 };

void
ui_unit_activity_panel_reset(ui_unit_activity_panel* panel) {
    if (panel) {
        DSD_MEMSET(panel, 0, sizeof(*panel));
    }
}

/* Keep @p row when it is held (update it in place) or among the UI_UNIT_ACTIVITY_SHOWN highest seq. */
static void
ui_ua_take(ui_unit_activity_panel* panel, const dsd_app_unit_activity_row* row) {
    int at = 0;
    while (at < panel->count && panel->rows[at].seq > row->seq) {
        at++;
    }
    if (at < panel->count && panel->rows[at].seq == row->seq) {
        panel->rows[at] = *row;
        return;
    }
    if (at >= UI_UNIT_ACTIVITY_SHOWN) {
        return; /* older than every row shown */
    }
    int last = panel->count < UI_UNIT_ACTIVITY_SHOWN ? panel->count : UI_UNIT_ACTIVITY_SHOWN - 1;
    for (int i = last; i > at; i--) {
        panel->rows[i] = panel->rows[i - 1];
    }
    panel->rows[at] = *row;
    if (panel->count < UI_UNIT_ACTIVITY_SHOWN) {
        panel->count++;
    }
}

/* Read every row revised after the panel's cursor. A new log drops the rows held. */
static void
ui_ua_read(ui_unit_activity_panel* panel, const dsd_state* snapshot) {
    for (int page = 0; page < UI_UA_MAX_PAGES; page++) {
        dsd_app_unit_activity_meta meta;
        DSD_MEMSET(&meta, 0, sizeof(meta));
        const int n = dsd_app_unit_activity_rows(snapshot, &panel->cursor, panel->page, UI_UNIT_ACTIVITY_PAGE, &meta);
        if (n < 0 || !meta.held) {
            return;
        }
        if (meta.reset) {
            panel->count = 0;
        }
        for (int i = 0; i < n; i++) {
            ui_ua_take(panel, &panel->page[i]);
        }
        if (!meta.more) {
            return;
        }
    }
}

void
ui_unit_activity_panel_update(ui_unit_activity_panel* panel, const dsd_state* snapshot) {
    if (!panel || !snapshot) {
        return;
    }
    const uint64_t key = dsd_app_unit_activity_name_key(snapshot);
    if (key != panel->name_key && panel->count > 0) {
        /* The rows held may carry old names: read the log from its start, which brings the new rows too and updates
           every row held in place. */
        panel->cursor.rev = 0U;
    }
    panel->name_key = key;
    ui_ua_read(panel, snapshot);
}

/* Append "<sep><text>" to @p out (used bytes in *@p used) when @p text is not empty. */
static void
ui_ua_append(char* out, size_t cap, size_t* used, const char* sep, const char* text) {
    if (!text || text[0] == '\0' || *used >= cap) {
        return;
    }
    const int wrote = DSD_SNPRINTF(out + *used, cap - *used, "%s%s", *used > 0U ? sep : "", text);
    if (wrote > 0) {
        *used += (size_t)wrote < cap - *used ? (size_t)wrote : cap - *used - 1U;
    }
}

void
ui_unit_activity_summary(const dsd_app_unit_activity_row* row, char* out, size_t cap) {
    if (!out || cap == 0U) {
        return;
    }
    out[0] = '\0';
    if (!row) {
        return;
    }
    size_t used = 0U;
    char time_text[16];
    (void)dsd_format_local_datetime((time_t)row->wall_s, DSD_LOCAL_DATETIME_TIME_COLON, time_text, sizeof(time_text));
    ui_ua_append(out, cap, &used, "", time_text);

    char headline[DSD_APP_UNIT_ACTIVITY_HEADLINE_SIZE];
    dsd_app_unit_activity_headline(row, headline, sizeof(headline));
    ui_ua_append(out, cap, &used, " ", headline);

    char subject[sizeof(row->subject_text) + sizeof(row->subject_name) + 3U];
    if (row->subject_name[0] != '\0' && row->subject_text[0] != '\0') {
        DSD_SNPRINTF(subject, sizeof(subject), "%s [%s]", row->subject_text, row->subject_name);
    } else {
        DSD_SNPRINTF(subject, sizeof(subject), "%s",
                     row->subject_text[0] != '\0' ? row->subject_text : row->subject_name);
    }
    ui_ua_append(out, cap, &used, "; ", subject);
    ui_ua_append(out, cap, &used, "; ", row->target_text);

    if (row->repeat > 1U) {
        char repeat[16];
        DSD_SNPRINTF(repeat, sizeof(repeat), "(x%u)", (unsigned)row->repeat);
        ui_ua_append(out, cap, &used, " ", repeat);
    }
}

void
ui_unit_activity_detail(const dsd_app_unit_activity_row* row, int with_system, char* out, size_t cap) {
    if (!out || cap == 0U) {
        return;
    }
    out[0] = '\0';
    if (!row) {
        return;
    }
    size_t used = 0U;
    /* A deny or queued headline already ends with the reason. */
    char headline[DSD_APP_UNIT_ACTIVITY_HEADLINE_SIZE];
    dsd_app_unit_activity_headline(row, headline, sizeof(headline));
    if (strcmp(headline, row->kind_label) == 0) {
        ui_ua_append(out, cap, &used, "; ", row->reason_text);
    }
    if (with_system) {
        char system[sizeof(row->protocol_text) + sizeof(row->scope_text) + 2U];
        if (row->protocol_text[0] != '\0' && row->scope_text[0] != '\0') {
            DSD_SNPRINTF(system, sizeof(system), "%s, %s", row->protocol_text, row->scope_text);
        } else {
            DSD_SNPRINTF(system, sizeof(system), "%s",
                         row->protocol_text[0] != '\0' ? row->protocol_text : row->scope_text);
        }
        ui_ua_append(out, cap, &used, "; ", system);
    }
}

int
ui_unit_activity_system_changes(const dsd_app_unit_activity_row* row, const dsd_app_unit_activity_row* newer) {
    if (!row) {
        return 0;
    }
    if (!newer) {
        return 1;
    }
    return strcmp(row->protocol_text, newer->protocol_text) != 0 || strcmp(row->scope_text, newer->scope_text) != 0;
}
