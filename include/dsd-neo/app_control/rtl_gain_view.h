// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Frontend-neutral tuner gain readout under --trunk-scan: the gain in force, the configured gain the controls
 * edit, and whether the parked target's own rtl_gain overrides it (issue #518 follow-up).
 *
 * A trunk-scan target can carry its own rtl_gain. While it is on air the gain in force is the target's, but the gain
 * controls edit the configured gain every target without one runs, and a save writes that, as with a row's
 * --squelch-db. Outside a trunk scan the configured gain is the gain in force. This view owns those decisions and the
 * terminal's text ("Gain... [20] (target: 10)") and toasts; Qt reads the same numbers.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_RTL_GAIN_VIEW_H_
#define DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_RTL_GAIN_VIEW_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Tuner gain readout. Gains are in driver dB units; 0 is AGC. */
typedef struct {
    int effective_gain;          /**< The gain in force (opts->rtl_gain_value). */
    int configured_gain;         /**< What the gain controls edit and saves write. */
    uint8_t row_override;        /**< 1 while the parked trunk-scan target runs its own rtl_gain. */
    uint8_t scan_configured;     /**< 1 while a trunk scan holds the configured gain and autogain. */
    uint8_t configured_autogain; /**< The configured tuner autogain; meaningful when scan_configured is 1. */
} dsd_app_rtl_gain_view;

/**
 * @brief Fill @p out from decoder state or a frontend snapshot pair. Zeroes @p out first. Returns 0, or -1 for NULL
 * arguments.
 */
int dsd_app_rtl_gain_view_get(const dsd_opts* opts, const dsd_state* state, dsd_app_rtl_gain_view* out);

/**
 * @brief Render the menu label: "Gain... [20]", "Gain... [AGC]", or "Gain... [20] (target: 10)" while the parked
 * target overrides the configured gain. Returns 0, or -1 when @p out is NULL or @p out_size is zero.
 */
int dsd_app_rtl_gain_view_label(const dsd_app_rtl_gain_view* view, char* out, size_t out_size);

/**
 * @brief Render the notice after the configured gain was edited: "Applied: RTL gain -> 20", or
 * "Default RTL gain -> 20; this channel overrides it (10)" when the parked target shadows the edit.
 * Returns 0, or -1 on bad arguments.
 */
int dsd_app_rtl_gain_view_edit_notice(const dsd_app_rtl_gain_view* view, char* out, size_t out_size);

/**
 * @brief Render the tuner autogain menu label. Outside a trunk scan, "Tuner autogain [On]" from @p live_on (the
 * stream's supervisor). Under one, the configured setting, with "(suspended: gain 10)" while a manual gain in force
 * keeps the supervisor off. Returns 0, or -1 on bad arguments.
 */
int dsd_app_rtl_autogain_view_label(const dsd_app_rtl_gain_view* view, int live_on, char* out, size_t out_size);

/**
 * @brief Render the notice after a trunk scan's configured tuner autogain was toggled: "Applied: tuner autogain ->
 * On", or "Default tuner autogain -> On; manual gain 10 suspends it" while a manual gain is in force.
 * Returns 0, or -1 on bad arguments.
 */
int dsd_app_rtl_autogain_view_edit_notice(const dsd_app_rtl_gain_view* view, char* out, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_RTL_GAIN_VIEW_H_ */
