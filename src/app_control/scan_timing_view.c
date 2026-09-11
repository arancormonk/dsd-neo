// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/app_control/scan_timing_view.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <stdint.h>

/**
 * @brief What one stay reason renders as, before the row's own numbers are applied.
 *
 * A table rather than a switch so the display rule is one readable list: the terminal
 * row, the Qt panel and the Android app all render from this, and a rule spelled out in
 * three places is a rule that drifts. Each flag is a verdict about whether the budget is
 * worth a column at all; whether it has a value to print is the publication's business.
 */
typedef struct {
    const char* phrase;  /**< Static English label; a phase variant may override it. */
    uint8_t dwell_state; /**< DSD_APP_SCAN_DWELL_* to report while the dwell is shown. */
    uint8_t show_dwell;  /**< 0 where the dwell is the live window, so the span already says it. */
    uint8_t show_hold;   /**< 0 where the hold is the live window, or the row is trunked. */
    uint8_t show_hang;   /**< 1 only where -t is what ends the stay. */
} dsd_scan_timing_row;

/* Indexed by dsd_scan_stay_reason. NONE is present so the array covers the enum and an
   out-of-range reason from a newer decoder is rejected by a bound rather than by luck. */
static const dsd_scan_timing_row k_scan_timing_rows[] = {
    [DSD_SCAN_STAY_NONE] = {"", DSD_APP_SCAN_DWELL_NONE, 0U, 0U, 0U},
    [DSD_SCAN_STAY_RETUNE_PENDING] = {"Retune pending", DSD_APP_SCAN_DWELL_SUSPENDED, 1U, 1U, 0U},
    [DSD_SCAN_STAY_RETUNE_RETRY] = {"Retune retry", DSD_APP_SCAN_DWELL_SUSPENDED, 1U, 1U, 0U},
    [DSD_SCAN_STAY_CC_ACQUIRE] = {"Acquiring control", DSD_APP_SCAN_DWELL_SUSPENDED, 1U, 0U, 0U},
    [DSD_SCAN_STAY_CALL_FOLLOW] = {"Following call", DSD_APP_SCAN_DWELL_SUSPENDED, 1U, 0U, 1U},
    [DSD_SCAN_STAY_VOICE] = {"Voice", DSD_APP_SCAN_DWELL_SUSPENDED, 1U, 0U, 0U},
    [DSD_SCAN_STAY_ACTIVITY_HOLD] = {"Activity hold", DSD_APP_SCAN_DWELL_SUSPENDED, 1U, 0U, 0U},
    [DSD_SCAN_STAY_MANUAL_HOLD] = {"Manual hold", DSD_APP_SCAN_DWELL_PAUSED, 1U, 1U, 0U},
    [DSD_SCAN_STAY_IDLE_DWELL] = {"Idle dwell", DSD_APP_SCAN_DWELL_NONE, 0U, 1U, 0U},
    [DSD_SCAN_STAY_HANGTIME] = {"Hangtime", DSD_APP_SCAN_DWELL_NONE, 0U, 0U, 0U},
};

/* A reason the decoder can publish but this table cannot render would reach the surfaces
   as a blank phrase. Sized against the last enumerator so adding one fails the build. */
_Static_assert((int)(sizeof(k_scan_timing_rows) / sizeof(k_scan_timing_rows[0])) == (int)DSD_SCAN_STAY_HANGTIME + 1,
               "every dsd_scan_stay_reason needs a display row");

/** @brief Longest countdown the view will report, guarding the cast below. */
#define DSD_APP_SCAN_REMAINING_MAX_S 86400.0

/**
 * @brief Whether there is a stay worth rendering at all.
 *
 * Both halves matter. A stale publication survives the scanner being switched off --
 * nothing clears it on the way out -- so the scanner flags are what make it current; and
 * a reason this build does not know is from a newer decoder writing the same snapshot
 * bytes, which must render nothing rather than an empty phrase.
 */
static int
scan_timing_is_active(const dsd_opts* opts, const dsd_scan_timing_publication* pub) {
    if (opts->trunk_scan_enabled != 1 && opts->scanner_mode != 1) {
        return 0;
    }
    if (pub->reason == (uint8_t)DSD_SCAN_STAY_NONE) {
        return 0;
    }
    return pub->reason < (uint8_t)(sizeof(k_scan_timing_rows) / sizeof(k_scan_timing_rows[0]));
}

