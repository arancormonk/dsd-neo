// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_SRC_APP_CONTROL_COMMANDS_INTERNAL_H_
#define DSD_NEO_SRC_APP_CONTROL_COMMANDS_INTERNAL_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Called by the decoder owner, serialized with draining, at every session edge. */
void dsd_app_command_session_set_open(int open);

int dsd_app_drain_cmds(dsd_opts* opts, dsd_state* state);
/* 1 when a command waits to be drained (the runtime's controls-pending query; issue #634). */
int dsd_app_commands_pending(void);
/* Whether a queued command needs the frame loop (dsd_runtime_controls_need_frame_loop(), issue #637), judged against
   the asking decoder's own @p state (NULL: a list switch takes the frame loop, a put back or a clear waits). Decoder
   thread. */
int dsd_app_commands_need_frame_loop(const dsd_state* state);
/* The stream notice sink app-control installs (dsd_runtime_stream_notice_set()): any thread posts, the next drain
   shows it as a toast. Cleared at the end of a session, so a notice is never the next session's. */
void dsd_app_stream_notice_post(int kind);
void dsd_app_stream_notice_clear(void);
/* The late settling of a Next or Previous walk off any scan whose tune timed out (issue #644 B-2), installed as the
   engine's walk hook (dsd_channel_walk_settle_hook_set()) with the session: a landing ends the calls, hands the
   trunking state machines over and, while the list and the row are still the ones walked, names the row; a late failure
   passes the row or keeps it for a retry, by the walk's direction. Decoder thread, outside the P25 SM tick guard. */
void dsd_app_channel_walk_settled(dsd_opts* opts, dsd_state* state, int row, int dir, uint32_t requested_hz,
                                  uint64_t list_rev, int landed);
/* Decoder thread: apply a posted DSD_STREAM_NOTICE_CAPTURE_STOPPED, if any (issue #806 X9): the I/Q capture is turned
   off for the session (iq_capture_requested cleared and published) and a toast says so (@p state may be NULL). Every
   stream start of the session runs it before it opens, so it never reopens a sealed capture's file. */
void dsd_app_take_capture_stopped(dsd_opts* opts, dsd_state* state);
#ifdef DSD_NEO_TEST_HOOKS

/* Counts contended policy transactions; relaxed, so observing it adds no synchronization. */
int dsd_app_command_test_policy_guard_waits(void);
/* Boolean probes only: tests must never export command payloads as diagnostics. */
int dsd_app_command_test_storage_cleared(void);
int dsd_app_command_test_tail_padding_cleared(void);
/* 1 when the last command the drain applied failed (a refusal, or a change it could not make), 0 otherwise. */
int dsd_app_command_test_last_failed(void);
int dsd_app_command_test_last_invalid_payload(void);
/* 1 when the last command the drain applied asked for a change only a restart makes, 0 otherwise. */
int dsd_app_command_test_last_restart_required(void);
/* Replace the frequency entry's tune leg (svc_rtl_tune_only()) with @p tune_leg, or put it back with NULL: the result
   it returns is the tune's, and it caches what a success applied in opts->rtlsdr_center_freq as the real one does. */
void dsd_app_command_test_set_tune_leg(int (*tune_leg)(dsd_opts* opts, dsd_state* state, uint32_t hz));
/* Replace the io_control tune of the manual retunes (the channel and candidate cycles, the returns to the control
   channel; io_control_set_freq_tagged()) with @p tune_leg, or put it back with NULL, under the same contract: it may
   hand a timed-out tune's ID back through @p out_pending_id (0 already). A table keyed by frequency in @p tune_leg
   scripts a list whose rows answer differently (issue #666). */
void dsd_app_command_test_set_manual_tune_leg(int (*tune_leg)(dsd_opts* opts, const dsd_state* state, uint32_t hz,
                                                              uint64_t* out_pending_id));
#endif

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_SRC_APP_CONTROL_COMMANDS_INTERNAL_H_ */
