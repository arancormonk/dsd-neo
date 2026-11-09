// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The Scan lists modal (issue #697, scan_list_modal.c), driven through its open, key and tick entry points on a
 * published snapshot pair, as menu_core.c drives it:
 *  - a row's Enter submits DSD_APP_CMD_SCAN_LIST_SET for its list with a fresh request id, and the row waits;
 *  - the retained result for that request ends the wait: the new state, or the refusal beside the list;
 *  - a switched row shows the state it asked for while the snapshot's trunk_scan_lists_seq is behind the result's,
 *    and the snapshot once it is not, also when the snapshot already holds a later switch at the answer;
 *  - a note goes once the snapshot's lists move past the result's, and a paused refusal goes on resume;
 *  - while the scan is paused, Enter sends nothing and is refused beside its row, and the footer offers no switch;
 *  - a repeated Enter on a waiting row sends nothing; rows wait independently and resolve in any order;
 *  - a submit the queue refuses says so at once;
 *  - Esc closes it, and so does the end of the session it opened on, dropping what it waited on;
 *  - results from before it opened are not its own.
 *
 * The real scan list view reads the pair; the command submit, request ids, the results ring and the toast are stubbed
 * at the app-control seam the modal calls. Nothing is drawn: the rows are read through the test hook that formats
 * them as the render path draws them.
 */

#include <assert.h>
#include <curses.h>
#include <dsd-neo/app_control/commands.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/ui/keymap.h>
#include <dsd-neo/ui/ui_prims.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"
#include "scan_list_modal.h"
#include "ui_text.h"

/* ---- App-control seam ---- */

static dsd_app_scan_list_set_payload g_sent[32];
static int g_sent_count;
static int g_reject_next;
static uint32_t g_next_request = 100U;
static dsd_app_scan_list_result g_results[512];
/* Results that land while each read runs (a busy decoder): published at the start of every read. */
static int g_publish_per_read;
static int g_result_count;
static uint64_t g_result_seq;
static char g_toast[256];

int
dsd_app_command_scan_list_set(const dsd_app_scan_list_set_payload* payload) {
    if (g_reject_next) {
        g_reject_next = 0;
        return DSD_APP_COMMAND_SUBMIT_REJECTED;
    }
    assert(g_sent_count < (int)(sizeof g_sent / sizeof g_sent[0]));
    g_sent[g_sent_count++] = *payload;
    return DSD_APP_COMMAND_SUBMIT_QUEUED;
}

uint32_t
dsd_app_scan_list_request_next(void) {
    return g_next_request++;
}

int
dsd_app_scan_list_result_get(dsd_app_scan_list_result* out) {
    if (g_result_count == 0) {
        DSD_MEMSET(out, 0, sizeof *out);
        return 0;
    }
    *out = g_results[g_result_count - 1];
    return 1;
}

static void publish_result_at(uint32_t request, int16_t list, uint8_t enabled, int32_t code, uint32_t lists_seq);

/* As the real history does, only the last DSD_APP_RESULT_HISTORY results are kept. */
int
dsd_app_scan_list_results_since(uint64_t after_sequence, dsd_app_scan_list_result* out, int cap) {
    for (int i = 0; i < g_publish_per_read; i++) {
        publish_result_at(8000U + (uint32_t)g_result_count, 2, 1, DSD_APP_SCAN_LIST_SET_UNCHANGED, 10U);
    }
    const uint64_t oldest =
        g_result_seq > (uint64_t)DSD_APP_RESULT_HISTORY ? g_result_seq - (uint64_t)DSD_APP_RESULT_HISTORY + 1U : 1U;
    int n = 0;
    for (int i = 0; i < g_result_count && n < cap; i++) {
        if (g_results[i].sequence > after_sequence && g_results[i].sequence >= oldest) {
            out[n++] = g_results[i];
        }
    }
    return n;
}

/* The published pair the modal reads (fixture() fills it). */
static dsd_opts g_opts;
static dsd_state g_state;

