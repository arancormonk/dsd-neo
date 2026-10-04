// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */

#ifndef DSD_NEO_APP_CONTROL_SCAN_ROW_EDIT_H
#define DSD_NEO_APP_CONTROL_SCAN_ROW_EDIT_H

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <stddef.h>
#include "services.h"

struct dsd_app_command;

enum {
    DSD_APP_SCAN_ROW_EDIT_DONE = 0,         /**< applied or stored */
    DSD_APP_SCAN_ROW_EDIT_REFUSED = -1,     /**< refused or failed, nothing changed */
    DSD_APP_SCAN_ROW_EDIT_BAD_PAYLOAD = -2, /**< malformed payload */
};

/**
 * @brief DSD_APP_CMD_SCAN_ROW_EDIT ("this channel", issue #518): apply one session edit to the scan row the payload
 * names, then do what an edit on air leaves to app-control -- hand the front end the new width, reopen the stream for
 * the new gain -- putting the previous edit back when that fails. Decoder thread, without the P25 SM tick guard (the
 * engine and the restart take it). Writes the toast to @p notice and its lifetime to @p ttl_s.
 *
 * @return DSD_APP_SCAN_ROW_EDIT_DONE, _REFUSED or _BAD_PAYLOAD.
 */
int dsd_app_apply_scan_row_edit(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* command, char* notice,
                                size_t notice_size, int* ttl_s);

/**
 * @brief Settle an RTL front end's refusal of a "this channel" width request (@p refusal with row_edit set, from
 * svc_take_monitor_request_outcome()), where it landed after the edit was applied: the edit goes back to the one before
 * it, the configured width untouched, and @p notice says why. Returns 1 when it settled @p refusal, 0 when it is not a
 * row edit's or no edit is recorded for it (the ordinary settle then reconciles it). Decoder thread only.
 */
int dsd_app_scan_row_edit_settle_refusal(dsd_opts* opts, dsd_state* state, const svc_monitor_refusal* refusal,
                                         char* notice, size_t notice_size);

#endif /* DSD_NEO_APP_CONTROL_SCAN_ROW_EDIT_H */
