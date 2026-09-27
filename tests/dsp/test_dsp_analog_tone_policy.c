// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The CTCSS/DCS receive policy (issue #527) in sample time, with the detectors' verdicts injected as the tap would
 * publish them: disabled, allow and block, each against a matching value, a nonmatching one, no tone, acquisition,
 * loss and reacquisition, a late blocked value, and a carrier drop or retune. The window is bounded: nothing is ever
 * rejected before a value is confirmed or the window ends, and the window's end is pinned to the read that crosses it
 * at several rates and read sizes.
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <dsd-neo/core/analog_tone.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/dsp/analog_rx.h>
#include <dsd-neo/runtime/analog_tones.h>
#include "analog_tone_policy.h"

enum { RATE_HZ = 48000, READ_SAMPLES = 960 /* 20 ms, the tap's read */ };

static dsd_analog_rx_publication
rx_idle(void) {
    dsd_analog_rx_publication rx;
    DSD_MEMSET(&rx, 0, sizeof(rx));
    rx.tone_state = DSD_ANALOG_TONE_STATE_IDLE;
    return rx;
}

/* A carrier the detectors are still evaluating (ACQUIRING) or have found no tone on (NONE). */
static dsd_analog_rx_publication
rx_carrier(int tone_state) {
    dsd_analog_rx_publication rx = rx_idle();
    rx.carrier_open = 1;
    rx.tone_state = tone_state;
    return rx;
}

static dsd_analog_rx_publication
rx_ctcss(int tenths_hz) {
    dsd_analog_rx_publication rx = rx_carrier(DSD_ANALOG_TONE_STATE_LOCKED);
    rx.tone_kind = DSD_ANALOG_TONE_KIND_CTCSS;
    rx.ctcss_tenths_hz = tenths_hz;
    return rx;
}

/* A locked code, named as the detector names it: the canonical member of its signal. */
static dsd_analog_rx_publication
rx_dcs(int code, int inverted) {
    dsd_analog_rx_publication rx = rx_carrier(DSD_ANALOG_TONE_STATE_LOCKED);
    int canon_code = -1;
    int canon_inverted = -1;
    assert(dsd_dcs_canonical(code, inverted, &canon_code, &canon_inverted) == 0);
    rx.tone_kind = DSD_ANALOG_TONE_KIND_DCS;
    rx.dcs_code = canon_code;
    rx.dcs_inverted = canon_inverted;
    return rx;
}

static dsd_tone_set
tone_list(const char* text) {
    dsd_tone_set set;
    assert(dsd_tone_set_parse(text, &set, NULL, 0) == 0);
    return set;
}

static void
policy_with(dsd_analog_tone_policy* policy, int mode, const char* list) {
    dsd_analog_tone_policy_init(policy);
    const dsd_tone_set set = tone_list(list);
    assert(dsd_analog_tone_policy_configure(policy, mode, &set) == 1);
}

/* Feed @p ms of 20 ms reads, all publishing @p rx; returns the verdict after the last one. */
static int
feed_ms(dsd_analog_tone_policy* policy, dsd_analog_rx_publication rx, int ms) {
    int gate = policy->gate;
    for (int t = 0; t < ms; t += 20) {
        gate = dsd_analog_tone_policy_step(policy, &rx, READ_SAMPLES, RATE_HZ);
    }
    return gate;
}

/* Feed reads of @p rx until @p ms of window, asserting the verdict stays @p gate at every read. */
static void
hold_ms(dsd_analog_tone_policy* policy, dsd_analog_rx_publication rx, int ms, int gate) {
    for (int t = 0; t < ms; t += 20) {
        assert(dsd_analog_tone_policy_step(policy, &rx, READ_SAMPLES, RATE_HZ) == gate);
    }
}

static void
test_window_constants(void) {
    /* The maintainer's window: 800 ms, extended to at most 1,600 ms for a DCS candidate, each at least one detector's
       lock ceiling plus 100 ms (two CTCSS hops). */
    assert(DSD_ANALOG_TONE_WINDOW_MS == 800);
    assert(DSD_ANALOG_TONE_WINDOW_DCS_MS == 1600);
    assert(DSD_ANALOG_TONE_WINDOW_MS >= DSD_ANALOG_CTCSS_LOCK_CEILING_MS + 100);
    assert(DSD_ANALOG_TONE_WINDOW_DCS_MS >= DSD_ANALOG_DCS_LOCK_CEILING_MS + 100);
}

