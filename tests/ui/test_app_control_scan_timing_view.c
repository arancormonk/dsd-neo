// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Shared scan-timing display decision, independent of any frontend (issue #508).
 */

#include <assert.h>
#include <dsd-neo/app_control/scan_timing_view.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* dsd_state is far too large for the stack; the view only reads scan_timing and the
   voice-gate phase, both of which a zeroed block already answers for. */
static dsd_state*
make_state(void) {
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    assert(state != NULL);
    return state;
}

/* A scanner is running and a target is on air: the precondition every row shares. */
static void
make_opts(dsd_opts* opts) {
    DSD_MEMSET(opts, 0, sizeof(*opts));
    opts->trunk_scan_enabled = 1;
}

/* One published stay, stamped the way the decoder stamps it: an absolute deadline, or
   a negative one when nothing is counting down. */
static void
publish(dsd_state* state, uint8_t reason, uint8_t conventional, double deadline_m, uint32_t span_ms, uint32_t dwell_ms,
        uint32_t hold_ms) {
    DSD_MEMSET(&state->scan_timing, 0, sizeof(state->scan_timing));
    /* A zeroed block leaves visit_deadline_m at 0.0, which reads as a per-visit deadline
       at monotonic 0 -- long past. The decoder never publishes that: an unanchored cap is
       negative (#507), so the seed says so too. */
    state->scan_timing.visit_deadline_m = -1.0;
    state->scan_timing.reason = reason;
    state->scan_timing.conventional = conventional;
    state->scan_timing.started_m = (deadline_m >= 0.0) ? deadline_m - ((double)span_ms / 1000.0) : -1.0;
    state->scan_timing.deadline_m = deadline_m;
    state->scan_timing.span_ms = span_ms;
    state->scan_timing.dwell_ms = dwell_ms;
    state->scan_timing.hold_ms = hold_ms;
}

static void
assert_phrase(const dsd_app_scan_timing* view, const char* phrase) {
    assert(view->phrase != NULL);
    assert(strcmp(view->phrase, phrase) == 0);
}

/* The per-visit cap on top of a published stay (issue #507). Separate from publish()
   because the cap is not a property of the stay reason: the engine anchors it at parking
   and it rides whatever else happens to be holding the row. */
static void
seed_visit(dsd_state* state, uint32_t limit_ms, double visit_deadline_m) {
    state->scan_timing.visit_limit_ms = limit_ms;
    state->scan_timing.visit_deadline_m = visit_deadline_m;
}

/* A backend retune that has not resolved: nothing counts down, the idle dwell is
   disarmed until it does, and a conventional row still states its activity hold. */
static void
test_retune_pending_row(void) {
    static dsd_opts opts;
    dsd_state* state = make_state();
    dsd_app_scan_timing view;

    make_opts(&opts);
    publish(state, DSD_SCAN_STAY_RETUNE_PENDING, 1U, -1.0, 0U, 3000U, 1200U);
    assert(dsd_app_scan_timing_view(&opts, state, 100.0, &view) == 1);
    assert(view.active == 1U);
    assert(view.reason == (uint8_t)DSD_SCAN_STAY_RETUNE_PENDING);
    assert_phrase(&view, "Retune pending");
    assert(view.timer_live == 0U);
    assert(view.remaining_ms == 0U);
    assert(view.span_ms == 0U);
    assert(view.show_dwell == 1U);
    assert(view.dwell_ms == 3000U);
    assert(view.dwell_state == DSD_APP_SCAN_DWELL_SUSPENDED);
    assert(view.show_hold == 1U);
    assert(view.hold_ms == 1200U);
    assert(view.show_hang == 0U);
    assert(view.hang_ms == 0U);

    free(state);
}

/* A failed retune cooling down in place: the cooldown is the live window. */
static void
test_retune_retry_row(void) {
    static dsd_opts opts;
    dsd_state* state = make_state();
    dsd_app_scan_timing view;

    make_opts(&opts);
    publish(state, DSD_SCAN_STAY_RETUNE_RETRY, 0U, 102.0, 2000U, 3000U, 0U);
    assert(dsd_app_scan_timing_view(&opts, state, 100.0, &view) == 1);
    assert_phrase(&view, "Retune retry");
    assert(view.timer_live == 1U);
    assert(view.remaining_ms == 2000U);
    assert(view.span_ms == 2000U);
    assert(view.show_dwell == 1U);
    assert(view.dwell_state == DSD_APP_SCAN_DWELL_SUSPENDED);
    assert(view.show_hold == 0U);

    free(state);
}

