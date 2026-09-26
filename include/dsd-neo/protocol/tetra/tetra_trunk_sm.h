// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * TETRA trunking state machine.
 * Phase 13: frequency-hop on D-TX-GRANTED, return-to-CC after hangtime.
 *
 * Three states:
 *   TETRA_SM_IDLE   – trunking disabled or CC not yet known
 *   TETRA_SM_ON_CC  – parked on CC, listening for grants
 *   TETRA_SM_TUNED  – on voice channel (active or hanging)
 *
 * The SM is a module-global singleton; call tetra_sm_init() once at startup
 * (or omit it — the zero-initialised global is a valid IDLE state).
 */
#ifndef DSD_NEO_PROTOCOL_TETRA_TRUNK_SM_H
#define DSD_NEO_PROTOCOL_TETRA_TRUNK_SM_H

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -----------------------------------------------------------------------
 * SM state enum
 * ----------------------------------------------------------------------- */
typedef enum {
    TETRA_SM_IDLE  = 0, /* not trunking / CC unknown    */
    TETRA_SM_ON_CC,     /* on CC, awaiting grants        */
    TETRA_SM_TUNED,     /* on VC (active or hanging)     */
} tetra_sm_state_e;

/* -----------------------------------------------------------------------
 * Event entry points
 * ----------------------------------------------------------------------- */

/**
 * @brief Reset SM to IDLE (call once at engine init, optional).
 */
void tetra_sm_init(void);

/**
 * @brief Called when SYSINFO is decoded and trunk_cc_freq is valid.
 *
 * When trunking is enabled and the CC frequency is positive, transitions
 * IDLE→ON_CC and registers the CC in the CC-candidates list.
 */
void tetra_sm_on_cc_sync(dsd_opts *opts, dsd_state *state);

/**
 * @brief Called when a MAC Channel Allocation IE resolves a VC frequency.
 *
 * If trunking is enabled and vc_freq_hz > 0 the SM tunes to the VC.
 * @param vc_freq_hz  Resolved downlink VC frequency in Hz (0 = same as CC).
 * @param slot        Four-bit assigned timeslot bitmap.
 */
void tetra_sm_on_grant(dsd_opts *opts, dsd_state *state,
                       long vc_freq_hz, uint8_t slot);

/**
 * @brief Called on D-RELEASE / D-DISCONNECT or MAC allocation release.
 *
 * Immediately returns to CC (hangtime elapsed or call cleared).
 */
void tetra_sm_on_release(dsd_opts *opts, dsd_state *state);

/**
 * @brief Reconcile a control-channel return completed by generic engine code.
 *
 * Clears traffic-only TETRA state without issuing another tuning request.
 * The resulting state is ON_CC when a positive control frequency remains,
 * otherwise IDLE.
 */
void tetra_sm_on_external_cc_return(dsd_state *state);

/**
 * @brief Settle a correlated asynchronous tune without requiring a TETRA frame.
 *
 * The engine calls this before its frame-dispatch gate. This lets a failed VC
 * or CC-return request roll staged state back and retire its failed gate even
 * though that gate is deliberately preventing protocol-frame dispatch.
 */
void tetra_sm_poll_tuning(dsd_opts *opts, dsd_state *state);

/**
 * @brief Periodic tick: enforces hangtime timeout while TUNED.
 *
 * Also returns to the CC when trunking is disabled while tuned. Call from
 * processTetraFrame() once per NDB burst (every 56.67 ms).
 */
void tetra_sm_tick(dsd_opts *opts, dsd_state *state);

/**
 * @brief Return the current SM state (for testing / display).
 */
tetra_sm_state_e tetra_sm_get_state(void);

/**
 * @brief Override the hangtime used for VC→CC return.  (Phase 37)
 *
 * When seconds > 0, this value is used instead of opts->trunk_hangtime
 * in tetra_sm_tick().  Call with 0 to revert to the opts value.
 */
void tetra_sm_set_hangtime(uint32_t seconds);

/**
 * @brief Return the currently configured hangtime override in seconds.
 *
 * Returns 0 when no per-SM override is set (opts default is used).
 */
uint32_t tetra_sm_get_hangtime(void);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_PROTOCOL_TETRA_TRUNK_SM_H */
