// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file
 * @brief Minimal P25 Phase 2 state machine surface.
 */
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Minimal P25 Phase 2 state machine (draft)
 *
 * 4 states: IDLE → FOLLOWING_VC → HANG → RETURN_CC → IDLE
 * Events: GRANT/PTT/ACTIVE/END/IDLE/NOSYNC
 *
 * Goals:
 *  - No cross-module timer writes (self-contained timing)
 *  - Explicit actions exposed via callbacks (tune VC, return CC, state change)
 *  - Deterministic transitions suitable for instrumentation and testing
 */

#pragma once

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DSD_P25P2_MIN_IDLE = 0,
    DSD_P25P2_MIN_ARMED = 1,        // tuned on GRANT, awaiting PTT/ACTIVE
    DSD_P25P2_MIN_FOLLOWING_VC = 2, // voice seen; actively following
    DSD_P25P2_MIN_HANG = 3,         // both slots quiet; hang timer running
    DSD_P25P2_MIN_RETURN_CC = 4
} dsd_p25p2_min_state_e;

typedef enum {
    DSD_P25P2_MIN_EV_GRANT = 0, // payload: channel, freq_hz
    DSD_P25P2_MIN_EV_PTT,       // payload: slot
    DSD_P25P2_MIN_EV_ACTIVE,    // payload: slot
    DSD_P25P2_MIN_EV_END,       // payload: slot
    DSD_P25P2_MIN_EV_IDLE,      // payload: slot
    DSD_P25P2_MIN_EV_NOSYNC     // payload: none
} dsd_p25p2_min_event_type_e;

typedef struct {
    dsd_p25p2_min_event_type_e type;
    int slot;     // 0 or 1 (when applicable), else -1
    int channel;  // 16-bit channel (when applicable), else 0
    long freq_hz; // Hz (when applicable), else 0
} dsd_p25p2_min_evt;

// Action callbacks (provided by caller); may be NULL.
typedef void (*dsd_p25p2_min_on_tune_vc_cb)(dsd_opts* opts, dsd_state* state, long freq_hz, int channel);
typedef void (*dsd_p25p2_min_on_return_cc_cb)(dsd_opts* opts, dsd_state* state);
typedef void (*dsd_p25p2_min_on_state_change_cb)(dsd_opts* opts, dsd_state* state, dsd_p25p2_min_state_e old_state,
                                                 dsd_p25p2_min_state_e new_state, const char* reason);

typedef struct {
    // Config
    double hangtime_s;            // hangtime in seconds (e.g., 1.0)
    double vc_grace_s;            // grace window after tune before eligible for release (e.g., 1.5)
    double min_follow_dwell_s;    // minimal dwell after first voice to avoid ping-pong (e.g., 0.7)
    double grant_voice_timeout_s; // max wait from GRANT (ARMED) to PTT/ACTIVE before returning (e.g., 2.0)
    double retune_backoff_s;      // ignore grants to same freq within this window after a return (e.g., 3.0)

    // Current state and VC context
    dsd_p25p2_min_state_e state;
    long vc_freq_hz; // current tuned VC (0 when none)
    int vc_channel;  // last tuned channel id (0 when none)

    // Slot activity
    uint8_t slot_active[2];

    // Internal clocks (self-contained; do not write global timers)
    time_t t_last_tune;
    time_t t_last_voice;
    time_t t_hang_start;
    time_t t_follow_start;

    // Callbacks
    dsd_p25p2_min_on_tune_vc_cb on_tune_vc;
    dsd_p25p2_min_on_return_cc_cb on_return_cc;
    dsd_p25p2_min_on_state_change_cb on_state_change;

    // Backoff bookkeeping
    long last_return_freq;
    time_t t_last_return;
} dsd_p25p2_min_sm;

/**
 * @brief Initialize the minimal P25P2 state machine with defaults.
 *
 * hangtime defaults to 1.0s and vc_grace to 1.5s; callbacks start as NULL.
 *
 * @param sm State-machine instance to initialize.
 */
void dsd_p25p2_min_init(dsd_p25p2_min_sm* sm);

/**
 * @brief Set state-machine callbacks; any callback may be NULL.
 *
 * @param sm State-machine instance.
 * @param tune_cb Callback invoked to tune to a voice channel.
 * @param ret_cb Callback invoked to return to the control channel.
 * @param state_cb Callback invoked on state transitions.
 */
void dsd_p25p2_min_set_callbacks(dsd_p25p2_min_sm* sm, dsd_p25p2_min_on_tune_vc_cb tune_cb,
                                 dsd_p25p2_min_on_return_cc_cb ret_cb, dsd_p25p2_min_on_state_change_cb state_cb);

/**
 * @brief Override core timing parameters (pass negative to keep existing).
 *
 * @param sm State-machine instance.
 * @param hangtime_s Hang time in seconds (negative to keep existing).
 * @param vc_grace_s Grace window after tuning before eligible for release (negative to keep existing).
 */
void dsd_p25p2_min_configure(dsd_p25p2_min_sm* sm, double hangtime_s, double vc_grace_s);

/**
 * @brief Extended configuration for all timing parameters (pass negative to keep existing).
 *
 * @param sm State-machine instance.
 * @param hangtime_s Hang time in seconds (negative to keep existing).
 * @param vc_grace_s Grace window after tuning before eligible for release (negative to keep existing).
 * @param min_follow_dwell_s Minimum dwell after first voice burst (negative to keep existing).
 * @param grant_voice_timeout_s Timeout from grant to active voice before returning (negative to keep existing).
 * @param retune_backoff_s Backoff before retuning to the same frequency (negative to keep existing).
 */
void dsd_p25p2_min_configure_ex(dsd_p25p2_min_sm* sm, double hangtime_s, double vc_grace_s, double min_follow_dwell_s,
                                double grant_voice_timeout_s, double retune_backoff_s);

/**
 * @brief Feed an event into the state machine (uses self-contained clocks).
 *
 * Does not modify global timers; intended to be driven by decoder callbacks.
 *
 * @param sm State-machine instance.
 * @param opts Decoder options.
 * @param state Decoder state.
 * @param ev Event to handle.
 */
void dsd_p25p2_min_handle_event(dsd_p25p2_min_sm* sm, dsd_opts* opts, dsd_state* state, const dsd_p25p2_min_evt* ev);

/**
 * @brief Periodic heartbeat enforcing hang-to-return transitions.
 *
 * Call at a regular cadence (e.g., 10 Hz or 1 Hz).
 *
 * @param sm State-machine instance.
 * @param opts Decoder options.
 * @param state Decoder state.
 */
void dsd_p25p2_min_tick(dsd_p25p2_min_sm* sm, dsd_opts* opts, dsd_state* state);

// Helper: current state query
static inline dsd_p25p2_min_state_e
dsd_p25p2_min_get_state(const dsd_p25p2_min_sm* sm) {
    return sm ? sm->state : DSD_P25P2_MIN_IDLE;
}

/**
 * @brief Access the process-global minimal P25P2 state-machine instance.
 *
 * Returns a singleton initialized with default callbacks (tune VC/return CC
 * via rigctl/RTL helpers).
 *
 * @return Pointer to the global state-machine instance.
 */
dsd_p25p2_min_sm* dsd_p25p2_min_get(void);

#ifdef __cplusplus
}
#endif
