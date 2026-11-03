// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief What each row of a frontend's scan-list switcher shows between a switch and the snapshot that shows it
 * (issue #697): the one rule the terminal's Scan lists modal and the Qt/Android Scan lists sheet share.
 *
 * A row's state comes from two places, as the result API asks (dsd_app_scan_list_results_since()): the retained
 * result of the row's own request says what that request did, and the view's lists_seq says what the lists are. The
 * result carries the lists' sequence it left them at, and can come a frame or more before the snapshot that shows
 * its switch, so a row a request switched (or found already as asked: UNCHANGED) shows the state it asked for while
 * the view's lists_seq is behind the result's; from then on the snapshot is the truth, whatever else switched the list
 * since. A refusal describes the
 * lists the result saw, or those on screen at the switch when they are later (a request dropped before a drain
 * reached it carries the lists the last drain left): its note goes once the view's lists_seq moves past them, and a
 * paused refusal goes on a resume that follows a pause the frontend has seen.
 *
 * Rows 0 .. list_count - 1 are the lists, in list order; a frontend may keep one more row at list_count for "every
 * list on" (DSD_APP_SCAN_LIST_ALL), whose successful answer switches every list row. The frontend owns the rows (a
 * zeroed row is idle), sends the requests itself and keeps its own wording, drawing and signals.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_SCAN_LIST_ROWS_H_
#define DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_SCAN_LIST_ROWS_H_

#include <dsd-neo/app_control/commands.h>
#include <dsd-neo/app_control/scan_list_view.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief What a row says beside its list once its request is answered, or why it was never sent. */
typedef enum {
    DSD_APP_SCAN_LIST_NOTE_NONE = 0,
    DSD_APP_SCAN_LIST_NOTE_NOT_SENT, /**< the queue refused the submit: no result will come */
    DSD_APP_SCAN_LIST_NOTE_ANSWER,   /**< the retained result's code, in @c code */
} dsd_app_scan_list_note;

/** @brief One switcher row. Zeroed: idle, showing the view. */
typedef struct {
    uint32_t request;    /**< the request this row waits on; 0 = none */
    uint8_t want;        /**< the state that request asked for */
    uint8_t note;        /**< dsd_app_scan_list_note */
    uint8_t held;        /**< 1 = show @c held_want while the view's lists_seq is behind @c held_seq */
    uint8_t held_want;   /**< the state an answered switch left the list in */
    int32_t code;        /**< the answer's dsd_app_scan_list_set_code, for DSD_APP_SCAN_LIST_NOTE_ANSWER */
    uint32_t held_seq;   /**< the switch's result's lists_seq */
    uint32_t note_seq;   /**< the lists the note describes: the later of the result's and @c submit_seq */
    uint32_t submit_seq; /**< the view's lists_seq at the switch */
    uint8_t seen_paused; /**< 1 = a PAUSED note has been seen with the scan paused, so a resume can retire it */
    uint8_t pad[3];      /* explicit, so a copy carries no indeterminate padding */
} dsd_app_scan_list_row;

/** @brief @p a is a later lists_seq than @p b: the sequence only moves forward, and wraps past UINT32_MAX. */
int dsd_app_scan_list_seq_after(uint32_t a, uint32_t b);

/**
 * @brief Whether list row @p list_index reads on, its own request aside: what its answered switch left it in while
 * @p view's lists_seq is behind that answer, then the view. 0 for a row the view has no list for, or NULL arguments.
 */
int dsd_app_scan_list_row_on(const dsd_app_scan_list_row* row, const dsd_app_scan_list_view* view, int list_index);

/** @brief A switch to @p want was queued as @p request, with the view at @p lists_seq: the row waits for its answer. */
void dsd_app_scan_list_row_sent(dsd_app_scan_list_row* row, uint32_t request, int want, uint32_t lists_seq);

/** @brief The queue refused the switch with the view at @p lists_seq: never pending; the row says so until the lists
 *  move or it is switched again. */
void dsd_app_scan_list_row_not_sent(dsd_app_scan_list_row* row, uint32_t lists_seq);

/**
 * @brief Match @p result to the row waiting on its request among @p rows[0 .. row_count - 1], of which the first
 * @p list_count are lists (a row at list_count is "every list on"). The row stops waiting and keeps the answer as its
 * note; an OK or UNCHANGED holds what it asked for (every list row, for the every-list row) until the view reaches the
 * result's lists_seq.
 * @return The row answered, or -1 when none waits on that request.
 */
int dsd_app_scan_list_rows_answer(dsd_app_scan_list_row* rows, int row_count, int list_count,
                                  const dsd_app_scan_list_result* result);

/** @brief Reads the retained results after @p after_sequence into @p out, oldest first, at most @p cap of them: the
 *  shape of dsd_app_scan_list_results_since(), with @p ctx for the caller. Returns how many it wrote. */
typedef int (*dsd_app_scan_list_results_reader)(void* ctx, uint64_t after_sequence, dsd_app_scan_list_result* out,
                                                int cap);

/**
 * @brief Read every retained result after @p *after with @p read, page by page, up to the newest: each answers its
 * row (dsd_app_scan_list_rows_answer()) and moves @p *after to its sequence. When the results do not follow on from
 * @p *after, the history moved past some never read (more than DSD_APP_RESULT_HISTORY since the last read), which may
 * have held a waiting row's answer: once every retained page is read, a row still waiting stops waiting and shows the
 * view, with no note, and a later answer to its request finds no row. An answer still retained, on any page, answers
 * its row first. NULL arguments change nothing.
 */
void dsd_app_scan_list_rows_read(dsd_app_scan_list_row* rows, int row_count, int list_count, uint64_t* after,
                                 dsd_app_scan_list_results_reader read, void* ctx);

/** @brief Retire, for @p view (the frame's), each hold the view has caught up with and each note that no longer
 *  describes the lists. Call once per frame, after the frame's results. */
void dsd_app_scan_list_rows_expire(dsd_app_scan_list_row* rows, int row_count, const dsd_app_scan_list_view* view);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_SCAN_LIST_ROWS_H_ */