/* A result as the decoder publishes it, leaving the lists at @p lists_seq. */
static void
publish_result_at(uint32_t request, int16_t list, uint8_t enabled, int32_t code, uint32_t lists_seq) {
    assert(g_result_count < (int)(sizeof g_results / sizeof g_results[0]));
    dsd_app_scan_list_result* r = &g_results[g_result_count++];
    DSD_MEMSET(r, 0, sizeof *r);
    r->sequence = ++g_result_seq;
    r->request = request;
    r->session = 7U;
    r->list = list;
    r->enabled = enabled;
    r->code = code;
    r->lists_seq = lists_seq;
}

/* A switch moves the lists to the next sequence (the snapshot showing it may come later); anything else leaves them. */
static void
publish_result(uint32_t request, int16_t list, uint8_t enabled, int32_t code) {
    const uint32_t now = g_state.trunk_scan_lists_seq;
    publish_result_at(request, list, enabled, code, code == DSD_APP_SCAN_LIST_SET_OK ? now + 1U : now);
}

void
ui_statusf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    (void)DSD_VSNPRINTF(g_toast, sizeof g_toast, fmt, ap);
    va_end(ap);
}

/* The render path's seams: never reached, nothing is drawn here. */
WINDOW*
ui_make_window(int h, int w, int y, int x) {
    (void)h;
    (void)w;
    (void)y;
    (void)x;
    return NULL;
}

int
ui_status_peek(char* buf, size_t n, time_t now) {
    (void)buf;
    (void)n;
    (void)now;
    return 0;
}

void
ui_status_clear_if_expired(time_t now) {
    (void)now;
}

/* ---- Fixture: a running trunk scan with three lists ---- */

static void
fixture(void) {
    DSD_MEMSET(&g_opts, 0, sizeof g_opts);
    DSD_MEMSET(&g_state, 0, sizeof g_state);
    g_opts.trunk_scan_enabled = 1;
    static const char* const names[] = {"security", "hospital", "municipality"};
    static const uint16_t targets[] = {12, 1, 3};
    for (int i = 0; i < 3; i++) {
        DSD_SNPRINTF(g_state.trunk_scan_lists[i].name, sizeof g_state.trunk_scan_lists[i].name, "%s", names[i]);
        g_state.trunk_scan_lists[i].enabled = 1;
        g_state.trunk_scan_lists[i].targets = targets[i];
    }
    /* No list shares a target: each list's targets are in its own membership mask only. */
    g_state.trunk_scan_membership_counts[0x1 - 1] = 12;
    g_state.trunk_scan_membership_counts[0x2 - 1] = 1;
    g_state.trunk_scan_membership_counts[0x4 - 1] = 3;
    g_state.trunk_scan_list_count = 3;
    g_state.trunk_scan_session = 7U;
    g_state.trunk_scan_lists_seq = 10U;
    g_state.trunk_scan_active_list = 1;
    g_state.trunk_scan_active_lists = 0x1;
    g_sent_count = 0;
    g_reject_next = 0;
    /* A fresh history: the newest result (none) and the sequence the next one takes start over together, as the
       results ring keeps them, so a modal opened here reads on from the newest with no gap. */
    g_result_count = 0;
    g_result_seq = 0U;
    g_publish_per_read = 0;
    g_toast[0] = '\0';
    scan_list_modal_close();
}

static void
expect_row(int row, const char* want) {
    char got[256];
    assert(scan_list_modal_row_for_test(row, got, sizeof got) == 1);
    if (strcmp(got, want) != 0) {
        DSD_FPRINTF(stderr, "row %d: got '%s'\n          want '%s'\n", row, got, want);
        assert(0);
    }
}

static void
expect_title(const char* want) {
    char got[256];
    scan_list_modal_title_for_test(got, sizeof got);
    if (strcmp(got, want) != 0) {
        DSD_FPRINTF(stderr, "title: got '%s' want '%s'\n", got, want);
        assert(0);
    }
}

/* A waiting row ends in the cut marker of this terminal's output ("…", or "..." without UTF-8). */
static void
expect_waiting_row(int row, const char* want_before_marker) {
    char want[256];
    DSD_SNPRINTF(want, sizeof want, "%s%s", want_before_marker, ui_text_marker(ui_text_unicode_output()));
    expect_row(row, want);
}

static void
key(int ch) {
    assert(scan_list_modal_handle_key(ch) == 1);
}

