// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The next-start rule (issue #671) on any options and state, live or a published snapshot pair.
 *
 * Module-private and pure: the save a change would leave is held to dsd_user_config_check_trunk_scan_startable(). The
 * decoder thread asks it through svc_check_next_start() (services.h), and a frontend's pre-check of a RadioReference
 * import asks it on the snapshot copies (dsd_app_rr_apply_block_reason()), so both answer from one body.
 */

#ifndef DSD_NEO_SRC_APP_CONTROL_NEXT_START_CHECK_H_
#define DSD_NEO_SRC_APP_CONTROL_NEXT_START_CHECK_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/runtime/config.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * The save the session would write after a change: @p save holds the one it would write now
 * (dsd_snapshot_opts_to_user_config(), the values a config left for the next start included), which the callback edits
 * as the change would leave it. @p ctx is the caller's.
 */
typedef void (*app_next_start_change_fn)(dsdneoUserConfig* save, const void* ctx);

/**
 * @brief Whether a change keeps the next start one that starts (issue #671).
 *
 * Every save writes what the session runs and what a config left for the next start, so a change must not make that
 * save one startup refuses: the save @p change leaves is held to dsd_user_config_check_trunk_scan_startable(), and only
 * a rule the change newly breaks refuses it (a rule the session's own save breaks already, kept or fixed, does not).
 *
 * @return 0 when the change may go ahead; -1 when it may not (or memory runs out), with the reason in @p why.
 */
int app_check_next_start(const dsd_opts* opts, const dsd_state* state, app_next_start_change_fn change, const void* ctx,
                         char* why, size_t why_size);

/**
 * @brief The change loading a channel map makes, and nothing else: @p ctx is its path (const char*). The decoder's
 *        channel-map import (svc_import_channel_map()) and a frontend's pre-check of loading files alone
 *        (dsd_app_rr_files_block_reason()) both ask it, so the two cannot drift apart.
 */
void app_next_start_load_channel_map(dsdneoUserConfig* save, const void* ctx);

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* DSD_NEO_SRC_APP_CONTROL_NEXT_START_CHECK_H_ */
