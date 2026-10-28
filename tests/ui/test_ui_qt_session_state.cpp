// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Unit tests: Android session-phase mapping consumed by the Qt Quick UI. */

#include <initializer_list>
#include <stdint.h>
#include <stdio.h>

#include "../../android/run_status.h"
#include "../../android/session_state_map.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/runtime/input_failure.h"

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

void
test_failure_recovery() {
    SessionPhaseTracker tracker;
    tracker.note_start_requested(40);
    expect("native failure", tracker.update("IDLE", false, 41, dsd_android::kRunFailed), kSessionFailed);
    (void)tracker.acknowledge_failure(41, "STOPPING");
    expect("teardown cannot be acknowledged", tracker.phase(), kSessionFailed);
    (void)tracker.acknowledge_failure(41, "IDLE");
    expect("acknowledged failure is idle", tracker.phase(), kSessionIdle);
    expect("retained result stays acknowledged", tracker.update("IDLE", false, 41, dsd_android::kRunFailed),
           kSessionIdle);
    (void)tracker.note_start_requested(41, "IDLE");
    expect("retry starts", tracker.update("RUNNING", true, 42, dsd_android::kRunPending), kSessionRunning);
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

/* A native terminal result can precede wake-lock release and service IDLE.
 * Retry controls follow the published phase, so neither a quick failure nor a
 * completed/cancelled run may enable them while that teardown is in flight. */
void
test_terminal_waits_for_service_idle() {
    for (const auto reason : {dsd_android::kRunFailed, dsd_android::kRunCompleted, dsd_android::kRunCancelled}) {
        for (const bool saw_running : {false, true}) {
            SessionPhaseTracker tracker;
            tracker.note_start_requested(40);
            if (saw_running) {
                expect("session running before native return",
                       tracker.update("RUNNING", true, 41, dsd_android::kRunPending), kSessionRunning);
            }
            int premature_retries = 0;
            for (const char* service_state : {"RUNNING", "STOPPING"}) {
                // More than the start grace period at the UI's 250 ms poll: teardown
                // must not become a failed retry simply because several polls passed.
                for (int poll = 0; poll < static_cast<int>(dsd_android::kStartGraceMs / 250) + 2; ++poll) {
                    const SessionPhase phase = tracker.update(service_state, false, 41, reason);
                    expect("native terminal result waits for service teardown", phase, kSessionStopping);
                    if (phase == kSessionIdle || phase == kSessionFailed) {
                        ++premature_retries;
                        tracker.note_start_requested(41);
                    }
                    // The host rechecks the service record even for a retry
                    // arriving before the next UI refresh has disabled its button.
                    if (tracker.note_start_requested(41, service_state)) {
                        DSD_FPRINTF(stderr, "host accepted retry before service IDLE\n");
                        ++g_failures;
                    }
                    expect("rejected retry leaves teardown pending", tracker.phase(), kSessionStopping);
                }
            }
            if (premature_retries != 0) {
                DSD_FPRINTF(stderr, "terminal result enabled %d premature retry attempt(s)\n", premature_retries);
                ++g_failures;
            }
            const SessionPhase final_phase = reason == dsd_android::kRunFailed ? kSessionFailed : kSessionIdle;
            expect("teardown publishes the retained terminal outcome", tracker.update("IDLE", false, 41, reason),
                   final_phase);
            if (!tracker.note_start_requested(41, "IDLE")) {
                DSD_FPRINTF(stderr, "host rejected retry after service IDLE\n");
                ++g_failures;
            }
            expect("retry waits for the next service session", tracker.update("IDLE", false, 41, reason),
                   kSessionStarting);
            expect("retry starts after teardown", tracker.update("STARTING", false, 42, dsd_android::kRunPending),
                   kSessionStarting);
            expect("retry reaches running", tracker.update("RUNNING", true, 42, dsd_android::kRunPending),
                   kSessionRunning);
        }
    }
}

void
expect_true(const char* what, bool ok) {
    if (!ok) {
        DSD_FPRINTF(stderr, "%s\n", what);
        g_failures++;
    }
}

constexpr dsd_android::RunReason kPending = dsd_android::kRunPending;

/* #718: Stop shows Stopping at once, and every record of the running attempt keeps it until the attempt ends. */
void
test_stop_request_during_running() {
    SessionPhaseTracker tracker;
    tracker.note_start_requested(40);
    expect("running before the stop", tracker.update("RUNNING", true, 41, kPending, 500), kSessionRunning);
    expect_true("a stop from Running is accepted", tracker.note_stop_requested(1000));
    expect("the stop publishes Stopping at once", tracker.phase(), kSessionStopping);
    expect_true("a second stop adds no request", !tracker.note_stop_requested(1100));
    for (const uint64_t now : {1250u, 1500u, 1750u, 2999u}) {
        expect("RUNNING polls before the service sees the stop", tracker.update("RUNNING", true, 41, kPending, now),
               kSessionStopping);
    }
    expect("service STOPPING", tracker.update("STOPPING", true, 41, kPending, 3250), kSessionStopping);
    expect("native cancel before service IDLE", tracker.update("STOPPING", false, 41, dsd_android::kRunCancelled, 3500),
           kSessionStopping);
    expect("terminal Idle ends the request", tracker.update("IDLE", false, 41, dsd_android::kRunCancelled, 3750),
           kSessionIdle);
    expect_true("an ended request never expires", !tracker.take_stop_expired());
    expect_true("Idle takes no request", !tracker.note_stop_requested(4000));
    expect("Idle stays Idle", tracker.phase(), kSessionIdle);
}

/* A start may still report the previous session's IDLE record, then a provisional STARTING, before it runs. */
void
test_stop_request_during_starting() {
    SessionPhaseTracker tracker;
    tracker.note_start_requested(40);
    expect("starting before the stop", tracker.update("IDLE", false, 40, kPending, 0), kSessionStarting);
    expect_true("a stop from Starting is accepted", tracker.note_stop_requested(10));
    expect("the stop publishes Stopping at once", tracker.phase(), kSessionStopping);
    expect("old session's IDLE record", tracker.update("IDLE", false, 40, kPending, 250), kSessionStopping);
    /* A long configure: the grace counts RUNNING time, not time since the request. */
    expect("new session STARTING", tracker.update("STARTING", false, 41, kPending, 500), kSessionStopping);
    expect("still STARTING", tracker.update("STARTING", false, 41, kPending, 4000), kSessionStopping);
    expect("RUNNING before the engine loop", tracker.update("RUNNING", false, 41, kPending, 4250), kSessionStopping);
    expect("RUNNING engine", tracker.update("RUNNING", true, 41, kPending, 4500), kSessionStopping);
    expect("RUNNING under the grace", tracker.update("RUNNING", true, 41, kPending, 6000), kSessionStopping);
    expect("STOPPING", tracker.update("STOPPING", true, 41, kPending, 6250), kSessionStopping);
    expect("terminal Idle", tracker.update("IDLE", false, 41, dsd_android::kRunCancelled, 6500), kSessionIdle);
    expect_true("no expiry after a delivered stop", !tracker.take_stop_expired());
}

void
test_stop_request_then_failure() {
    SessionPhaseTracker tracker;
    tracker.note_start_requested(40);
    expect_true("stop during the start", tracker.note_stop_requested(0));
    expect("STARTING", tracker.update("STARTING", false, 41, kPending, 250), kSessionStopping);
    expect("failure publishes as today", tracker.update("IDLE", false, 41, dsd_android::kRunFailed, 500),
           kSessionFailed);
    expect_true("the failure latches", tracker.failed());
    expect("later polls keep the failure", tracker.update("IDLE", false, 41, dsd_android::kRunFailed, 750),
           kSessionFailed);
    expect_true("Failed takes no request", !tracker.note_stop_requested(1000));
    expect("Failed stays Failed", tracker.phase(), kSessionFailed);

    /* A start that never reached the service fails as today too. */
    SessionPhaseTracker undelivered;
    undelivered.note_start_requested(40);
    expect_true("stop during an undelivered start", undelivered.note_stop_requested(0));
    SessionPhase phase = kSessionStopping;
    for (int poll = 0; poll < static_cast<int>(dsd_android::kStartGraceMs / 250); ++poll) {
        phase = undelivered.update("IDLE", false, 40, kPending, 250u * (uint64_t)(poll + 1));
    }
    expect("the start grace still fails the start", phase, kSessionFailed);
}

void
test_later_start_clears_stop_request() {
    SessionPhaseTracker tracker;
    tracker.note_start_requested(40);
    (void)tracker.update("RUNNING", true, 41, kPending, 0);
    expect_true("stop accepted", tracker.note_stop_requested(100));
    tracker.note_start_requested(41);
    expect("a new attempt starts", tracker.phase(), kSessionStarting);
    expect("its STARTING is not held", tracker.update("STARTING", false, 42, kPending, 200), kSessionStarting);
    expect("its RUNNING is not held", tracker.update("RUNNING", true, 42, kPending, 300), kSessionRunning);
}

/* A stop the service never saw: intents are handled in order, so 2 s of RUNNING means it was lost. */
void
test_stop_request_grace() {
    SessionPhaseTracker tracker;
    tracker.note_start_requested(40);
    (void)tracker.update("RUNNING", true, 41, kPending, 0);
    expect_true("stop accepted", tracker.note_stop_requested(1000));
    expect("RUNNING at 1.25 s", tracker.update("RUNNING", true, 41, kPending, 2250), kSessionStopping);
    expect("RUNNING just under 2 s", tracker.update("RUNNING", true, 41, kPending, 2999), kSessionStopping);
    expect_true("no expiry under the grace", !tracker.take_stop_expired());
    expect("RUNNING for 2 s restores Running", tracker.update("RUNNING", true, 41, kPending, 3000), kSessionRunning);
    expect_true("the expiry is reported once", tracker.take_stop_expired());
    expect_true("and only once", !tracker.take_stop_expired());
    expect("Running stays", tracker.update("RUNNING", true, 41, kPending, 3250), kSessionRunning);
    expect_true("Stop can be asked again", tracker.note_stop_requested(3300));
    expect("the second stop holds", tracker.update("RUNNING", true, 41, kPending, 3500), kSessionStopping);
    expect("and is delivered", tracker.update("STOPPING", true, 41, kPending, 3750), kSessionStopping);
    /* Once STOPPING was seen the stop was delivered: no grace can bring Running back. */
    expect("delivered stop never expires", tracker.update("RUNNING", true, 41, kPending, 9000), kSessionStopping);
    expect_true("no expiry after delivery", !tracker.take_stop_expired());

    SessionPhaseTracker stopping;
    stopping.note_start_requested(40);
    (void)stopping.update("RUNNING", true, 41, kPending, 0);
    expect("service already STOPPING", stopping.update("STOPPING", true, 41, kPending, 250), kSessionStopping);
    expect_true("Stopping takes no request", !stopping.note_stop_requested(300));
}

/* RUNNING records of session 41 from @p from_ms to @p to_ms; the phase they publish, or Idle at the first that is not
 * Stopping. */
SessionPhase
hold_through_running(SessionPhaseTracker& tracker, uint64_t from_ms, uint64_t to_ms) {
    for (uint64_t now = from_ms; now <= to_ms; now += 250) {
        const SessionPhase phase = tracker.update("RUNNING", true, 41, kPending, now);
        if (phase != kSessionStopping) {
            return phase;
        }
    }
    return kSessionStopping;
}

/* #647: a committed receiver ending is an acknowledged stop. The session's cleanup (the rdio upload drain) can keep the
 * service RUNNING for minutes, and Stopping holds through all of it until that session's terminal record; the 2 s
 * grace for an unanswered user Stop never applies. */
void
test_receiver_end_holds_stopping_through_long_running() {
    SessionPhaseTracker tracker;
    tracker.note_start_requested(40);
    expect("running before the receiver ends", tracker.update("RUNNING", true, 41, kPending, 0), kSessionRunning);
    expect_true("an older session's ending is not this one's", !tracker.note_receiver_end(40));
    expect("an older session's ending changes nothing", tracker.phase(), kSessionRunning);
    expect_true("the running session's ending is taken", tracker.note_receiver_end(41));
    expect("the ending publishes Stopping at once", tracker.phase(), kSessionStopping);
    expect("30 s of RUNNING records hold Stopping", hold_through_running(tracker, 250, 30000), kSessionStopping);
    expect_true("the hold never expires", !tracker.take_stop_expired());
    expect("engine down, service still RUNNING", tracker.update("RUNNING", false, 41, kPending, 30250),
           kSessionStopping);
    expect("native result before service IDLE",
           tracker.update("STOPPING", false, 41, dsd_android::kRunCancelled, 30500), kSessionStopping);
    expect("the terminal record publishes Idle", tracker.update("IDLE", false, 41, dsd_android::kRunCancelled, 30750),
           kSessionIdle);
    expect_true("Idle takes no ending", !tracker.note_receiver_end(41));
    expect("the retained record stays Idle", tracker.update("IDLE", false, 41, dsd_android::kRunCancelled, 31000),
           kSessionIdle);

    /* A user Stop standing when the receiver ends is acknowledged by the ending: it no longer expires. */
    SessionPhaseTracker stopped;
    stopped.note_start_requested(40);
    (void)stopped.update("RUNNING", true, 41, kPending, 0);
    expect_true("stop accepted", stopped.note_stop_requested(100));
    expect_true("the ending is taken over the stop", stopped.note_receiver_end(41));
    expect("a stop with an ending holds past the grace", hold_through_running(stopped, 250, 30000), kSessionStopping);
    expect_true("and never expires", !stopped.take_stop_expired());
    expect("its terminal record publishes Idle", stopped.update("IDLE", false, 41, dsd_android::kRunCancelled, 30250),
           kSessionIdle);

    /* An ending while the session still starts holds too, and a failed run still publishes Failed. */
    SessionPhaseTracker starting;
    starting.note_start_requested(40);
    expect("starting", starting.update("STARTING", false, 41, kPending, 0), kSessionStarting);
    expect_true("an ending during the start is taken", starting.note_receiver_end(41));
    expect("RUNNING before the loop holds", starting.update("RUNNING", false, 41, kPending, 250), kSessionStopping);
    expect("the failure publishes as today", starting.update("IDLE", false, 41, dsd_android::kRunFailed, 500),
           kSessionFailed);

    /* A new start ends the hold. */
    SessionPhaseTracker restarted;
    restarted.note_start_requested(40);
    (void)restarted.update("RUNNING", true, 41, kPending, 0);
    expect_true("ending taken", restarted.note_receiver_end(41));
    restarted.note_start_requested(41);
    expect("the new attempt starts", restarted.phase(), kSessionStarting);
    expect("its RUNNING is not held", restarted.update("RUNNING", true, 42, kPending, 250), kSessionRunning);

    /* Nothing to end: a fresh or idle tracker takes no ending. */
    SessionPhaseTracker fresh;
    expect_true("a fresh tracker takes no ending", !fresh.note_receiver_end(41));
    expect("and stays Idle", fresh.phase(), kSessionIdle);
}

/* #782: the start grace is two seconds of real time from the request, not eight polls. A Qt thread that polls fast
 * must not fail a start early, and one that polls late fails it on the first poll past the grace. */
void
test_start_grace_is_real_time() {
    SessionPhaseTracker fast;
    fast.note_start_requested(40);
    for (uint64_t now = 50; now <= 1000; now += 50) {
        expect("twenty fast polls within a second are still the grace", fast.update("IDLE", false, 40, kPending, now),
               kSessionStarting);
    }
    expect_true("no expiry under the grace", !fast.take_start_expired());

    SessionPhaseTracker timed;
    expect_true("an idle service takes the start", timed.note_start_requested(40, "IDLE", 10000));
    expect("1999 ms after the request", timed.update("IDLE", false, 40, kPending, 11999), kSessionStarting);
    expect("2000 ms after the request", timed.update("IDLE", false, 40, kPending, 12000), kSessionFailed);
    uint64_t waited = 0;
    expect_true("the expiry is reported", timed.take_start_expired(&waited));
    expect_true("with how long the start waited", waited == 2000);
    expect_true("once", !timed.take_start_expired());
    expect("the failure stays", timed.update("IDLE", false, 40, kPending, 20000), kSessionFailed);
    expect_true("and expires no more", !timed.take_start_expired());

    /* One late poll: the first record read after the grace fails the start. */
    SessionPhaseTracker late;
    (void)late.note_start_requested(40, "IDLE", 500);
    expect("a poll 5 s late", late.update("IDLE", false, 40, kPending, 5500), kSessionFailed);
    expect_true("reports its wait", late.take_start_expired(&waited) && waited == 5000);

    /* The service showing the start ends the grace, however long it then takes. */
    SessionPhaseTracker taken;
    (void)taken.note_start_requested(40, "IDLE", 0);
    expect("STARTING", taken.update("STARTING", false, 41, kPending, 500), kSessionStarting);
    expect("a long configure", taken.update("STARTING", false, 41, kPending, 9000), kSessionStarting);
    expect_true("never expires", !taken.take_start_expired());
}

/* #782: the host computed the expiry from an IDLE record, but the service took the start before the retirement. */
void
test_start_taken_after_the_snapshot() {
    SessionPhaseTracker tracker;
    (void)tracker.note_start_requested(40, "IDLE", 0);
    expect("expired on the old IDLE record", tracker.update("IDLE", false, 40, kPending, 2000), kSessionFailed);
    expect_true("expiry reported", tracker.take_start_expired());
    tracker.note_start_taken(2000);
    expect("Starting again", tracker.phase(), kSessionStarting);
    expect_true("no failure latched", !tracker.failed());
    expect("its session's STARTING", tracker.update("STARTING", false, 41, kPending, 2250), kSessionStarting);
    expect("its session runs", tracker.update("RUNNING", true, 41, kPending, 2500), kSessionRunning);
    expect("its session ends", tracker.update("IDLE", false, 41, dsd_android::kRunCompleted, 9000), kSessionIdle);
}

/* #782: a configured session the service cancelled ends cancelled and no longer owns its receiver. */
void
test_discarded_configuration() {
    dsd_android::RunStatus status;
    status.begin(41);
    status.note_configured(dsd_android::kReceiverRtl);
    expect_true("a configured session owns its receiver", status.uses_local_receiver());
    status.discard_configured();
    expect_true("a discarded one does not", !status.uses_local_receiver());
    expect_true("it ended cancelled", status.reason == dsd_android::kRunCancelled && status.session_id == 41);
    status.discard_configured();
    expect_true("again is harmless", status.reason == dsd_android::kRunCancelled);

    dsd_android::RunStatus finished;
    finished.begin(42);
    finished.note_configured(dsd_android::kReceiverAirspy);
    finished.finish(1, false, -6);
    finished.discard_configured();
    expect_true("a finished run keeps its result", finished.reason == dsd_android::kRunFailed);
}

/* The service's "failureKind" values and the kinds Main.qml words; dsd_input_failure_kind ends at 7. */
void
test_attempt_failure_wire() {
    using dsd_android::attempt_failure_from_wire;
    expect_true("previous_stopping", attempt_failure_from_wire("previous_stopping") == 8);
    expect_true("usb_permission", attempt_failure_from_wire("usb_permission") == 10);
    expect_true("usb_open_failed", attempt_failure_from_wire("usb_open_failed") == 11);
    expect_true("usb_gone", attempt_failure_from_wire("usb_gone") == 12);
    expect_true("undelivered is the host's own", dsd_android::kAttemptUndelivered == 9);
    expect_true("empty", attempt_failure_from_wire("") == dsd_android::kAttemptNone);
    expect_true("null", attempt_failure_from_wire(nullptr) == dsd_android::kAttemptNone);
    expect_true("unknown", attempt_failure_from_wire("usb_on_fire") == dsd_android::kAttemptNone);
    expect_true("after the input failure kinds",
                static_cast<int>(dsd_android::kAttemptPreviousStopping) > static_cast<int>(DSD_INPUT_FAILURE_DEVICE));
}

} // namespace

