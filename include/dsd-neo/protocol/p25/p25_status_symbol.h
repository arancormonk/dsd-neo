// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief P25 Phase 1 status symbol accumulator and AFC gate logic.
 *
 * Implements TIA-102.BAAA-A §8.4 status symbol classification for AFC gating.
 * Status symbols are 2-bit values transmitted every 36 dibits in P25 Phase 1
 * data units. Their values indicate whether the transmitter is infrastructure
 * (repeater) or a subscriber (portable/mobile).
 *
 * Status symbol values per TIA-102.BAAA-A §8.4 Table 8-2:
 *   - 01: Inbound Channel Busy (repeater only)
 *   - 00: Unknown / talk-around (subscriber)
 *   - 10: Unknown (repeater or subscriber)
 *   - 11: Inbound Channel Idle (repeater only)
 *
 * The accumulator collects status symbol values during frame processing and
 * produces a classification at the end of each data unit. The classification
 * drives the AFC gate: only infrastructure transmissions are allowed to update
 * the auto-PPM frequency correction loop.
 */

#pragma once

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Status symbol classification result. */
typedef enum {
    P25_SS_CLASS_UNKNOWN = 0,        /**< No symbols collected, or all-10 pattern */
    P25_SS_CLASS_INFRASTRUCTURE = 1, /**< At least one 01 or 11 observed (repeater) */
    P25_SS_CLASS_SUBSCRIBER = 2      /**< All symbols are 00 (talk-around) */
} p25_ss_classification_t;

/**
 * Maximum status symbols in any P25 Phase 1 data unit.
 * LDU frames contain approximately 11 status symbols; 12 provides headroom.
 */
#define P25_STATUS_ACCUM_MAX 12

/**
 * @brief Reset the status accumulator for a new data unit.
 *
 * Must be called at the start of each frame processor (LDU1, LDU2, TDU,
 * TDULC, HDU). Clears the symbol buffer and count; classification reverts
 * to UNKNOWN.
 *
 * @param state Decoder state containing the accumulator fields.
 *              If NULL, the function is a no-op.
 */
void p25_status_accum_reset(dsd_state* state);

/**
 * @brief Add a status symbol value to the accumulator.
 *
 * Called at each status symbol position during frame processing (where
 * getDibit() is currently called and the result discarded). The value is
 * clamped to 2 bits (& 0x03) before storage. If the accumulator is full
 * (count >= P25_STATUS_ACCUM_MAX), the value is silently ignored.
 *
 * @param state       Decoder state containing the accumulator fields.
 *                    If NULL, the function is a no-op.
 * @param dibit_value The 2-bit status symbol value (0x00, 0x01, 0x02, or 0x03).
 */
void p25_status_accum_add(dsd_state* state, int dibit_value);

/**
 * @brief Classify the accumulated status symbols and set the AFC gate flag.
 *
 * Examines all collected symbols and determines the transmission source:
 *   - Any 0x01 or 0x03 present → INFRASTRUCTURE (gate open, AFC update allowed)
 *   - All 0x00             → SUBSCRIBER (gate closed, AFC update suppressed)
 *   - All 0x02 or empty   → UNKNOWN (gate closed, conservative suppression)
 *
 * Sets state->p25_ss_classification and state->p25_afc_gate_allow.
 * Increments the appropriate counter (p25_afc_allowed_count or
 * p25_afc_suppressed_count).
 *
 * If opts->p25_afc_gate_disable is set, the gate is forced open regardless
 * of classification (legacy pass-through behavior).
 *
 * @param state Decoder state containing the accumulator fields and gate output.
 *              If NULL, the function is a no-op.
 * @param opts  Decoder options (checked for gating disable flag).
 *              If NULL, gating is treated as enabled (default behavior).
 */
void p25_status_accum_classify(dsd_state* state, const dsd_opts* opts);

#ifdef __cplusplus
}
#endif