static void
test_disabled(void) {
    dsd_analog_tone_policy policy;
    dsd_analog_tone_policy_init(&policy);
    assert(policy.gate == DSD_ANALOG_TONE_GATE_OFF);
    /* Off decides nothing, whatever arrives, however long. */
    assert(feed_ms(&policy, rx_carrier(DSD_ANALOG_TONE_STATE_ACQUIRING), 2000) == DSD_ANALOG_TONE_GATE_OFF);
    assert(feed_ms(&policy, rx_ctcss(1000), 100) == DSD_ANALOG_TONE_GATE_OFF);
    assert(feed_ms(&policy, rx_idle(), 100) == DSD_ANALOG_TONE_GATE_OFF);
    /* So is a list policy without a list, and a mode that is none. */
    const dsd_tone_set empty = {0};
    assert(dsd_analog_tone_policy_configure(&policy, DSD_TONE_FILTER_ALLOW, &empty) == 0);
    assert(feed_ms(&policy, rx_ctcss(670), 1000) == DSD_ANALOG_TONE_GATE_OFF);
    assert(dsd_analog_tone_policy_configure(&policy, DSD_TONE_FILTER_BLOCK, NULL) == 0);
    const dsd_tone_set set = tone_list("100.0");
    assert(dsd_analog_tone_policy_configure(&policy, 7, &set) == 0);
    assert(feed_ms(&policy, rx_ctcss(670), 1000) == DSD_ANALOG_TONE_GATE_OFF);
    /* Off keeps the list: switching allow/block keeps it too. */
    assert(dsd_analog_tone_policy_configure(&policy, DSD_TONE_FILTER_OFF, &set) == 0);
    assert(policy.gate == DSD_ANALOG_TONE_GATE_OFF);
}

/* Allow list: a listed value passes the moment it is confirmed, however early. */
static void
test_allow_match(void) {
    dsd_analog_tone_policy policy;
    policy_with(&policy, DSD_TONE_FILTER_ALLOW, "100.0/D023N");
    /* Armed before any carrier: nothing plays. */
    assert(feed_ms(&policy, rx_idle(), 200) == DSD_ANALOG_TONE_GATE_PENDING);
    hold_ms(&policy, rx_carrier(DSD_ANALOG_TONE_STATE_ACQUIRING), 360, DSD_ANALOG_TONE_GATE_PENDING);
    assert(feed_ms(&policy, rx_ctcss(1000), 20) == DSD_ANALOG_TONE_GATE_ALLOWED);
    assert(policy.no_tone == 0);
    hold_ms(&policy, rx_ctcss(1000), 3000, DSD_ANALOG_TONE_GATE_ALLOWED);
    /* A code on the list passes the same way. */
    dsd_analog_tone_policy_reset(&policy);
    assert(feed_ms(&policy, rx_dcs(0023, 0), 20) == DSD_ANALOG_TONE_GATE_ALLOWED);
}

/* Allow list: a confirmed value it does not list is rejected at once, not first at the window's end. */
static void
test_allow_nonmatch(void) {
    dsd_analog_tone_policy policy;
    policy_with(&policy, DSD_TONE_FILTER_ALLOW, "100.0");
    hold_ms(&policy, rx_carrier(DSD_ANALOG_TONE_STATE_ACQUIRING), 300, DSD_ANALOG_TONE_GATE_PENDING);
    assert(feed_ms(&policy, rx_ctcss(1035), 20) == DSD_ANALOG_TONE_GATE_REJECTED);
    assert(policy.no_tone == 0 && policy.value_kind == DSD_ANALOG_TONE_KIND_CTCSS && policy.value_ctcss == 1035);
    /* And stays rejected while it lasts, and after it is lost. */
    hold_ms(&policy, rx_ctcss(1035), 1000, DSD_ANALOG_TONE_GATE_REJECTED);
    hold_ms(&policy, rx_carrier(DSD_ANALOG_TONE_STATE_NONE), 2000, DSD_ANALOG_TONE_GATE_REJECTED);
    /* A code not on the list too. */
    dsd_analog_tone_policy_reset(&policy);
    assert(feed_ms(&policy, rx_dcs(0754, 0), 20) == DSD_ANALOG_TONE_GATE_REJECTED);
}

