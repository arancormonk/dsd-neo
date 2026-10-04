// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Frontend-neutral squelch readout: the effective threshold, the configured default and
 * whether a scan row overrides it (issue #521).
 *
 * A channel-map row or trunk-scan target can carry its own `--squelch-db`. While it is on air
 * the threshold in force is the row's, but every editor still edits the configured default
 * and every save writes it. Every frontend follows the same rule: the effective value first,
 * then the row note and the default while a row overrides it.
 *
 * This view owns the decisions (which level is in force, whether a row overrides it, whether
 * each level is off) and the terminal's text ("-60.0 dB (row; default -80.0 dB)") and toast. The
 * Qt/Android radio panel lays the same decisions out as its whole-dB stepper reading, a "row"
 * badge and "default X", from the numeric pair and the off flags, and gives the control this
 * text as its accessible name.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_SQUELCH_VIEW_H_
#define DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_SQUELCH_VIEW_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Squelch readout. Levels are mean-power thresholds (rtl_squelch_level's units; 0 = off). */
typedef struct {
    double effective_level;  /**< The threshold in force on the row on air (a level setting's). */
    double configured_level; /**< The configured default; what editors change and saves write. */
    uint8_t row_override;    /**< 1 while the active row or target sets its own squelch. */
    uint8_t effective_off;   /**< 1 when the setting in force gates nothing: a level at or below 0, or auto off here. */
    uint8_t configured_off;  /**< 1 when configured_level gates nothing under a level default. */
    /* The auto squelch (issue #518 follow-up). Each setting is a level or AUTO with a margin over the floor the
       demodulator learns; the level fields above still hold the level beneath an AUTO setting. */
    uint8_t effective_auto;   /**< 1 when the setting in force is AUTO. */
    uint8_t configured_auto;  /**< 1 when the configured default is AUTO. */
    int effective_margin_db;  /**< Its margin, when effective_auto. */
    int configured_margin_db; /**< Its margin, when configured_auto. */
    uint8_t auto_resolution;  /**< Under effective_auto: dsd_squelch_resolution (off without a radio input, off on
                                   a digital channel, or as set). */
    uint8_t auto_running;     /**< The stream runs the floor tracker (dsd_state::squelch_auto_active). */
    uint8_t auto_learning;    /**< Running, and no floor yet. */
    uint8_t auto_plan_valid;  /**< Running with a channel plan (0: the gate stays open). */
    uint8_t auto_gate_open;   /**< Running, and the gate is open. */
    double auto_floor_db;     /**< Running with a floor: it, on rtl_squelch_level's dB scale. */
} dsd_app_squelch_view;

/**
 * @brief Fill @p out from decoder state or a frontend snapshot pair.
 *
 * Correct on the decoder thread while a scan scope is suspended for a command, where dsd_opts
 * holds the configured default and the row's value comes from its installed options. Zeroes
 * @p out first. Returns 0, or -1 for NULL arguments.
 */
int dsd_app_squelch_view_get(const dsd_opts* opts, const dsd_state* state, dsd_app_squelch_view* out);

/**
 * @brief Render the readout: "-60.0 dB (row; default -80.0 dB)" while a row overrides the
 * squelch, otherwise just "-80.0 dB". Either level reads "off" when it gates nothing.
 * An auto squelch reads "auto +10 dB" with what it shows in force: "(floor -78.3 dB)", "(learning)",
 * "(off: no radio input)", "(off on digital)", "(off: no channel plan)", joined with the row note:
 * "auto +6 dB (floor -81.0 dB; row; default -60.0 dB)".
 * Returns 0, or -1 when @p out is NULL or @p out_size is zero.
 */
int dsd_app_squelch_view_format(const dsd_app_squelch_view* view, char* out, size_t out_size);

/**
 * @brief What an AUTO setting in force shows, without the setting or the row note: "floor -78.3 dB", "learning",
 * "off: no radio input", "off on digital" or "off: no channel plan"; "" for a level. The parenthesis
 * dsd_app_squelch_view_format() prints, for a frontend that lays it out on its own line.
 * Returns 0, or -1 when @p out is NULL, @p out_size is zero or @p view is NULL.
 */
int dsd_app_squelch_view_auto_status(const dsd_app_squelch_view* view, char* out, size_t out_size);

/**
 * @brief Render the notice after the configured default was edited.
 *
 * "Default squelch -75.0 dB; this channel overrides it (-60.0 dB)" when the row on air shadows
 * the edit, otherwise "Applied: RTL squelch -> -75.0 dB". Returns 0, or -1 on bad arguments.
 */
int dsd_app_squelch_view_edit_notice(const dsd_app_squelch_view* view, char* out, size_t out_size);

/**
 * @brief The configured default as its setting reads in an editor: "auto+10", "-60", "off" (the squelch grammar, so it
 * parses back to the same setting, a level within its one-place rounding). Returns 0, or -1 on bad arguments.
 */
int dsd_app_squelch_view_configured_text(const dsd_app_squelch_view* view, char* out, size_t out_size);

/**
 * @brief A threshold in dB in the rtl_sql convention, for numeric frontends: 0 when it is off,
 * otherwise its level in dB, clamped to -120..0 like pwr_to_dB(). Every threshold the dB forms
 * can set is negative; only the legacy linear form can reach full scale and read as 0, so a
 * frontend tells off from full scale with the view's off flags, never with the sign of this.
 */
double dsd_app_squelch_db_or_off(double level);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_SQUELCH_VIEW_H_ */
