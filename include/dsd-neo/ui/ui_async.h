// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Async ncurses UI thread lifecycle and command helpers.
 */
#pragma once

#include <stddef.h>

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

/**
 * @brief Start the async UI subsystem (spawns UI thread).
 * @return 0 on success or when already running; negative on failure.
 */
int ui_start(dsd_opts* opts, dsd_state* state);

/** @brief Stop the async UI subsystem and join the UI thread. */
void ui_stop(void);

/** @brief Request a redraw from demod/decoder side (marks UI dirty). */
void ui_request_redraw(void);

/**
 * @brief Post a UI command from the UI thread producer.
 *
 * Truncates payloads to the maximum queue payload size if needed.
 */
int ui_post_cmd(int cmd_id, const void* payload, size_t payload_sz);

/**
 * @brief Drain queued UI commands on the demod/decoder consumer thread.
 * @return Number of commands applied.
 */
int ui_drain_cmds(dsd_opts* opts, dsd_state* state);

/**
 * @brief Returns non-zero when executing in the UI thread context.
 *
 * Use to keep ncurses calls on the UI thread while decoder threads only
 * publish snapshots.
 */
int ui_is_thread_context(void);

/**
 * @brief Publish opts/state snapshots and request a redraw (thread-safe).
 */
void ui_publish_both_and_redraw(const dsd_opts* opts, const dsd_state* state);
