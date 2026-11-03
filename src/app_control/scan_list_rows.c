// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The scan-list switcher's row rule (issue #697). See scan_list_rows.h for the contract.
 */

#include <dsd-neo/app_control/commands.h>
#include <dsd-neo/app_control/scan_list_rows.h>
#include <dsd-neo/app_control/scan_list_view.h>
#include <stddef.h>
#include <stdint.h>

int
dsd_app_scan_list_seq_after(uint32_t a, uint32_t b) {
    return a != b && (uint32_t)(a - b) < 0x80000000U;
}

int
dsd_app_scan_list_row_on(const dsd_app_scan_list_row* row, const dsd_app_scan_list_view* view, int list_index) {
    if (row == NULL || view == NULL || list_index < 0 || list_index >= (int)view->list_count) {
        return 0;
    }
    if (row->held && dsd_app_scan_list_seq_after(row->held_seq, view->lists_seq)) {
        return row->held_want ? 1 : 0;
    }
    return view->lists[list_index].enabled ? 1 : 0;
}

void
dsd_app_scan_list_row_sent(dsd_app_scan_list_row* row, uint32_t request, int want, uint32_t lists_seq) {
    if (row == NULL) {
        return;
    }
    row->request = request;
    row->want = want ? 1U : 0U;
    row->submit_seq = lists_seq;
    row->note = (uint8_t)DSD_APP_SCAN_LIST_NOTE_NONE;
    row->may_be_lost = 0U;
}

void
dsd_app_scan_list_row_not_sent(dsd_app_scan_list_row* row, uint32_t lists_seq) {
    if (row == NULL) {
        return;
    }
    row->note = (uint8_t)DSD_APP_SCAN_LIST_NOTE_NOT_SENT;
    row->note_seq = lists_seq;
}

/* Switched, or already as asked: hold what it asked for until the snapshot reaches the lists it left. "Every list on"
   holds every list row on. */
static void
scan_list_rows_hold(dsd_app_scan_list_row* rows, int answered, int list_count, const dsd_app_scan_list_result* result) {
    const int every = answered >= list_count;
    const int first = every ? 0 : answered;
    const int last = every ? list_count - 1 : answered;
    for (int i = first; i <= last; i++) {
        rows[i].held = 1U;
        rows[i].held_seq = result->lists_seq;
        rows[i].held_want = every ? 1U : rows[answered].want;
    }
}

int
dsd_app_scan_list_rows_answer(dsd_app_scan_list_row* rows, int row_count, int list_count,
                              const dsd_app_scan_list_result* result) {
    if (rows == NULL || result == NULL || result->request == 0U) {
        return -1;
    }
    for (int r = 0; r < row_count; r++) {
        dsd_app_scan_list_row* row = &rows[r];
        if (row->request != result->request) {
            continue;
        }
        row->request = 0U;
        row->note = (uint8_t)DSD_APP_SCAN_LIST_NOTE_ANSWER;
        row->code = result->code;
        /* At least the lists the row was asked on: a request dropped before a drain reached it carries the lists the
           last drain left, which can be older than the ones on screen at the switch. */
        row->note_seq =
            dsd_app_scan_list_seq_after(row->submit_seq, result->lists_seq) ? row->submit_seq : result->lists_seq;
        row->seen_paused = 0U;
        /* UNCHANGED too: the list already was as asked, though the snapshot on screen may not show it yet (another
           frontend switched it). */
        if (result->code == DSD_APP_SCAN_LIST_SET_OK || result->code == DSD_APP_SCAN_LIST_SET_UNCHANGED) {
            scan_list_rows_hold(rows, r, list_count, result);
        }
        return r;
    }
    return -1;
}

enum {
    SCAN_LIST_ROWS_PAGE = 16,
    /* The history holds DSD_APP_RESULT_HISTORY; a few pages more let results that land during the read in too. */
    SCAN_LIST_ROWS_PAGES_MAX = (DSD_APP_RESULT_HISTORY / SCAN_LIST_ROWS_PAGE) + 4,
};

