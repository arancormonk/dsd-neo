// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The Scan lists modal (issue #697): switch the --trunk-scan target lists of this session on or off.
 *
 * One row per list and an All lists on row. The modal owns everything it draws: a copy of the frame's
 * dsd_app_scan_list_view, taken from the snapshot pair at every menu tick and never from the decoder's structs, and
 * the request each row is waiting on. Enter on a row submits DSD_APP_CMD_SCAN_LIST_SET with a fresh request id; the
 * row shows that it is waiting until the retained result for that request arrives, then the list's new state or the
 * refusal. Rows wait independently, and a repeated Enter on a waiting row is ignored. The modal stays open until Esc
 * or Left, and closes by itself when the trunk-scan session it opened on ends or is replaced, dropping what it was
 * waiting on.
 *
 * Driven by menu_core.c like the RadioReference panel. This header is internal to src/ui/terminal/ and names no curses
 * type, so the targets that compile menu_core.c alone can include it.
 */
#ifndef DSD_NEO_SRC_UI_TERMINAL_SCAN_LIST_MODAL_H_
#define DSD_NEO_SRC_UI_TERMINAL_SCAN_LIST_MODAL_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#ifdef DSD_NEO_TEST_HOOKS
#include <stddef.h>
#include <time.h>
#endif

/**
 * @brief Open the modal on the frame's snapshot pair. Call only from an on_select.
 *
 * @return 1 when it opened: a trunk scan, running or paused, has two or more lists. 0, and nothing opens, otherwise.
 */
int scan_list_modal_open(const dsd_opts* opts, const dsd_state* state);
/** @brief Nonzero while the modal owns the overlay. */
int scan_list_modal_active(void);
/** @brief Handle one key; returns 1 while the modal is open (every key is the modal's), 0 when it is not. */
int scan_list_modal_handle_key(int ch);
/** @brief Take the frame's view and the answers that arrived; closes the modal when its session is gone. */
void scan_list_modal_tick(const dsd_opts* opts, const dsd_state* state);
/** @brief Draw the modal. Must not call ui_commit_frame(). */
void scan_list_modal_render(void);
/** @brief Close the modal and drop every request it was waiting on. */
void scan_list_modal_close(void);

#ifdef DSD_NEO_TEST_HOOKS
/** @brief Test hook: the title as drawn. */
void scan_list_modal_title_for_test(char* out, size_t out_size);
/** @brief Test hook: row @p row (0-based; the lists, then All lists on) as drawn, trailing blanks dropped. */
int scan_list_modal_row_for_test(int row, char* out, size_t out_size);
/** @brief Test hook: the highlighted row. */
int scan_list_modal_selected_for_test(void);
/** @brief Test hook: the decoder's toast the toast row shows at @p now when the terminal has no notice; 1 if live. */
int scan_list_modal_decoder_toast_for_test(time_t now, char* out, size_t out_size);
#endif

#endif /* DSD_NEO_SRC_UI_TERMINAL_SCAN_LIST_MODAL_H_ */
