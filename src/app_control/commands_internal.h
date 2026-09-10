// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_SRC_APP_CONTROL_COMMANDS_INTERNAL_H_
#define DSD_NEO_SRC_APP_CONTROL_COMMANDS_INTERNAL_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Called by the decoder owner, serialized with draining, at every session edge. */
void dsd_app_command_session_set_open(int open);

int dsd_app_drain_cmds(dsd_opts* opts, dsd_state* state);
#ifdef DSD_NEO_TEST_HOOKS
/* Boolean probes only: tests must never export command payloads as diagnostics. */
int dsd_app_command_test_storage_cleared(void);
int dsd_app_command_test_tail_padding_cleared(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_SRC_APP_CONTROL_COMMANDS_INTERNAL_H_ */