/* One page read on from @p reader's cursor: each result answers its row and moves the cursor. Returns how many came;
   a result that does not follow on from the cursor marks the answers lost (scan_list_rows_mark_lost()). */
static int
scan_list_rows_read_page(dsd_app_scan_list_row* rows, int row_count, int list_count, dsd_app_scan_list_reader* reader,
                         dsd_app_scan_list_results_reader read, void* ctx) {
    dsd_app_scan_list_result page[SCAN_LIST_ROWS_PAGE];
    const int n = read(ctx, reader->after, page, SCAN_LIST_ROWS_PAGE);
    for (int i = 0; i < n && i < SCAN_LIST_ROWS_PAGE; i++) {
        if (page[i].sequence != reader->after + 1U) {
            /* Every row waiting now may have had its answer among those lost. */
            reader->gap_open = 1U;
            for (int r = 0; r < row_count; r++) {
                rows[r].may_be_lost = rows[r].request != 0U ? 1U : rows[r].may_be_lost;
            }
        }
        reader->after = page[i].sequence;
        (void)dsd_app_scan_list_rows_answer(rows, row_count, list_count, &page[i]);
    }
    return n;
}

void
dsd_app_scan_list_rows_read(dsd_app_scan_list_row* rows, int row_count, int list_count,
                            dsd_app_scan_list_reader* reader, dsd_app_scan_list_results_reader read, void* ctx) {
    if (rows == NULL || reader == NULL || read == NULL) {
        return;
    }
    int drained = 0;
    for (int pages = 0; pages < SCAN_LIST_ROWS_PAGES_MAX && !drained; pages++) {
        drained = scan_list_rows_read_page(rows, row_count, list_count, reader, read, ctx) < SCAN_LIST_ROWS_PAGE;
    }
    if (!reader->gap_open || !drained) {
        /* Not drained yet (results kept coming): the lost mark stays for the next read. */
        return;
    }
    /* Every retained answer has been read since the history moved past results never read: a row that was waiting
       then and still is had its answer among them, or it is late. It stops waiting and shows the view, with nothing to
       say beside it. */
    reader->gap_open = 0U;
    for (int r = 0; r < row_count; r++) {
        if (rows[r].may_be_lost && rows[r].request != 0U) {
            rows[r].request = 0U;
            rows[r].note = (uint8_t)DSD_APP_SCAN_LIST_NOTE_NONE;
        }
        rows[r].may_be_lost = 0U;
    }
}

/* Whether @p row's note no longer describes the lists: they moved past the ones it described, or a paused refusal
   saw the scan paused and then resumed. Only a pause seen counts: the frame an answer arrives in can predate the
   pause that refused it. */
static int
scan_list_note_expired(dsd_app_scan_list_row* row, const dsd_app_scan_list_view* view) {
    if (dsd_app_scan_list_seq_after(view->lists_seq, row->note_seq)) {
        return 1;
    }
    if (row->note != (uint8_t)DSD_APP_SCAN_LIST_NOTE_ANSWER || row->code != DSD_APP_SCAN_LIST_SET_PAUSED) {
        return 0;
    }
    if (view->paused) {
        row->seen_paused = 1U;
        return 0;
    }
    return row->seen_paused;
}

void
dsd_app_scan_list_rows_expire(dsd_app_scan_list_row* rows, int row_count, const dsd_app_scan_list_view* view) {
    if (rows == NULL || view == NULL) {
        return;
    }
    for (int r = 0; r < row_count; r++) {
        dsd_app_scan_list_row* row = &rows[r];
        if (row->held && !dsd_app_scan_list_seq_after(row->held_seq, view->lists_seq)) {
            row->held = 0U;
        }
        if (row->note != (uint8_t)DSD_APP_SCAN_LIST_NOTE_NONE && scan_list_note_expired(row, view)) {
            row->note = (uint8_t)DSD_APP_SCAN_LIST_NOTE_NONE;
        }
    }
}
