// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Module-private CTCSS/DCS receive policy (issue #527): the verdict state machine.
 *
 * Pure: no dsd_state, no clock. It reads what the received-tone tap publishes (dsd_analog_rx_publication) and keeps
 * time in samples, so its tests inject detections in sample time and a fast replay decides exactly as a live input
 * does. src/dsp/analog_rx.c runs it after every read of the tap and publishes the verdict in dsd_state::analog_rx.
 *
 * With a list policy (allow or block) in force, every reception starts PENDING. The window opens with the first read
 * that has a carrier and lasts DSD_ANALOG_TONE_WINDOW_MS of sample time; while a tone or code confirmed by the
 * detectors is judged at once, the window only bounds how long a reception may go without one:
 *
 *   PENDING   a confirmed value: allow list, listed -> ALLOWED, else REJECTED; block list, listed -> REJECTED,
 *             else ALLOWED. Nothing confirmed when the window ends: allow -> REJECTED, block -> ALLOWED ("no tone").
 *             An off-value CTCSS tone (dsd_analog_rx_publication::ctcss_off_value, issue #643) the list does not pass
 *             is judged only when the window ends: until then the detector may still confirm another value on its own
 *             rules, and a scanner must not leave the reception on a tone that rests on tone error alone.
 *             The window runs on to at most DSD_ANALOG_TONE_WINDOW_DCS_MS while the list holds a DCS code and the DCS
 *             detector holds a candidate code (dsd_analog_rx_publication::dcs_candidate).
 *   ALLOWED   allow list: a confirmed value it does not list -> REJECTED; the value lost (after the detector's own
 *             hysteresis) -> PENDING with a fresh window. Block list: a confirmed listed value -> REJECTED; a loss
 *             keeps ALLOWED. Either list: an off-value CTCSS tone it does not pass -> PENDING with a fresh window.
 *   REJECTED  kept for the rest of the reception, except that a newly confirmed value the list passes -> ALLOWED.
 *             Another confirmed value it does not pass keeps REJECTED but becomes its reason (a "no tone" rejection
 *             that later hears an unlisted tone names that tone).
 *
 * A reception ends when the tap's carrier hangover runs out or the tap resets (a retune, a row change, a stream
 * pause): the caller then resets the policy, and the next carrier opens a new window.
 */

#ifndef DSD_NEO_SRC_DSP_ANALOG_TONE_POLICY_H_
#define DSD_NEO_SRC_DSP_ANALOG_TONE_POLICY_H_

#include <dsd-neo/core/analog_tone.h>
#include <dsd-neo/core/state_fwd.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief The policy's working state. Zero it with dsd_analog_tone_policy_init(). */
typedef struct {
    int mode;           /**< dsd_tone_filter_mode in force */
    dsd_tone_set set;   /**< the list in force */
    int gate;           /**< dsd_analog_tone_gate: the verdict */
    int no_tone;        /**< 1 when the verdict was decided because nothing was confirmed within the window */
    int window_open;    /**< 1 once the reception's carrier opened the window */
    int64_t window_us;  /**< sample time since the window opened, in microseconds */
    int64_t window_rem; /**< the part of a microsecond window_us has not counted yet, times window_rate_hz */
    int window_rate_hz; /**< the rate window_rem is counted at */
    int value_kind;     /**< dsd_analog_tone_kind of the value that decided the verdict; NONE for no tone */
    int value_ctcss;    /**< that tone, tenths of a hertz */
    int value_dcs_code; /**< that code, as the detector named it */
    int value_dcs_inverted;
} dsd_analog_tone_policy;

/** @brief No policy in force, nothing decided. */
void dsd_analog_tone_policy_init(dsd_analog_tone_policy* policy);

/**
 * @brief Install the policy in force: @p mode (dsd_tone_filter_mode; anything else, or allow/block with an empty or
 * NULL @p set, is OFF) and its list.
 *
 * A different policy starts the reception's evaluation over (a list policy: PENDING with a fresh window at the next
 * read with a carrier); the same one changes nothing, a list that only respells a code included
 * (dsd_tone_set_same_signals()), which the policy then holds as given.
 *
 * @return 1 when the policy changed, 0 otherwise.
 */
int dsd_analog_tone_policy_configure(dsd_analog_tone_policy* policy, int mode, const dsd_tone_set* set);

/** @brief End the reception: the next carrier opens a new window. The configured policy stays. */
void dsd_analog_tone_policy_reset(dsd_analog_tone_policy* policy);

/**
 * @brief Take one read of the tap: @p samples of sample time at @p rate_hz, after which the tap published @p rx.
 *
 * @return The verdict (dsd_analog_tone_gate), also in policy->gate.
 */
int dsd_analog_tone_policy_step(dsd_analog_tone_policy* policy, const dsd_analog_rx_publication* rx,
                                unsigned int samples, int rate_hz);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_SRC_DSP_ANALOG_TONE_POLICY_H_ */