/* Allow list, no tone: pending for the whole window, never rejected before it ends, rejected as it ends. */
static void
test_allow_no_tone(void) {
    for (int state = DSD_ANALOG_TONE_STATE_ACQUIRING; state <= DSD_ANALOG_TONE_STATE_NONE; state++) {
        if (state == DSD_ANALOG_TONE_STATE_LOCKED) {
            continue;
        }
        dsd_analog_tone_policy policy;
        policy_with(&policy, DSD_TONE_FILTER_ALLOW, "100.0");
        /* The read that opens the window is its start; 40 reads later it has run 800 ms. */
        hold_ms(&policy, rx_carrier(state), 800, DSD_ANALOG_TONE_GATE_PENDING);
        assert(feed_ms(&policy, rx_carrier(state), 20) == DSD_ANALOG_TONE_GATE_REJECTED);
        assert(policy.no_tone == 1 && policy.value_kind == DSD_ANALOG_TONE_KIND_NONE);
        /* A carrier whose rate the front end cannot use hears no tone either. */
    }
    dsd_analog_tone_policy policy;
    policy_with(&policy, DSD_TONE_FILTER_ALLOW, "D023N");
    hold_ms(&policy, rx_carrier(DSD_ANALOG_TONE_STATE_UNAVAILABLE), 800, DSD_ANALOG_TONE_GATE_PENDING);
    assert(feed_ms(&policy, rx_carrier(DSD_ANALOG_TONE_STATE_UNAVAILABLE), 20) == DSD_ANALOG_TONE_GATE_REJECTED);
}

/* Block list: a listed value is rejected at once, anything else and no tone pass, no tone only once the window ends. */
static void
test_block(void) {
    dsd_analog_tone_policy policy;
    policy_with(&policy, DSD_TONE_FILTER_BLOCK, "67.0/D023N");
    hold_ms(&policy, rx_carrier(DSD_ANALOG_TONE_STATE_ACQUIRING), 200, DSD_ANALOG_TONE_GATE_PENDING);
    assert(feed_ms(&policy, rx_ctcss(670), 20) == DSD_ANALOG_TONE_GATE_REJECTED);
    assert(policy.no_tone == 0);

    dsd_analog_tone_policy_reset(&policy);
    assert(feed_ms(&policy, rx_ctcss(1000), 20) == DSD_ANALOG_TONE_GATE_ALLOWED);
    assert(policy.no_tone == 0);

    dsd_analog_tone_policy_reset(&policy);
    hold_ms(&policy, rx_carrier(DSD_ANALOG_TONE_STATE_NONE), 800, DSD_ANALOG_TONE_GATE_PENDING);
    assert(feed_ms(&policy, rx_carrier(DSD_ANALOG_TONE_STATE_NONE), 20) == DSD_ANALOG_TONE_GATE_ALLOWED);
    assert(policy.no_tone == 1);
}

/* Allow list: an allowed tone lost (the detector's own hysteresis already spent) starts a fresh window; the tone coming
   back within it passes again, and a window that ends without it rejects. */
static void
test_allow_loss_and_reacquire(void) {
    dsd_analog_tone_policy policy;
    policy_with(&policy, DSD_TONE_FILTER_ALLOW, "100.0");
    assert(feed_ms(&policy, rx_ctcss(1000), 200) == DSD_ANALOG_TONE_GATE_ALLOWED);
    /* Lost: pending again, with the whole window ahead even though the reception is already older than one. */
    assert(feed_ms(&policy, rx_ctcss(1000), 1000) == DSD_ANALOG_TONE_GATE_ALLOWED);
    assert(feed_ms(&policy, rx_carrier(DSD_ANALOG_TONE_STATE_NONE), 20) == DSD_ANALOG_TONE_GATE_PENDING);
    hold_ms(&policy, rx_carrier(DSD_ANALOG_TONE_STATE_NONE), 400, DSD_ANALOG_TONE_GATE_PENDING);
    assert(feed_ms(&policy, rx_ctcss(1000), 20) == DSD_ANALOG_TONE_GATE_ALLOWED);
    /* Lost again, not back: rejected as the fresh window ends, for want of a tone. */
    assert(feed_ms(&policy, rx_carrier(DSD_ANALOG_TONE_STATE_ACQUIRING), 20) == DSD_ANALOG_TONE_GATE_PENDING);
    hold_ms(&policy, rx_carrier(DSD_ANALOG_TONE_STATE_ACQUIRING), 780, DSD_ANALOG_TONE_GATE_PENDING);
    assert(feed_ms(&policy, rx_carrier(DSD_ANALOG_TONE_STATE_ACQUIRING), 20) == DSD_ANALOG_TONE_GATE_REJECTED);
    assert(policy.no_tone == 1);
    /* A listed tone confirmed after the rejection still passes. */
    assert(feed_ms(&policy, rx_ctcss(1000), 20) == DSD_ANALOG_TONE_GATE_ALLOWED);
    assert(policy.no_tone == 0);
    /* Allowed, then another tone: rejected. */
    assert(feed_ms(&policy, rx_ctcss(1072), 20) == DSD_ANALOG_TONE_GATE_REJECTED);
}

