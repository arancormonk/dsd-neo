// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* The retained results of DSD_APP_CMD_SCAN_LIST_SET (issue #697) and the request ids they are matched by. */

#ifndef DSD_NEO_SRC_APP_CONTROL_SCAN_LIST_RESULTS_H
#define DSD_NEO_SRC_APP_CONTROL_SCAN_LIST_RESULTS_H

#include <stdint.h>

struct dsd_app_command;

/* Retain the result of @p command when it is a DSD_APP_CMD_SCAN_LIST_SET (anything else is ignored): its request as
   submitted, read field by field so a short payload still names what it carried, and @p code
   (dsd_app_scan_list_set_code). Any thread; it takes only the results' own lock, so the queue may hold its lock. */
void dsd_app_scan_list_publish_result(const struct dsd_app_command* command, int32_t code);

#endif /* DSD_NEO_SRC_APP_CONTROL_SCAN_LIST_RESULTS_H */
