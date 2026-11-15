// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/app_control/scan_timing_view.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/runtime/tuner_owner.h>
#include <stdint.h>
#include <string.h>

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
    uint8_t show_dwell;  /**< 0 where the dwell is the live window, so the span already says it, or runs on unchanged
                              beneath the stay (a tone check). */
    uint8_t show_hold;   /**< 0 where the hold is the live window, or the row is trunked. */
    uint8_t show_hang;   /**< 1 only where protocol hangtime is what ends the stay. */
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
    /* An analog row's carrier holds it the way voice holds a digital one: the dwell is disarmed and
       the live window is the hold (or -Y hangtime) running from the last carrier. */
    [DSD_SCAN_STAY_CARRIER] = {"Carrier", DSD_APP_SCAN_DWELL_SUSPENDED, 1U, 0U, 0U},
    /* The same carrier while the tone policy is still checking it (issue #527): muted, and held only while it
       lasts, for no longer than the policy's bounded window, after which the row plays (Carrier) or moves on. The
       check is no activity: it has no window of its own, and whatever ran before it -- the -Y hangtime, a trunk-scan
       hold or idle dwell -- runs on beneath it unchanged, so no budget is shown as suspended. */
    [DSD_SCAN_STAY_TONE_PENDING] = {"Tone check", DSD_APP_SCAN_DWELL_NONE, 0U, 0U, 0U},
    /* -Y with no channel map imported (issue #697): it stays on the frequency until one is, so no window runs and no
       budget applies. */
    [DSD_SCAN_STAY_NO_LIST] = {"No channel map", DSD_APP_SCAN_DWELL_NONE, 0U, 0U, 0U},
};

/* A reason the decoder can publish but this table cannot render would reach the surfaces
   as a blank phrase. Sized against the last enumerator so adding one fails the build. */
_Static_assert((int)(sizeof(k_scan_timing_rows) / sizeof(k_scan_timing_rows[0])) == (int)DSD_SCAN_STAY_NO_LIST + 1,
               "every dsd_scan_stay_reason needs a display row");

/**
 * @brief What the silent-channel skip's status renders as on the stay it is shown on (issue #834).
 *
 * The compact reason is the phrase itself, at most 17 characters (the widest plain phrase), so the phrase column and
 * every cut of it hold. The sentence is the full reason for a screen reader; the WARN line in the log says how to fix
 * it. WIDTH and NO_WIDTH share a phrase but not a sentence, since their remedies differ.
 */
typedef struct {
    const char* phrase;
    const char* text;
} dsd_scan_quiet_row;

static const dsd_scan_quiet_row k_scan_quiet_checking = {
    "Quiet check", "Checking whether this channel is silent. If it is, the scan moves on early."};

/* Indexed by dsd_scan_quiet_reason, for DSD_SCAN_QUIET_STATUS_NO_CHECK. */
static const dsd_scan_quiet_row k_scan_quiet_no_check[] = {
    [DSD_SCAN_QUIET_REASON_NONE] = {NULL, NULL},
    [DSD_SCAN_QUIET_REASON_INPUT] = {"No check: input",
                                     "The silent channel skip cannot check channels on this input. Normal scan timing "
                                     "applies."},
    [DSD_SCAN_QUIET_REASON_TUNER] = {"No check: tuner",
                                     "The silent channel skip cannot check channels with this tuner as it is set up. "
                                     "Normal scan timing applies."},
    [DSD_SCAN_QUIET_REASON_RATE] = {"No check: rate",
                                    "The silent channel skip cannot check channels at this sample rate. Normal scan "
                                    "timing applies."},
    [DSD_SCAN_QUIET_REASON_MODES] = {"No check: modes",
                                     "The silent channel skip cannot check this channel with the decoders in use. Give "
                                     "the channel a mode to have it checked. Normal scan timing applies."},
    [DSD_SCAN_QUIET_REASON_WIDTH] = {"No check: width",
                                     "The silent channel skip cannot check this channel at its channel width. Normal "
                                     "scan timing applies."},
    [DSD_SCAN_QUIET_REASON_NO_WIDTH] = {"No check: width",
                                        "The silent channel skip cannot check this channel with no NFM channel width "
                                        "set. Set a width to have it checked. Normal scan timing applies."},
};

/* A reason the decoder can publish but this table cannot render would show the plain phrase and hide why. */
_Static_assert((int)(sizeof(k_scan_quiet_no_check) / sizeof(k_scan_quiet_no_check[0]))
                   == (int)DSD_SCAN_QUIET_REASON_NO_WIDTH + 1,
               "every dsd_scan_quiet_reason needs a display row");

/** @brief Longest countdown the view will report, guarding the cast below. */
#define DSD_APP_SCAN_REMAINING_MAX_S ((double)UINT32_MAX / 1000.0)

/**
 * @brief Whether there is a stay worth rendering at all.
 *
 * Both halves matter. A stale publication survives the scanner being switched off --
 * nothing clears it on the way out -- so the scanner flags are what make it current; and
 * a reason this build does not know is from a newer decoder writing the same snapshot
 * bytes, which must render nothing rather than an empty phrase.
 */
