// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The scan-list switcher's row rule (issue #697), shared by the terminal modal and the Qt/Android sheet: the
 * sequence order across the wrap, what a row reads between a switch and the snapshot that shows it, which row a result
 * answers, and when a note stops describing the lists.
 */

#include <assert.h>
#include <dsd-neo/app_control/commands.h>
#include <dsd-neo/app_control/scan_list_rows.h>
#include <dsd-neo/app_control/scan_list_view.h>
#include <dsd-neo/core/safe_api.h>
#include <stdint.h>
#include <stdio.h>

/* Three lists at lists_seq @p seq, list i on per @p on[i]; paused or running. */
static dsd_app_scan_list_view
view_of(uint32_t seq, const int on[3], int paused) {
    dsd_app_scan_list_view v;
    DSD_MEMSET(&v, 0, sizeof v);
    v.session = 7U;
    v.lists_seq = seq;
    v.list_count = 3U;
    v.paused = paused ? 1U : 0U;
    v.running = paused ? 0U : 1U;
    v.toggleable = paused ? 0U : 1U;
    for (int i = 0; i < 3; i++) {
        v.lists[i].number = (uint8_t)(i + 1);
        v.lists[i].enabled = on[i] ? 1U : 0U;
    }
    return v;
}

static dsd_app_scan_list_result
result_of(uint32_t request, int32_t code, uint32_t lists_seq) {
    dsd_app_scan_list_result r;
    DSD_MEMSET(&r, 0, sizeof r);
    r.request = request;
    r.code = code;
    r.lists_seq = lists_seq;
    return r;
}

static void
test_seq_after(void) {
    assert(dsd_app_scan_list_seq_after(2U, 1U));
    assert(!dsd_app_scan_list_seq_after(1U, 2U));
    assert(!dsd_app_scan_list_seq_after(5U, 5U));
    /* Across the wrap: 1 comes after UINT32_MAX. */
    assert(dsd_app_scan_list_seq_after(1U, UINT32_MAX));
    assert(!dsd_app_scan_list_seq_after(UINT32_MAX, 1U));
}

static void
test_null_arguments(void) {
    const int on[3] = {1, 1, 1};
    const dsd_app_scan_list_view v = view_of(1U, on, 0);
    dsd_app_scan_list_row row;
    DSD_MEMSET(&row, 0, sizeof row);
    assert(dsd_app_scan_list_row_on(NULL, &v, 0) == 0);
    assert(dsd_app_scan_list_row_on(&row, NULL, 0) == 0);
    assert(dsd_app_scan_list_row_on(&row, &v, 3) == 0 && dsd_app_scan_list_row_on(&row, &v, -1) == 0);
    const dsd_app_scan_list_result r = result_of(1U, DSD_APP_SCAN_LIST_SET_OK, 2U);
    assert(dsd_app_scan_list_rows_answer(NULL, 1, 1, &r) == -1);
    assert(dsd_app_scan_list_rows_answer(&row, 1, 1, NULL) == -1);
    /* A result with request 0 answers no idle row. */
    const dsd_app_scan_list_result zero = result_of(0U, DSD_APP_SCAN_LIST_SET_OK, 2U);
    assert(dsd_app_scan_list_rows_answer(&row, 1, 1, &zero) == -1);
    dsd_app_scan_list_rows_expire(NULL, 1, &v);
    dsd_app_scan_list_rows_expire(&row, 1, NULL);
    dsd_app_scan_list_row_sent(NULL, 1U, 1, 1U);
    dsd_app_scan_list_row_not_sent(NULL, 1U);
}

/* An OK answer ahead of the snapshot holds what it asked for; the snapshot takes over once it reaches the answer's
   lists, and a later change from elsewhere shows. */