static void
tick(void) {
    scan_list_modal_tick(&g_opts, &g_state);
}

/* Highlight row @p row from the top. */
static void
select_row(int row) {
    key(KEY_HOME);
    for (int i = 0; i < row; i++) {
        key(KEY_DOWN);
    }
    assert(scan_list_modal_selected_for_test() == row);
}

/* ---- Cases ---- */

static void
test_opens_only_with_lists_to_switch(void) {
    fixture();
    g_opts.trunk_scan_enabled = 0;
    assert(scan_list_modal_open(&g_opts, &g_state) == 0);
    assert(scan_list_modal_active() == 0);
    g_opts.trunk_scan_enabled = 1;
    g_state.trunk_scan_list_count = 1;
    assert(scan_list_modal_open(&g_opts, &g_state) == 0);
    g_state.trunk_scan_list_count = 3;
    g_state.trunk_scan_session = 0U;
    assert(scan_list_modal_open(&g_opts, &g_state) == 0);
    g_state.trunk_scan_session = 7U;
    assert(scan_list_modal_handle_key('\n') == 0); /* closed: not its key */

    assert(scan_list_modal_open(NULL, &g_state) == 0);
    assert(scan_list_modal_open(&g_opts, &g_state) == 1);
    assert(scan_list_modal_active() == 1);
    expect_title("Scan lists (3 of 3 on, 16 targets)");
    expect_row(0, "1  security      On   12 targets  on air");
    expect_row(1, "2  hospital      On    1 target");
    expect_row(2, "3  municipality  On    3 targets");
    expect_row(3, "All lists on");
    assert(scan_list_modal_selected_for_test() == 0);
    key(KEY_UP); /* wraps */
    assert(scan_list_modal_selected_for_test() == 3);
    key(KEY_DOWN);
    assert(scan_list_modal_selected_for_test() == 0);
    assert(g_sent_count == 0);

    assert(strcmp(scan_list_modal_footer_for_test(), "Arrows  Enter: switch on/off  Esc/Left: close") == 0);

    /* A paused scan keeps its lists open here: Enter is refused at once, beside the row and on the notice line, with
       nothing sent (the decoder could only refuse it, and a DMR call on air would hold that answer back), and the
       footer names the way out instead of a switch. */
    g_state.trunk_scan_paused = 1;
    tick();
    assert(scan_list_modal_active() == 1);
    expect_title("Scan lists (3 of 3 on, scan paused)");
    assert(strcmp(scan_list_modal_footer_for_test(), "Arrows  Esc/Left: close, then y resumes the scan") == 0);
    key('\n');
    assert(g_sent_count == 0);
    expect_row(0, "1  security      On   12 targets  on air  Scan paused: resume it first");
    assert(strcmp(g_toast, "Scan paused: resume it first") == 0);
    select_row(3);
    key('\n');
    assert(g_sent_count == 0);
    expect_row(3, "All lists on                              Scan paused: resume it first");
    select_row(0);
    tick();
    expect_row(3, "All lists on                              Scan paused: resume it first");
    tick(); /* still paused: still true */
    expect_row(0, "1  security      On   12 targets  on air  Scan paused: resume it first");
    /* Resumed (the lists unchanged): the refusal is no longer true, and the switch is offered again. */
    g_state.trunk_scan_paused = 0;
    tick();
    expect_row(0, "1  security      On   12 targets  on air");
    expect_row(3, "All lists on");
    assert(strcmp(scan_list_modal_footer_for_test(), "Arrows  Enter: switch on/off  Esc/Left: close") == 0);
    scan_list_modal_close();
    assert(scan_list_modal_active() == 0);
}

