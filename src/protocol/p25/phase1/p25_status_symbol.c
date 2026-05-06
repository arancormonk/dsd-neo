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
 * (infrastructure vs subscriber) per TIA-102.BAAA-A §8.4 Table 8-2.
 *
 * The classification drives the AFC gate: only infrastructure transmissions
 * (status symbols 01 or 11 present) are allowed to update the auto-PPM
 * frequency correction loop. Subscriber transmissions (all 00) and unknown
 * patterns (all 10 or empty) are suppressed to prevent poor subscriber
 * oscillator stability from corrupting the frequency estimate.
 *
 * Thread safety: the gate flag (p25_afc_gate_allow) is a single byte written
 * by the decoder thread and read by the RTL thread. This is safe because a
 * single-byte write is atomic on all target platforms, and a stale read
 * (seeing the previous frame's value) is harmless for a slowly-varying AFC
 * loop.
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
    state->p25_ss_classification = (uint8_t)P25_SS_CLASS_UNKNOWN;
}

void
p25_status_accum_add(dsd_state* state, int dibit_value) {
    if (!state) {
        return;
    }

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
        /* No symbols collected — conservative: classify as UNKNOWN. */
        classification = P25_SS_CLASS_UNKNOWN;
    } else {
        /*
         * Scan the buffer for infrastructure indicators (0x01 or 0x03).
         * Per TIA-102.BAAA-A §8.4:
         *   - 01 = Inbound Channel Busy (repeater only)
         *   - 11 = Inbound Channel Idle (repeater only)
         * A single infrastructure indicator is sufficient to confirm
         * the transmission originates from a repeater.
         */
        int has_infra = 0;
        int all_zero = 1;

        for (uint8_t i = 0; i < state->p25_ss_count; i++) {
            uint8_t sym = state->p25_ss_buf[i];

            if (sym == 0x01 || sym == 0x03) {
                has_infra = 1;
                all_zero = 0;
                break; /* One infrastructure indicator is sufficient. */
            }

            if (sym != 0x00) {
                all_zero = 0;
            }
        }

        if (has_infra) {
            classification = P25_SS_CLASS_INFRASTRUCTURE;
        } else if (all_zero) {
            classification = P25_SS_CLASS_SUBSCRIBER;
        } else {
            /* All symbols are 0x02 (or mix of 0x00 and 0x02 with no infra). */
            classification = P25_SS_CLASS_UNKNOWN;
        }
    }

    state->p25_ss_classification = (uint8_t)classification;

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
