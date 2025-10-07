// SPDX-License-Identifier: GPL-2.0-or-later
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

#include <dsd-neo/core/dsd.h>
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

// Initialize with defaults: hangtime 1.0s, vc_grace 1.5s. Callbacks are NULL.
void dsd_p25p2_min_init(dsd_p25p2_min_sm* sm);

// Set callbacks (any may be NULL)
void dsd_p25p2_min_set_callbacks(dsd_p25p2_min_sm* sm, dsd_p25p2_min_on_tune_vc_cb tune_cb,
                                 dsd_p25p2_min_on_return_cc_cb ret_cb, dsd_p25p2_min_on_state_change_cb state_cb);

// Override timing parameters (pass negative to keep existing)
void dsd_p25p2_min_configure(dsd_p25p2_min_sm* sm, double hangtime_s, double vc_grace_s);

// Feed an event into the SM. Uses self-contained clocks; does not write global timers.
void dsd_p25p2_min_handle_event(dsd_p25p2_min_sm* sm, dsd_opts* opts, dsd_state* state, const dsd_p25p2_min_evt* ev);

// Periodic heartbeat (e.g., 10 Hz or 1 Hz). Enforces hang→return transitions.
void dsd_p25p2_min_tick(dsd_p25p2_min_sm* sm, dsd_opts* opts, dsd_state* state);

// Helper: current state query
static inline dsd_p25p2_min_state_e
dsd_p25p2_min_get_state(const dsd_p25p2_min_sm* sm) {
    return sm ? sm->state : DSD_P25P2_MIN_IDLE;
}

// Global singleton accessor used when wiring into existing paths.
// Returns a process-global instance initialized with default callbacks
// (tune VC/return CC via rigctl/RTL helpers).
dsd_p25p2_min_sm* dsd_p25p2_min_get(void);

#ifdef __cplusplus
}
#endif
