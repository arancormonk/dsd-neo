// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Folds the service state machine and the engine atomic into one UI phase.
 *
 * Two sources disagree about what "running" means. DecoderService owns the
 * IDLE/STARTING/RUNNING/STOPPING transitions, but only the native @c g_running atomic
 * knows whether the engine loop actually turns; a service that says RUNNING may still
 * be inside nativeConfigure. Neither alone can tell a start that failed from a start
 * that never happened, because DecoderService.failStart() drops straight back to IDLE.
 *
 * Deliberately free of Qt and JNI so the host test can drive it directly — the whole
 * point of pulling it out of DecoderHostAndroid::refresh().
 */

#ifndef DSD_NEO_ANDROID_SESSION_STATE_MAP_H_
#define DSD_NEO_ANDROID_SESSION_STATE_MAP_H_

#include <stdint.h>
#include <string.h>
#include "run_status.h"

namespace dsd_android {

/**
 * @brief What the UI should show. Mirrors dsd_qt::DecoderHost::SessionState.
 *
 * The values are pinned because decoder_host_android.cpp static_asserts them against
 * the Q_ENUM the QML binds to.
 */
enum SessionPhase {
    kSessionIdle = 0,
    kSessionStarting = 1,
    kSessionRunning = 2,
    kSessionStopping = 3,
    kSessionFailed = 4
};

/**
 * @brief Whether a decoder engine is starting, running or winding down (#580).
 *
 * A restore must not replace the configuration under an engine, and the UI phase cannot rule one out: it reads Idle
 * before the host's first poll and trails the service by a tick. So this takes the two authorities themselves. Any
 * service state but IDLE is busy (STARTING covers nativeConfigure, before the engine atomic is set), and so is a
 * running engine whatever the service says. A state that could not be read counts as busy.
 *
 * @param service_state DecoderService.stateName(), or null or empty when the call failed.
 * @param engine_running The native g_running atomic.
 */
inline bool
engine_busy(const char* service_state, bool engine_running) {
    return engine_running || service_state == nullptr || strcmp(service_state, "IDLE") != 0;
}

/**
 * @brief How long the service may stay IDLE between asking for a start and seeing it.
 *
 * startForegroundService() only queues an intent, so the service is still IDLE for a
 * tick or two afterwards. Measured in real time from the request, not in polls (#782): a
 * busy Qt thread polls late, and eight late polls are far more than two seconds. After it
 * an undelivered start is reported as a failure rather than silently dropping the user
 * back to the setup screen; the host retires the attempt with it, so it never runs later.
 */
constexpr uint64_t kStartGraceMs = 2000;

/**
 * @brief How long a stop request may meet nothing but RUNNING records before it is presumed lost (#718).
 *
 * DecoderService handles its intents in order on the Android main thread, and startDecoding() runs a whole start
 * there, so a delivered stop always finds the service RUNNING and turns it STOPPING. Polls run on the Qt thread and the
 * intent on the main thread, so two seconds of RUNNING after the request means the main thread has not handled the
 * intent yet (it is busy), and the UI gives the user a working Stop button back instead of holding Stopping forever.
 */
constexpr uint64_t kStopGraceMs = 2000;

/**
 * @brief Sticky-failure state machine over successive polls.
 *
 * Not thread-safe: like everything else the UI touches, it belongs to the Qt main
 * thread and is driven from the single poll tick.
 */
class SessionPhaseTracker {
  public:
    /**
     * @brief Note that a start was just asked for.
     *
     * Clears any previous failure and holds the phase at Starting through the gap
     * before the service picks the intent up.
     */
    void
    note_start_requested() {
        m_observed = true;
        m_ignore_retained = false;
        m_waiting_for_session = false;
        m_failed = false;
        m_attempting = true;
        m_saw_running = false;
        m_start_pending = true;
        m_start_requested_ms = 0;
        m_start_grace_from_ms = 0;
        m_start_expired = false;
        m_phase = kSessionStarting;
        clear_stop_request();
        m_receiver_end = false;
    }