/* Pending until its own result; the result's state until the snapshot moves; the snapshot from then on. */
static void
test_pending_then_result(void) {
    fixture();
    assert(scan_list_modal_open(&g_opts, &g_state) == 1);
    select_row(1);
    key(KEY_ENTER);
    assert(g_sent_count == 1);
    assert(g_sent[0].session == 7U && g_sent[0].request == g_next_request - 1U);
    assert(g_sent[0].list == 2 && g_sent[0].enabled == 0 && g_sent[0].pad == 0);
    expect_waiting_row(1, "2  hospital      On    1 target           Switching off");
    tick();
    expect_waiting_row(1, "2  hospital      On    1 target           Switching off");

    /* An answer to someone else's request leaves the row waiting. */
    publish_result(g_sent[0].request + 50U, 2, 0, DSD_APP_SCAN_LIST_SET_OK);
    tick();
    expect_waiting_row(1, "2  hospital      On    1 target           Switching off");

    /* Its answer: off, before the snapshot shows it. */
    publish_result(g_sent[0].request, 2, 0, DSD_APP_SCAN_LIST_SET_OK);
    tick();
    expect_row(1, "2  hospital      Off   1 target");
    expect_title("Scan lists (2 of 3 on, 15 targets)");
    tick(); /* a frame later the snapshot still lags: still the answer's */
    expect_row(1, "2  hospital      Off   1 target");

    /* The snapshot shows it; and from then on the snapshot is the truth, whatever switches the list next. */
    g_state.trunk_scan_lists[1].enabled = 0;
    g_state.trunk_scan_lists_seq = 11U;
    tick();
    expect_row(1, "2  hospital      Off   1 target");
    g_state.trunk_scan_lists[1].enabled = 1;
    g_state.trunk_scan_lists_seq = 12U;
    tick();
    expect_row(1, "2  hospital      On    1 target");

    /* Enter again asks for the other state of what is shown now. */
    key('\r');
    assert(g_sent_count == 2 && g_sent[1].list == 2 && g_sent[1].enabled == 0);
    assert(g_sent[1].request != g_sent[0].request);
    scan_list_modal_close();
}

static void
test_refusal_shown_beside_the_list(void) {
    fixture();
    g_state.trunk_scan_lists[1].enabled = 0;
    g_state.trunk_scan_lists[2].enabled = 0;
    assert(scan_list_modal_open(&g_opts, &g_state) == 1);
    expect_title("Scan lists (1 of 3 on, 12 targets)");
    key('\n');
    assert(g_sent_count == 1 && g_sent[0].list == 1 && g_sent[0].enabled == 0);
    publish_result(g_sent[0].request, 1, 0, DSD_APP_SCAN_LIST_SET_REFUSED_LAST);
    tick();
    expect_row(0, "1  security      On   12 targets  on air  Nothing else to scan");
    expect_title("Scan lists (1 of 3 on, 12 targets)");
    /* The refusal describes the lists the result saw: true while they stand, gone once another frontend switches
       hospital on and there is something else to scan. */
    tick();
    expect_row(0, "1  security      On   12 targets  on air  Nothing else to scan");
    g_state.trunk_scan_lists[1].enabled = 1;
    g_state.trunk_scan_lists_seq = 11U;
    tick();
    expect_row(0, "1  security      On   12 targets  on air");
    expect_title("Scan lists (2 of 3 on, 13 targets)");

    static const struct {
        int32_t code;
        const char* note;
    } k_notes[] = {
        {DSD_APP_SCAN_LIST_SET_BUSY, "Busy; try again"},
        {DSD_APP_SCAN_LIST_SET_STALE, "Scan changed; nothing applied"},
        {DSD_APP_SCAN_LIST_SET_UNAVAILABLE, "Trunk scan not running"},
        {DSD_APP_SCAN_LIST_SET_SEARCH, "Stop Signal search first"},
        {DSD_APP_SCAN_LIST_SET_INVALID, "Not applied"},
        {DSD_APP_SCAN_LIST_SET_DROPPED, "Not applied; try again"},
        {DSD_APP_SCAN_LIST_SET_UNCHANGED, ""},
    };

    for (size_t i = 0; i < sizeof k_notes / sizeof k_notes[0]; i++) {
        key('\n');
        publish_result(g_sent[g_sent_count - 1].request, 1, 0, k_notes[i].code);
        tick();
        char want[128];
        DSD_SNPRINTF(want, sizeof want, "1  security      On   12 targets  on air%s%s", k_notes[i].note[0] ? "  " : "",
                     k_notes[i].note);
        expect_row(0, want);
    }
    scan_list_modal_close();
}