/* Hunting a control channel: the trunking state machine owns the clock, so there is
   no deadline to count down and no conventional hold to state. */
static void
test_cc_acquire_row(void) {
    static dsd_opts opts;
    dsd_state* state = make_state();
    dsd_app_scan_timing view;

    make_opts(&opts);
    publish(state, DSD_SCAN_STAY_CC_ACQUIRE, 0U, -1.0, 0U, 3000U, 0U);
    assert(dsd_app_scan_timing_view(&opts, state, 100.0, &view) == 1);
    assert_phrase(&view, "Acquiring control");
    assert(view.timer_live == 0U);
    assert(view.show_dwell == 1U);
    assert(view.dwell_state == DSD_APP_SCAN_DWELL_SUSPENDED);
    assert(view.show_hold == 0U);
    assert(view.show_hang == 0U);

    free(state);
}

/* Following a trunked call: -t is what ends the stay, so it is the one budget worth
   naming beside the suspended dwell. */
static void
test_call_follow_row_shows_hangtime(void) {
    static dsd_opts opts;
    dsd_state* state = make_state();
    dsd_app_scan_timing view;

    make_opts(&opts);
    opts.trunk_hangtime = 2.0f;
    publish(state, DSD_SCAN_STAY_CALL_FOLLOW, 0U, -1.0, 0U, 3000U, 0U);
    assert(dsd_app_scan_timing_view(&opts, state, 100.0, &view) == 1);
    assert_phrase(&view, "Following call");
    assert(view.timer_live == 0U);
    assert(view.show_dwell == 1U);
    assert(view.dwell_state == DSD_APP_SCAN_DWELL_SUSPENDED);
    assert(view.show_hold == 0U);
    assert(view.show_hang == 1U);
    assert(view.hang_ms == 2000U);

    /* A fractional -t rounds to the nearest millisecond rather than truncating. */
    opts.trunk_hangtime = 1.25f;
    assert(dsd_app_scan_timing_view(&opts, state, 100.0, &view) == 1);
    assert(view.hang_ms == 1250U);

    /* -t 0 leaves nothing to state. */
    opts.trunk_hangtime = 0.0f;
    assert(dsd_app_scan_timing_view(&opts, state, 100.0, &view) == 1);
    assert(view.show_hang == 0U);
    assert(view.hang_ms == 0U);

    free(state);
}

/* Hangtime belongs to a followed trunked call and to nothing else: no other row may
   claim -t, whatever it is set to. */
static void
test_hangtime_is_only_shown_while_following_a_call(void) {
    static const uint8_t others[] = {
        DSD_SCAN_STAY_RETUNE_PENDING, DSD_SCAN_STAY_RETUNE_RETRY, DSD_SCAN_STAY_CC_ACQUIRE, DSD_SCAN_STAY_VOICE,
        DSD_SCAN_STAY_ACTIVITY_HOLD,  DSD_SCAN_STAY_MANUAL_HOLD,  DSD_SCAN_STAY_IDLE_DWELL, DSD_SCAN_STAY_HANGTIME,
    };
    static dsd_opts opts;
    dsd_state* state = make_state();
    dsd_app_scan_timing view;

    make_opts(&opts);
    opts.trunk_hangtime = 2.0f;
    for (size_t i = 0; i < sizeof(others) / sizeof(others[0]); i++) {
        publish(state, others[i], 1U, -1.0, 0U, 3000U, 1200U);
        assert(dsd_app_scan_timing_view(&opts, state, 100.0, &view) == 1);
        assert(view.show_hang == 0U);
        assert(view.hang_ms == 0U);
    }

    free(state);
}

/* Conventional voice on air: the hold window is the live timer, so restating it as a
   budget would print the same number twice. */
static void
test_voice_row(void) {
    static dsd_opts opts;
    dsd_state* state = make_state();
    dsd_app_scan_timing view;

    make_opts(&opts);
    publish(state, DSD_SCAN_STAY_VOICE, 1U, 101.2, 2000U, 3000U, 2000U);
    state->scan_voice_gate_phase = DSD_SCAN_VOICE_GATE_VOICE;
    assert(dsd_app_scan_timing_view(&opts, state, 100.0, &view) == 1);
    assert_phrase(&view, "Voice");
    assert(view.timer_live == 1U);
    assert(view.remaining_ms == 1200U);
    assert(view.span_ms == 2000U);
    assert(view.show_dwell == 1U);
    assert(view.dwell_state == DSD_APP_SCAN_DWELL_SUSPENDED);
    assert(view.show_hold == 0U);
    assert(view.hold_ms == 2000U);

    free(state);
}