    /** Ignore the previous session's retained result while an intent is in flight. */
    void
    note_start_requested(uint64_t last_session) {
        note_start_requested();
        m_last_session = last_session;
        m_waiting_for_session = true;
    }

    /** A native return does not release the service's wake lock or worker slot.
     * The host must check a fresh service record before submitting a retry, even
     * if its last published UI phase still allowed starting. A rejected retry
     * leaves the failure/session latch and start grace period untouched. */
    bool
    note_start_requested(uint64_t last_session, const char* service_state) {
        if (service_state && service_state[0] && strcmp(service_state, "IDLE") != 0) {
            return false;
        }
        note_start_requested(last_session);
        return true;
    }

    /**
     * @brief As above, with the start grace measured from @p now_ms (dsd_realtime_mono_ms()), the clock update()
     * is given. The overloads without it measure from 0.
     */
    bool
    note_start_requested(uint64_t last_session, const char* service_state, uint64_t now_ms) {
        if (!note_start_requested(last_session, service_state)) {
            return false;
        }
        m_start_requested_ms = now_ms;
        m_start_grace_from_ms = now_ms;
        return true;
    }

    /**
     * @brief Whether the start grace just ran out and failed the start (true once per expiry), and how long the
     * start had waited then (@p waited_ms, when given). The host retires the attempt and logs it.
     */
    bool
    take_start_expired(uint64_t* waited_ms = nullptr) {
        const bool expired = m_start_expired;
        m_start_expired = false;
        if (expired && waited_ms != nullptr) {
            *waited_ms = m_start_waited_ms;
        }
        return expired;
    }

    /**
     * @brief Undo an expiry: the service took the start after the IDLE record the expiry was computed from (#782).
     *
     * The attempt is starting after all, so the phase is Starting again and the record of its session, which the
     * next poll reads, is followed as for any start; the grace restarts from @p now_ms meanwhile.
     */
    void
    note_start_taken(uint64_t now_ms) {
        m_failed = false;
        m_attempting = true;
        m_saw_running = false;
        m_start_pending = true;
        m_start_requested_ms = now_ms;
        m_start_grace_from_ms = now_ms;
        m_start_expired = false;
        m_phase = kSessionStarting;
    }

    /**
     * @brief Fold one lifecycle record into the phase.
     * @param now_ms Real monotonic milliseconds (dsd_realtime_mono_ms()), read while a start waits for the service
     *        and while a stop request stands. The default 0 means no clock: neither ever expires.
     */
    SessionPhase
    update(const char* service_state, bool engine_running, uint64_t session_id, RunReason reason, uint64_t now_ms = 0) {
        m_now_ms = now_ms;
        if (m_receiver_end && session_id > m_receiver_end_session) {
            m_receiver_end = false; // a later session's record: the ended one is long gone
        }
        return hold_receiver_end(
            hold_stop(service_state, fold_record(service_state, engine_running, session_id, reason), now_ms));
    }

    /**
     * @brief Fold a lifecycle record of another start attempt than the one this tracker follows (gate r5 L1).
     *
     * The host passes here every record whose attempt is not its current one: an attempt it retired or superseded,
     * or the previous session's, while the current start waits for the service. Such a record is not the current
     * attempt's start, run or end, so the phase, the session followed, a stop request and a receiver ending are left
     * as they are. Only the start grace goes on: while the start waits for the service, an expiry is judged here as on
     * an IDLE record of the previous session. A STARTING record restarts the grace from @p now_ms: the service is
     * busy with a start that can no longer run (a retirement cancels it once its configure returns), and handles the
     * current START after it, on the same thread. The expiry still reports the wait since the request.
     *
     * @param service_state The record's DecoderService state name.
     * @param now_ms As for update().
     */
    SessionPhase
    update_other_attempt(const char* service_state, uint64_t now_ms) {
        m_now_ms = now_ms;
        if (!m_start_pending) {
            return m_phase;
        }
        if (service_state != nullptr && strcmp(service_state, "STARTING") == 0) {
            m_start_grace_from_ms = now_ms;
            return m_phase;
        }
        if (idle_phase() == kSessionFailed) {
            // The expiry ends the start, and with it a stop request or receiver ending standing over it, as the
            // terminal Failed of an IDLE record does in hold_stop() and hold_receiver_end().
            m_phase = kSessionFailed;
            clear_stop_request();
            m_receiver_end = false;
        }
        return m_phase;
    }