static void
test_switch_holds_until_the_snapshot(void) {
    dsd_app_scan_list_row rows[3];
    DSD_MEMSET(rows, 0, sizeof rows);
    const int all_on[3] = {1, 1, 1};
    const int two_off[3] = {1, 0, 1};
    dsd_app_scan_list_view v = view_of(10U, all_on, 0);
    assert(dsd_app_scan_list_row_on(&rows[1], &v, 1) == 1);
    dsd_app_scan_list_row_sent(&rows[1], 101U, 0, v.lists_seq);
    assert(rows[1].request == 101U && rows[1].want == 0U && rows[1].submit_seq == 10U);
    /* Waiting: the view still says on. */
    assert(dsd_app_scan_list_row_on(&rows[1], &v, 1) == 1);
    /* Another request's answer leaves it waiting. */
    dsd_app_scan_list_result other = result_of(99U, DSD_APP_SCAN_LIST_SET_OK, 11U);
    assert(dsd_app_scan_list_rows_answer(rows, 3, 3, &other) == -1 && rows[1].request == 101U);
    dsd_app_scan_list_result ok = result_of(101U, DSD_APP_SCAN_LIST_SET_OK, 11U);
    assert(dsd_app_scan_list_rows_answer(rows, 3, 3, &ok) == 1);
    assert(rows[1].request == 0U && rows[1].note == DSD_APP_SCAN_LIST_NOTE_ANSWER && rows[1].held);
    dsd_app_scan_list_rows_expire(rows, 3, &v);
    assert(dsd_app_scan_list_row_on(&rows[1], &v, 1) == 0); /* held ahead of the snapshot */
    v = view_of(11U, two_off, 0);
    dsd_app_scan_list_rows_expire(rows, 3, &v);
    assert(!rows[1].held && dsd_app_scan_list_row_on(&rows[1], &v, 1) == 0);
    v = view_of(12U, all_on, 0);
    dsd_app_scan_list_rows_expire(rows, 3, &v);
    assert(dsd_app_scan_list_row_on(&rows[1], &v, 1) == 1);
    /* The OK note says nothing more once the lists move past it. */
    assert(rows[1].note == DSD_APP_SCAN_LIST_NOTE_NONE);
    /* A snapshot already past the answer: no hold to show. */
    dsd_app_scan_list_row_sent(&rows[0], 102U, 0, 12U);
    v = view_of(14U, all_on, 0);
    ok = result_of(102U, DSD_APP_SCAN_LIST_SET_OK, 13U);
    assert(dsd_app_scan_list_rows_answer(rows, 3, 3, &ok) == 0);
    dsd_app_scan_list_rows_expire(rows, 3, &v);
    assert(!rows[0].held && dsd_app_scan_list_row_on(&rows[0], &v, 0) == 1);
}

/* The "every list on" row at list_count holds every list row on. */
static void
test_every_list_row(void) {
    dsd_app_scan_list_row rows[4];
    DSD_MEMSET(rows, 0, sizeof rows);
    const int some_off[3] = {1, 0, 0};
    dsd_app_scan_list_view v = view_of(20U, some_off, 0);
    dsd_app_scan_list_row_sent(&rows[3], 201U, 1, v.lists_seq);
    dsd_app_scan_list_row_sent(&rows[1], 202U, 0, v.lists_seq);
    const dsd_app_scan_list_result ok = result_of(201U, DSD_APP_SCAN_LIST_SET_OK, 21U);
    assert(dsd_app_scan_list_rows_answer(rows, 4, 3, &ok) == 3);
    dsd_app_scan_list_rows_expire(rows, 4, &v);
    for (int i = 0; i < 3; i++) {
        assert(dsd_app_scan_list_row_on(&rows[i], &v, i) == 1);
    }
    /* A list row's own request still waits through the every-list answer. */
    assert(rows[1].request == 202U);
}

/* UNCHANGED: the list was already as asked, though the view on screen may not show it yet (another frontend switched
   it, the snapshot not yet published). The row holds what it asked for until the view reaches the answer's lists, as an
   OK does; the "every list on" row holds every list row on. */
