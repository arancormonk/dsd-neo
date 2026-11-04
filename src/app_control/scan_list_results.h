// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* The retained results of DSD_APP_CMD_SCAN_LIST_SET (issue #697) and the request ids they are matched by. */

#ifndef DSD_NEO_SRC_APP_CONTROL_SCAN_LIST_RESULTS_H
#define DSD_NEO_SRC_APP_CONTROL_SCAN_LIST_RESULTS_H

#include <dsd-neo/core/state_fwd.h>
#include <stdint.h>

struct dsd_app_command;

/* Retain the result of @p command when it is a DSD_APP_CMD_SCAN_LIST_SET (anything else is ignored): its request as
   submitted, read field by field so a short payload still names what it carried, @p code
   (dsd_app_scan_list_set_code), and the lists' sequence: @p state's trunk_scan_lists_seq after handling (the drain's
   decoder state), or with @p state NULL (a request dropped before the decoder saw it) the last one noted. Any thread;
   it takes only the results' own lock, so the queue may hold its lock. */
void dsd_app_scan_list_publish_result(const struct dsd_app_command* command, int32_t code, const dsd_state* state);

/* Note the decoder's trunk_scan_lists_seq as a drain leaves it, for the results of requests dropped before a drain
   reached them. Decoder thread, once per drain that applied a command. */
void dsd_app_scan_list_note_lists_seq(uint32_t lists_seq);

/* Mark every result retained so far as an earlier decoder run's, so dsd_app_scan_list_session_results_since() no
   longer returns it. The command queue calls it as its session opens, after it dropped what the last one left queued,
   under its own lock (the results' lock is taken after it). */
void dsd_app_scan_list_note_session_open(void);

#endif /* DSD_NEO_SRC_APP_CONTROL_SCAN_LIST_RESULTS_H */