/* Block list: a loss keeps the pass, and a blocked value confirmed later -- after a pass on another value or on no
   tone -- closes the gate. */
static void
test_block_late_blocked_value(void) {
    dsd_analog_tone_policy policy;
    policy_with(&policy, DSD_TONE_FILTER_BLOCK, "D023N");
    assert(feed_ms(&policy, rx_ctcss(1000), 100) == DSD_ANALOG_TONE_GATE_ALLOWED);
    hold_ms(&policy, rx_carrier(DSD_ANALOG_TONE_STATE_NONE), 3000, DSD_ANALOG_TONE_GATE_ALLOWED);
    assert(feed_ms(&policy, rx_dcs(0023, 0), 20) == DSD_ANALOG_TONE_GATE_REJECTED);
    hold_ms(&policy, rx_carrier(DSD_ANALOG_TONE_STATE_NONE), 1000, DSD_ANALOG_TONE_GATE_REJECTED);
    /* A value the list passes, confirmed after the rejection, opens it again. */
    assert(feed_ms(&policy, rx_ctcss(1000), 20) == DSD_ANALOG_TONE_GATE_ALLOWED);

    dsd_analog_tone_policy_reset(&policy);
    assert(feed_ms(&policy, rx_carrier(DSD_ANALOG_TONE_STATE_NONE), 840) == DSD_ANALOG_TONE_GATE_ALLOWED);
    assert(policy.no_tone == 1);
    /* The listed code's other spelling, D047I, is the same signal. */
    assert(feed_ms(&policy, rx_dcs(0047, 1), 20) == DSD_ANALOG_TONE_GATE_REJECTED);
    assert(policy.no_tone == 0);
}

/* A reception ends with the carrier hangover or a reset (a retune, a row change): what it decided goes with it, and the
   next carrier opens a new window. */
static void
test_carrier_drop_and_retune(void) {
    dsd_analog_tone_policy policy;
    policy_with(&policy, DSD_TONE_FILTER_ALLOW, "100.0");
    assert(feed_ms(&policy, rx_ctcss(670), 100) == DSD_ANALOG_TONE_GATE_REJECTED);
    /* The hangover ran out: the tap reset, and publishes no carrier. */
    dsd_analog_tone_policy_reset(&policy);
    assert(feed_ms(&policy, rx_idle(), 500) == DSD_ANALOG_TONE_GATE_PENDING);
    assert(policy.window_open == 0 && policy.no_tone == 0);
    /* A new transmission carries the listed tone. */
    hold_ms(&policy, rx_carrier(DSD_ANALOG_TONE_STATE_ACQUIRING), 400, DSD_ANALOG_TONE_GATE_PENDING);
    assert(feed_ms(&policy, rx_ctcss(1000), 20) == DSD_ANALOG_TONE_GATE_ALLOWED);
    /* A publication with no carrier ends the reception too, whatever the caller says. */
    assert(feed_ms(&policy, rx_idle(), 20) == DSD_ANALOG_TONE_GATE_PENDING);
    hold_ms(&policy, rx_carrier(DSD_ANALOG_TONE_STATE_ACQUIRING), 800, DSD_ANALOG_TONE_GATE_PENDING);
    assert(feed_ms(&policy, rx_carrier(DSD_ANALOG_TONE_STATE_ACQUIRING), 20) == DSD_ANALOG_TONE_GATE_REJECTED);
}

/* The window's end falls on the read that crosses it, in sample time, at any rate and read size: the tap reads 20 ms of
   input at the input's rate, which at 44.1 kHz is 882 samples, and at 2500 Hz 50. */