static void
test_repeated_enter_and_out_of_order_answers(void) {
    fixture();
    assert(scan_list_modal_open(&g_opts, &g_state) == 1);
    key('\n');
    key('\n'); /* waiting: ignored */
    key(KEY_ENTER);
    assert(g_sent_count == 1);
    const uint32_t first = g_sent[0].request;
    select_row(2);
    key('\n');
    assert(g_sent_count == 2 && g_sent[1].list == 3 && g_sent[1].enabled == 0);
    const uint32_t second = g_sent[1].request;
    expect_waiting_row(0, "1  security      On   12 targets  on air  Switching off");
    expect_waiting_row(2, "3  municipality  On    3 targets          Switching off");

    /* The later request is answered first: only its row moves. */
    publish_result(second, 3, 0, DSD_APP_SCAN_LIST_SET_BUSY);
    tick();
    expect_waiting_row(0, "1  security      On   12 targets  on air  Switching off");
    expect_row(2, "3  municipality  On    3 targets          Busy; try again");
    publish_result(first, 1, 0, DSD_APP_SCAN_LIST_SET_OK);
    tick();
    expect_row(0, "1  security      Off  12 targets  on air");
    expect_row(2, "3  municipality  On    3 targets          Busy; try again");
    /* Both answers came in one batch on a later frame too. */
    select_row(1);
    key('\n');
    select_row(2);
    key('\n');
    assert(g_sent_count == 4);
    publish_result(g_sent[3].request, 3, 0, DSD_APP_SCAN_LIST_SET_OK);
    publish_result(g_sent[2].request, 2, 0, DSD_APP_SCAN_LIST_SET_REFUSED_LAST);
    tick();
    expect_row(1, "2  hospital      On    1 target           Nothing else to scan");
    expect_row(2, "3  municipality  Off   3 targets");
    expect_title("Scan lists (1 of 3 on, 1 target)");
    scan_list_modal_close();
}

static void
test_all_lists_on(void) {
    fixture();
    g_state.trunk_scan_lists[0].enabled = 0;
    g_state.trunk_scan_lists[2].enabled = 0;
    g_state.trunk_scan_active_list = 2;
    g_state.trunk_scan_active_lists = 0x2;
    assert(scan_list_modal_open(&g_opts, &g_state) == 1);
    expect_title("Scan lists (1 of 3 on, 1 target)");
    select_row(3);
    key('\n');
    assert(g_sent_count == 1 && g_sent[0].list == DSD_APP_SCAN_LIST_ALL && g_sent[0].enabled == 1);
    expect_waiting_row(3, "All lists on                              Switching on");
    /* A list row waiting on its own request keeps waiting through the All answer. */
    select_row(0);
    key('\n');
    assert(g_sent_count == 2 && g_sent[1].list == 1 && g_sent[1].enabled == 1);
    publish_result(g_sent[0].request, DSD_APP_SCAN_LIST_ALL, 1, DSD_APP_SCAN_LIST_SET_OK);
    tick();
    expect_waiting_row(0, "1  security      On   12 targets          Switching on");
    expect_row(1, "2  hospital      On    1 target   on air");
    expect_row(2, "3  municipality  On    3 targets");
    expect_row(3, "All lists on");
    expect_title("Scan lists (3 of 3 on, 16 targets)");
    /* Already on: answered at the lists the All switch left (11), so the row holds On as an OK would until the
       snapshot shows it. */
    publish_result_at(g_sent[1].request, 1, 1, DSD_APP_SCAN_LIST_SET_UNCHANGED, 11U);
    tick();
    expect_row(0, "1  security      On   12 targets");
    expect_title("Scan lists (3 of 3 on, 16 targets)");
    scan_list_modal_close();
}

static void
test_queue_full(void) {
    fixture();
    assert(scan_list_modal_open(&g_opts, &g_state) == 1);
    g_reject_next = 1;
    key('\n');
    assert(g_sent_count == 0);
    expect_row(0, "1  security      On   12 targets  on air  Not sent; try again");
    assert(strcmp(g_toast, "Scan list switch not sent: too many commands waiting; try again") == 0);
    tick();
    expect_row(0, "1  security      On   12 targets  on air  Not sent; try again");
    /* The lists it was not sent against moved on: the note goes with them. */
    g_state.trunk_scan_lists_seq = 11U;
    tick();
    expect_row(0, "1  security      On   12 targets  on air");
    /* Not waiting on anything: the next Enter sends. */
    key('\n');
    assert(g_sent_count == 1);
    expect_waiting_row(0, "1  security      On   12 targets  on air  Switching off");
    scan_list_modal_close();
}

