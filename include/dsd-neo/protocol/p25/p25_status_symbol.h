// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief P25 Phase 1 status symbol accumulator and advisory AFC gate logic.
 *
 * Implements P25 Phase 1 status symbol classification for diagnostics and an
 * optional AFC gate.
 * Status symbols are 2-bit values transmitted every 36 dibits in P25 Phase 1
 * data units. Their values indicate whether the transmitter is infrastructure
 * (repeater) or a subscriber (portable/mobile).
 *
 * Status symbol values:
 *   - 01: Inbound Channel Busy (repeater only)
 *   - 00: Unknown / talk-around (subscriber)
 *   - 10: Unknown (repeater or subscriber)
 *   - 11: Inbound Channel Idle (repeater only)
 *
 * The accumulator collects status symbol values during frame processing and
 * produces a classification at the end of each data unit. The classification is
 * advisory by default. It can drive an opt-in AFC gate, but status-derived
 * direction is not reliable on every system.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_P25_P25_STATUS_SYMBOL_H_H
#define DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_P25_P25_STATUS_SYMBOL_H_H

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Status symbol classification result. */
typedef enum __attribute__((packed)) {
    P25_SS_CLASS_UNKNOWN = 0,        /**< No symbols collected, or all-10 pattern */
    P25_SS_CLASS_INFRASTRUCTURE = 1, /**< Repeater symbols outnumber subscriber symbols */
    P25_SS_CLASS_SUBSCRIBER = 2      /**< Subscriber symbols present without repeater majority */
} p25_ss_classification_t;

/**
 * Maximum status symbols in any P25 Phase 1 data unit.
 * LDU1/LDU2 frames contain 24 status symbols.
 */
#define P25_STATUS_ACCUM_MAX 24

/**
 * @brief Reset the status accumulator for a new data unit.
 *
 * Called when a new P25 Phase 1 data unit starts. The dispatcher calls this
 * before reading the NID so the NID status symbol can be preserved. Direct
 * frame-processor tests can also call it explicitly. Clears the symbol buffer
 * and count, invalidates any previous advisory gate decision, and reverts
 * classification to UNKNOWN.
 *
 * @param state Decoder state containing the accumulator fields.
 *              If NULL, the function is a no-op.
 */
void p25_status_accum_reset(dsd_state* state);

/**
 * @brief Start a status-symbol accumulator only when one is not already active.
 *
 * Dispatcher code can collect the status symbol embedded in the NID before the
 * DUID-specific processor runs. DUID processors call this helper at entry so
 * direct unit tests still get a fresh accumulator while dispatcher-owned frames
 * preserve the NID status symbol.
 *
 * @param state Decoder state containing the accumulator fields.
 *              If NULL, the function is a no-op.
 */
void p25_status_accum_ensure_started(dsd_state* state);

/**
 * @brief Add a status symbol value to the accumulator.
 *
 * Called at each status symbol position during frame processing. The value is
 * clamped to 2 bits (& 0x03) before storage. If the accumulator is full
 * (count >= P25_STATUS_ACCUM_MAX), the value is silently ignored.
 *
 * @param state       Decoder state containing the accumulator fields.
 *                    If NULL, the function is a no-op.
 * @param dibit_value The 2-bit status symbol value (0x00, 0x01, 0x02, or 0x03).
 */
void p25_status_accum_add(dsd_state* state, int dibit_value);

/**
 * @brief Classify the accumulated status symbols and set the advisory AFC gate flag.
 *
 * Examines all collected symbols and determines the transmission source:
 *   - 0x01 and 0x03 increment the infrastructure/repeater count
 *   - 0x00 increments the subscriber count
 *   - 0x02 is ignored because both sides may use it
 *   - infrastructure wins only when repeater_count > subscriber_count
 *   - subscriber wins when subscriber_count > 0 and repeater_count does not win
 *   - empty or all-0x02 patterns remain UNKNOWN
 *
 * Sets state->p25_ss_classification and state->p25_afc_gate_allow. The gate
 * result is advisory unless opts->p25_afc_status_gate_enable is set by the
 * caller path that consumes it. Increments the appropriate classification
 * counter (p25_afc_allowed_count or p25_afc_suppressed_count).
 *
 * @param state Decoder state containing the accumulator fields and gate output.
 *              If NULL, the function is a no-op.
 * @param opts  Decoder options, reserved for future classification policy.
 */
void p25_status_accum_classify(dsd_state* state, const dsd_opts* opts);

#ifdef __cplusplus
}
#endif
#endif /* DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_P25_P25_STATUS_SYMBOL_H_H */
