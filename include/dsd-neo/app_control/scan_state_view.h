// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Frontend-neutral scan state: which scan the session has, and whether it runs or the operator paused it
 * (issue #671).
 *
 * A -Y scan the operator paused has scanner_mode 0 and dsd_state::lcn_scan_paused set; a paused --trunk-scan keeps
 * dsd_opts::trunk_scan_enabled (its configured setting) and sets dsd_state::trunk_scan_paused. Reading those four
 * fields right is easy to get wrong in one place and not another, so this view is the one predicate the terminal's
 * labels and gates, the Qt/Android metrics and the API feed's "scan" object read. It reads only the fields it names,
 * from a published snapshot pair or from the decoder's own structs, and costs a few loads.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_SCAN_STATE_VIEW_H_
#define DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_SCAN_STATE_VIEW_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Which scan the session has. */
typedef enum {
    DSD_APP_SCAN_KIND_NONE = 0,    /**< No scan: neither -Y running or paused, nor --trunk-scan. */
    DSD_APP_SCAN_KIND_CHANNEL = 1, /**< The conventional -Y channel scan, running or paused. */
    DSD_APP_SCAN_KIND_TRUNK = 2,   /**< --trunk-scan, running or paused; it lives until shutdown. */
} dsd_app_scan_kind;

/**
 * @brief The scan state as every frontend shows it.
 *
 * For a scan, exactly one of @c running and @c paused is 1; with @c kind NONE both are 0.
 */
typedef struct {
    uint8_t kind;           /**< A dsd_app_scan_kind. */
    uint8_t running;        /**< 1 = the scan owns the tuner now (dsd_trunk_scan_owns_tuner(), or -Y on). */
    uint8_t paused;         /**< 1 = the operator paused it; it waits for a resume. */
    uint8_t resume_pending; /**< 1 = a -Y resume waits for its row's tune; still @c paused until it lands. */
    uint16_t target_count;  /**< The trunk scan's targets, or the -Y list's rows; 0 with no scan. */
    uint8_t operator_tuned; /**< 1 = the operator moved the receiver off its session (a pause, a release, a tune). */
} dsd_app_scan_state_view;

/**
 * @brief Fill @p out from decoder state or a frontend snapshot pair. Zeroes @p out first.
 *
 * A trunk scan wins when --trunk-scan is configured (the two scans exclude each other). @c operator_tuned is reported
 * whatever the kind: a tuner release of trunking (Explore under -T) or a manual tune with no scan sets it too.
 *
 * @return 0, or -1 for NULL arguments.
 */
int dsd_app_scan_state_view_get(const dsd_opts* opts, const dsd_state* state, dsd_app_scan_state_view* out);

/** @brief The API spelling of @p kind: "none", "channel" or "trunk". */
const char* dsd_app_scan_kind_name(uint8_t kind);

/** @brief The API spelling of the view's state: "off" (no scan), "running" or "paused". */
const char* dsd_app_scan_state_name(const dsd_app_scan_state_view* view);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_SCAN_STATE_VIEW_H_ */