static void
test_esc_and_session_change(void) {
    fixture();
    assert(scan_list_modal_open(&g_opts, &g_state) == 1);
    key('\n');
    key(DSD_KEY_ESC);
    assert(scan_list_modal_active() == 0);
    /* Its answer comes after it closed: nothing to show it on, and nothing breaks. */
    publish_result(g_sent[0].request, 1, 0, DSD_APP_SCAN_LIST_SET_OK);
    tick();
    assert(scan_list_modal_active() == 0);
    assert(scan_list_modal_open(&g_opts, &g_state) == 1);
    key(KEY_LEFT);
    assert(scan_list_modal_active() == 0);

    /* A new session (stop and start, a restart): the modal closes and what it waited on goes with it. */
    assert(scan_list_modal_open(&g_opts, &g_state) == 1);
    key('\n');
    g_state.trunk_scan_session = 8U;
    tick();
    assert(scan_list_modal_active() == 0);
    assert(scan_list_modal_open(&g_opts, &g_state) == 1);
    expect_row(0, "1  security      On   12 targets  on air");
    scan_list_modal_close();

    /* The scan going away closes it too, and so does a frame with lists of one. */
    assert(scan_list_modal_open(&g_opts, &g_state) == 1);
    g_opts.trunk_scan_enabled = 0;
    tick();
    assert(scan_list_modal_active() == 0);
    g_opts.trunk_scan_enabled = 1;
    assert(scan_list_modal_open(&g_opts, &g_state) == 1);
    tick();
    assert(scan_list_modal_active() == 1);
    g_state.trunk_scan_list_count = 1;
    tick();
    assert(scan_list_modal_active() == 0);
}

/* Results retained from before it opened belong to other requests (or to an earlier modal): never matched. */
static void
test_results_before_open_are_not_its_own(void) {
    fixture();
    publish_result(g_next_request, 1, 0, DSD_APP_SCAN_LIST_SET_REFUSED_LAST);
    assert(scan_list_modal_open(&g_opts, &g_state) == 1);
    key('\n');
    assert(g_sent[0].request == g_results[0].request);
    tick();
    expect_waiting_row(0, "1  security      On   12 targets  on air  Switching off");
    scan_list_modal_close();
}

/* The snapshot can already hold a later switch of the same list when the answer comes (another frontend switched it
   back within the frame): the result's lists are behind the snapshot's, so the row shows the snapshot at once and
   keeps showing it, rather than the state this request asked for. */
static void
test_snapshot_already_past_the_answer(void) {
    fixture();
    assert(scan_list_modal_open(&g_opts, &g_state) == 1);
    select_row(1);
    key('\n');
    assert(g_sent_count == 1 && g_sent[0].list == 2 && g_sent[0].enabled == 0);
    /* This switch left the lists at 11 (hospital off), another switched it on again at 12; the frame shows 12. */
    publish_result_at(g_sent[0].request, 2, 0, DSD_APP_SCAN_LIST_SET_OK, 11U);
    g_state.trunk_scan_lists_seq = 12U;
    tick();
    expect_row(1, "2  hospital      On    1 target");
    expect_title("Scan lists (3 of 3 on, 16 targets)");
    tick();
    tick();
    expect_row(1, "2  hospital      On    1 target");
    scan_list_modal_close();
}

/* A request dropped before a drain reached it carries the lists the last drain left, which can be older than the
   ones on screen at Enter: its note keys on the later of the two, so it stays until the lists really change. */