/**
 * @brief The two rows whose label depends on what the voice gate is doing.
 *
 * The gate's phase is the only thing that separates a hold that is still following voice
 * from one counting out its tail, and an idle dwell from the qualify window that shares
 * its clock. Both are cosmetic refinements of the same stay, which is why they are a
 * phrase choice here rather than reasons of their own.
 */
static const char*
scan_timing_phrase(const dsd_scan_timing_row* row, uint8_t reason, uint8_t phase) {
    if (reason == (uint8_t)DSD_SCAN_STAY_ACTIVITY_HOLD && phase == (uint8_t)DSD_SCAN_VOICE_GATE_TAIL) {
        return "Voice tail";
    }
    if (reason == (uint8_t)DSD_SCAN_STAY_IDLE_DWELL && phase == (uint8_t)DSD_SCAN_VOICE_GATE_QUALIFY) {
        return "Qualify";
    }
    return row->phrase;
}

/**
 * @brief Milliseconds left on the published deadline, clamped at zero.
 *
 * The decoder decides when the receiver moves; a poll that lands after the deadline is a
 * frame late, not a scanner that overstayed. Clamping through the comparison's negation
 * also settles a NaN clock, which would otherwise cast to an arbitrary count.
 */
static uint32_t
scan_timing_remaining_ms(double deadline_m, double now_m) {
    double remaining_s = deadline_m - now_m;
    if (!(remaining_s > 0.0)) {
        return 0U;
    }
    if (remaining_s > DSD_APP_SCAN_REMAINING_MAX_S) {
        remaining_s = DSD_APP_SCAN_REMAINING_MAX_S;
    }
    return (uint32_t)((remaining_s * 1000.0) + 0.5);
}

/** @brief -t in milliseconds, rounded, or 0 when it is not configured. */
static uint32_t
scan_timing_hang_ms(const dsd_opts* opts) {
    double hang_s = (double)opts->trunk_hangtime;
    if (!(hang_s > 0.0)) {
        return 0U;
    }
    if (hang_s > DSD_APP_SCAN_REMAINING_MAX_S) {
        hang_s = DSD_APP_SCAN_REMAINING_MAX_S;
    }
    return (uint32_t)((hang_s * 1000.0) + 0.5);
}

/**
 * @brief Fill in the budgets beside the phrase.
 *
 * Every effective value is copied through whether or not it is shown -- a surface with
 * room for more than one line can still report the dwell that is counting down -- while
 * the show_ flags carry the display rule.
 */
static void
scan_timing_fill_budgets(dsd_app_scan_timing* out, const dsd_scan_timing_row* row,
                         const dsd_scan_timing_publication* pub, const dsd_opts* opts) {
    out->dwell_ms = pub->dwell_ms;
    if (row->show_dwell != 0U && pub->dwell_ms != 0U) {
        out->show_dwell = 1U;
        out->dwell_state = row->dwell_state;
    }
    out->hold_ms = pub->hold_ms;
    if (row->show_hold != 0U && pub->conventional == 1U && pub->hold_ms != 0U) {
        out->show_hold = 1U;
    }
    if (row->show_hang != 0U) {
        out->hang_ms = scan_timing_hang_ms(opts);
        out->show_hang = (out->hang_ms != 0U) ? 1U : 0U;
    }
}

int
dsd_app_scan_timing_view(const dsd_opts* opts, const dsd_state* state, double now_m, dsd_app_scan_timing* out) {
    if (out) {
        DSD_MEMSET(out, 0, sizeof(*out));
    }
    if (!opts || !state || !out) {
        return -1;
    }
    const dsd_scan_timing_publication* pub = &state->scan_timing;
    if (!scan_timing_is_active(opts, pub)) {
        return 0;
    }
    const dsd_scan_timing_row* row = &k_scan_timing_rows[pub->reason];
    out->active = 1U;
    out->reason = pub->reason;
    out->phrase = scan_timing_phrase(row, pub->reason, state->scan_voice_gate_phase);
    if (pub->deadline_m >= 0.0) {
        out->timer_live = 1U;
        out->remaining_ms = scan_timing_remaining_ms(pub->deadline_m, now_m);
        out->span_ms = pub->span_ms;
    }
    scan_timing_fill_budgets(out, row, pub, opts);
    return 1;
}