    /**
     * @brief Fold one poll that carried no lifecycle record into the phase.
     * @param service_state DecoderService.stateName(), or null when the call failed.
     * @param engine_running The native g_running atomic.
     * @param now_ms As for the record overload (default 0 = no clock: a stop request never expires).
     */
    SessionPhase
    update(const char* service_state, bool engine_running, uint64_t now_ms = 0) {
        m_now_ms = now_ms;
        return hold_receiver_end(hold_stop(service_state, fold_state(service_state, engine_running), now_ms));
    }

    /**
     * @brief Note that the user asked the attempt this tracker follows to stop (#718).
     *
     * Publishes Stopping at once, before the service has even seen the intent. The request belongs to the start
     * attempt, not to a session id, because a start may still be reporting the previous session's IDLE record or a
     * provisional STARTING before its own id exists. While it stands every record of the attempt publishes Stopping.
     * It ends at the attempt's terminal record (Idle, or a failure, published as without a request), when a later
     * start attempt begins, or after kStopGraceMs of RUNNING with no STOPPING (take_stop_expired()).
     *
     * @param now_ms Real monotonic milliseconds.
     * @return true when a request was taken: only from Starting or Running, and only once per attempt.
     */
    bool
    note_stop_requested(uint64_t now_ms) {
        if (m_stop_requested || (m_phase != kSessionStarting && m_phase != kSessionRunning)) {
            return false;
        }
        m_stop_requested = true;
        m_stop_delivered = false;
        // From Running the RUNNING run is already under way; from Starting it begins at the first RUNNING record.
        m_stop_running = m_phase == kSessionRunning;
        m_stop_running_since_ms = now_ms;
        m_phase = kSessionStopping;
        return true;
    }

    /**
     * @brief Note that the radio layer ended @p session's receiver (#647): an unplug or a stream that stopped.
     *
     * The session is over and winding down, so Stopping is published at once and held as an acknowledged stop until
     * that session's terminal record. Its cleanup can keep the service RUNNING for minutes (the rdio upload drain), so
     * kStopGraceMs never applies, and a user Stop standing beside it no longer expires either. Taken only for the
     * session this tracker follows while it starts, runs or stops; repeating it is harmless.
     *
     * @return true when the ending was taken (the phase is now Stopping).
     */
    bool
    note_receiver_end(uint64_t session) {
        if (session == 0 || session != m_last_session || m_waiting_for_session
            || (m_phase != kSessionStarting && m_phase != kSessionRunning && m_phase != kSessionStopping)) {
            return false;
        }
        m_receiver_end = true;
        m_receiver_end_session = session;
        m_stop_delivered = m_stop_requested;
        m_phase = kSessionStopping;
        return true;
    }

    /**
     * @brief Note that the host gave up on the start it follows after the service had taken it after all (#782).
     *
     * The expiry failed the start, and the retirement it made cancels the attempt the service is configuring: it ends
     * cancelled without running. Its failure dismissed, the attempt is winding down, so Stopping is published and held
     * as a stop request is, until that attempt's terminal record; a RUNNING run of kStopGraceMs (which a cancelled
     * attempt never reaches) gives it back.
     */
    void
    note_attempt_retired(uint64_t now_ms) {
        m_failed = false;
        m_attempting = false;
        m_saw_running = false;
        m_start_pending = false;
        m_stop_requested = true;
        m_stop_delivered = false;
        m_stop_running = false;
        m_stop_running_since_ms = now_ms;
        m_phase = kSessionStopping;
    }

    /** @brief Whether a stop request was just dropped by the grace (true once per drop), for one diagnostics line. */
    bool
    take_stop_expired() {
        const bool expired = m_stop_expired;
        m_stop_expired = false;
        return expired;
    }