static void
test_dropped_answer_behind_the_view(void) {
    fixture();
    assert(scan_list_modal_open(&g_opts, &g_state) == 1);
    key('\n');
    assert(g_sent_count == 1);
    publish_result_at(g_sent[0].request, 1, 0, DSD_APP_SCAN_LIST_SET_DROPPED, 9U); /* the view shows 10 */
    tick();
    expect_row(0, "1  security      On   12 targets  on air  Not applied; try again");
    tick();
    expect_row(0, "1  security      On   12 targets  on air  Not applied; try again");
    g_state.trunk_scan_lists_seq = 11U;
    tick();
    expect_row(0, "1  security      On   12 targets  on air");
    scan_list_modal_close();
}

/* The scan paused between Enter and the answer, and the frame the answer arrives in still shows it running: the paused
   refusal stays until the modal has seen the pause and then the resume. */
static void
test_paused_answer_before_the_pause_shows(void) {
    fixture();
    assert(scan_list_modal_open(&g_opts, &g_state) == 1);
    key('\n');
    publish_result(g_sent[0].request, 1, 0, DSD_APP_SCAN_LIST_SET_PAUSED);
    tick(); /* the frame predates the pause */
    expect_row(0, "1  security      On   12 targets  on air  Scan paused: resume it first");
    g_state.trunk_scan_paused = 1;
    g_state.trunk_scan_active_list = 0;
    g_state.trunk_scan_active_lists = 0;
    tick();
    expect_row(0, "1  security      On   12 targets          Scan paused: resume it first");
    g_state.trunk_scan_paused = 0;
    g_state.trunk_scan_active_list = 1;
    g_state.trunk_scan_active_lists = 0x1;
    tick();
    expect_row(0, "1  security      On   12 targets  on air");
    scan_list_modal_close();
}

/* More answers than the history keeps went by between two frames: the row's own may be among those lost, so it stops
   waiting and shows the snapshot, with no note, and Enter on it switches again. */
static void
test_a_history_gap_ends_the_wait(void) {
    fixture();
    assert(scan_list_modal_open(&g_opts, &g_state) == 1);
    key('\n');
    assert(g_sent_count == 1);
    expect_waiting_row(0, "1  security      On   12 targets  on air  Switching off");
    for (int i = 0; i < DSD_APP_RESULT_HISTORY + 4; i++) {
        publish_result(9000U + (uint32_t)i, 2, 1, DSD_APP_SCAN_LIST_SET_UNCHANGED);
    }
    tick();
    expect_row(0, "1  security      On   12 targets  on air");
    key('\n');
    assert(g_sent_count == 2 && g_sent[1].list == 1 && g_sent[1].enabled == 0);
    scan_list_modal_close();
}

/* More than the history keeps went by between two frames, but the row's own answer is still kept, on the third page
   of sixteen: every kept page is read before any wait is given up, so the row takes its answer. */
static void
test_an_answer_kept_past_a_history_gap_answers_its_row(void) {
    fixture();
    assert(scan_list_modal_open(&g_opts, &g_state) == 1);
    key('\n');
    assert(g_sent_count == 1);
    for (int i = 0; i < 50; i++) {
        publish_result(9000U + (uint32_t)i, 2, 1, DSD_APP_SCAN_LIST_SET_UNCHANGED);
    }
    publish_result(g_sent[0].request, 1, 0, DSD_APP_SCAN_LIST_SET_OK);
    for (int i = 0; i < 30; i++) {
        publish_result(9100U + (uint32_t)i, 2, 1, DSD_APP_SCAN_LIST_SET_UNCHANGED);
    }
    tick();
    expect_row(0, "1  security      Off  12 targets  on air");
    scan_list_modal_close();
}

/* Answers were lost while results kept coming, so no frame's read reached the newest: the row keeps waiting until a
   read drains, then stops waiting and shows the snapshot. */
static void
test_a_gap_found_while_results_keep_coming_ends_the_wait_once_drained(void) {
    fixture();
    assert(scan_list_modal_open(&g_opts, &g_state) == 1);
    key('\n');
    assert(g_sent_count == 1);
    for (int i = 0; i < 100; i++) {
        publish_result(9000U + (uint32_t)i, 2, 1, DSD_APP_SCAN_LIST_SET_UNCHANGED);
    }
    g_publish_per_read = 16;
    tick();
    expect_waiting_row(0, "1  security      On   12 targets  on air  Switching off");
    tick();
    expect_waiting_row(0, "1  security      On   12 targets  on air  Switching off");
    g_publish_per_read = 0;
    tick();
    expect_row(0, "1  security      On   12 targets  on air");
    scan_list_modal_close();
}