static void
test_unchanged_holds_until_the_snapshot(void) {
    dsd_app_scan_list_row rows[4];
    DSD_MEMSET(rows, 0, sizeof rows);
    const int all_on[3] = {1, 1, 1};
    const int two_off[3] = {1, 0, 1};
    dsd_app_scan_list_view v = view_of(50U, all_on, 0);
    dsd_app_scan_list_row_sent(&rows[1], 501U, 0, v.lists_seq);
    const dsd_app_scan_list_result unchanged = result_of(501U, DSD_APP_SCAN_LIST_SET_UNCHANGED, 51U);
    assert(dsd_app_scan_list_rows_answer(rows, 4, 3, &unchanged) == 1);
    assert(rows[1].request == 0U && rows[1].held && rows[1].held_seq == 51U);
    dsd_app_scan_list_rows_expire(rows, 4, &v);
    assert(dsd_app_scan_list_row_on(&rows[1], &v, 1) == 0); /* held ahead of the snapshot */
    v = view_of(51U, two_off, 0);
    dsd_app_scan_list_rows_expire(rows, 4, &v);
    assert(!rows[1].held && dsd_app_scan_list_row_on(&rows[1], &v, 1) == 0);

    /* Every list on, already so though the view shows one off. */
    dsd_app_scan_list_row_sent(&rows[3], 502U, 1, v.lists_seq);
    const dsd_app_scan_list_result all_unchanged = result_of(502U, DSD_APP_SCAN_LIST_SET_UNCHANGED, 52U);
    assert(dsd_app_scan_list_rows_answer(rows, 4, 3, &all_unchanged) == 3);
    dsd_app_scan_list_rows_expire(rows, 4, &v);
    for (int i = 0; i < 3; i++) {
        assert(dsd_app_scan_list_row_on(&rows[i], &v, i) == 1);
    }
}

/* A results history as the queue keeps it: sequence 1 .. count, only the last DSD_APP_RESULT_HISTORY retained. */
typedef struct {
    dsd_app_scan_list_result results[200];
    int count;
    int reads;
} test_history;

static void
history_add(test_history* h, uint32_t request, int32_t code) {
    assert(h->count < (int)(sizeof h->results / sizeof h->results[0]));
    dsd_app_scan_list_result* r = &h->results[h->count];
    *r = result_of(request, code, 61U);
    r->sequence = (uint64_t)++h->count;
}

static int
history_read(void* ctx, uint64_t after, dsd_app_scan_list_result* out, int cap) {
    test_history* h = (test_history*)ctx;
    h->reads++;
    const int oldest = h->count > DSD_APP_RESULT_HISTORY ? h->count - DSD_APP_RESULT_HISTORY + 1 : 1;
    int n = 0;
    for (int i = oldest - 1; i < h->count && n < cap; i++) {
        if (h->results[i].sequence > after) {
            out[n++] = h->results[i];
        }
    }
    return n;
}

/* Results read in order answer their rows and move the cursor. When the history moved past answers never read (more
   than 64 since the last read), every retained page is read first: an answer still kept, here on the third page of
   sixteen, answers its row. Only a row whose answer is still missing then stops waiting and shows the view, with no
   note, and its answer, should it come later, finds no row. */
