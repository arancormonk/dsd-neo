// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief CTCSS/DCS receive policy (issue #527): see analog_tone_policy.h for the state machine.
 */

#include "analog_tone_policy.h"

#include <dsd-neo/core/analog_tone.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/dsp/analog_rx.h>
#include <dsd-neo/runtime/analog_tones.h>
#include <stdint.h>

/* The tone-check window (#518): 800 ms, extended to at most 1,600 ms while the list holds a DCS code and the DCS
   detector holds a candidate code (dsd_analog_rx_publication::dcs_candidate). Each covers its detector's lock ceiling
   with two CTCSS hops to spare, so a value within the detectors' contracts is never taken for no tone. */
_Static_assert(DSD_ANALOG_TONE_WINDOW_MS == 800, "the tone-check window is 800 ms");
_Static_assert(DSD_ANALOG_TONE_WINDOW_MS >= DSD_ANALOG_CTCSS_LOCK_CEILING_MS + 100,
               "the window covers the CTCSS lock ceiling plus 100 ms");
_Static_assert(DSD_ANALOG_TONE_WINDOW_DCS_MS <= 1600, "the DCS extension ends by 1,600 ms");
_Static_assert(DSD_ANALOG_TONE_WINDOW_DCS_MS >= DSD_ANALOG_DCS_LOCK_CEILING_MS + 100,
               "the DCS extension covers the DCS lock ceiling plus 100 ms");
_Static_assert(DSD_ANALOG_TONE_WINDOW_DCS_MS >= DSD_ANALOG_TONE_WINDOW_MS, "the extension only lengthens the window");

static int
policy_mode_on(int mode) {
    return mode == DSD_TONE_FILTER_ALLOW || mode == DSD_TONE_FILTER_BLOCK;
}

/* No value decided the verdict. */
static void
policy_clear_value(dsd_analog_tone_policy* policy) {
    policy->no_tone = 0;
    policy->value_kind = DSD_ANALOG_TONE_KIND_NONE;
    policy->value_ctcss = 0;
    policy->value_dcs_code = 0;
    policy->value_dcs_inverted = 0;
}

static void
policy_close_window(dsd_analog_tone_policy* policy) {
    policy->window_open = 0;
    policy->window_us = 0;
    policy->window_rem = 0;
    policy->window_rate_hz = 0;
}

/* Undecided: PENDING with a list policy in force, OFF without one; the window starts again at the next carrier. */
static void
policy_undecided(dsd_analog_tone_policy* policy) {
    policy->gate = policy_mode_on(policy->mode) ? DSD_ANALOG_TONE_GATE_PENDING : DSD_ANALOG_TONE_GATE_OFF;
    policy_clear_value(policy);
    policy_close_window(policy);
}

void
dsd_analog_tone_policy_init(dsd_analog_tone_policy* policy) {
    if (!policy) {
        return;
    }
    DSD_MEMSET(policy, 0, sizeof(*policy));
    policy->mode = DSD_TONE_FILTER_OFF;
    policy_undecided(policy);
}

int
dsd_analog_tone_policy_configure(dsd_analog_tone_policy* policy, int mode, const dsd_tone_set* set) {
    if (!policy) {
        return 0;
    }
    /* A list policy without a list decides nothing: the configuration refuses one, and this keeps it that way. */
    if (!policy_mode_on(mode) || !set || dsd_tone_set_count(set) == 0) {
        mode = DSD_TONE_FILTER_OFF;
    }
    dsd_tone_set next;
    DSD_MEMSET(&next, 0, sizeof(next));
    if (mode != DSD_TONE_FILTER_OFF) {
        next = *set;
    }
    if (mode == policy->mode && dsd_tone_set_same_signals(&next, &policy->set)) {
        /* The same policy, its codes perhaps respelled (D023I for D047N): the verdict stands. */
        policy->set = next;
        return 0;
    }
    policy->mode = mode;
    policy->set = next;
    policy_undecided(policy);
    return 1;
}

void
dsd_analog_tone_policy_reset(dsd_analog_tone_policy* policy) {
    if (policy) {
        policy_undecided(policy);
    }
}

