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
/* 1 when a command waits to be drained (the runtime's controls-pending query; issue #634). */
int dsd_app_commands_pending(void);
/* Whether a queued command needs the frame loop (dsd_runtime_controls_need_frame_loop(), issue #637). */
int dsd_app_commands_need_frame_loop(void);
/* The stream notice sink app-control installs (dsd_runtime_stream_notice_set()): any thread posts, the next drain
   shows it as a toast. Cleared at the end of a session, so a notice is never the next session's. */
void dsd_app_stream_notice_post(int kind);
void dsd_app_stream_notice_clear(void);
#ifdef DSD_NEO_TEST_HOOKS
/* Counts contended policy transactions; relaxed, so observing it adds no synchronization. */
int dsd_app_command_test_policy_guard_waits(void);
/* Boolean probes only: tests must never export command payloads as diagnostics. */
int dsd_app_command_test_storage_cleared(void);
int dsd_app_command_test_tail_padding_cleared(void);
/* 1 when the last command the drain applied failed (a refusal, or a change it could not make), 0 otherwise. */
int dsd_app_command_test_last_failed(void);
int dsd_app_command_test_last_invalid_payload(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_SRC_APP_CONTROL_COMMANDS_INTERNAL_H_ */