static void
test_read_recovers_from_a_history_gap(void) {
    dsd_app_scan_list_row rows[4];
    DSD_MEMSET(rows, 0, sizeof rows);
    const int on[3] = {1, 1, 1};
    const dsd_app_scan_list_view v = view_of(60U, on, 0);
    static test_history h;
    DSD_MEMSET(&h, 0, sizeof h);
    uint64_t after = 0U;
    dsd_app_scan_list_row_sent(&rows[0], 601U, 0, v.lists_seq);
    dsd_app_scan_list_row_sent(&rows[1], 602U, 0, v.lists_seq);
    dsd_app_scan_list_row_sent(&rows[2], 603U, 0, v.lists_seq);
    /* In order: row 2's answer comes, rows 0 and 1 keep waiting. */
    history_add(&h, 990U, DSD_APP_SCAN_LIST_SET_OK);
    history_add(&h, 603U, DSD_APP_SCAN_LIST_SET_REFUSED_LAST);
    dsd_app_scan_list_rows_read(rows, 4, 3, &after, history_read, &h);
    assert(after == 2U && rows[0].request == 601U && rows[1].request == 602U);
    assert(rows[2].request == 0U && rows[2].note == DSD_APP_SCAN_LIST_NOTE_ANSWER);
    /* 100 answers go by unread, so the history keeps sequences 39 .. 102. Row 1's is sequence 79, the 41st kept: on
       the third page. Row 0's never comes. */
    for (int i = 0; i < 100; i++) {
        history_add(&h, i == 76 ? 602U : 9000U + (uint32_t)i, DSD_APP_SCAN_LIST_SET_OK);
    }
    h.reads = 0;
    dsd_app_scan_list_rows_read(rows, 4, 3, &after, history_read, &h);
    assert(after == 102U && h.reads == 5);
    assert(rows[1].request == 0U && rows[1].note == DSD_APP_SCAN_LIST_NOTE_ANSWER
           && rows[1].code == DSD_APP_SCAN_LIST_SET_OK && rows[1].held);
    assert(rows[0].request == 0U && rows[0].note == DSD_APP_SCAN_LIST_NOTE_NONE && !rows[0].held);
    assert(dsd_app_scan_list_row_on(&rows[0], &v, 0) == 1);
    /* Row 2's answered note is not a waiting row's: it stays. */
    assert(rows[2].note == DSD_APP_SCAN_LIST_NOTE_ANSWER);
    /* The lost answer, were it to come after all, answers no row. */
    history_add(&h, 601U, DSD_APP_SCAN_LIST_SET_OK);
    dsd_app_scan_list_rows_read(rows, 4, 3, &after, history_read, &h);
    assert(after == 103U && rows[0].note == DSD_APP_SCAN_LIST_NOTE_NONE && !rows[0].held);
    /* Nothing new, or NULL arguments, change nothing. */
    dsd_app_scan_list_rows_read(rows, 4, 3, &after, history_read, &h);
    dsd_app_scan_list_rows_read(NULL, 4, 3, &after, history_read, &h);
    dsd_app_scan_list_rows_read(rows, 4, 3, NULL, history_read, &h);
    dsd_app_scan_list_rows_read(rows, 4, 3, &after, NULL, &h);
    assert(after == 103U);
}

