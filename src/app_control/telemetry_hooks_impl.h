// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_SRC_APP_CONTROL_TELEMETRY_HOOKS_IMPL_H_
#define DSD_NEO_SRC_APP_CONTROL_TELEMETRY_HOOKS_IMPL_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#ifdef __cplusplus
extern "C" {
#endif

void ui_terminal_telemetry_publish_snapshot(const dsd_state* state);
void ui_terminal_telemetry_publish_opts_snapshot(const dsd_opts* opts);
void ui_terminal_telemetry_request_redraw(void);
const dsd_state* ui_get_latest_snapshot(void);
const dsd_opts* ui_get_latest_opts_snapshot(void);

void ui_terminal_install_telemetry_hooks(void);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_SRC_APP_CONTROL_TELEMETRY_HOOKS_IMPL_H_ */
