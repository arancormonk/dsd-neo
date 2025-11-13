// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#pragma once

#include <stddef.h>

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

/* Start the async UI subsystem.
 * Returns 0 on success. */
int ui_start(dsd_opts* opts, dsd_state* state);

/* Stop the async UI subsystem. */
void ui_stop(void);

/* Request a redraw from demod/decoder side. */
void ui_request_redraw(void);

/* Post a UI command (UI thread producer). */
int ui_post_cmd(int cmd_id, const void* payload, size_t payload_sz);

/* Drain queued UI commands (demod/decoder consumer). */
int ui_drain_cmds(dsd_opts* opts, dsd_state* state);

/* True when executing in the UI thread context. Used to allow legacy
 * ncurses drawing helpers from the UI thread while preventing curses calls
 * from demod/decoder threads when async UI is enabled. */
int ui_is_thread_context(void);

/* Publish both opts and state snapshots and optionally request a redraw.
 * Helper to keep hot paths concise. Safe to call from any thread. */
void ui_publish_both_and_redraw(const dsd_opts* opts, const dsd_state* state);