int
main(void) {
    test_terminal_waits_for_service_idle();
    {
        SessionPhaseTracker tracker;
        expect("retained failure is not this UI attempt", tracker.update("IDLE", false, 77, dsd_android::kRunFailed),
               kSessionIdle);
        expect("retained failure remains ignored", tracker.update("IDLE", false, 77, dsd_android::kRunFailed),
               kSessionIdle);
    }
    {
        SessionPhaseTracker tracker;
        if (!tracker.note_start_requested(77, nullptr)) {
            ++g_failures;
        }
        expect("missing record admits start", tracker.phase(), kSessionStarting);
    }
    // Terminal results cannot be inferred from elapsed time or a sampled running
    // flag: an entire failed run can fit between two UI ticks.
    for (int polls : {0, 12, 20}) {
        SessionPhaseTracker tracker;
        tracker.note_start_requested(10);
        for (int i = 0; i < polls; ++i) {
            tracker.update("RUNNING", true, 11, dsd_android::kRunPending);
        }
        expect("failure before first poll / after 3s / after startup",
               tracker.update("IDLE", false, 11, dsd_android::kRunFailed), kSessionFailed);
        expect("terminal failure stays latched", tracker.update("IDLE", false, 11, dsd_android::kRunFailed),
               kSessionFailed);
        tracker.note_start_requested(11);
        expect("retry ignores previous terminal result", tracker.update("IDLE", false, 11, dsd_android::kRunFailed),
               kSessionStarting);
        expect("cancel is not failure", tracker.update("IDLE", false, 12, dsd_android::kRunCancelled), kSessionIdle);
        tracker.note_start_requested(12);
        expect("short normal run is not failure", tracker.update("IDLE", false, 13, dsd_android::kRunCompleted),
               kSessionIdle);
    }
    {
        dsd_android::RunStatus result;
        result.begin(42);
        if (result.initialized || result.reason != dsd_android::kRunPending) {
            ++g_failures;
        }
        result.mark_initialized();
        result.finish(1, false, -6);
        if (!result.initialized || result.session_id != 42 || result.reason != dsd_android::kRunFailed
            || result.device_error != -6 || result.run_code != 1) {
            ++g_failures;
        }
        result.begin(43);
        if (result.initialized || result.device_error != 0) {
            ++g_failures;
        }
        result.finish(1, true, 0);
        if (result.reason != dsd_android::kRunCancelled) {
            ++g_failures;
        }
    }

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
        for (uint64_t now = 250; now < dsd_android::kStartGraceMs; now += 250) {
            expect("within grace", tracker.update("IDLE", false, now), kSessionStarting);
        }
        expect("grace expired", tracker.update("IDLE", false, dsd_android::kStartGraceMs), kSessionFailed);
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

    test_failure_recovery();
    test_start_grace_is_real_time();
    test_start_taken_after_the_snapshot();
    test_discarded_configuration();
    test_attempt_failure_wire();
    test_stop_request_during_running();
    test_stop_request_during_starting();
    test_stop_request_then_failure();
    test_later_start_clears_stop_request();
    test_stop_request_grace();
    test_receiver_end_holds_stopping_through_long_running();
    if (g_failures != 0) {
        DSD_FPRINTF(stderr, "session state map: %d failure(s)\n", g_failures);
        return 1;
    }
    return 0;
}
