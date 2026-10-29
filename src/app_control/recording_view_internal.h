// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_SRC_APP_CONTROL_RECORDING_VIEW_INTERNAL_H_
#define DSD_NEO_SRC_APP_CONTROL_RECORDING_VIEW_INTERNAL_H_

#include <dsd-neo/core/opts_fwd.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Build the recording view from @p opts and this run's failure counts and publish it; the options snapshot's
   publication calls it on the decode thread (dsd_app_telemetry_publish_opts_snapshot()). */
void dsd_app_recording_view_publish(const dsd_opts* opts);

/* Whether command @p cmd_id is a recording command the view reports on. The queue passes a config apply's id only when
   the config carries a [recording] section, and 0 (none) otherwise. */
int dsd_app_recording_command_tracked(int cmd_id);

/* The command queue's account of recording commands: one admitted to the queue, and how one left it (a
   DSD_APP_RECORDING_COMMAND_* outcome). Both ignore commands that are not tracked. */
void dsd_app_recording_command_submitted(int cmd_id);
void dsd_app_recording_command_resolved(int cmd_id, int outcome);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_SRC_APP_CONTROL_RECORDING_VIEW_INTERNAL_H_ */