/* The activity hold reads as a voice tail only while the voice gate says the last
   voice frame has already gone by. */
static void
test_activity_hold_row_names_the_tail(void) {
    static dsd_opts opts;
    dsd_state* state = make_state();
    dsd_app_scan_timing view;

    make_opts(&opts);
    publish(state, DSD_SCAN_STAY_ACTIVITY_HOLD, 1U, 100.8, 2000U, 3000U, 2000U);
    assert(dsd_app_scan_timing_view(&opts, state, 100.0, &view) == 1);
    assert_phrase(&view, "Activity hold");
    assert(view.timer_live == 1U);
    assert(view.remaining_ms == 800U);
    assert(view.show_dwell == 1U);
    assert(view.dwell_state == DSD_APP_SCAN_DWELL_SUSPENDED);
    assert(view.show_hold == 0U);

    state->scan_voice_gate_phase = DSD_SCAN_VOICE_GATE_TAIL;
    assert(dsd_app_scan_timing_view(&opts, state, 100.0, &view) == 1);
    assert_phrase(&view, "Voice tail");

    /* QUALIFY and VOICE are not a tail: only TAIL renames this row. */
    state->scan_voice_gate_phase = DSD_SCAN_VOICE_GATE_QUALIFY;
    assert(dsd_app_scan_timing_view(&opts, state, 100.0, &view) == 1);
    assert_phrase(&view, "Activity hold");

    free(state);
}

/* An operator hold: the dwell is paused rather than suspended, because nothing on the
   air is holding the row and nothing will release it but the operator. */
static void
test_manual_hold_row_pauses_the_dwell(void) {
    static dsd_opts opts;
    dsd_state* state = make_state();
    dsd_app_scan_timing view;

    make_opts(&opts);
    publish(state, DSD_SCAN_STAY_MANUAL_HOLD, 1U, -1.0, 0U, 3000U, 1200U);
    assert(dsd_app_scan_timing_view(&opts, state, 100.0, &view) == 1);
    assert_phrase(&view, "Manual hold");
    assert(view.timer_live == 0U);
    assert(view.show_dwell == 1U);
    assert(view.dwell_state == DSD_APP_SCAN_DWELL_PAUSED);
    assert(view.show_hold == 1U);
    assert(view.hold_ms == 1200U);

    free(state);
}

/* The idle dwell is the live window, so the row shows it as a countdown and not also
   as a budget; the -Y qualify window is the same window under another name. */
static void
test_idle_dwell_row_and_qualify_variant(void) {
    static dsd_opts opts;
    dsd_state* state = make_state();
    dsd_app_scan_timing view;

    make_opts(&opts);
    publish(state, DSD_SCAN_STAY_IDLE_DWELL, 1U, 101.8, 3000U, 3000U, 2000U);
    assert(dsd_app_scan_timing_view(&opts, state, 100.0, &view) == 1);
    assert_phrase(&view, "Idle dwell");
    assert(view.timer_live == 1U);
    assert(view.remaining_ms == 1800U);
    assert(view.span_ms == 3000U);
    assert(view.show_dwell == 0U);
    assert(view.dwell_ms == 3000U);
    assert(view.dwell_state == DSD_APP_SCAN_DWELL_NONE);
    assert(view.show_hold == 1U);
    assert(view.hold_ms == 2000U);

    state->scan_voice_gate_phase = DSD_SCAN_VOICE_GATE_QUALIFY;
    assert(dsd_app_scan_timing_view(&opts, state, 100.0, &view) == 1);
    assert_phrase(&view, "Qualify");

    /* Any other phase is the plain idle dwell again. */
    state->scan_voice_gate_phase = DSD_SCAN_VOICE_GATE_TAIL;
    assert(dsd_app_scan_timing_view(&opts, state, 100.0, &view) == 1);
    assert_phrase(&view, "Idle dwell");

    free(state);
}

/* The -Y legacy rule: the whole stay is -t since the last sync, so there is no dwell
   or hold to state beside it. */