static void
test_window_in_sample_time(void) {
    static const struct {
        int rate_hz;
        unsigned int read;
    } k_reads[] = {{48000, 960}, {44100, 882}, {8000, 160}, {2500, 50}, {78125, 1563}, {48000, 7}};

    for (size_t k = 0; k < sizeof(k_reads) / sizeof(k_reads[0]); k++) {
        dsd_analog_tone_policy policy;
        policy_with(&policy, DSD_TONE_FILTER_ALLOW, "100.0");
        const dsd_analog_rx_publication rx = rx_carrier(DSD_ANALOG_TONE_STATE_NONE);
        uint64_t samples_after_open = 0;
        int gate = dsd_analog_tone_policy_step(&policy, &rx, k_reads[k].read, k_reads[k].rate_hz);
        while (gate == DSD_ANALOG_TONE_GATE_PENDING) {
            gate = dsd_analog_tone_policy_step(&policy, &rx, k_reads[k].read, k_reads[k].rate_hz);
            samples_after_open += k_reads[k].read;
            assert(samples_after_open < (uint64_t)k_reads[k].rate_hz * 2U);
        }
        assert(gate == DSD_ANALOG_TONE_GATE_REJECTED);
        /* Rejected on the first read at or past 800 ms after the read that opened the window, never before it. */
        const uint64_t window = ((uint64_t)k_reads[k].rate_hz * DSD_ANALOG_TONE_WINDOW_MS) / 1000U;
        assert(samples_after_open >= window);
        assert(samples_after_open < window + k_reads[k].read + 1U);
    }
}

/* The window runs on for a DCS candidate, to at most 1,600 ms, only when the list holds a DCS code; a CTCSS-only list
   and a reception with no candidate keep 800 ms, and a code confirmed in the extension is judged as ever. */
static void
test_dcs_extension(void) {
    dsd_analog_rx_publication acquiring = rx_carrier(DSD_ANALOG_TONE_STATE_NONE);
    acquiring.dcs_acquiring = 1;

    dsd_analog_tone_policy policy;
    policy_with(&policy, DSD_TONE_FILTER_ALLOW, "100.0/D023N");
    hold_ms(&policy, acquiring, 1600, DSD_ANALOG_TONE_GATE_PENDING);
    assert(feed_ms(&policy, acquiring, 20) == DSD_ANALOG_TONE_GATE_REJECTED);
    assert(policy.no_tone == 1);

    /* The candidate goes: decided on that read. */
    policy_with(&policy, DSD_TONE_FILTER_ALLOW, "D023N");
    hold_ms(&policy, acquiring, 1000, DSD_ANALOG_TONE_GATE_PENDING);
    assert(feed_ms(&policy, rx_carrier(DSD_ANALOG_TONE_STATE_NONE), 20) == DSD_ANALOG_TONE_GATE_REJECTED);

    /* The code locks late, inside the extension: allowed, never rejected first. */
    policy_with(&policy, DSD_TONE_FILTER_ALLOW, "D023N");
    hold_ms(&policy, acquiring, 1400, DSD_ANALOG_TONE_GATE_PENDING);
    assert(feed_ms(&policy, rx_dcs(0023, 0), 20) == DSD_ANALOG_TONE_GATE_ALLOWED);

    /* A CTCSS-only list keeps 800 ms, candidate or not. */
    policy_with(&policy, DSD_TONE_FILTER_ALLOW, "100.0");
    hold_ms(&policy, acquiring, 800, DSD_ANALOG_TONE_GATE_PENDING);
    assert(feed_ms(&policy, acquiring, 20) == DSD_ANALOG_TONE_GATE_REJECTED);

    /* A block list with a code waits as well before passing no-tone traffic. */
    policy_with(&policy, DSD_TONE_FILTER_BLOCK, "D023N");
    hold_ms(&policy, acquiring, 1600, DSD_ANALOG_TONE_GATE_PENDING);
    assert(feed_ms(&policy, acquiring, 20) == DSD_ANALOG_TONE_GATE_ALLOWED);

    /* The extension applies to the fresh window after a loss too. */
    policy_with(&policy, DSD_TONE_FILTER_ALLOW, "D023N");
    assert(feed_ms(&policy, rx_dcs(0023, 0), 200) == DSD_ANALOG_TONE_GATE_ALLOWED);
    assert(feed_ms(&policy, acquiring, 20) == DSD_ANALOG_TONE_GATE_PENDING);
    hold_ms(&policy, acquiring, 1580, DSD_ANALOG_TONE_GATE_PENDING);
    assert(feed_ms(&policy, acquiring, 20) == DSD_ANALOG_TONE_GATE_REJECTED);
}