    /**
     * @brief Note that the start could not even be handed to the service.
     *
     * The host calls this when it has no Android context or cannot marshal the
     * arguments — failures that never reach DecoderService at all.
     */
    void
    note_start_failed() {
        m_failed = true;
        m_attempting = false;
        m_saw_running = false;
        m_start_pending = false;
        m_phase = kSessionFailed;
        clear_stop_request();
        m_receiver_end = false;
    }

    /** Explicit dismissal adopts only a confirmed inactive service's retained result. */
    bool
    acknowledge_failure(uint64_t session, const char* service_state) {
        if (m_phase != kSessionFailed || !service_state || strcmp(service_state, "IDLE") != 0) {
            return false;
        }
        *this = SessionPhaseTracker();
        (void)update(service_state, false, session, kRunPending);
        return true;
    }

    SessionPhase
    phase() const {
        return m_phase;
    }

    /** @brief Whether the current session has a latched failure, including during teardown. */
    bool
    failed() const {
        return m_failed;
    }

  private:
    SessionPhase
    fold_record(const char* service_state, bool engine_running, uint64_t session_id, RunReason reason) {
        if (!m_observed) {
            m_observed = true;
            m_last_session = session_id;
            if (!service_state || strcmp(service_state, "IDLE") == 0) {
                m_ignored_session = session_id;
                m_ignore_retained = true;
            }
        }
        if (m_ignore_retained && session_id == m_ignored_session) {
            return fold_state(service_state, engine_running);
        }
        if (m_waiting_for_session && session_id <= m_last_session) {
            m_phase = idle_phase();
            return m_phase;
        }
        if (session_id < m_last_session) {
            return m_phase;
        }
        if (session_id > m_last_session) {
            m_last_session = session_id;
            m_waiting_for_session = false;
            m_failed = false;
        }
        // Retain the terminal outcome immediately, but only service IDLE means
        // the wake lock and worker slot have been released. Publishing Failed or
        // Idle earlier enables a retry the service would reject, followed by a
        // misleading start timeout. Stopping keeps restart disabled in that gap.
        if (reason == kRunFailed || reason == kRunCompleted || reason == kRunCancelled) {
            return terminal_phase(service_state, reason);
        }
        return fold_state(service_state, engine_running);
    }

    SessionPhase
    fold_state(const char* service_state, bool engine_running) {
        const char* name = (service_state != nullptr) ? service_state : "IDLE";

        if (strcmp(name, "STARTING") == 0) {
            m_attempting = true;
            m_start_pending = false;
            m_phase = kSessionStarting;
        } else if (strcmp(name, "RUNNING") == 0) {
            m_attempting = true;
            m_start_pending = false;
            // The service flips to RUNNING before the engine thread has entered the
            // loop, so the atomic is what promotes Starting to Running.
            if (engine_running) {
                m_saw_running = true;
                m_phase = kSessionRunning;
            } else {
                m_phase = m_saw_running ? kSessionStopping : kSessionStarting;
            }
        } else if (strcmp(name, "STOPPING") == 0) {
            m_start_pending = false;
            m_phase = kSessionStopping;
        } else {
            m_phase = idle_phase();
        }
        return m_phase;
    }

    /**
     * @brief Hold Stopping over @p folded while a stop request stands.
     *
     * Idle and Failed are the attempt's terminal records and end the request. STOPPING proves the intent arrived,
     * after which nothing but the terminal record can end it. RUNNING records are timed against kStopGraceMs.
     */
    SessionPhase
    hold_stop(const char* service_state, SessionPhase folded, uint64_t now_ms) {
        if (!m_stop_requested) {
            return folded;
        }
        if (folded == kSessionIdle || folded == kSessionFailed) {
            clear_stop_request();
            return folded;
        }
        const bool running = service_state != nullptr && strcmp(service_state, "RUNNING") == 0;
        if (service_state != nullptr && strcmp(service_state, "STOPPING") == 0) {
            m_stop_delivered = true;
        }
        if (!running) {
            m_stop_running = false;
        } else if (!m_stop_delivered) {
            if (!m_stop_running) {
                m_stop_running = true;
                m_stop_running_since_ms = now_ms;
            } else if (now_ms >= m_stop_running_since_ms && now_ms - m_stop_running_since_ms >= kStopGraceMs) {
                clear_stop_request();
                m_stop_expired = true;
                return folded;
            }
        }
        m_phase = kSessionStopping;
        return m_phase;
    }