static void
test_hangtime_row(void) {
    static dsd_opts opts;
    dsd_state* state = make_state();
    dsd_app_scan_timing view;

    DSD_MEMSET(&opts, 0, sizeof(opts));
    opts.scanner_mode = 1;
    opts.trunk_hangtime = 2.0f;
    publish(state, DSD_SCAN_STAY_HANGTIME, 1U, 101.4, 2000U, 0U, 0U);
    assert(dsd_app_scan_timing_view(&opts, state, 100.0, &view) == 1);
    assert_phrase(&view, "Hangtime");
    assert(view.timer_live == 1U);
    assert(view.remaining_ms == 1400U);
    assert(view.span_ms == 2000U);
    assert(view.show_dwell == 0U);
    assert(view.show_hold == 0U);
    assert(view.show_hang == 0U);

    /* No anchor yet (never synced): the reason still holds, the countdown does not. */
    publish(state, DSD_SCAN_STAY_HANGTIME, 1U, -1.0, 0U, 0U, 0U);
    assert(dsd_app_scan_timing_view(&opts, state, 100.0, &view) == 1);
    assert(view.timer_live == 0U);
    assert(view.remaining_ms == 0U);
    assert(view.span_ms == 0U);

    free(state);
}

/* A deadline the poll arrived after reads as zero, never as a wrapped unsigned age:
   the decoder decides when to move, and the UI must not imply it already has not. */
static void
test_remaining_clamps_at_zero(void) {
    static dsd_opts opts;
    dsd_state* state = make_state();
    dsd_app_scan_timing view;

    make_opts(&opts);
    publish(state, DSD_SCAN_STAY_IDLE_DWELL, 1U, 97.5, 3000U, 3000U, 2000U);
    assert(dsd_app_scan_timing_view(&opts, state, 100.0, &view) == 1);
    assert(view.timer_live == 1U);
    assert(view.remaining_ms == 0U);
    assert(view.span_ms == 3000U);

    /* Exactly on the deadline is zero too, not a rounded-up millisecond. */
    publish(state, DSD_SCAN_STAY_IDLE_DWELL, 1U, 100.0, 3000U, 3000U, 2000U);
    assert(dsd_app_scan_timing_view(&opts, state, 100.0, &view) == 1);
    assert(view.timer_live == 1U);
    assert(view.remaining_ms == 0U);

    free(state);
}

/* A trunked row has no conventional activity hold to state, and the coordinator
   publishes none; neither half of that rule may be the only one holding the line. */
static void
test_hold_is_conventional_only(void) {
    static dsd_opts opts;
    dsd_state* state = make_state();
    dsd_app_scan_timing view;

    make_opts(&opts);
    publish(state, DSD_SCAN_STAY_IDLE_DWELL, 0U, 101.8, 3000U, 3000U, 2000U);
    assert(dsd_app_scan_timing_view(&opts, state, 100.0, &view) == 1);
    assert(view.show_hold == 0U);

    /* Conventional with nothing configured stays quiet as well. */
    publish(state, DSD_SCAN_STAY_IDLE_DWELL, 1U, 101.8, 3000U, 3000U, 0U);
    assert(dsd_app_scan_timing_view(&opts, state, 100.0, &view) == 1);
    assert(view.show_hold == 0U);

    free(state);
}

/* A dwell nobody configured is not a budget worth a column. */
static void
test_dwell_needs_a_value(void) {
    static dsd_opts opts;
    dsd_state* state = make_state();
    dsd_app_scan_timing view;

    make_opts(&opts);
    publish(state, DSD_SCAN_STAY_MANUAL_HOLD, 1U, -1.0, 0U, 0U, 1200U);
    assert(dsd_app_scan_timing_view(&opts, state, 100.0, &view) == 1);
    assert(view.show_dwell == 0U);
    assert(view.dwell_state == DSD_APP_SCAN_DWELL_NONE);
    assert(view.show_hold == 1U);

    free(state);
}

/* Every reason answers with a phrase; a row with none would render a bare label. */
static void
test_every_reason_has_a_phrase(void) {
    static dsd_opts opts;
    dsd_state* state = make_state();
    dsd_app_scan_timing view;

    make_opts(&opts);
    for (uint8_t reason = (uint8_t)DSD_SCAN_STAY_RETUNE_PENDING; reason <= (uint8_t)DSD_SCAN_STAY_HANGTIME; reason++) {
        publish(state, reason, 1U, -1.0, 0U, 3000U, 1200U);
        assert(dsd_app_scan_timing_view(&opts, state, 100.0, &view) == 1);
        assert(view.reason == reason);
        assert(view.phrase != NULL);
        assert(view.phrase[0] != '\0');
    }

    /* A reason from a newer decoder than this build knows is not rendered at all. */
    publish(state, (uint8_t)(DSD_SCAN_STAY_HANGTIME + 1), 1U, -1.0, 0U, 3000U, 1200U);
    assert(dsd_app_scan_timing_view(&opts, state, 100.0, &view) == 0);
    assert(view.active == 0U);

    free(state);
}

