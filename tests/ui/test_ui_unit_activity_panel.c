// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */
/**
 * @file
 * @brief The terminal's Unit Activity rows (src/ui/terminal/unit_activity_panel.c) over a real ring and the real
 * app-control view: the newest rows by seq, a repeated message counted in place, a new log, names after an alias is
 * learned, and the text of each row's lines.
 *
 * All IDs are made up (WACN 0xBEE00, SYS 0x123, RID 42 / 1234567).
 */
#include <assert.h>
#include <dsd-neo/app_control/unit_activity_view.h>
#include <dsd-neo/core/alias_learn.h>
#include <dsd-neo/core/received_identity.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/source_alias.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/core/time_format.h>
#include <dsd-neo/core/unit_activity.h>
#include <dsd-neo/core/unit_activity_reason.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "unit_activity_panel.h"

static const uint64_t T0_NS = UINT64_C(1700000000000000000);

static dsd_state*
new_state(void) {
    dsd_state* st = (dsd_state*)calloc(1, sizeof(*st));
    assert(st != NULL);
    return st;
}

static void
free_state(dsd_state* st) {
    dsd_state_ext_free_all(st);
    free(st);
}

static void
tune_p25(dsd_state* st) {
    dsd_rxid_reset(st);
    dsd_rxid_note_p25_network(st, 0xBEE00U, 0x123U);
}

/* A frontend's copy of @p live, as the terminal draws from. */
static dsd_state*
snapshot_of(const dsd_state* live) {
    dsd_state* copy = new_state();
    copy->rx_id = live->rx_id;
    assert(dsd_unit_activity_copy_snapshot(copy, live) == 0);
    assert(dsd_tg_policy_copy_snapshot(copy, live) == 0);
    assert(dsd_source_alias_copy_snapshot(copy, live) == 0);
    return copy;
}

/* Update @p panel from a fresh snapshot of @p live. */
static void
update_from(ui_unit_activity_panel* panel, const dsd_state* live) {
    dsd_state* snap = snapshot_of(live);
    ui_unit_activity_panel_update(panel, snap);
    free_state(snap);
}

static dsd_unit_activity
join_row(uint32_t rid) {
    dsd_unit_activity r;
    DSD_MEMSET(&r, 0, sizeof(r));
    r.protocol = DSD_UA_PROTO_P25P1;
    r.kind = DSD_UA_GROUP_AFFILIATION;
    r.role = DSD_UA_ROLE_RESPONSE;
    r.result = DSD_UA_RESULT_ACCEPTED;
    r.subject.id = rid;
    r.subject.subtype = DSD_UA_ID_RADIO;
    r.subject.valid = 1U;
    r.group = 100U;
    r.group_valid = 1U;
    return r;
}

static ui_unit_activity_panel*
new_panel(void) {
    ui_unit_activity_panel* panel = (ui_unit_activity_panel*)calloc(1, sizeof(*panel));
    assert(panel != NULL);
    return panel;
}

static void
test_no_log(void) {
    dsd_state* st = new_state();
    ui_unit_activity_panel* panel = new_panel();
    update_from(panel, st);
    assert(panel->count == 0);
    ui_unit_activity_panel_update(NULL, st);
    ui_unit_activity_panel_update(panel, NULL);
    assert(panel->count == 0);
    free(panel);
    free_state(st);
}

/* Ten rows: the eight with the highest seq, newest first. A repeat of the newest counts in place; a repeat of a row
 * below them changes nothing shown. */
static void
test_newest_rows_and_merges(void) {
    dsd_state* st = new_state();
    dsd_decode_clock_use_test(T0_NS);
    tune_p25(st);
    ui_unit_activity_panel* panel = new_panel();
    for (uint32_t i = 0U; i < 10U; i++) {
        dsd_unit_activity r = join_row(1001U + i);
        assert(dsd_unit_activity_push(st, &r) == 1);
    }
    update_from(panel, st);
    assert(panel->count == UI_UNIT_ACTIVITY_SHOWN);
    for (int i = 0; i < panel->count; i++) {
        assert(panel->rows[i].seq == (uint64_t)(10 - i));
    }
    assert(strcmp(panel->rows[0].subject_text, "1010") == 0);

    dsd_unit_activity again = join_row(1010U);
    assert(dsd_unit_activity_push(st, &again) == 0);
    again = join_row(1001U); /* seq 1, below the rows shown */
    assert(dsd_unit_activity_push(st, &again) == 0);
    update_from(panel, st);
    assert(panel->count == UI_UNIT_ACTIVITY_SHOWN);
    assert(panel->rows[0].seq == 10U && panel->rows[0].repeat == 2U);
    assert(panel->rows[UI_UNIT_ACTIVITY_SHOWN - 1].seq == 3U);

    /* A snapshot holding no log keeps the rows; a new log replaces them. */
    dsd_state* empty = new_state();
    ui_unit_activity_panel_update(panel, empty);
    free_state(empty);
    assert(panel->count == UI_UNIT_ACTIVITY_SHOWN);
    dsd_state_ext_free_all(st);
    dsd_unit_activity fresh = join_row(2001U);
    assert(dsd_unit_activity_push(st, &fresh) == 1);
    update_from(panel, st);
    assert(panel->count == 1 && panel->rows[0].seq == 1U);
    assert(strcmp(panel->rows[0].subject_text, "2001") == 0);

    ui_unit_activity_panel_reset(panel);
    assert(panel->count == 0 && panel->cursor.instance == 0U);
    free(panel);
    free_state(st);
}