/* Advance the open window by @p samples at @p rate_hz, carrying the part of a microsecond the division leaves, so the
   window ends on the read that crosses it at any rate and read size. A new rate starts the carry over. */
static void
policy_advance(dsd_analog_tone_policy* policy, unsigned int samples, int rate_hz) {
    if (rate_hz <= 0 || samples == 0U) {
        return;
    }
    if (rate_hz != policy->window_rate_hz) {
        policy->window_rate_hz = rate_hz;
        policy->window_rem = 0;
    }
    const int64_t num = ((int64_t)samples * 1000000) + policy->window_rem;
    policy->window_us += num / rate_hz;
    policy->window_rem = num % rate_hz;
}

/* A value the detectors confirmed: a locked, supported tone or code. */
static int
policy_confirmed(const dsd_analog_rx_publication* rx) {
    if (rx->tone_state != DSD_ANALOG_TONE_STATE_LOCKED) {
        return 0;
    }
    if (rx->tone_kind == DSD_ANALOG_TONE_KIND_CTCSS) {
        return dsd_ctcss_tone_index(rx->ctcss_tenths_hz) >= 0;
    }
    return rx->tone_kind == DSD_ANALOG_TONE_KIND_DCS && dsd_dcs_code_index(rx->dcs_code) >= 0;
}

/* Whether the list in force lets the confirmed value in @p rx through. */
static int
policy_passes(const dsd_analog_tone_policy* policy, const dsd_analog_rx_publication* rx) {
    const int listed = (rx->tone_kind == DSD_ANALOG_TONE_KIND_CTCSS)
                           ? dsd_tone_set_contains_ctcss(&policy->set, rx->ctcss_tenths_hz)
                           : dsd_tone_set_contains_dcs(&policy->set, rx->dcs_code, rx->dcs_inverted);
    return (policy->mode == DSD_TONE_FILTER_ALLOW) ? listed : !listed;
}

/* Decide on the confirmed value in @p rx. */
static void
policy_judge_value(dsd_analog_tone_policy* policy, const dsd_analog_rx_publication* rx) {
    policy->gate = policy_passes(policy, rx) ? DSD_ANALOG_TONE_GATE_ALLOWED : DSD_ANALOG_TONE_GATE_REJECTED;
    policy_clear_value(policy);
    policy->value_kind = rx->tone_kind;
    policy->value_ctcss = rx->tone_kind == DSD_ANALOG_TONE_KIND_CTCSS ? rx->ctcss_tenths_hz : 0;
    policy->value_dcs_code = rx->tone_kind == DSD_ANALOG_TONE_KIND_DCS ? rx->dcs_code : 0;
    policy->value_dcs_inverted = rx->tone_kind == DSD_ANALOG_TONE_KIND_DCS ? rx->dcs_inverted : 0;
}

/* Whether the window has ended with nothing confirmed: DSD_ANALOG_TONE_WINDOW_MS, run on to at most
   DSD_ANALOG_TONE_WINDOW_DCS_MS while the list holds a code and the DCS detector holds a candidate. */
static int
policy_window_ended(const dsd_analog_tone_policy* policy, const dsd_analog_rx_publication* rx) {
    if (policy->window_us < (int64_t)DSD_ANALOG_TONE_WINDOW_MS * 1000) {
        return 0;
    }
    const int dcs_extends = rx->dcs_candidate && dsd_tone_set_has_dcs(&policy->set);
    return !dcs_extends || policy->window_us >= (int64_t)DSD_ANALOG_TONE_WINDOW_DCS_MS * 1000;
}

/* A confirmed value the list does not pass that rests on transmitter tone error alone (issue #643): an off-value CTCSS
   tone, which the detector names while its on-value rules may still confirm another. It may not close the gate before
   the window ends. */
static int
policy_off_value_reject(const dsd_analog_tone_policy* policy, const dsd_analog_rx_publication* rx) {
    return rx->tone_kind == DSD_ANALOG_TONE_KIND_CTCSS && rx->ctcss_off_value && !policy_passes(policy, rx);
}