/* No scanner is running: a publication left over from an earlier -Y or --trunk-scan
   session must not put a countdown on a receiver that is not scanning. */
static void
test_inactive_without_a_scanner(void) {
    static dsd_opts opts;
    dsd_state* state = make_state();
    dsd_app_scan_timing view;

    DSD_MEMSET(&opts, 0, sizeof(opts));
    publish(state, DSD_SCAN_STAY_IDLE_DWELL, 1U, 101.8, 3000U, 3000U, 2000U);
    assert(dsd_app_scan_timing_view(&opts, state, 100.0, &view) == 0);
    assert(view.active == 0U);
    assert(view.phrase == NULL);
    assert(view.remaining_ms == 0U);
    assert(view.show_dwell == 0U);
    assert(view.show_hold == 0U);

    /* Either scanner is enough. */
    opts.scanner_mode = 1;
    assert(dsd_app_scan_timing_view(&opts, state, 100.0, &view) == 1);
    opts.scanner_mode = 0;
    opts.trunk_scan_enabled = 1;
    assert(dsd_app_scan_timing_view(&opts, state, 100.0, &view) == 1);

    free(state);
}

/* Nothing holds the row and nothing is published: the surface renders no row at all
   rather than a phrase for reason zero. */
static void
test_inactive_when_reason_is_none(void) {
    static dsd_opts opts;
    dsd_state* state = make_state();
    dsd_app_scan_timing view;

    make_opts(&opts);
    publish(state, DSD_SCAN_STAY_NONE, 1U, 101.8, 3000U, 3000U, 2000U);
    assert(dsd_app_scan_timing_view(&opts, state, 100.0, &view) == 0);
    assert(view.active == 0U);
    assert(view.phrase == NULL);

    free(state);
}

/* No cap configured: nothing to show, whatever else the row says. A stale visit
   deadline without a limit is not a cap either -- the engine only anchors one while a
   limit is in force, and a leftover stamp must not put a countdown on the row. */
static void
test_visit_cap_off_is_not_shown(void) {
    static dsd_opts opts;
    dsd_state* state = make_state();
    dsd_app_scan_timing view;

    make_opts(&opts);
    publish(state, DSD_SCAN_STAY_IDLE_DWELL, 1U, 101.8, 3000U, 3000U, 2000U);
    assert(dsd_app_scan_timing_view(&opts, state, 100.0, &view) == 1);
    assert(view.show_visit == 0U);
    assert(view.visit_ms == 0U);
    assert(view.visit_live == 0U);
    assert(view.visit_remaining_ms == 0U);

    seed_visit(state, 0U, 118.0);
    assert(dsd_app_scan_timing_view(&opts, state, 100.0, &view) == 1);
    assert(view.show_visit == 0U);
    assert(view.visit_live == 0U);
    assert(view.visit_remaining_ms == 0U);

    free(state);
}

/* A cap anchored at parking counts down against the same clock the windows use, and
   floors at zero once the poll lands after it: the decoder decides when the receiver
   moves on, so the row must not imply it already should have. */
static void
test_visit_cap_counts_down(void) {
    static dsd_opts opts;
    dsd_state* state = make_state();
    dsd_app_scan_timing view;

    make_opts(&opts);
    publish(state, DSD_SCAN_STAY_IDLE_DWELL, 1U, 101.8, 3000U, 3000U, 2000U);
    seed_visit(state, 20000U, 112.3);
    assert(dsd_app_scan_timing_view(&opts, state, 100.0, &view) == 1);
    assert(view.show_visit == 1U);
    assert(view.visit_ms == 20000U);
    assert(view.visit_live == 1U);
    assert(view.visit_remaining_ms == 12300U);

    seed_visit(state, 20000U, 97.5);
    assert(dsd_app_scan_timing_view(&opts, state, 100.0, &view) == 1);
    assert(view.show_visit == 1U);
    assert(view.visit_live == 1U);
    assert(view.visit_remaining_ms == 0U);

    free(state);
}