static int
scan_timing_is_active(const dsd_opts* opts, const dsd_state* state, const dsd_scan_timing_publication* pub) {
    /* A paused trunk scan runs no timer (issue #671). */
    if (!dsd_trunk_scan_owns_tuner(opts, state) && opts->scanner_mode != 1) {
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
    if (remaining_s >= DSD_APP_SCAN_REMAINING_MAX_S) {
        return UINT32_MAX;
    }
    return (uint32_t)((remaining_s * 1000.0) + 0.5);
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
                         const dsd_scan_timing_publication* pub) {
    out->dwell_ms = pub->dwell_ms;
    // After hold release the next decoder tick arms the dwell. Until then its
    // effective budget is still useful even though no remaining/total pair runs.
    const int dwell_unarmed = pub->reason == (uint8_t)DSD_SCAN_STAY_IDLE_DWELL && !out->timer_live;
    if ((row->show_dwell != 0U || dwell_unarmed) && pub->dwell_ms != 0U) {
        out->show_dwell = 1U;
        out->dwell_state = row->dwell_state;
    }
    out->hold_ms = pub->hold_ms;
    if (row->show_hold != 0U && pub->conventional == 1U && pub->hold_ms != 0U) {
        out->show_hold = 1U;
    }
    if (row->show_hang != 0U) {
        out->hang_ms = pub->hang_ms;
        out->show_hang = (out->hang_ms != 0U) ? 1U : 0U;
    }
}

/**
 * @brief Fill in the per-visit cap, which every row carries (issue #507).
 *
 * Deliberately outside k_scan_timing_rows[]: the cap is a ceiling on the whole visit
 * rather than a budget one stay reason owns, so no reason may withhold it -- the receiver
 * moves on when it expires whatever is on the air. The deadline decides only whether it
 * is counting: negative means disabled, not yet anchored, or suspended by a hold, and a
 * surface has to say so in words, because a zero countdown reads as a visit that expired.
 */
static void
scan_timing_fill_visit(dsd_app_scan_timing* out, const dsd_scan_timing_publication* pub, double now_m) {
    out->visit_ms = pub->visit_limit_ms;
    if (pub->visit_limit_ms == 0U) {
        return;
    }
    out->show_visit = 1U;
    if (pub->visit_deadline_m >= 0.0) {
        out->visit_live = 1U;
        out->visit_remaining_ms = scan_timing_remaining_ms(pub->visit_deadline_m, now_m);
    }
}

/**
 * @brief The silent-channel skip's variant of the phrase, when the decoder published one (issue #834).
 *
 * Only what the decoder said, on the stays it says it on: an idle dwell (the trunk scan's, or the -Y voice gate's
 * qualify window) or a -Y hangtime. A status on any other stay, an unknown status, or a NO_CHECK without a reason this
 * build knows leaves the plain phrase: the view never makes up a state. The variant outranks "Qualify", the gate's
 * name for the same idle dwell: on that window it is the check that may end the visit early.
 */
static void
scan_timing_fill_quiet(dsd_app_scan_timing* out, const dsd_scan_timing_publication* pub) {
    if (pub->reason != (uint8_t)DSD_SCAN_STAY_IDLE_DWELL && pub->reason != (uint8_t)DSD_SCAN_STAY_HANGTIME) {
        return;
    }
    const dsd_scan_quiet_row* quiet = NULL;
    uint8_t reason = (uint8_t)DSD_SCAN_QUIET_REASON_NONE;
    if (pub->quiet_status == (uint8_t)DSD_SCAN_QUIET_STATUS_CHECKING) {
        quiet = &k_scan_quiet_checking;
    } else if (pub->quiet_status == (uint8_t)DSD_SCAN_QUIET_STATUS_NO_CHECK
               && pub->quiet_reason < (uint8_t)(sizeof k_scan_quiet_no_check / sizeof k_scan_quiet_no_check[0])) {
        quiet = &k_scan_quiet_no_check[pub->quiet_reason];
        reason = pub->quiet_reason;
    }
    if (!quiet || !quiet->phrase) {
        return;
    }
    out->quiet_status = pub->quiet_status;
    out->quiet_reason = reason;
    out->phrase = quiet->phrase;
    out->quiet_text = quiet->text;
}

static size_t
scan_timing_longest(size_t width, const char* phrase) {
    const size_t n = phrase ? strlen(phrase) : 0U;
    return (n > width) ? n : width;
}

size_t
dsd_app_scan_timing_phrase_width(void) {
    static const char* const variants[] = {"Voice tail", "Qualify"};
    size_t width = 0U;
    for (size_t i = 0; i < sizeof k_scan_timing_rows / sizeof k_scan_timing_rows[0]; i++) {
        width = scan_timing_longest(width, k_scan_timing_rows[i].phrase);
    }
    for (size_t i = 0; i < sizeof variants / sizeof variants[0]; i++) {
        width = scan_timing_longest(width, variants[i]);
    }
    width = scan_timing_longest(width, k_scan_quiet_checking.phrase);
    for (size_t i = 0; i < sizeof k_scan_quiet_no_check / sizeof k_scan_quiet_no_check[0]; i++) {
        width = scan_timing_longest(width, k_scan_quiet_no_check[i].phrase);
    }
    return width;
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
    if (!scan_timing_is_active(opts, state, pub)) {
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
    scan_timing_fill_quiet(out, pub);
    scan_timing_fill_budgets(out, row, pub);
    scan_timing_fill_visit(out, pub, now_m);
    return 1;
}