static void
policy_step_pending(dsd_analog_tone_policy* policy, const dsd_analog_rx_publication* rx) {
    if (policy_confirmed(rx)) {
        if (!policy_off_value_reject(policy, rx) || policy_window_ended(policy, rx)) {
            policy_judge_value(policy, rx);
        }
        return;
    }
    if (policy_window_ended(policy, rx)) {
        policy->gate =
            (policy->mode == DSD_TONE_FILTER_ALLOW) ? DSD_ANALOG_TONE_GATE_REJECTED : DSD_ANALOG_TONE_GATE_ALLOWED;
        policy_clear_value(policy);
        policy->no_tone = 1;
    }
}

static void
policy_step_allowed(dsd_analog_tone_policy* policy, const dsd_analog_rx_publication* rx) {
    if (policy_confirmed(rx)) {
        if (policy_off_value_reject(policy, rx)) {
            /* One resting on tone error alone is checked again, from a fresh window, as a lost value is. */
            policy->gate = DSD_ANALOG_TONE_GATE_PENDING;
            policy_clear_value(policy);
            policy->window_us = 0;
            policy->window_rem = 0;
            return;
        }
        /* Another value, or the same one: judged afresh, so a blocked or unlisted one closes the gate. */
        policy_judge_value(policy, rx);
        return;
    }
    if (policy->mode == DSD_TONE_FILTER_ALLOW && policy->value_kind != DSD_ANALOG_TONE_KIND_NONE) {
        /* The allowed value is gone (the detector's own hysteresis is spent): check again, from a fresh window. */
        policy->gate = DSD_ANALOG_TONE_GATE_PENDING;
        policy_clear_value(policy);
        policy->window_us = 0;
        policy->window_rem = 0;
    }
    /* A block list's pass does not hang on a value: losing one keeps it. */
}

/* Whether @p rx confirms another value than the one the verdict names: any value, after a "no tone" verdict. */
static int
policy_value_changed(const dsd_analog_tone_policy* policy, const dsd_analog_rx_publication* rx) {
    if (rx->tone_kind != policy->value_kind) {
        return 1;
    }
    if (rx->tone_kind == DSD_ANALOG_TONE_KIND_CTCSS) {
        return rx->ctcss_tenths_hz != policy->value_ctcss;
    }
    return rx->dcs_code != policy->value_dcs_code || rx->dcs_inverted != policy->value_dcs_inverted;
}

static void
policy_step_rejected(dsd_analog_tone_policy* policy, const dsd_analog_rx_publication* rx) {
    /* Kept for the reception, unless a value the list passes is confirmed after all. Another value the list does not
       pass keeps it too, but becomes its reason: a "no tone" rejection that later hears an unlisted tone is then
       "not allowed", naming the tone the received row shows. */
    if (policy_confirmed(rx) && (policy_passes(policy, rx) || policy_value_changed(policy, rx))) {
        policy_judge_value(policy, rx);
    }
}

int
dsd_analog_tone_policy_step(dsd_analog_tone_policy* policy, const dsd_analog_rx_publication* rx, unsigned int samples,
                            int rate_hz) {
    if (!policy) {
        return DSD_ANALOG_TONE_GATE_OFF;
    }
    if (!rx || !policy_mode_on(policy->mode)) {
        return policy->gate;
    }
    if (!rx->carrier_open) {
        /* No carrier: the reception is over, or has not begun. */
        policy_undecided(policy);
        return policy->gate;
    }
    if (!policy->window_open) {
        /* The read that brings the carrier opens the window. */
        policy_undecided(policy);
        policy->window_open = 1;
    } else {
        policy_advance(policy, samples, rate_hz);
    }
    switch (policy->gate) {
        case DSD_ANALOG_TONE_GATE_ALLOWED: policy_step_allowed(policy, rx); break;
        case DSD_ANALOG_TONE_GATE_REJECTED: policy_step_rejected(policy, rx); break;
        default: policy_step_pending(policy, rx); break;
    }
    return policy->gate;
}
