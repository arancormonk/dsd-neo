// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief P25 Phase 1 status symbol accumulator and AFC gate implementation.
 *
 * Implements the status symbol accumulator that collects 2-bit status values
 * during P25 Phase 1 frame processing and classifies the transmission source
 * (infrastructure vs subscriber).
 *
 * The classification drives the AFC gate: only infrastructure transmissions are
 * allowed to update the auto-PPM frequency correction loop. Subscriber and
 * unknown patterns are suppressed to prevent poor subscriber oscillator
 * stability from corrupting the frequency estimate.
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/protocol/p25/p25_status_symbol.h>

#include <stdint.h>
#include <string.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

void
p25_status_accum_reset(dsd_state* state) {
    if (!state) {
        return;
    }

    /* Zero the symbol buffer and count; revert classification to UNKNOWN. */
    memset(state->p25_ss_buf, 0, sizeof(state->p25_ss_buf));
    state->p25_ss_count = 0;
    state->p25_ss_frame_active = 1;
    state->p25_ss_classification = (uint8_t)P25_SS_CLASS_UNKNOWN;
}

void
p25_status_accum_ensure_started(dsd_state* state) {
    if (!state) {
        return;
    }
    if (!state->p25_ss_frame_active) {
        p25_status_accum_reset(state);
    }
}

void
p25_status_accum_add(dsd_state* state, int dibit_value) {
    if (!state) {
        return;
    }

    state->p25_ss_frame_active = 1;

    /* Clamp to 2-bit range to handle any out-of-range input gracefully. */
    uint8_t val = (uint8_t)(dibit_value & 0x03);

    /* Store only if we have room; silently ignore overflow (>12 symbols). */
    if (state->p25_ss_count < P25_STATUS_ACCUM_MAX) {
        state->p25_ss_buf[state->p25_ss_count] = val;
        state->p25_ss_count++;
    }
}

void
p25_status_accum_classify(dsd_state* state, const dsd_opts* opts) {
    if (!state) {
        return;
    }

    p25_ss_classification_t classification = P25_SS_CLASS_UNKNOWN;

    if (state->p25_ss_count == 0) {
        /* No symbols collected: conservative classify as UNKNOWN. */
        classification = P25_SS_CLASS_UNKNOWN;
    } else {
        /*
         * Match the sdrtrunk channel status semantics: 00 increments the
         * subscriber count, 01/11 increment the repeater count, and 10 is
         * ignored because it can be sent by either side. For AFC gating, use a
         * conservative majority so one noisy repeater-only symbol does not open
         * the gate for a mostly subscriber-originated frame.
         */
        unsigned int subscriber_count = 0;
        unsigned int repeater_count = 0;

        for (uint8_t i = 0; i < state->p25_ss_count; i++) {
            uint8_t sym = state->p25_ss_buf[i];

            if (sym == 0x01 || sym == 0x03) {
                repeater_count++;
            } else if (sym == 0x00) {
                subscriber_count++;
            }
        }

        if (repeater_count > subscriber_count) {
            classification = P25_SS_CLASS_INFRASTRUCTURE;
        } else if (subscriber_count > 0) {
            classification = P25_SS_CLASS_SUBSCRIBER;
        } else {
            /* Empty or all 0x02 symbols. */
            classification = P25_SS_CLASS_UNKNOWN;
        }
    }

    state->p25_ss_classification = (uint8_t)classification;
    state->p25_ss_frame_active = 0;
    state->p25_afc_gate_valid = 1;

    /*
     * Gate decision: allow AFC update only for infrastructure transmissions,
     * unless gating is disabled via configuration (legacy pass-through).
     *
     * When opts is NULL, treat as gating enabled (default/safe behavior).
     */
    int gating_disabled = (opts != NULL) ? opts->p25_afc_gate_disable : 0;

    if (gating_disabled || classification == P25_SS_CLASS_INFRASTRUCTURE) {
        state->p25_afc_gate_allow = 1;
        state->p25_afc_allowed_count++;
    } else {
        state->p25_afc_gate_allow = 0;
        state->p25_afc_suppressed_count++;
    }
}
