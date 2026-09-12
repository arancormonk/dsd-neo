// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Frontend-neutral decisions about what the scan timing line should say (issue #508).
 *
 * The decoder publishes why the scanner is staying on the row on air and the monotonic
 * deadline of whichever window is running (dsd_state::scan_timing). A status surface has
 * to turn that into a phrase, a remaining time and a set of "show this budget" verdicts.
 * That translation lives here, shared, so the terminal row, the Qt panel and the Android
 * app cannot drift apart on what "suspended" or "hold" means.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_SCAN_TIMING_VIEW_H_
#define DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_SCAN_TIMING_VIEW_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief What the idle dwell budget is doing while the row is on air. */
enum {
    /* Value 1 is reserved; keep these values stable for existing QML consumers. */
    DSD_APP_SCAN_DWELL_NONE = 0,      /**< No dwell budget applies (or it is the live window). */
    DSD_APP_SCAN_DWELL_SUSPENDED = 2, /**< Disarmed while something else holds the row. */
    DSD_APP_SCAN_DWELL_PAUSED = 3,    /**< Disarmed by the operator hold. */
};

/**
 * @brief Display-ready scan timing for the row on air.
 *
 * @c phrase is a static English label; frontends that translate wrap it themselves.
 * Every millisecond field is already clamped and derived, so a surface only formats.
 */
typedef struct {
    uint8_t active;        /**< 0 = no scanner on, or nothing published: render nothing. */
    uint8_t reason;        /**< dsd_scan_stay_reason. */
    const char* phrase;    /**< Static label for @c reason (and the voice-gate phase). */
    uint8_t timer_live;    /**< 1 = @c remaining_ms / @c span_ms describe a running window. */
    uint32_t remaining_ms; /**< max(0, deadline - now). */
    uint32_t span_ms;      /**< Full width of the running window. */
    uint8_t show_dwell;    /**< 1 = @c dwell_ms is worth printing beside the phrase. */
    uint32_t dwell_ms;     /**< Effective idle dwell (or -Y qualify window). */
    uint8_t dwell_state;   /**< One of the DSD_APP_SCAN_DWELL_* values. */
    uint8_t show_hold;     /**< 1 = @c hold_ms applies (conventional rows only). */
    uint32_t hold_ms;      /**< Effective activity hold. */
    uint8_t show_hang;     /**< 1 = @c hang_ms governs the current stay (-t). */
    uint32_t hang_ms;      /**< Active protocol's effective voice/sync-loss hangtime. */
} dsd_app_scan_timing;

/**
 * @brief Fill @p out from the published scan timing.
 *
 * Zeroes @p out first. @p now_m is monotonic seconds, the clock the decoder stamps the
 * deadlines from. Returns 1 when there is something to show (@c active), 0 when the row
 * should be left blank, and -1 for invalid arguments.
 */
int dsd_app_scan_timing_view(const dsd_opts* opts, const dsd_state* state, double now_m, dsd_app_scan_timing* out);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_SCAN_TIMING_VIEW_H_ */