/* Suspended or not yet anchored: the cap still applies to the row, so its width is worth
   stating, but there is no countdown to report and zero would read as expiry. */
static void
test_visit_cap_suspended_is_not_live(void) {
    static dsd_opts opts;
    dsd_state* state = make_state();
    dsd_app_scan_timing view;

    make_opts(&opts);
    publish(state, DSD_SCAN_STAY_MANUAL_HOLD, 1U, -1.0, 0U, 3000U, 1200U);
    seed_visit(state, 20000U, -1.0);
    assert(dsd_app_scan_timing_view(&opts, state, 100.0, &view) == 1);
    assert(view.show_visit == 1U);
    assert(view.visit_ms == 20000U);
    assert(view.visit_live == 0U);
    assert(view.visit_remaining_ms == 0U);

    free(state);
}

/* The cap is not driven by the stay reason the way the dwell, hold and hang budgets are:
   it governs the whole visit, so every reason reports it. */
static void
test_visit_cap_applies_to_every_reason(void) {
    static dsd_opts opts;
    dsd_state* state = make_state();
    dsd_app_scan_timing view;

    make_opts(&opts);
    opts.trunk_hangtime = 2.0f;
    for (uint8_t reason = (uint8_t)DSD_SCAN_STAY_RETUNE_PENDING; reason <= (uint8_t)DSD_SCAN_STAY_HANGTIME; reason++) {
        publish(state, reason, 1U, -1.0, 0U, 3000U, 1200U);
        seed_visit(state, 45000U, 130.0);
        assert(dsd_app_scan_timing_view(&opts, state, 100.0, &view) == 1);
        assert(view.show_visit == 1U);
        assert(view.visit_ms == 45000U);
        assert(view.visit_live == 1U);
        assert(view.visit_remaining_ms == 30000U);
    }

    /* Nothing is rotating: a leftover cap says nothing about a receiver parked by hand. */
    DSD_MEMSET(&opts, 0, sizeof(opts));
    publish(state, DSD_SCAN_STAY_IDLE_DWELL, 1U, 101.8, 3000U, 3000U, 2000U);
    seed_visit(state, 45000U, 130.0);
    assert(dsd_app_scan_timing_view(&opts, state, 100.0, &view) == 0);
    assert(view.show_visit == 0U);
    assert(view.visit_ms == 0U);
    assert(view.visit_remaining_ms == 0U);

    free(state);
}

static void
test_null_arguments_are_safe(void) {
    static dsd_opts opts;
    dsd_state* state = make_state();
    dsd_app_scan_timing view;

    make_opts(&opts);
    publish(state, DSD_SCAN_STAY_IDLE_DWELL, 1U, 101.8, 3000U, 3000U, 2000U);

    /* A rejected call still leaves the caller's view zeroed, so a frontend that
       ignores the status cannot render a stale row. */
    DSD_MEMSET(&view, 0xFF, sizeof(view));
    assert(dsd_app_scan_timing_view(NULL, state, 100.0, &view) == -1);
    assert(view.active == 0U);
    assert(view.phrase == NULL);

    DSD_MEMSET(&view, 0xFF, sizeof(view));
    assert(dsd_app_scan_timing_view(&opts, NULL, 100.0, &view) == -1);
    assert(view.active == 0U);

    assert(dsd_app_scan_timing_view(&opts, state, 100.0, NULL) == -1);

    free(state);
}

int
main(void) {
    test_retune_pending_row();
    test_retune_retry_row();
    test_cc_acquire_row();
    test_call_follow_row_shows_hangtime();
    test_hangtime_is_only_shown_while_following_a_call();
    test_voice_row();
    test_activity_hold_row_names_the_tail();
    test_manual_hold_row_pauses_the_dwell();
    test_idle_dwell_row_and_qualify_variant();
    test_hangtime_row();
    test_remaining_clamps_at_zero();
    test_hold_is_conventional_only();
    test_dwell_needs_a_value();
    test_every_reason_has_a_phrase();
    test_visit_cap_off_is_not_shown();
    test_visit_cap_counts_down();
    test_visit_cap_suspended_is_not_live();
    test_visit_cap_applies_to_every_reason();
    test_inactive_without_a_scanner();
    test_inactive_when_reason_is_none();
    test_null_arguments_are_safe();
    printf("APP_CONTROL_SCAN_TIMING_VIEW ok\n");
    return 0;
}
