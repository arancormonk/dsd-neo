// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief P25 Phase 1 status symbol accumulator and advisory AFC gate implementation.
 *
 * Implements the status symbol accumulator that collects 2-bit status values
 * during P25 Phase 1 frame processing and classifies the transmission source
 * (infrastructure vs subscriber).
 *
 * The classification can drive an opt-in AFC gate, but remains advisory by
 * default because some systems do not emit reliable status-derived direction
 * hints.
 */

#include <dsd-neo/core/state.h>
#include <dsd-neo/protocol/p25/p25_status_symbol.h>
#include <stdint.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

void
p25_status_accum_reset(dsd_state* state) {
    if (!state) {
        return;
    }

    /* Zero the symbol buffer and count; revert classification to UNKNOWN. */
    DSD_MEMSET(state->p25_ss_buf, 0, sizeof(state->p25_ss_buf));
    state->p25_ss_count = 0;
    state->p25_ss_frame_active = 1;
    state->p25_ss_classification = (uint8_t)P25_SS_CLASS_UNKNOWN;
    state->p25_afc_gate_allow = 0;
    state->p25_afc_gate_valid = 0;
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
         * ignored because it can be sent by either side. Use a conservative
         * majority so one noisy repeater-only symbol does not classify a mostly
         * subscriber-originated frame as infrastructure.
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

    (void)opts;

    if (classification == P25_SS_CLASS_INFRASTRUCTURE) {
        state->p25_afc_gate_allow = 1;
        state->p25_afc_allowed_count++;
    } else {
        state->p25_afc_gate_allow = 0;
        state->p25_afc_suppressed_count++;
    }
}
