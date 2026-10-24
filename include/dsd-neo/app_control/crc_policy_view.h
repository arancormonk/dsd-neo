// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Frontend-neutral Relaxed CRC policy (-F): the configured value, the value in force, and whether a scan row
 * decides it.
 *
 * Relaxed CRC is one setting (dsd_opts_set_relaxed_crc()). The configured value is the one the menu toggle
 * (DSD_APP_CMD_AGGR_SYNC_TOGGLE) edits: while a scan scope is live it is the scope's configured baseline
 * (dsd_scan_mode_configured_view()), which a row's -F or --strict-crc runs over until the row leaves. The terminal
 * menu reads it here, and so does any other frontend that shows or explains the policy, so they agree.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_CRC_POLICY_VIEW_H_
#define DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_CRC_POLICY_VIEW_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief The Relaxed CRC policy as every frontend shows it. */
typedef struct {
    uint8_t configured;   /**< 1 = Relaxed CRC is configured on: what the toggle edits and a row's leave returns to. */
    uint8_t effective;    /**< 1 = Relaxed CRC is in force now. */
    uint8_t row_override; /**< 1 = the scan row on air sets its own (-F or --strict-crc), which decides @c effective. */
} dsd_app_crc_policy;

/** @brief Room for dsd_app_crc_policy_edit_notice(), terminator included (a toast, dsd_state::ui_msg). */
enum { DSD_APP_CRC_POLICY_NOTICE_SIZE = 96 };

/**
 * @brief Fill @p out from decoder state or a frontend snapshot pair.
 *
 * Zeroes @p out first. A NULL @p state reads @p opts alone (no scan scope); a NULL @p opts leaves everything off.
 * Right in the middle of a command too: while the scope is suspended dsd_opts holds the configured value and the
 * installed row options still name the row's own.
 */
void dsd_app_crc_policy_view(const dsd_opts* opts, const dsd_state* state, dsd_app_crc_policy* out);

/**
 * @brief Write the notice after an edit of the configured value: "Applied: relaxed CRC -> On", or while the scan row on
 * air sets its own "Default relaxed CRC -> On; this channel overrides it (Off)", as the squelch and width edits say it.
 * Returns 1 when a row overrides the edit, 0 when it does not, and -1 for NULL @p out or a zero @p out_size.
 */
int dsd_app_crc_policy_edit_notice(const dsd_opts* opts, const dsd_state* state, char* out, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_CRC_POLICY_VIEW_H_ */