/* Issue #697: lists that share targets. security's file holds 12 (s, which hospital holds too, m, which municipality
   holds too, and 10 of its own), hospital's 1 (s), municipality's 3 (m and 2 of its own): each row keeps its file's
   count, the title counts each target once for the lists the rows show on (an answered switch ahead of its snapshot
   included; a refused or out-of-order answer moves nothing it should not), and every list holding the target on air
   (s) says so. */
static void
test_shared_targets_count_once(void) {
    fixture();
    DSD_MEMSET(g_state.trunk_scan_membership_counts, 0, sizeof g_state.trunk_scan_membership_counts);
    g_state.trunk_scan_membership_counts[0x1 - 1] = 10;
    g_state.trunk_scan_membership_counts[0x3 - 1] = 1; /* s */
    g_state.trunk_scan_membership_counts[0x5 - 1] = 1; /* m */
    g_state.trunk_scan_membership_counts[0x4 - 1] = 2;
    g_state.trunk_scan_active_lists = 0x3;
    assert(scan_list_modal_open(&g_opts, &g_state) == 1);
    expect_title("Scan lists (3 of 3 on, 14 targets)");
    expect_row(0, "1  security      On   12 targets  on air");
    expect_row(1, "2  hospital      On    1 target   on air");
    expect_row(2, "3  municipality  On    3 targets");

    /* municipality off, refused: the row (On while it waits) and the total stay. */
    select_row(2);
    key('\n');
    assert(g_sent_count == 1 && g_sent[0].list == 3 && g_sent[0].enabled == 0);
    expect_title("Scan lists (3 of 3 on, 14 targets)");
    publish_result(g_sent[0].request, 3, 0, DSD_APP_SCAN_LIST_SET_STALE);
    tick();
    expect_row(2, "3  municipality  On    3 targets          Scan changed; nothing applied");
    expect_title("Scan lists (3 of 3 on, 14 targets)");

    /* hospital off, then municipality off; the later answered first (busy), then hospital's (switched, ahead of the
       snapshot): s stays counted through security all along. */
    select_row(1);
    key('\n');
    select_row(2);
    key('\n');
    assert(g_sent_count == 3 && g_sent[1].list == 2 && g_sent[2].list == 3);
    expect_title("Scan lists (3 of 3 on, 14 targets)");
    publish_result(g_sent[2].request, 3, 0, DSD_APP_SCAN_LIST_SET_BUSY);
    tick();
    expect_title("Scan lists (3 of 3 on, 14 targets)");
    publish_result(g_sent[1].request, 2, 0, DSD_APP_SCAN_LIST_SET_OK);
    tick();
    expect_row(1, "2  hospital      Off   1 target   on air");
    expect_title("Scan lists (2 of 3 on, 14 targets)");
    /* The snapshot shows it: s still on air, named by security, held by hospital too. */
    g_state.trunk_scan_lists[1].enabled = 0;
    g_state.trunk_scan_lists_seq = 11U;
    tick();
    expect_row(0, "1  security      On   12 targets  on air");
    expect_row(1, "2  hospital      Off   1 target   on air");
    expect_title("Scan lists (2 of 3 on, 14 targets)");
    scan_list_modal_close();
}

int
main(void) {
    test_opens_only_with_lists_to_switch();
    test_pending_then_result();
    test_refusal_shown_beside_the_list();
    test_repeated_enter_and_out_of_order_answers();
    test_all_lists_on();
    test_queue_full();
    test_esc_and_session_change();
    test_results_before_open_are_not_its_own();
    test_snapshot_already_past_the_answer();
    test_dropped_answer_behind_the_view();
    test_paused_answer_before_the_pause_shows();
    test_a_history_gap_ends_the_wait();
    test_an_answer_kept_past_a_history_gap_answers_its_row();
    test_a_gap_found_while_results_keep_coming_ends_the_wait_once_drained();
    test_shared_targets_count_once();
    printf("UI_SCAN_LIST_MODAL: OK\n");
    return 0;
}