    /** @brief Hold Stopping over @p folded while a receiver ending stands; the terminal record (Idle or Failed) ends it. */
    SessionPhase
    hold_receiver_end(SessionPhase folded) {
        if (!m_receiver_end) {
            return folded;
        }
        if (folded == kSessionIdle || folded == kSessionFailed) {
            m_receiver_end = false;
            return folded;
        }
        m_phase = kSessionStopping;
        return m_phase;
    }

    void
    clear_stop_request() {
        m_stop_requested = false;
        m_stop_delivered = false;
        m_stop_running = false;
        m_stop_running_since_ms = 0;
    }

    SessionPhase
    terminal_phase(const char* service_state, RunReason reason) {
        if (reason == kRunFailed) {
            (void)latch_failure();
        } else {
            m_attempting = false;
            m_saw_running = false;
            m_start_pending = false;
        }
        const bool service_idle = service_state && strcmp(service_state, "IDLE") == 0;
        m_phase = service_idle ? (m_failed ? kSessionFailed : kSessionIdle) : kSessionStopping;
        return m_phase;
    }

    /**
     * @brief Resolve an IDLE poll, which is three different things.
     *
     * Before the service has seen the intent it means "not yet"; after a start that
     * reached STARTING but never ran it means the start failed; otherwise it is a
     * genuine idle. A failure latches until a start request or explicit dismissal so the reason
     * stays on screen instead of flashing past in one 250 ms tick.
     */
    SessionPhase
    idle_phase() {
        if (m_start_pending) {
            if (m_now_ms < m_start_grace_from_ms || m_now_ms - m_start_grace_from_ms < kStartGraceMs) {
                return kSessionStarting;
            }
            m_start_expired = true;
            m_start_waited_ms = m_now_ms - m_start_requested_ms;
            return latch_failure();
        }
        if (m_attempting && !m_saw_running) {
            return latch_failure();
        }
        m_attempting = false;
        m_saw_running = false;
        return m_failed ? kSessionFailed : kSessionIdle;
    }

    SessionPhase
    latch_failure() {
        m_failed = true;
        m_attempting = false;
        m_saw_running = false;
        m_start_pending = false;
        return kSessionFailed;
    }

    SessionPhase m_phase = kSessionIdle;
    uint64_t m_last_session = 0;
    bool m_observed = false;
    bool m_ignore_retained = false;
    uint64_t m_ignored_session = 0;
    bool m_waiting_for_session = false;
    bool m_failed = false;
    bool m_attempting = false;
    bool m_saw_running = false;
    // A start was asked for at m_start_requested_ms and the service has not shown it yet (kStartGraceMs).
    bool m_start_pending = false;
    uint64_t m_start_requested_ms = 0;
    // Where the grace is measured from: the request, or a later STARTING record of another attempt the service
    // handled first (update_other_attempt()).
    uint64_t m_start_grace_from_ms = 0;
    bool m_start_expired = false;
    uint64_t m_start_waited_ms = 0;
    // The clock of the record being folded (update()'s now_ms).
    uint64_t m_now_ms = 0;
    bool m_stop_requested = false;
    // A STOPPING record of the attempt was seen since the request.
    bool m_stop_delivered = false;
    // RUNNING records have arrived without a break since m_stop_running_since_ms.
    bool m_stop_running = false;
    uint64_t m_stop_running_since_ms = 0;
    bool m_stop_expired = false;
    // note_receiver_end() holds Stopping for m_receiver_end_session until its terminal record.
    bool m_receiver_end = false;
    uint64_t m_receiver_end_session = 0;
};

} // namespace dsd_android

#endif /* DSD_NEO_ANDROID_SESSION_STATE_MAP_H_ */