/* A refusal: the view's state, and the note until the lists move past the later of the answer's and the switch's. */
static void
test_refusals_and_notes(void) {
    dsd_app_scan_list_row rows[3];
    DSD_MEMSET(rows, 0, sizeof rows);
    const int on[3] = {1, 1, 1};
    dsd_app_scan_list_view v = view_of(30U, on, 0);
    dsd_app_scan_list_row_sent(&rows[0], 301U, 0, v.lists_seq);
    dsd_app_scan_list_result refused = result_of(301U, DSD_APP_SCAN_LIST_SET_REFUSED_LAST, 30U);
    assert(dsd_app_scan_list_rows_answer(rows, 3, 3, &refused) == 0);
    assert(!rows[0].held && rows[0].code == DSD_APP_SCAN_LIST_SET_REFUSED_LAST && rows[0].note_seq == 30U);
    dsd_app_scan_list_rows_expire(rows, 3, &v);
    assert(rows[0].note == DSD_APP_SCAN_LIST_NOTE_ANSWER && dsd_app_scan_list_row_on(&rows[0], &v, 0) == 1);
    v = view_of(31U, on, 0);
    dsd_app_scan_list_rows_expire(rows, 3, &v);
    assert(rows[0].note == DSD_APP_SCAN_LIST_NOTE_NONE);

    /* Dropped before a drain reached it, with older lists than at the switch: kept until the view passes the
       switch's. */
    dsd_app_scan_list_row_sent(&rows[1], 302U, 0, 31U);
    const dsd_app_scan_list_result dropped = result_of(302U, DSD_APP_SCAN_LIST_SET_DROPPED, 28U);
    assert(dsd_app_scan_list_rows_answer(rows, 3, 3, &dropped) == 1 && rows[1].note_seq == 31U);
    dsd_app_scan_list_rows_expire(rows, 3, &v);
    assert(rows[1].note == DSD_APP_SCAN_LIST_NOTE_ANSWER);
    v = view_of(32U, on, 0);
    dsd_app_scan_list_rows_expire(rows, 3, &v);
    assert(rows[1].note == DSD_APP_SCAN_LIST_NOTE_NONE);

    /* Not sent: never pending, the note at the view's lists, and gone once they move. */
    dsd_app_scan_list_row_not_sent(&rows[2], 32U);
    assert(rows[2].request == 0U && rows[2].note == DSD_APP_SCAN_LIST_NOTE_NOT_SENT && rows[2].note_seq == 32U);
    dsd_app_scan_list_rows_expire(rows, 3, &v);
    assert(rows[2].note == DSD_APP_SCAN_LIST_NOTE_NOT_SENT);
    /* Switched again: the note goes at once. */
    dsd_app_scan_list_row_sent(&rows[2], 303U, 0, 32U);
    assert(rows[2].note == DSD_APP_SCAN_LIST_NOTE_NONE);
    v = view_of(33U, on, 0);
    dsd_app_scan_list_rows_expire(rows, 3, &v);
}

/* A paused refusal goes on a resume that follows a pause seen; an answer in a frame from before the pause keeps it. */
static void
test_paused_refusal(void) {
    dsd_app_scan_list_row rows[3];
    DSD_MEMSET(rows, 0, sizeof rows);
    const int on[3] = {1, 1, 1};
    dsd_app_scan_list_view running = view_of(40U, on, 0);
    const dsd_app_scan_list_view paused = view_of(40U, on, 1);
    dsd_app_scan_list_row_sent(&rows[0], 401U, 0, 40U);
    const dsd_app_scan_list_result r = result_of(401U, DSD_APP_SCAN_LIST_SET_PAUSED, 40U);
    assert(dsd_app_scan_list_rows_answer(rows, 3, 3, &r) == 0 && !rows[0].seen_paused);
    dsd_app_scan_list_rows_expire(rows, 3, &running);
    assert(rows[0].note == DSD_APP_SCAN_LIST_NOTE_ANSWER);
    dsd_app_scan_list_rows_expire(rows, 3, &paused);
    assert(rows[0].note == DSD_APP_SCAN_LIST_NOTE_ANSWER && rows[0].seen_paused);
    dsd_app_scan_list_rows_expire(rows, 3, &running);
    assert(rows[0].note == DSD_APP_SCAN_LIST_NOTE_NONE);
    /* Any other refusal does not go on a resume. */
    dsd_app_scan_list_row_sent(&rows[1], 402U, 0, 40U);
    const dsd_app_scan_list_result busy = result_of(402U, DSD_APP_SCAN_LIST_SET_BUSY, 40U);
    assert(dsd_app_scan_list_rows_answer(rows, 3, 3, &busy) == 1);
    dsd_app_scan_list_rows_expire(rows, 3, &paused);
    dsd_app_scan_list_rows_expire(rows, 3, &running);
    assert(rows[1].note == DSD_APP_SCAN_LIST_NOTE_ANSWER);
}

int
main(void) {
    test_seq_after();
    test_null_arguments();
    test_switch_holds_until_the_snapshot();
    test_every_list_row();
    test_unchanged_holds_until_the_snapshot();
    test_read_recovers_from_a_history_gap();
    test_refusals_and_notes();
    test_paused_refusal();
    DSD_FPRINTF(stderr, "OK\n");
    return 0;
}
