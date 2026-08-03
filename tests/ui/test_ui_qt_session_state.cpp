// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Unit tests: Android session-phase mapping consumed by the Qt Quick UI. */

#include <stdio.h>

#include "dsd-neo/core/safe_api.h"
#include "session_state_map.h"

using dsd_android::kSessionFailed;
using dsd_android::kSessionIdle;
using dsd_android::kSessionRunning;
using dsd_android::kSessionStarting;
using dsd_android::kSessionStopping;
using dsd_android::SessionPhase;
using dsd_android::SessionPhaseTracker;

namespace {

int g_failures = 0;

const char*
phase_name(SessionPhase phase) {
    switch (phase) {
        case kSessionIdle: return "Idle";
        case kSessionStarting: return "Starting";
        case kSessionRunning: return "Running";
        case kSessionStopping: return "Stopping";
        case kSessionFailed: return "Failed";
        default: return "?";
    }
}

void
expect(const char* what, SessionPhase got, SessionPhase want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %s want %s\n", what, phase_name(got), phase_name(want));
        g_failures++;
    }
}

/* Burn the whole start grace period on IDLE polls. */
SessionPhase
poll_idle(SessionPhaseTracker& tracker, int count) {
    SessionPhase phase = tracker.phase();
    for (int i = 0; i < count; i++) {
        phase = tracker.update("IDLE", false);
    }
    return phase;
}

} // namespace

int
main(void) {
    /* A fresh tracker is idle, and stays idle however often it is polled. */
    {
        SessionPhaseTracker tracker;
        expect("fresh", tracker.phase(), kSessionIdle);
        expect("idle poll", poll_idle(tracker, 32), kSessionIdle);
        if (tracker.failed()) {
            DSD_FPRINTF(stderr, "fresh tracker reports a failure\n");
            g_failures++;
        }
    }

    /* The happy path: request, service picks it up, engine comes up, engine goes down. */
    {
        SessionPhaseTracker tracker;
        tracker.note_start_requested();
        expect("requested", tracker.phase(), kSessionStarting);
        expect("service starting", tracker.update("STARTING", false), kSessionStarting);
        /* The service flips to RUNNING before the engine thread enters the loop. */
        expect("running, engine down", tracker.update("RUNNING", false), kSessionStarting);
        expect("running, engine up", tracker.update("RUNNING", true), kSessionRunning);
        expect("stopping", tracker.update("STOPPING", true), kSessionStopping);
        expect("stopping, engine down", tracker.update("STOPPING", false), kSessionStopping);
        expect("stopped", tracker.update("IDLE", false), kSessionIdle);
        if (tracker.failed()) {
            DSD_FPRINTF(stderr, "clean run reported as a failure\n");
            g_failures++;
        }
        /* And it stays idle rather than decaying into a failure. */
        expect("stopped, settled", poll_idle(tracker, 32), kSessionIdle);
    }

    /* The engine thread can return without a stop ever being asked for: the service
       sets IDLE from the thread body, so RUNNING is never observed again. */
    {
        SessionPhaseTracker tracker;
        tracker.note_start_requested();
        (void)tracker.update("RUNNING", true);
        expect("engine exited on its own", tracker.update("IDLE", false), kSessionIdle);
    }

    /* failStart(): STARTING straight back to IDLE, never having run. */
    {
        SessionPhaseTracker tracker;
        tracker.note_start_requested();
        expect("service starting", tracker.update("STARTING", false), kSessionStarting);
        expect("failed start", tracker.update("IDLE", false), kSessionFailed);
        if (!tracker.failed()) {
            DSD_FPRINTF(stderr, "failed start not latched\n");
            g_failures++;
        }
        /* Sticky: the reason has to survive more than one 250 ms tick. */
        expect("failure sticks", poll_idle(tracker, 32), kSessionFailed);
    }

    /* A failure clears when the user tries again, and a second attempt can succeed. */
    {
        SessionPhaseTracker tracker;
        tracker.note_start_requested();
        (void)tracker.update("STARTING", false);
        expect("failed start", tracker.update("IDLE", false), kSessionFailed);

        tracker.note_start_requested();
        expect("retry clears failure", tracker.phase(), kSessionStarting);
        if (tracker.failed()) {
            DSD_FPRINTF(stderr, "retry did not clear the failure latch\n");
            g_failures++;
        }
        (void)tracker.update("STARTING", false);
        expect("retry running", tracker.update("RUNNING", true), kSessionRunning);
        expect("retry stopped", tracker.update("IDLE", false), kSessionIdle);
    }

    /* The intent never reaches the service: hold Starting through the grace window,
       then report a failure rather than snapping back to the setup screen. */
    {
        SessionPhaseTracker tracker;
        tracker.note_start_requested();
        for (int i = 1; i < dsd_android::kStartGraceTicks; i++) {
            expect("within grace", tracker.update("IDLE", false), kSessionStarting);
        }
        expect("grace expired", tracker.update("IDLE", false), kSessionFailed);
    }

    /* A start the host could not even hand over is a failure immediately. */
    {
        SessionPhaseTracker tracker;
        tracker.note_start_requested();
        tracker.note_start_failed();
        expect("host-side failure", tracker.phase(), kSessionFailed);
        expect("host-side failure sticks", poll_idle(tracker, 4), kSessionFailed);
    }

    /* A null state name (the JNI call itself failed) reads as IDLE, not a crash. */
    {
        SessionPhaseTracker tracker;
        expect("null service state", tracker.update(nullptr, false), kSessionIdle);
    }

    /* An unknown state name from a future service revision must not wedge the UI in
       a monitoring view it can never leave. */
    {
        SessionPhaseTracker tracker;
        tracker.note_start_requested();
        (void)tracker.update("RUNNING", true);
        expect("unknown name", tracker.update("SOMETHING_NEW", false), kSessionIdle);
    }

    if (g_failures != 0) {
        DSD_FPRINTF(stderr, "session state map: %d failure(s)\n", g_failures);
        return 1;
    }
    return 0;
}