/* An alias learned after a row was read names the row on the next update, without a second row. */
static void
test_rename_updates_rows(void) {
    dsd_state* st = new_state();
    dsd_decode_clock_use_test(T0_NS);
    tune_p25(st);
    ui_unit_activity_panel* panel = new_panel();
    dsd_unit_activity r = join_row(42U);
    assert(dsd_unit_activity_push(st, &r) == 1);
    update_from(panel, st);
    assert(panel->count == 1 && panel->rows[0].subject_name[0] == '\0');

    assert(dsd_alias_learn_radio(NULL, st, NULL, 42U, "Alice", NULL) == DSD_TG_POLICY_LEARN_ADDED);
    update_from(panel, st);
    assert(panel->count == 1);
    assert(strcmp(panel->rows[0].subject_name, "Alice") == 0);
    free(panel);
    free_state(st);
}

/* The lines, from rows the real view resolved: the headline (a deny carries its reason), the subject and its name,
 * the target, the count; the second line carries any other reason, and the system when it changes. */
static void
test_line_text(void) {
    dsd_state* st = new_state();
    dsd_decode_clock_use_test(T0_NS);
    tune_p25(st);
    assert(dsd_alias_learn_radio(NULL, st, NULL, 42U, "Alice", NULL) == DSD_TG_POLICY_LEARN_ADDED);
    ui_unit_activity_panel* panel = new_panel();

    dsd_unit_activity r = join_row(42U);
    assert(dsd_unit_activity_push(st, &r) == 1);
    assert(dsd_unit_activity_push(st, &r) == 0);
    r = join_row(1234567U);
    r.kind = DSD_UA_DENY;
    r.result = DSD_UA_RESULT_DENIED;
    r.group_valid = 0U;
    r.group = 0U;
    r.reason_set = DSD_UA_REASON_P25_DENY;
    r.reason_code = 0x20U;
    assert(dsd_unit_activity_push(st, &r) == 1);
    r = join_row(1234567U);
    r.protocol = DSD_UA_PROTO_DMR;
    r.kind = DSD_UA_REGISTRATION;
    r.result = DSD_UA_RESULT_REFUSED;
    r.group_valid = 0U;
    r.group = 0U;
    r.reason_set = DSD_UA_REASON_DMR_TIER3;
    r.reason_code = 0x2FU;
    assert(dsd_unit_activity_push(st, &r) == 1);
    update_from(panel, st);
    assert(panel->count == 3);

    char time_text[16];
    (void)dsd_format_local_datetime((time_t)(T0_NS / UINT64_C(1000000000)), DSD_LOCAL_DATETIME_TIME_COLON, time_text,
                                    sizeof time_text);
    char line[UI_UNIT_ACTIVITY_LINE_SIZE];
    char expected[UI_UNIT_ACTIVITY_LINE_SIZE];
    const dsd_app_unit_activity_row* refused = &panel->rows[0];
    const dsd_app_unit_activity_row* denied = &panel->rows[1];
    const dsd_app_unit_activity_row* joined = &panel->rows[2];

    ui_unit_activity_summary(joined, line, sizeof line);
    DSD_SNPRINTF(expected, sizeof expected, "%s Joined talkgroup; 42 [Alice]; TG 100 (x2)", time_text);
    assert(strcmp(line, expected) == 0);
    ui_unit_activity_detail(joined, ui_unit_activity_system_changes(joined, denied), line, sizeof line);
    assert(line[0] == '\0'); /* same system as the row above, no reason */
    ui_unit_activity_detail(joined, 1, line, sizeof line);
    assert(strcmp(line, "P25 Phase 1, WACN BEE00 SYS 123") == 0);

    ui_unit_activity_summary(denied, line, sizeof line);
    DSD_SNPRINTF(expected, sizeof expected, "%s Denied: Target Unit Not Valid (0x20); 1234567", time_text);
    assert(strcmp(line, expected) == 0);
    ui_unit_activity_detail(denied, 0, line, sizeof line);
    assert(line[0] == '\0'); /* the headline already carries the reason */

    ui_unit_activity_summary(refused, line, sizeof line);
    DSD_SNPRINTF(expected, sizeof expected, "%s Registration refused; 1234567", time_text);
    assert(strcmp(line, expected) == 0);
    assert(ui_unit_activity_system_changes(refused, NULL) == 1);
    assert(ui_unit_activity_system_changes(denied, refused) == 1); /* DMR above P25 */
    ui_unit_activity_detail(refused, 0, line, sizeof line);
    assert(strcmp(line, "Talkgroup not allowed (0x2F)") == 0);

    ui_unit_activity_summary(NULL, line, sizeof line);
    assert(line[0] == '\0');
    ui_unit_activity_detail(NULL, 1, line, sizeof line);
    assert(line[0] == '\0');
    free(panel);
    free_state(st);
}

int
main(void) {
    test_no_log();
    test_newest_rows_and_merges();
    test_rename_updates_rows();
    test_line_text();
    printf("UI_UNIT_ACTIVITY_PANEL: OK\n");
    return 0;
}