/* Matching goes by the DCS signal: a listed D023I passes a received D047N, the name the detector gives that signal. */
static void
test_dcs_match_by_signal(void) {
    dsd_analog_tone_policy policy;
    policy_with(&policy, DSD_TONE_FILTER_ALLOW, "D023I");
    const dsd_analog_rx_publication rx = rx_dcs(0047, 0);
    assert(rx.dcs_code == 0047 && rx.dcs_inverted == 0);
    assert(feed_ms(&policy, rx, 20) == DSD_ANALOG_TONE_GATE_ALLOWED);
    dsd_analog_tone_policy_reset(&policy);
    assert(feed_ms(&policy, rx_dcs(0023, 0), 20) == DSD_ANALOG_TONE_GATE_REJECTED);
}

/* A different policy starts the reception over; the same one changes nothing. */
static void
test_reconfigure(void) {
    dsd_analog_tone_policy policy;
    policy_with(&policy, DSD_TONE_FILTER_ALLOW, "100.0");
    assert(feed_ms(&policy, rx_ctcss(1000), 1000) == DSD_ANALOG_TONE_GATE_ALLOWED);
    dsd_tone_set same = tone_list("100");
    assert(dsd_analog_tone_policy_configure(&policy, DSD_TONE_FILTER_ALLOW, &same) == 0);
    assert(policy.gate == DSD_ANALOG_TONE_GATE_ALLOWED);
    /* Allow becomes block: the same tone is now blocked. */
    assert(dsd_analog_tone_policy_configure(&policy, DSD_TONE_FILTER_BLOCK, &same) == 1);
    assert(policy.gate == DSD_ANALOG_TONE_GATE_PENDING && policy.window_open == 0);
    assert(feed_ms(&policy, rx_ctcss(1000), 20) == DSD_ANALOG_TONE_GATE_REJECTED);
    /* Off: the ordinary squelch again, at once. */
    assert(dsd_analog_tone_policy_configure(&policy, DSD_TONE_FILTER_OFF, &same) == 1);
    assert(policy.gate == DSD_ANALOG_TONE_GATE_OFF);
    assert(feed_ms(&policy, rx_ctcss(1000), 20) == DSD_ANALOG_TONE_GATE_OFF);
}

/* What each verdict does: the monitor plays only OFF and ALLOWED; every verdict but REJECTED holds a scan row. */
static void
test_outputs(void) {
    assert(dsd_analog_tone_gate_audible(DSD_ANALOG_TONE_GATE_OFF));
    assert(!dsd_analog_tone_gate_audible(DSD_ANALOG_TONE_GATE_PENDING));
    assert(dsd_analog_tone_gate_audible(DSD_ANALOG_TONE_GATE_ALLOWED));
    assert(!dsd_analog_tone_gate_audible(DSD_ANALOG_TONE_GATE_REJECTED));
    assert(dsd_analog_tone_gate_holds(DSD_ANALOG_TONE_GATE_OFF));
    assert(dsd_analog_tone_gate_holds(DSD_ANALOG_TONE_GATE_PENDING));
    assert(dsd_analog_tone_gate_holds(DSD_ANALOG_TONE_GATE_ALLOWED));
    assert(!dsd_analog_tone_gate_holds(DSD_ANALOG_TONE_GATE_REJECTED));
    /* No rate or no samples moves no time. */
    dsd_analog_tone_policy policy;
    policy_with(&policy, DSD_TONE_FILTER_ALLOW, "100.0");
    const dsd_analog_rx_publication rx = rx_carrier(DSD_ANALOG_TONE_STATE_NONE);
    for (int i = 0; i < 100; i++) {
        assert(dsd_analog_tone_policy_step(&policy, &rx, 960U, 0) == DSD_ANALOG_TONE_GATE_PENDING);
        assert(dsd_analog_tone_policy_step(&policy, &rx, 0U, 48000) == DSD_ANALOG_TONE_GATE_PENDING);
    }
    assert(dsd_analog_tone_policy_step(NULL, &rx, 960U, 48000) == DSD_ANALOG_TONE_GATE_OFF);
    assert(dsd_analog_tone_policy_step(&policy, NULL, 960U, 48000) == DSD_ANALOG_TONE_GATE_PENDING);
}

int
main(void) {
    test_window_constants();
    test_disabled();
    test_allow_match();
    test_allow_nonmatch();
    test_allow_no_tone();
    test_block();
    test_allow_loss_and_reacquire();
    test_block_late_blocked_value();
    test_carrier_drop_and_retune();
    test_window_in_sample_time();
    test_dcs_extension();
    test_dcs_match_by_signal();
    test_reconfigure();
    test_outputs();
    printf("DSP_ANALOG_TONE_POLICY: ok\n");
    return 0;
}
